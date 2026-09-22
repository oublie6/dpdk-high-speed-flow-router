# 架构说明：Goal 003 TAP RTC Dataplane 与 Packet Parser

## 1. 当前实现

Goal 003 在 Goal 002 long-running packet path 中加入 C/DPDK parser：

~~~text
Go CLI / signal handler
        |
control/dataplane        纯 Go lifecycle manager
        |
dataplane/native         唯一 cgo package + package-local C
        |
DPDK EAL -> TAP RX/RXQ0 -> fixed owner lcore
         -> Ethernet/IPv4/TCP/UDP parse -> TAP TX/TXQ0
~~~

worker 在每次 RX burst 后先记录 RX，再逐包解析。parse OK 的 mbuf 在原 RX pointer
数组中压缩后执行一次 TX burst；unsupported/malformed mbuf 立即由 worker free。
当前没有 lookup、rewrite、RSS、multi-queue 或 multi-lcore。

## 2. 构建组织

Goal 001 的旧假设是通过 `control/dataplane/runtime_linux.c` include 外部 `.c`
文件，并用 `go build -a` 绕开 cache。真实 dataplane 增加多个 C 文件后，这种方式
无法可靠表达 Go package 的依赖边界。

现在所有项目 native 文件都在 `dataplane/native`：

~~~text
binding_linux.go       cgo 与 C memory 转换
dp_binding.c/.h        argv 数组分配、设置与释放 helper
dp_api.h               粗粒度项目 API
dp_internal.h          C-only runtime state
dp_runtime.c           EAL 与状态快照
dp_port.c              port / queue / mempool lifecycle
dp_worker.c            RTC loop 与 atomic stop
dp_packet.h            metadata 与 parse result
dp_parser.c/.h         Ethernet/IPv4/TCP/UDP parser
dp_parser_test.c       package 内 deterministic parser fixture
dp_tx.c / dp_tx.h      TX ownership 完成逻辑
dp_test.h              package 内 parser/ownership 测试钩子
~~~

cgo 会把同目录 `.c` 作为 package source 正常跟踪，因此普通 `go build` / `go test`
可以发现 C 改动，不再需要 `go build -a`。当前规模不值得增加 `libflowdp.a/.so`
或第二套项目 Meson build；DPDK flags 仍由 `pkg-config libdpdk` 提供。

旧版 cgo 对 DPDK pkg-config flags 的精确 allowlist 由 Makefile export，仅作用于
`make build/test/vet` 的项目进程。安装脚本不写用户级 persistent Go environment；
直接运行 Go 命令时由调用者显式设置同一 allowlist。

`control/dataplane` 不 import C，也不接触 mbuf。它只导入 `dataplane/native`，
管理 config、ready、stop、wait 和 owner goroutine。源码编译期与运行时均锁定
DPDK 25.11.3，不维护 19.11 compatibility branch。

## 3. EAL 与 owner thread 生命周期

Go goroutine 会在 OS thread 间迁移，而 EAL 会设置 thread-local lcore state 与
affinity。因此整个数据面生命周期固定在一个 `runtime.LockOSThread()` goroutine：

~~~text
native.Init(EAL argv)
-> native.GetInfo()
-> native.Setup(TAP ports, pool, queues)
-> close ready channel
-> native.Run()                 blocking C RTC loop
-> atomic stop observed
-> worker returns / 已无 pending mbuf
-> native.Teardown()            port stop + close，再检查/free pool
-> native.GetStats()
-> Teardown 成功：native.Cleanup()  最后执行 rte_eal_cleanup
-> Teardown 失败：返回错误           不调用 cleanup，由进程退出兜底
-> locked goroutine exits
~~~

另一个 Go goroutine 只允许调用 `native.RequestStop()`。该函数仅写 C11
`atomic_bool`，不从非 owner thread 操作 ethdev、queue 或 mempool。ready/done
channel close 发布 owner 写入的 Go snapshot，调用者不会并发读取未发布状态。

EAL 是 process-global 且不能重新初始化。任何 init 尝试后，同一进程都拒绝
第二次 lifecycle；正常集成测试使用独立子进程隔离 case。DPDK 25.11.3 的
argparse 遇到未知 EAL 参数会直接非零退出子进程，回归测试按这个真实行为断言。

Go owner 显式保存 `teardownErr`。它仍会在状态允许时读取最终 stats，但只有
Teardown 成功才调用 Cleanup。C 的 `dp_runtime_cleanup()` 也会检查 `running`、
port ownership/start 状态和 pool 指针；任何资源仍存活都返回 `-EBUSY`。这层
防御避免未来调用方绕过 Go 顺序后直接进入 `rte_eal_cleanup()`。

## 4. argv 与跨边界 ownership

`rte_eal_init()` 可以修改 argv，所以 binding 为 argv array 与所有 string 分配
C memory，并另存原始 string 地址。`dp_argv_alloc/set/free_array` 三个小型 C helper
负责 C 数组布局，Go 只表达分配、逐项设置和释放，不再使用 `uintptr + offset`。
cleanup 成功后逐一释放；init 或 cleanup
终止性失败时保留这批小内存到进程退出，避免释放可能仍被部分 EAL state 引用的
地址。没有 Go pointer 长期保存到 C。

`Info` 与 `Stats` 返回 Go-owned snapshot。DPDK version 的静态 C string 在 EAL
cleanup 前复制为 Go string。项目 C API 返回 0 或负 errno，binding 添加 operation
context 并转为 Go error。

## 5. port、queue 与 mempool ownership

EAL 参数创建两个独立 TAP vdev。setup 通过 `rte_eth_dev_get_port_by_name()` 按
`net_tap_rx` / `net_tap_tx` identity 查找动态 port id，不假定它们是 0 和 1，
并检查 driver 必须是 `net_tap`。

当前仅有一个 C-owned pool：4096 个 mbuf、cache 128、
`RTE_MBUF_DEFAULT_BUF_SIZE`，socket id 取 owner thread 的 `rte_socket_id()`。
单 lcore、单向软件链路没有拆多个 pool 的 locality 或 ownership 收益。

TAP PMD 的实现要求每个 port 配置对称的 RXQ0/TXQ0，descriptor 初值为 256，
再由 `rte_eth_dev_adjust_nb_rx_tx_desc()` 调整。实际 hot path 只 poll RX port 的
RXQ0，只向 TX port 的 TXQ0 发送；另两个兼容 queue 不进入 worker。

teardown 顺序固定为：

~~~text
worker 已返回
-> 两个 ethdev stop
-> 两个 ethdev close（TAP RX queue 归还其预分配 mbuf）
-> 验证 mempool in-use == 0
-> rte_mempool_free()
-> rte_eal_cleanup()
~~~

任一 port close 失败时不释放 pool，进程以错误退出，避免 PMD use-after-free。

## 6. Parser、metadata 与 mbuf ownership

`dp_parse_packet()` 只接受 single-segment mbuf，不修改也不释放 packet。它先检查
`nb_segs == 1`，再以当前 segment 的 `data_len` 为连续内存边界；multi-segment
直接返回 `DP_PARSE_UNSUPPORTED`，本阶段不使用 `rte_pktmbuf_read()`、linearize
或跨 segment copy。

parser 将 EtherType、IPv4 src/dst、TCP/UDP src/dst port 全部转为 host byte order。
metadata 同时记录 L2/L3/L4 header length 与 L4 offset。IPv4 IHL 和 TCP data offset
按 packet 字段计算，不假定 header 固定为 20 bytes。IPv4 fragment、非 IPv4 与
非 TCP/UDP 属于 unsupported；header 截断和非法 length/version/offset 属于
malformed。parser hot path 没有 heap allocation、锁或逐包日志。

每个 burst 的 ownership 转移为：

~~~text
rte_eth_rx_burst 返回 [0,n)       application owns
parse unsupported/malformed       worker 立即 free
parse OK                          原地压缩到 [0,tx_count)
rte_eth_tx_burst 接受 [0,sent)    ownership 转给 TX PMD
TX 未接受 [sent,tx_count)         application 仍 owns，立即 free
~~~

当前采用 zero-retry policy：只调用一次 TX，不做无限 retry，也不做软件 TX buffer。
`dp_complete_tx()` 只更新 `tx_accepted`、`tx_unsent`、`drop` 并释放 TX 未接受尾部；
RX 与 parser 分类由 worker 在 parse 阶段更新，避免 parse drop 从 RX 统计中消失。
最终保持：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + tx_unsent
~~~

真实 worker 与 partial-return 集成测试调用同一个函数。测试模拟 4 个 mbuf 中只接受
2 个，先证明 pool 仍有 2 个 in-use，再模拟 PMD 释放 accepted mbuf，最终 in-use
必须回到 0。因此不会 free 已转移的 mbuf，也不会泄漏未发送 mbuf。

hot loop 没有逐包日志、allocation、Go callback 或 lock。

## 7. TAP 端到端验证

`scripts/verify_tap_forwarding.py` 为每次运行生成唯一 `dfrx<suffix>` /
`dftx<suffix>`，记录创建后的 ifindex，只操作这些接口。它选择当前进程允许的
一个 CPU，将其映射到 DPDK logical lcore 0，使用 `--no-pci --no-huge` 启动。

验证器通过 AF_PACKET 先注入一个 IHL=4 的 deterministic malformed IPv4 frame，
再注入包含 `dpdk-flow-router-goal003` 的合法 66-byte Ethernet/IPv4/UDP frame。
TX TAP 捕获后比较合法 frame 的全部字节，并拒绝 malformed frame。随后发送
SIGTERM，并断言：

- `parse_malformed >= 1`，且三条 parser/TX/drop 统计守恒成立；
- 两个 port 已 close；
- pool in-use 为 0 且已 free；
- EAL cleanup 成功；
- 不遗留 router process、TAP interface 或临时文件。

脚本有整体 timeout；失败也会 graceful stop，必要时才 kill 子进程，并按已记录
ifindex 清理自己的 TAP。它不配置地址、默认路由、firewall、管理网卡、
Kubernetes/Cilium、VFIO 或 HugePage。

本机 Go runtime 占用了 realtime signals，TAP PMD 因而打印 `No Rx trigger signal
available` 并退回非阻塞 polling；functional forwarding 不受影响。该现象说明本次
结果不应被解释为性能数据。

## 8. 软件仿真边界与下一步

当前证据证明 Go/cgo/C ownership、EAL lifecycle、TAP PMD、single-segment
Ethernet/IPv4/TCP/UDP parsing、parser failure drop、单 queue RTC 原样 forwarding
和 cleanup。它不能证明真实 NIC DMA、hardware RSS、cross-NUMA cost、descriptor
行为、line-rate、latency 或 throughput。

flow/route table、rewrite、RCU/QSBR、RSS、multi-queue、multi-lcore、API/Web、NAT
与 conntrack 都仍未实现。Goal 003 完成后只等待验收，不开始 Goal 004。
