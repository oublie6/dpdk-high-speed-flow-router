# 架构说明：Goal 004-005 Static Lookup 与 Packet Action

## 1. 当前实现

Goal 004-005 在 Goal 003 parser 后加入不可变 lookup/action snapshot：

~~~text
Go CLI / static JSON / signal handler
        |
control/dataplane        JSON 校验 + lifecycle manager
        |
dataplane/native         唯一 cgo package + package-local C
        |
DPDK EAL -> TAP RX/RXQ0 -> fixed owner lcore
         -> Ethernet/IPv4/TCP/UDP parse
         -> exact flow -> IPv4 LPM fallback -> action -> TAP TX/TXQ0
~~~

worker 在每次 RX burst 后先记录 RX，再逐包完成 parse、lookup 和 action。DROP packet
立即 free；FORWARD 与成功 REWRITE 的 mbuf 在原 RX pointer 数组中压缩后执行一次
TX burst。当前没有 runtime rule update、RSS、multi-queue 或 multi-lcore。

## 2. 构建与模块组织

所有项目 native 文件都在 `dataplane/native`，由 cgo 作为同一 package 的 source
跟踪：

~~~text
binding_linux.go          cgo 与 C memory 转换
dp_binding.c/.h           argv/rule array 的命名 helper
dp_api.h                  粗粒度项目 API 与跨边界数值结构
dp_internal.h             C-only runtime state
dp_runtime.c              EAL、状态快照与 cleanup guard
dp_port.c                 port / queue / mempool / rules teardown
dp_worker.c/.h            RTC loop 与单包 ownership
dp_packet.h               metadata 与 parse result
dp_parser.c/.h            Ethernet/IPv4/TCP/UDP parser
dp_parser_test.c           deterministic parser fixture
dp_lookup.c/.h            rte_hash、rte_lpm、action store lifecycle
dp_action.c/.h            IPv4/TCP/UDP rewrite 与 checksum
dp_lookup_action_test.c    lookup/action/ownership fixture
dp_tx.c/.h                TX partial-return ownership
dp_test.h                  package-private test hooks
~~~

普通 `go build` / `go test` 能发现 C 改动，不需要 `go build -a` 或第二套 Meson
工程。DPDK flags 由 `pkg-config libdpdk` 提供；旧版 cgo 的精确 allowlist 只由 Makefile
在项目进程中 export，安装脚本不修改用户 persistent Go environment。

`control/dataplane` 不 import C，也不接触 mbuf。它只导入 `dataplane/native`，管理
config、ready、stop、wait 和 owner goroutine。源码编译期与运行时均锁定 DPDK
25.11.3。

## 3. EAL、规则与 owner thread 生命周期

Go goroutine 会在 OS thread 间迁移，而 EAL 会设置 thread-local lcore state 与
CPU affinity。因此整个生命周期固定在一个 `runtime.LockOSThread()` goroutine：

~~~text
native.Init(EAL argv)
-> native.GetInfo()
-> native.ConfigureRules(static snapshot)
-> native.Setup(TAP ports, pool, queues)
-> close ready channel
-> native.Run()                 blocking C RTC loop
-> atomic stop observed
-> worker returns / 无 pending mbuf
-> native.Teardown()            rules -> ports -> pool
-> native.GetStats()
-> Teardown 成功：native.Cleanup()
-> Teardown 失败：返回错误，不调用 Cleanup，由进程退出兜底
~~~

另一个 goroutine 只允许调用 `native.RequestStop()`，该函数只写 C11 atomic flag。
EAL 是 process-global 且不能重新初始化；同一进程拒绝第二次 lifecycle，集成测试使用
独立子进程隔离。

Go owner 显式保存 `teardownErr`，只有 Teardown 成功才调用 Cleanup。C 的
`dp_runtime_cleanup()` 同时检查 worker、port、pool、flow table、route table、action
store 和规则配置状态；任何资源仍存活都返回 `-EBUSY`，不得进入
`rte_eal_cleanup()`。

## 4. 静态 JSON 与 cgo ownership

CLI 的 `--rules-file <json>` 只在启动前读取一次。Go 使用 strict JSON decoder，拒绝
未知字段、尾随值、非 IPv4 地址、非法 CIDR、非 TCP/UDP protocol、非法 action、
空 rewrite、duplicate flow、规范化后重复 route 与 capacity overflow。CIDR 会 mask
到 network address，IPv4/port 在 snapshot 中保存为 host byte order 数值。

thin binding 用 `dp_flow_rules_alloc/set` 与 `dp_route_rules_alloc/set` 填写临时 C array，
Go 不计算 C array 元素地址。`dp_configure_rules()` 在返回前把全部规则复制到 C-owned
资源，临时 array 随即释放；C 不长期保存 Go pointer。运行开始后没有 Add、Delete、
Replace 或 reload API，native 层也拒绝重复配置和 Run 后配置。

EAL argv 继续由 C helper 分配和设置。EAL 可能重排 argv，因此原始 C string 地址保留
到 cleanup；`Info`、`Stats` 都返回 Go-owned snapshot。

## 5. Exact flow、IPv4 LPM 与 action store

exact flow key 固定为 16 bytes：

~~~c
struct dp_flow_key {
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t l4_proto;
    uint8_t reserved[3];
};
~~~

`dp_flow_key_from_meta()` 先清零整个 key，再填写 Goal 003 host-order metadata，保证
reserved/padding 不携带未初始化数据。`rte_hash` 的 data pointer 直接引用统一的
immutable action store。`rte_lpm` 以 `meta.dst_ipv4` 查询，next-hop 值保存同一 action
store 的 index。lookup 顺序固定为：

~~~text
rte_hash exact TCP/UDP 5-tuple
-> miss: rte_lpm IPv4 longest-prefix
-> miss: default DROP
~~~

flow hit 后立即返回，禁止继续 route lookup。route 同时存在 `/8` 与 `/24` 时，
`rte_lpm` 选择 `/24`。当前容量为 1024 flows、1024 routes；没有 wildcard、range、
ACL、bulk lookup、锁、热路径 allocation 或动态 writer。

统一 action 只支持 DROP、FORWARD、REWRITE。rewrite mask 只允许 src/dst IPv4 和
src/dst TCP/UDP port；未设置字段保持不变，不修改 MAC、TTL、VLAN 或 payload。任何
rewrite 都先把 IPv4 与 L4 checksum 字段清零，再调用 `rte_ipv4_cksum()` 和
`rte_ipv4_udptcp_cksum()` 软件重算，不依赖 TX checksum offload。

## 6. Parser、worker stats 与 mbuf ownership

`dp_parse_packet()` 仍只接受 single-segment mbuf，以 segment `data_len` 为连续边界；
multi-segment 返回 unsupported。metadata 的 EtherType、IPv4 地址和 TCP/UDP port
全部为 host byte order，IHL、TCP data offset 和 L4 offset 由 packet 字段计算。
parser 不修改或 free mbuf。

每个 burst 的 ownership 转移为：

~~~text
rte_eth_rx_burst 返回 [0,n)       application owns
parse unsupported/malformed       worker 立即 free
lookup miss / action DROP         worker 立即 free
FORWARD / REWRITE                 原地压缩到 [0,tx_count)
rte_eth_tx_burst 接受 [0,sent)    ownership 转给 TX PMD
TX 未接受 [sent,tx_count)         application 立即 free
~~~

仍采用一次 TX burst、zero-retry policy。最终维持五条守恒：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = flow_hit + route_hit + lookup_miss
parse_ok = action_drop + action_forward + action_rewrite
action_forward + action_rewrite = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + action_drop + tx_unsent
~~~

production worker、lookup/action integration test 和 partial-return test 共用同一
ownership helper。hot loop 没有逐包日志、heap allocation、Go callback 或 lock。

worker 返回后 Teardown 先释放 hash/LPM/action store，再关闭两个 ethdev。port close
后验证 mempool in-use 为 0 才释放 pool。此顺序保证 worker 不再读取静态规则，也避免
PMD 仍持有 mbuf 时提前释放 pool。

## 7. TAP 端到端验证

`scripts/verify_tap_forwarding.py` 为每次运行生成临时 rules JSON、唯一 TAP 名和
ifindex ownership 记录。它选择允许的 CPU 映射为 logical lcore 0，以 `--no-pci
--no-huge` 启动，并注入：

1. exact flow DROP，同时目标 `/24` route 是 FORWARD；
2. exact flow REWRITE，修改 dst IPv4 与 dst UDP port；
3. flow miss 后 `/24` route FORWARD；
4. flow/route miss 后 default DROP；
5. Goal 003 IHL=4 malformed IPv4。

TX TAP 必须看不到三个 drop marker；FORWARD frame 必须逐字节不变；REWRITE frame
必须保持 payload，并通过 IPv4/UDP checksum 验证。脚本不把 TAP background packet
计数写死，而是检查 deterministic case 的最低命中数和全部五条 stats 守恒。

停止后还要求两个 port close、pool in-use=0、pool/hash/LPM/action store 全部 free、
EAL cleanup 成功，并且不遗留 router process、TAP、rules JSON 或临时目录。失败路径
同样先 graceful stop，只按已记录 ifindex 清理自身接口。脚本不配置主机地址、route、
firewall、管理网卡、VFIO 或 HugePage。

本机 Go runtime 占用 realtime signals 时，TAP PMD 会退回非阻塞 polling；functional
结果不受影响，但该软件实验不能解释为性能数据。

## 8. 软件仿真边界与下一步

当前证据覆盖 Go/cgo/C ownership、EAL lifecycle、TAP PMD、Ethernet/IPv4/TCP/UDP
parser、exact flow、IPv4 LPM、DROP/FORWARD/REWRITE、软件 checksum、单 queue RTC
和完整 cleanup。它不能证明真实 NIC DMA、hardware RSS、cross-NUMA cost、descriptor
行为、line-rate、latency 或 throughput。

runtime rule update、RCU/QSBR、RSS、multi-queue、multi-lcore、API/Web、NAT 与
conntrack 都仍未实现。Goal 004-005 完成后停止开发，等待 ChatGPT 验收。
