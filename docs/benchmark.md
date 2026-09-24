# Software TAP Benchmark 方法

## 1. 目的与边界

Goal 008-009 的 benchmark 使用：

~~~text
Python AF_PACKET raw-socket generator
-> Linux TAP multi-queue
-> DPDK net_tap RX queues
-> fixed RTC workers
-> DPDK net_tap TX queues
-> Linux TAP
-> AF_PACKET capture
~~~

这是 **software TAP / kernel / raw-socket end-to-end benchmark**。结果包含
Python generator/capture、Linux kernel、TAP PMD、scheduler、共享 mempool 和 lookup
table 的成本。它用于验证 queue/worker 架构、软件流量分布、offered load 与相对趋势，
不能证明 physical NIC DMA、hardware RSS/RETA、PCIe throughput、cross-NUMA locality
或 line-rate。

## 2. 运行方式

默认矩阵为 18 case：

~~~text
workers: 1 / 2 / 4
packet L2 length: 64 / 256 / 1500 bytes
flow count: 1 / 1024
action: exact-flow FORWARD
warmup: 1 second
measurement: 5 seconds
~~~

运行完整矩阵：

~~~bash
make build
./scripts/run_benchmark.sh
~~~

Goal 的正式最低 12-case 矩阵去掉 256-byte case：

~~~bash
./scripts/run_benchmark.sh --minimum
~~~

仅用于开发 harness 的快速冒烟：

~~~bash
./scripts/run_benchmark.sh --minimum --quick
~~~

`--quick` 结果不能作为正式验收结果。正式输出固定写入：

~~~text
results/goal008009-software-benchmark.csv
results/goal008009-software-benchmark.md
~~~

## 3. Workload

每个 case 创建独立的两个短名称 TAP、一个 flow-router 进程和一个临时 JSON rules
snapshot。1-flow case 重复发送同一 UDP 5-tuple；1024-flow case轮转 1024 个 exact UDP
5-tuples。每个 flow 的 action 都是 FORWARD。frame 在 Python 中预构建，packet hot path
不解析 benchmark marker。

worker 映射固定为：

~~~text
queue_id == worker_id == QSBR reader_id
worker0 -> EAL main lcore
worker1..N-1 -> DPDK remote lcores
~~~

脚本只从当前进程允许的 CPU 集合中选择 1/2/4 个 CPU，不硬编码宿主 CPU。每个 case
结束后发送 SIGTERM；只有 worker 全部 join、reader offline/unregister、规则/QSBR、port、
mempool 和 EAL 全部清理成功，才写入该 case。

## 4. 指标口径

CSV 记录：git commit/dirty 状态、DPDK、kernel、CPU、allowed CPU、NUMA、PMD、workers、
lcore map、queue 数、burst、packet size、flow count、warmup/measurement/duration、offered
packets/pps、RX/TX/capture/drop、Mpps、Gbps、进程 CPU seconds/utilization 与 per-worker
flow-hit distribution。

当前 runtime 只在 workers 全部停止后聚合无竞争的 per-worker stats，因此一个 case 的
router counters 覆盖连续的 warmup + measurement 总区间。`duration` 与吞吐分母采用同一
总区间，避免把 6 秒 counters 除以 5 秒。Gbps 使用：

~~~text
TX packets * configured L2 frame bytes * 8 / duration
~~~

不额外伪造 preamble、IFG 或 FCS。`process_cpu_seconds / duration * 100` 以单个逻辑 CPU
为 100%，因此 2/4 worker case 可以超过 100%。

## 5. 解释规则

- TX 接近 offered 且没有 TX short return：generator/load path 可能是瓶颈；
- `tx_unsent > 0`：记录 software TAP TX short return；
- offered 明显高于 router RX，但 `tx_unsent=0`：丢失发生在 generator/TAP/kernel 与
  router 可见 counters 之间，不能描述成纯 router saturation；
- worker 增加后吞吐下降也保留原始结果，不改变 generator、duration 或统计口径；
- perf 权限或工具不可用时明确记录 unavailable，不生成虚假 hardware counters。

显式 `rte_flow RSS` 是 optional evidence。当前 lab 没有 clang，未启用 TAP 的 optional
eBPF/toolchain 路径；required evidence 来自 Linux TAP/kernel multi-queue flow-based
distribution，不能写成 hardware RSS 或 RETA verification。
