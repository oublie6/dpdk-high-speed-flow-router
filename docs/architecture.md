# 架构说明：Multi-Queue RTC、Dynamic Snapshot 与 QSBR

## 1. 当前实现

当前数据路径为：

~~~text
Go CLI / JSON / SIGHUP / Runtime CRUD
        |
control/dataplane        校验、Go-owned snapshot、writer mutex
        |
dataplane/native         thin cgo、C writer mutex、snapshot builder
        |
        +-> atomic active_rules exchange -> DPDK QSBR -> reclaim old generation
        |
DPDK EAL -> TAP RXQi -> fixed worker i / lcore i
         -> burst 级 acquire-load snapshot
         -> Ethernet/IPv4/TCP/UDP parse
         -> exact flow -> IPv4 LPM fallback -> action -> TAP TXQi
         -> QSBR reader i quiescent
~~~

packet hot path 仍全部位于 C。Go 不逐包跨 cgo；worker 不使用 mutex、refcount、逐包
allocation 或逐包日志。当前支持 1/2/4 workers，每个 RXQ/TXQ 只有一个 owner；所有
workers 共享 immutable rule snapshot 和一个 C-owned mempool，不跨 core 传 packet。

## 2. 从静态全局资源到整代 snapshot

Goal 004-005 的旧假设是 Run 期间规则不变，因此 `flow_table`、`route_table` 和
`actions` 可以作为三个独立字段保存在全局 `dp_state`。运行期 publish 引入后，这种
布局会允许 worker 观察到 generation N 的 table 与 generation N+1 的 action store，
产生悬空指针或错误 action。

Goal 006-007 将依赖资源收敛为：

~~~c
struct dp_rule_snapshot {
    struct rte_hash *flow_table;
    struct rte_lpm *route_table;
    struct dp_rule_action *actions;
    uint32_t action_count;
    uint64_t generation;
};
~~~

`dp_state` 只发布一个 `_Atomic(struct dp_rule_snapshot *) active_rules`。flow hash 的
data pointer 指向同一 snapshot 的 action store；LPM next-hop index 也只在该 snapshot
的 action store 内解释。hash、LPM、action 与 snapshot object 必须整代构建、整代
发布、整代回收。

每代 DPDK object 使用短且唯一的 `dfr_f_<generation>`、`dfr_r_<generation>` 名称，
避免新旧两代在 grace period 内共存时发生 name collision。builder 的所有 allocation
都使用 EAL init 阶段保存的 `dp.info.socket_id`；publish 可以从普通 Go goroutine
进入，不能使用该调用线程的 `rte_socket_id()` 决定 NUMA socket。

## 3. 初始配置与动态 publish

初始 JSON 仍在 EAL/port 启动前完成严格解析和校验。`dp_configure_rules()` 创建 QSBR、
完整构建 generation 1，然后以 release store 发布。任何 partial build failure 都立即
释放新建资源，不修改 active pointer 或 generation。

动态 writer 的顺序固定为：

~~~text
Go clone + mutation + validate
-> Go rulesMu 串行化
-> cgo 临时 rule arrays
-> C writer_lock 防御性串行化
-> build generation N+1 完整 snapshot
-> atomic_exchange(active_rules, new, memory_order_acq_rel)
-> rte_rcu_qsbr_start()
-> rte_rcu_qsbr_check(..., wait=true)
-> free old hash/LPM/actions/snapshot
-> 更新 Go currentRules
~~~

`rte_rcu_qsbr_start()` 刻意位于 pointer exchange 之后。这样 publish 之前发生的
quiescent 不能错误地证明旧代已经无人引用。同步 publish 可以阻塞 control writer，
packet worker 不等待 writer。

新 snapshot build 失败时，C active generation 与 Go `currentRules` 均保持不变。
atomic exchange 一旦成功，新代已经对 reader 可见；正常 grace-period 路径没有回滚
分支。`rules_reclaimed` 只在旧 snapshot 的全部资源真实 free 后增加。

## 4. QSBR read side

每个 worker 使用 `reader_id = worker_id = queue_id`，生命周期为：

~~~text
Run
-> rte_rcu_qsbr_thread_register(i)
-> rte_rcu_qsbr_thread_online(i)
-> loop
   -> rte_eth_rx_burst()
   -> atomic_load_explicit(active_rules, memory_order_acquire) 一次
   -> 整个 burst 使用同一个 snapshot
   -> parse / lookup / action / TX ownership 完成
   -> rte_rcu_qsbr_quiescent(i)
-> rte_rcu_qsbr_thread_offline(i)
-> rte_rcu_qsbr_thread_unregister(i)
~~~

`rte_eth_rx_burst()` 返回 0 时仍然立即报告 quiescent。否则 idle dataplane 的 writer
可能永远等不到 grace period。非空 burst 的 quiescent 位于 lookup/action 与 TX burst
之后；到达该点时，本 burst 不再解引用 snapshot，且全部 mbuf 已经 free 或转移给 PMD。

## 5. Go 动态规则管理

`Runtime` 保存深拷贝的 Go-owned `RuleSnapshot` 和当前 generation，并提供：

~~~go
ReplaceRules(snapshot RuleSnapshot) error
AddFlow(rule FlowRule) error
DeleteFlow(key FlowKey) error
AddRoute(rule RouteRule) error
DeleteRoute(key RouteKey) error
RulesGeneration() uint64
~~~

每次操作在 `rulesMu` 内 clone 当前 snapshot、执行 mutation、复用既有严格校验、调用
`native.PublishRules`，成功后才替换 Go 当前值。duplicate Add、missing Delete、非法
Replace 都返回错误。传入 slice 会先复制，caller 后续修改不会改变 Runtime 状态。

Go mutex 提供业务级 single writer 和 current snapshot 一致性；C `writer_lock` 防止
native API 被其他 caller 误用。Run 返回后，owner 先在同一把 Go mutex 下关闭动态
发布入口，再执行 Teardown，避免 publisher 与资源释放并发。

## 6. API threading contract

跨边界规则为：

- `dp_dataplane_request_stop()`：允许跨线程，只写 C11 atomic stop flag；
- `dp_publish_rules()`：允许跨线程，C 边界内部保证 single writer；
- packet worker：只做 atomic acquire load 和 QSBR read-side 操作；
- 其他 lifecycle API：仍由锁定 OS thread 的 owner goroutine 串行调用。

cgo 通过命名 C helper 分配、填写和释放 rule arrays，不在 Go 做 C array pointer
arithmetic。C builder 在调用返回前完成复制，因此不长期借用 Go pointer 或临时 C array。

## 7. Lookup、action 与 mbuf ownership

exact flow 仍使用固定 16-byte host-order TCP/UDP 5-tuple key，并在构造时整体清零。
route 仍以 host-order destination IPv4 查询 `rte_lpm`。优先级保持：

~~~text
exact flow hit
-> flow miss: longest-prefix route
-> route miss: default DROP
~~~

DROP、FORWARD、REWRITE、rewrite mask、IPv4/TCP/UDP software checksum 与 Goal 004-005
语义保持不变。每个 burst 的 mbuf ownership 为：

~~~text
RX 返回 [0,n)                     application owns
parser/lookup/action DROP          worker free
FORWARD / successful REWRITE       原地压缩到 [0,tx_count)
TX 接受 [0,sent)                   PMD owns
TX 未接受 [sent,tx_count)          application 立即 free，zero retry
~~~

packet stats 继续满足五条守恒：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = flow_hit + route_hit + lookup_miss
parse_ok = action_drop + action_forward + action_rewrite
action_forward + action_rewrite = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + action_drop + tx_unsent
~~~

## 8. Teardown 与 cleanup guard

worker offline/unregister 并返回后，Teardown 在 writer lock 下 detach current snapshot，
释放当前 hash/LPM/action/snapshot，再释放 QSBR，随后关闭 port 并在 pool in-use 为 0 时
释放 mempool。最终 stats 记录 `snapshot_freed` 与 `qsbr_freed`。

`dp_runtime_cleanup()` 对以下任一状态返回 `EBUSY`：

- worker 仍运行；
- port 或 mempool 仍存活；
- `active_rules != NULL`；
- QSBR 仍存活；
- writer/update 正在进行；
- rules lifecycle 尚未 teardown。

Go owner 仍只有在 Teardown 成功后才进入 EAL cleanup；Teardown 失败时返回错误，让进程
退出兜底。

## 9. Runtime E2E 与确定性测试

TAP verifier 使用同一个 PID、EAL、两个 port 和 mempool 验证同一个 UDP flow：

1. generation 1 exact flow DROP，且同时匹配 `/24` FORWARD route；
2. SIGHUP 发布 generation 2，改为 REWRITE destination IPv4/port，并验证两个 checksum；
3. 写入非法 JSON 后 SIGHUP，旧 generation 2 行为保持；
4. SIGHUP 发布 generation 3，删除 exact flow，同一原始 frame 经 route 原样 FORWARD。

日志必须只有一次 EAL init 和一次 port setup；最终要求 generation=3、publish_success=3、
reclaimed=2，并继续验证 malformed、lookup miss、packet stats 守恒和完整 cleanup。

native 确定性测试让两个 readers online 后启动异步 publisher。测试先观察到 generation 2
active pointer，同时确认 publisher 尚未返回且旧代未回收；reader0 单独 quiescent 仍不
允许回收，reader1 quiescent 后 writer 才完成并令 reclaimed 增加。测试再让两个只报告
RX=0 quiescent 的 idle readers 推进
同步 publish，随后验证非法 build 保留旧 generation，并在 reader offline 状态连续
publish 50 次；最终 generation 53、旧代累计回收 52 次，current snapshot 与 QSBR
在 teardown 释放。

## 10. Multi-Queue worker 与 stats

Go CLI 的 `--workers` 支持 1～4，并要求 EAL enabled lcore 数完全一致。main lcore 执行
worker0，其余 workers 由 `rte_eal_remote_launch()` 启动。Setup 保存并打印每个
`worker_id/lcore_id/rx_queue_id/tx_queue_id`，同时验证两个 TAP port 的 queue capability
和所有 selected lcores 位于初始化 mempool 的同一 NUMA socket。当前单 NUMA software
baseline 遇到跨 NUMA mapping 直接拒绝，不实现 per-NUMA mempool。

每个 worker 保持 RTC：

~~~text
RX burst on RXQi
-> acquire-load active snapshot once
-> parse / exact flow / LPM / action / rewrite
-> bounded TX on TXQi
-> reader i quiescent
~~~

worker context 按 `RTE_CACHE_LINE_SIZE` 对齐，结构大小也是完整 cache-line 倍数。hot path
只写当前 worker 的 `dp_packet_stats`；规则 publish/reclaim counters 仍由 writer control
path 写。所有 workers stop/join 后，`GetStats` 才逐项聚合 worker-local counters，因此
没有共享 packet counter 的 atomic increment，也没有 concurrent stats snapshot 的数据竞争。

remote launch 中途失败时设置全局 stop 并 join 已启动 workers；任一 worker 返回错误也
设置 stop，main worker 和其余 remote workers 退出后统一 join。Teardown 与 EAL cleanup
同时检查 active worker、launched worker、registered reader，不能在任何 lcore 仍引用
DPDK resource 时继续。

## 11. Multi-reader QSBR 与软件 affinity

native deterministic test 同时 online reader0 和 reader1。publish 交换 pointer 后，
reader0 单独 quiescent 时 writer 仍被 reader1 阻塞且旧代未回收；reader1 quiescent 后
grace period 才完成。两个 idle reader 也持续报告 quiescent，之后继续覆盖 build rollback、
50 次 publish 与 teardown。

TAP functional verifier 的真实结果：同一 5-tuple 的 32 packets 全部命中一个 worker且
payload sequence 保序；32 flows 在 2/4 workers 均产生 multi-worker distribution。该证据
只说明 Linux TAP/kernel software flow-based multi-queue distribution，不等价于 hardware
RSS、RETA 或真实 NIC line-rate。显式 TAP `rte_flow RSS` 因当前 lab 缺少 clang 而跳过。

## 12. 软件 benchmark 与封板边界

当前证据覆盖 Go/cgo/C ownership、运行期 whole-snapshot replacement、C11 atomic、真实
multi-reader DPDK QSBR、TAP multi-queue、1/2/4-lcore RTC、per-worker stats、parser、
lookup/action/checksum、software flow affinity 与可重复 software benchmark。

`scripts/run_benchmark.sh` 默认执行 18 case；正式最低矩阵执行 1/2/4 workers × 64/1500B
× 1/1024 flows 共 12 case。方法见 `docs/benchmark.md`，真实结果保存在 `results/`。
这些结果不能证明真实 NIC DMA、hardware RSS/RETA、cross-NUMA cost、line-rate 或 PCIe
throughput。DPDK Flow Router v0.1 在 Goal 008-009 后阶段性封板；REST/gRPC/Web、NAT、
conntrack、ARP、IPv6、VPP 和硬件扩展都不在本 Goal。
