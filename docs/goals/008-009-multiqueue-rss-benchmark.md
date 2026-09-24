# Goal 008-009：Multi-Queue / RSS / Multi-Lcore + Benchmark

日期：2026-09-25  
状态：✅ Codex 已完成，待 ChatGPT 验收

## 1. 背景与目标

Goal 001～007 已全部验收通过。当前数据面已经具备：

~~~text
Go control plane
-> dynamic immutable rule snapshot
-> atomic publish
-> QSBR reclaim

single RXQ / single lcore
-> RX burst
-> parse
-> exact flow / LPM
-> DROP / FORWARD / REWRITE
-> TX burst
~~~

本 Goal 是 DPDK 项目的最后一个实现阶段，合并原 Goal 008（multi-queue / RSS / multi-lcore）与 Goal 009（benchmark / profiling）。

目标架构：

~~~text
                    shared immutable rules
                           |
                           v
RX port
├─ RXQ0 -> lcore0 -> RTC -> TXQ0
├─ RXQ1 -> lcore1 -> RTC -> TXQ1
├─ RXQ2 -> lcore2 -> RTC -> TXQ2
└─ RXQ3 -> lcore3 -> RTC -> TXQ3

每个 worker：
RX burst
-> acquire active snapshot once
-> parse
-> flow/LPM lookup
-> action/rewrite
-> TX burst
-> QSBR quiescent
~~~

本 Goal 完成后，本 DPDK Flow Router v0.1 阶段性封板，后续转入 VPP / GoVPP。

## 2. 核心原则

### 2.1 Queue single ownership

必须保持：

~~~text
one RXQ -> exactly one worker
one TXQ -> exactly one worker
~~~

禁止两个 worker 同时 poll 同一个 RXQ。
禁止多个 worker 共享同一个 TXQ 并依赖隐式锁。

默认映射：

~~~text
queue_id == worker_id == QSBR reader_id
~~~

lcore id 可以不同，但映射必须显式保存并打印。

### 2.2 RTC 保持不变

每个 worker 仍采用 Run-To-Completion：

~~~text
RXQ
-> parse
-> lookup
-> action
-> rewrite
-> TXQ
~~~

不引入 rte_ring pipeline，不做 packet cross-core handoff。

### 2.3 Packet hot path 不共享 mutable stats

Goal 006-007 仍有单 worker global packet stats。本 Goal 必须改为 per-worker stats。

建议：

~~~c
struct dp_worker_ctx {
    unsigned int worker_id;
    unsigned int lcore_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
    struct dp_packet_stats stats;
} __rte_cache_aligned;
~~~

每个 worker 只写自己的 stats cache line。
不得让多个 worker 在 hot path 上原子递增同一 packet counter。

规则 publish / reclaim counters 仍属于 control/writer side。
最终 GetStats 在 workers 全部停止后聚合 per-worker packet stats + control stats。

## 3. Worker 配置

Go CLI 新增：

~~~text
--workers N
~~~

默认 N=1。本 Goal 支持 1 <= N <= 4。
允许实现更高上限，但测试和 benchmark 至少覆盖 1 / 2 / 4。

为了保持 mapping 简单、可解释，当前 v0.1 要求：

~~~text
EAL enabled lcore count == worker count
~~~

例如 4 worker：

~~~text
--workers 4 -- --lcores="0@CPU0,1@CPU1,2@CPU2,3@CPU3" ...
~~~

main lcore 同时作为 worker0；其余 worker 使用 DPDK remote lcore。
禁止为本 Goal 再引入独立 control lcore。

如果 EAL lcore 数与 worker 数不匹配，必须在 Run 前失败并给出明确错误。

## 4. Worker launch

推荐生命周期：

~~~text
Setup queues
-> build worker contexts
-> launch worker1..N-1 with rte_eal_remote_launch
-> main lcore runs worker0
-> stop requested
-> worker0 exits
-> wait/join all remote lcores
-> all readers offline/unregistered
-> aggregate stats
-> teardown rules/ports/pool/EAL
~~~

要求：

- remote launch 失败时，停止并 join 已成功启动的 workers；
- 任一 worker 返回错误时，触发全局 stop，并最终向 Go 返回错误；
- worker 结束前不得遗留 mbuf ownership；
- worker 停止、QSBR unregister 全部完成后，Teardown 才能释放 active rules / QSBR；
- 不允许 remote lcore 在 EAL cleanup 后仍运行。

## 5. Queue / Port setup

RX/TX port 都按 worker count 配置 queue。

对于 TAP PMD：

~~~text
rx_queues == tx_queues == worker_count
~~~

每个 queue 都必须显式 setup。
worker i：

~~~text
RX port / RXQi
-> worker i
-> TX port / TXQi
~~~

端口能力检查至少包括：

~~~text
max_rx_queues >= workers
max_tx_queues >= workers
~~~

不满足时在 Setup 阶段明确失败。

继续使用一个 C-owned shared mempool；当前 software lab 为单 NUMA，可以依赖 DPDK mempool per-lcore cache。

## 6. NUMA 边界

当前软件环境只有一个 NUMA node，本 Goal 不扩展成完整 per-NUMA mempool framework。

必须显式检查 selected worker lcores 的 socket：

~~~text
rte_lcore_to_socket_id(lcore)
~~~

software/TAP baseline 要求所有 worker 与初始化时保存的 dp.info.socket_id 位于同一 socket。

如果发现跨 NUMA worker，直接拒绝并说明：

~~~text
Goal008-009 software baseline requires all workers on one NUMA socket
~~~

文档必须明确当前结果不能证明 cross-NUMA 性能；真实 NIC 环境应进一步考虑 NIC / RXQ / lcore / mempool / hot state NUMA locality。本 Goal 不实现 per-NUMA mempool。

## 7. RSS / Flow Affinity：软件环境的准确边界

当前项目仍只使用 TAP Virtual PMD，不 bind 真实 NIC。

TAP PMD 支持 multi-queue；默认 packet distribution 由 Linux kernel 做 flow-based distribution。显式 rte_flow RSS 依赖 TAP RSS/eBPF 构建能力。

### 7.1 Required：TAP multi-queue flow distribution

必须真实验证：

~~~text
same 5-tuple
-> stable RX queue / worker affinity

many different 5-tuples
-> traffic can spread across multiple RX queues/workers
~~~

只能表述为：

~~~text
software TAP/kernel flow-based multi-queue distribution
~~~

不得写成 hardware RSS verified、hardware RETA verified 或 NIC line-rate verified。

### 7.2 Optional：explicit rte_flow RSS

如果当前 DPDK/TAP build 能通过 rte_flow_validate 创建 RSS action，可额外实现并验证 IPv4 TCP/UDP 5-tuple -> RSS -> queues 0..N-1。

如果缺少 clang/bpftool/libbpf 或 PMD 返回 ENOTSUP：

- 不允许为了本 Goal 强行扩大系统依赖；
- 记录 capability / skip reason；
- required TAP flow-affinity test 仍必须通过。

### 7.3 RETA

当前 TAP software experiment 不要求真实 NIC RETA programming。
文档可以记录 future real NIC 的 RSS hash -> RETA bucket -> RX queue，但不得伪造运行证据。

## 8. Flow affinity functional E2E

必须新增 deterministic multi-queue E2E，不能只看程序能启动多个线程。

### Case A：single-flow affinity

启动 2 workers / 2 RXQ / 2 TXQ。
规则仅允许一个 exact UDP flow。

连续发送同一个 5-tuple 的多个带 sequence payload 的 packet。

要求：

- 全部 deterministic packet 被同一个 worker 的 flow_hit 计数；
- 不允许同一个测试 flow 同时出现在两个 worker；
- TX 侧 payload sequence 不乱序；
- packet conservation 成立。

### Case B：multi-flow spread

构造至少 32 个不同 UDP 5-tuple。

要求：

- 2-worker run 中至少两个 worker 都收到 deterministic flow；
- 4-worker run 中记录每个 worker 的命中分布；
- 不要求绝对均匀；
- 不允许宣称 TAP distribution 等价于硬件 RETA。

为了准确识别 deterministic flow，可以使用测试专用规则 / marker / per-worker exact-flow hit 统计。
不要在 production hot path 增加 payload parsing。

### Case C：same queue ownership

测试必须能证明 RXQi 只被 worker i poll，TXQi 只被 worker i 使用。
可以通过 worker context + test counters / assertions 实现。

## 9. Per-worker stats

把 packet stats 与 rule/control stats 明确拆开。

每个 worker 至少记录：

~~~text
worker_id
lcore_id
rx_queue_id
tx_queue_id
rx
parse_ok
parse_unsupported
parse_malformed
flow_hit
route_hit
lookup_miss
action_drop
action_forward
action_rewrite
tx_accepted
tx_unsent
drop
rx_bursts
rx_empty_polls
tx_bursts
~~~

最终 aggregate stats 必须继续满足：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = flow_hit + route_hit + lookup_miss
parse_ok = action_drop + action_forward + action_rewrite
action_forward + action_rewrite = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + action_drop + tx_unsent
~~~

而且 aggregate(field) == sum(worker[i].field)，至少对所有 packet counters 成立。

CLI 退出时打印简洁 worker summary。

## 10. QSBR 扩展为 multi-reader

Goal 006-007 当前 reader ID 固定为 0。

本 Goal 必须改成 reader_id = worker_id。

每个 worker：

~~~text
register(reader_id)
-> online(reader_id)
-> burst
-> quiescent(reader_id)
-> ...
-> offline(reader_id)
-> unregister(reader_id)
~~~

所有 worker 的 idle RX=0 分支也必须持续 quiescent。

### Multi-reader reclamation test

新增 deterministic test：

~~~text
reader0 online
reader1 online

publish Gen2
-> pointer exchange completed

reader0 quiescent
-> writer 仍不能 reclaim Gen1

reader1 还没 quiescent
-> old snapshot still live

reader1 quiescent
-> grace period satisfied
-> Gen1 reclaimed
~~~

至少用 2 readers 证明 writer 等的是所有 online readers，而不是任意一个 reader。

继续保留 idle reader、build failure rollback、repeated publish、teardown cleanup。

## 11. TX ownership

每个 worker 独占 TXQ，因此现有：

~~~text
one TX burst
-> accepted prefix ownership transfers to PMD
-> unsent tail remains app-owned
-> zero-retry free unsent
~~~

语义保持不变。

必须回归 partial TX helper、每个 worker local tx_accepted/tx_unsent、aggregate ownership conservation、worker exit 无 pending mbuf。

## 12. Dynamic rules + multi-worker E2E

Goal 006-007 的 runtime reload 必须在 multi-worker 条件下再跑一次。

至少：

~~~text
2 workers online
Gen1: exact flow DROP
-> publish Gen2: same flow REWRITE
-> publish Gen3: delete flow, route FORWARD
~~~

要求：

- 同一 PID；
- EAL/ports/mempool/queues 不重建；
- 两个 QSBR readers 都在线；
- old generation 只有在两个 reader 都跨过 quiescent 后才 reclaim；
- packet behavior仍然是 DROP -> REWRITE -> route FORWARD；
- checksum regression PASS。

## 13. Benchmark：目标与边界

benchmark 是本 Goal 的正式交付。

当前没有真实 DPDK NIC，因此结果必须称为：

~~~text
software TAP / kernel / raw-socket end-to-end benchmark
~~~

它可以验证 worker/queue architecture、software throughput、queue scaling trend、offered load/drop、CPU cost 和 flow distribution。

它不能证明 physical NIC DMA、hardware RSS、RETA、NUMA NIC locality、vector PMD、line-rate 或 PCIe throughput。

## 14. Benchmark harness

新增实际需要的 benchmarks/、scripts/run_benchmark.*、docs/benchmark.md、results/。

benchmark harness 要尽量自动化：

1. 自动读取可用 CPU；
2. 选择 1 / 2 / 4 workers；
3. 构造对应 EAL lcore mapping；
4. 启动 flow-router；
5. 创建/使用本次临时 TAP；
6. 生成 deterministic UDP flows；
7. 固定时间持续发送；
8. 捕获 TX / 读取 router final stats；
9. 记录进程 CPU 时间；
10. 输出 CSV + Markdown summary；
11. 清理进程、TAP、临时规则和临时文件。

失败路径也必须清理自身资源。
不得修改管理网卡、默认路由、firewall、VFIO 或系统全局网络配置。

## 15. Benchmark matrix

默认完整矩阵至少：

~~~text
workers: 1 / 2 / 4
packet L2 length: 64 / 256 / 1500
flow count: 1 / 1024
action: FORWARD
~~~

共 18 cases。

每 case 默认 warmup >= 1s，measure >= 5s。

可以提供 --quick，但最终 Goal 验收必须至少实际跑：

~~~text
workers 1 / 2 / 4
packet size 64 / 1500
flow count 1 / 1024
~~~

即至少 12 个正式 case。

## 16. Benchmark metrics

每个 case 至少记录：

~~~text
git_commit
dpdk_version
kernel
cpu_model
allowed_cpu_list
numa_topology
pmd
workers
lcore_map
rx_queues
tx_queues
burst_size
packet_size
flow_count
duration
offered_packets
offered_pps
rx_packets
tx_packets
drop_packets
rx_mpps
tx_mpps
gbps
process_cpu_seconds
cpu_utilization
worker_distribution
~~~

如果环境允许，可额外记录 cycles/packet、instructions/packet、cache-misses。

perf 权限不足不能让 Goal 失败；必须记录 perf unavailable / permission denied，而不是伪造数据。

## 17. Benchmark 解释规则

### Router-limited

只有 offered_pps > tx_pps 且出现 router RX/drop saturation 时，才讨论 router bottleneck。

### Generator-limited

如果 offered_pps ≈ tx_pps、generator CPU 已满、router 无 drop，必须明确 generator/load path may be the bottleneck。

### TAP/kernel-limited

结果文档必须说明 measured result includes Linux kernel + TAP + generator/capture overhead，只能用于软件实验相对比较。

## 18. Profiling / evidence

至少输出：

~~~text
1 worker:
...

2 workers:
...

4 workers:
...

observed bottleneck:
...
~~~

不能预设 worker 越多越快。

如果 2/4 workers 下降，也必须如实记录，并分析 TAP/kernel serialization、raw socket generator、scheduler、shared mempool/cache、shared lookup table cache pressure、software PMD overhead 等可能因素。

不要为了得到漂亮数字修改测试口径。

## 19. Functional tests

新增/更新测试至少覆盖：

### Worker mapping
- workers=1；
- workers=2；
- workers=4；
- lcore count mismatch；
- workers > port max queues；
- duplicate RXQ owner 不允许；
- duplicate TXQ owner 不允许；
- selected lcore cross-NUMA -> software baseline reject。

### Per-worker stats
- worker-local counters；
- aggregate == sum(worker)；
- cache-line alignment / non-overlap 静态或运行期检查；
- packet conservation。

### Multi-reader QSBR
- 2 readers；
- reader0 quiescent 不足以 reclaim；
- reader1 quiescent 后才 reclaim；
- idle readers；
- stop/offline/unregister。

### Failure path
- remote launch failure；
- one worker error triggers global stop；
- setup partial queue failure；
- teardown joins all workers；
- no mbuf leak；
- cleanup guard 不允许 live worker / port / pool / active rules / QSBR。

## 20. Existing regression

必须继续通过：

~~~text
Goal001 EAL lifecycle

Goal002
TAP RTC
TX partial return ownership
port/pool cleanup

Goal002R
cleanup guard
cgo helper
teardown failure

Goal003
20 parser fixtures
malformed / unsupported

Goal004-005
exact flow
LPM longest-prefix
flow > route
DROP / FORWARD / REWRITE
TCP/UDP checksum
stats conservation

Goal006-007
Go CRUD
atomic snapshot publish
QSBR
SIGHUP reload
build rollback
50+ publish stress
same-PID DROP -> REWRITE -> FORWARD
~~~

单-worker行为不得因为 multi-worker 重构而回归。

## 21. Non-goals

本 Goal 不做：

- real NIC/VFIO；
- hardware RETA programming；
- cross-NUMA optimization；
- per-NUMA mempool；
- RX interrupt mode；
- adaptive polling；
- cross-core packet pipeline；
- rte_ring packet handoff；
- NAT / conntrack / ARP / neighbor / IPv6 / ACL；
- TCP termination；
- REST/gRPC/Web；
- DB/persistence；
- VPP integration；
- SmartNIC/offload；
- RDMA。

不要因为这是最后一个 Goal 就继续扩 scope。

## 22. 文档交付

必须更新：

~~~text
README.md
AGENTS.md
docs/architecture.md
docs/benchmark.md
本 Goal 文档
~~~

benchmark 真实结果写到：

~~~text
results/goal008009-software-benchmark.md
results/goal008009-software-benchmark.csv
~~~

不要提交巨大日志、pcap 或生成物。
结果必须记录真实 git commit 与运行环境。

## 23. 验收命令

开始前：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

同步后完整阅读 AGENTS.md、README.md、docs/architecture.md、Goal 006-007 和本 Goal。

基础：

~~~bash
git diff --check
make build
make test
make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
./scripts/check_env.sh
./scripts/verify_tap_forwarding.sh
~~~

如果新增 benchmark shell/python，必须 syntax check 并实际运行正式 benchmark matrix 的最低 12 case。

真实 EAL tests 必须使用允许的 CPU list，不硬编码 host CPU。

## 24. 验收结果必须证明

~~~text
1 worker PASS
2 workers PASS
4 workers PASS

RXQ0 -> worker0 single owner
RXQ1 -> worker1 single owner
...
TXQ0 -> worker0 single owner
TXQ1 -> worker1 single owner
...

same 5-tuple flow affinity PASS
multi-flow spread across workers PASS

per-worker stats PASS
aggregate == worker sum PASS
packet conservation PASS

2-reader QSBR:
reader0 quiescent alone does not reclaim
reader1 quiescent completes grace period
PASS

multi-worker dynamic rules:
DROP -> REWRITE -> route FORWARD
PASS

partial TX ownership PASS

all workers stopped/joined
all QSBR readers offline/unregistered
current snapshot freed
QSBR freed
ports closed
pool in-use=0
pool freed
EAL cleanup succeeded

software benchmark:
1/2/4 workers
64/1500B
1/1024 flows
>=12 cases
results committed

no process/TAP/temp leak
~~~

如果 explicit rte_flow RSS 不可用，还必须记录 explicit TAP rte_flow RSS: unsupported/skipped (<reason>)。不能把 skip 写成 PASS，但不阻塞 Goal 验收。

## 25. 最终封板标准

本 Goal 验收通过后 README/AGENTS 应明确：

~~~text
DPDK Flow Router v0.1 phase complete

Implemented:
- Go + thin cgo + C/DPDK
- EAL / PMD / mbuf / mempool
- parser
- rte_hash / rte_lpm
- DROP / FORWARD / REWRITE
- checksum
- dynamic immutable rules
- atomic publish
- QSBR
- multi-queue
- multi-lcore RTC
- fixed RXQ/TXQ ownership
- per-worker stats
- software RSS-affinity evidence
- reproducible software benchmark

Not proven:
- physical NIC DMA
- hardware RSS/RETA
- cross-NUMA performance
- line-rate
~~~

下一步：VPP / GoVPP。
Codex 不得自行继续扩展 DPDK 项目。

## 26. 交付

完成后：

1. 更新 README / AGENTS / architecture / benchmark；
2. 本 Goal 追加真实实现与测试记录；
3. 写入 benchmark 真实结果；
4. 状态改为：✅ Codex 已完成，待 ChatGPT 验收；
5. 创建 focused commit；
6. 停止开发。

建议 commit：

~~~text
dataplane: add multi-queue workers and benchmark
~~~

最终报告必须包含：

~~~text
commit SHA
worker/lcore/queue mapping
worker launch/stop lifecycle
per-worker stats design
false-sharing avoidance
multi-reader QSBR design
TAP flow-affinity evidence
explicit RSS capability/skip result
dynamic rule multi-worker regression
benchmark matrix
benchmark environment
benchmark results
observed bottleneck
cleanup evidence
all previous Goal regressions
remaining hardware limitations
~~~

---

## 27. Codex 实现记录（2026-09-24）

### 27.1 Worker、queue 与 lifecycle

CLI 新增 `--workers`，支持 1/2/4，要求 EAL enabled lcore 数与 worker 数相等。Setup
显式建立并打印：

~~~text
queue_id == worker_id == QSBR reader_id
worker0 -> EAL main lcore
worker1..N-1 -> rte_eal_remote_launch
~~~

两个 TAP ports 都按 worker 数配置对称 RX/TX queues，并检查 `max_rx_queues`、
`max_tx_queues`。每个 selected lcore 通过 `rte_lcore_to_socket_id()` 检查与初始化
socket 相同；本 software baseline 不实现 cross-NUMA/per-NUMA mempool。

worker i 只 poll RXQi、只发送 TXQi，保持完整 RTC，不使用 `rte_ring` 或 packet
cross-core handoff。remote launch 中途失败会 stop 并 join 已启动 workers；任一 worker
错误会触发全局 stop。Run 只有在全部 remote lcores join 后才返回。真实故障注入分别覆盖
remote launch failure 和 remote worker failure，之后 snapshot/QSBR/ports/pool/EAL 均能
安全清理。

### 27.2 Per-worker stats 与 false sharing

packet counters 已从 `dp_state.stats` hot-path global writes 移到每个 `dp_worker_ctx` 内的
`dp_packet_stats`。context 使用 `__rte_cache_aligned`，并用两个 `_Static_assert` 保证
alignment 和结构大小都是 cache-line 边界。每个 worker 只写自己的 counters；rule
generation/publish/reclaim 仍是 control writer counters。

worker 全部 stop/join 后，`GetStats` 才逐项 aggregate，并返回固定 mapping 与 worker
明细。deterministic test 给两个 worker 填入覆盖全部 packet/burst fields 的不同值，验证
逐项 aggregate、cache-line non-overlap 与五条 packet conservation。

### 27.3 Multi-reader QSBR

reader API 改为显式接收 `reader_id`；真实 worker 使用 worker ID。RX=0 与非空 burst 都在
不再引用 snapshot 后报告各自 quiescent。

native 真实 DPDK test 同时 online reader0/reader1。generation 2 pointer exchange 可见后：

~~~text
reader0 quiescent
-> publisher 仍未返回
-> rules_reclaimed 不变

reader1 quiescent
-> grace period 完成
-> generation 1 reclaimed
~~~

两个 idle readers 也能共同推进同步 publish。build rollback、offline/unregister、50 次
连续 publish、generation=53/reclaimed=52 与 final snapshot/QSBR free 回归继续通过。

### 27.4 TAP affinity、spread 与动态规则

`scripts/verify_multiqueue.sh` 真实运行结果：

~~~text
single-flow affinity distribution: [0, 32]
32-flow / 2-worker distribution: [36, 28]
32-flow / 4-worker distribution: [16, 20, 18, 10]
PASS
~~~

single-flow 的 32 个 sequence payload 保持顺序，且 exact `flow_hit` 只出现在一个 worker。
32 flows 在 2/4 worker case 都形成 multi-worker distribution。脚本同时断言每个 mapping
为 `RXQi -> workeri -> TXQi`，aggregate flow hits 与 deterministic packets 相等。

随后在 2 workers online 条件下，同一个 PID/EAL/ports/queues/mempool 完成 generation 1
DROP -> generation 2 REWRITE + IPv4/UDP checksum -> invalid reload 保持旧代 -> generation 3
route FORWARD。最终 generation=3、publish_success=3、reclaimed=2，两个 worker 都有真实
RX/poll 活动，packet conservation 与完整 teardown 通过。

这是 Linux TAP/kernel software flow-based distribution evidence。optional explicit TAP
`rte_flow RSS` 因 lab 没有 clang、未启用 optional eBPF/toolchain path 而 skipped；没有写成
PASS，也不声称 hardware RSS/RETA。

### 27.5 正式 software benchmark

benchmark source commit：

~~~text
f4b28ab26dfb34034922c63ae5240521b22fc5aa
worktree_dirty=false
~~~

环境：DPDK 25.11.3、kernel 5.4.0-216-generic、Intel Xeon E5-2698 v4、8 个 allowed
CPUs、单 NUMA node 0、net_tap software PMD。正式执行：

~~~bash
./scripts/run_benchmark.sh --minimum
~~~

12 cases 全部使用 1s warmup + 5s measurement；统计的连续区间为 6s。矩阵完整覆盖
1/2/4 workers × 64/1500B × 1/1024 flows。TX 吞吐范围：

~~~text
1 worker: 0.060003 - 0.081845 Mpps
2 workers: 0.066255 - 0.091159 Mpps
4 workers: 0.082581 - 0.097918 Mpps
~~~

最高记录为 4 workers、1500B、1 flow：0.097918 Mpps / 1.175022 Gbps。该数字包含
同机 Python raw-socket generator/capture、kernel 和 TAP；多个 case 的 offered > router
visible RX，另一些 case 接近 generator offered ceiling。结果只说明 generator/TAP/kernel
组合路径与相对趋势，没有宣称 router-limited scaling 或 line-rate。原始逐 case counters、
CPU utilization、lcore map 和 worker distribution 见 `results/goal008009-software-benchmark.csv`，
摘要见同名 Markdown。

### 27.6 Cleanup 与回归

功能脚本、benchmark 每个 case 结束均检查：所有 workers stopped/joined、所有 readers
offline/unregistered、current snapshot/QSBR freed、ports_closed=2、pool_in_use=0、pool
freed、EAL cleanup succeeded。正式矩阵后再次检查，没有 flow-router process、测试 TAP、
temp directory 或 Python cache 残留。

实际通过的门禁与回归包括：

~~~bash
git diff --check
make build
make test
make vet
FLOW_ROUTER_TEST_CPU=0 make test

bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
bash -n scripts/verify_multiqueue.sh
bash -n scripts/run_benchmark.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
python3 -m py_compile scripts/verify_multiqueue.py
python3 -m py_compile scripts/run_benchmark.py

./scripts/check_env.sh
./scripts/verify_tap_forwarding.sh
./scripts/verify_multiqueue.sh
./scripts/run_benchmark.sh --minimum

gcc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdpdk) \
  -Idataplane/native -fsyntax-only \
  dataplane/native/dp_lookup.c dataplane/native/dp_worker.c \
  dataplane/native/dp_runtime.c dataplane/native/dp_port.c \
  dataplane/native/dp_lookup_action_test.c dataplane/native/dp_tx.c
~~~

回归覆盖 EAL lifecycle、Teardown failure、cleanup guards（含 live worker/reader）、partial
TX ownership、20 parser fixtures、rte_hash/rte_lpm、flow precedence、DROP/FORWARD/REWRITE、
TCP/UDP checksum、Go CRUD、atomic publication、2-reader QSBR、idle readers、build rollback、
50+ publish stress、SIGHUP reload、worker mapping/stats/failure paths。1024-flow benchmark 还
发现原 hash table 的内部 entries 不足以保证装满公开上限；内部容量增至 2048，公开规则
上限仍为 1024，并由正式 1024-flow cases 真实覆盖。

本 Goal 没有实现或声称证明 real NIC/VFIO、hardware RETA、cross-NUMA optimization、
per-NUMA mempool、NAT、conntrack、ARP、IPv6、Web/API、VPP、RDMA 或 SmartNIC。DPDK Flow
Router v0.1 已阶段性封板，等待 ChatGPT 验收。
