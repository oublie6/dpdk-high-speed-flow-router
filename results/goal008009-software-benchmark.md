# Goal 008-009 Software Benchmark 结果

本结果是 **software TAP / kernel / raw-socket end-to-end benchmark**。
它包含 Linux kernel、TAP PMD、Python raw-socket generator/capture 与调度开销，
不证明 physical NIC DMA、hardware RSS/RETA、NUMA NIC locality 或 line-rate。

每个 case 使用 1.0s warmup + 5.0s measurement；由于运行期统计只在 workers 停止并 join 后聚合，表中 duration 与 counters 覆盖二者的连续总区间。

| workers | bytes | flows | offered pps | RX Mpps | TX Mpps | Gbps | CPU % | worker flow hits |
|---:|---:|---:|---:|---:|---:|---:|---:|:---|
| 1 | 64 | 1 | 79129.300 | 0.077803 | 0.077802 | 0.039835 | 99.833 | 466813 |
| 1 | 64 | 1024 | 83991.929 | 0.081847 | 0.081845 | 0.041905 | 99.833 | 491073 |
| 1 | 1500 | 1 | 70221.233 | 0.067794 | 0.067793 | 0.813518 | 99.990 | 406799 |
| 1 | 1500 | 1024 | 60559.445 | 0.060004 | 0.060003 | 0.720038 | 100.162 | 360037 |
| 2 | 64 | 1 | 81599.348 | 0.079395 | 0.079394 | 0.040650 | 199.994 | 0;476376 |
| 2 | 64 | 1024 | 66434.426 | 0.066256 | 0.066255 | 0.033922 | 199.995 | 193917;203620 |
| 2 | 1500 | 1 | 92698.300 | 0.091160 | 0.091159 | 1.093906 | 199.833 | 0;546954 |
| 2 | 1500 | 1024 | 75519.329 | 0.075420 | 0.075419 | 0.905032 | 200.000 | 220962;231554 |
| 4 | 64 | 1 | 92730.011 | 0.082582 | 0.082581 | 0.042282 | 400.220 | 0;0;495628;0 |
| 4 | 64 | 1024 | 92094.800 | 0.092094 | 0.092092 | 0.047151 | 399.166 | 135443;134377;146772;135964 |
| 4 | 1500 | 1 | 104493.459 | 0.097920 | 0.097918 | 1.175022 | 400.500 | 0;0;587511;0 |
| 4 | 1500 | 1024 | 90885.050 | 0.090882 | 0.090881 | 1.090569 | 399.995 | 133669;132606;144822;134194 |

## 环境

- git commit: `f4b28ab26dfb34034922c63ae5240521b22fc5aa`（worktree_dirty=false）
- DPDK: `25.11.3`
- kernel: `5.4.0-216-generic`
- CPU: `Intel(R) Xeon(R) CPU E5-2698 v4 @ 2.20GHz`
- allowed CPUs: `0,1,2,3,4,5,6,7`
- NUMA: `nodes=0`
- PMD/load path: `net_tap (software TAP/kernel/raw-socket)`

## 观察

- 1 worker: TX Mpps 范围 0.060003～0.081845。
- 2 worker: TX Mpps 范围 0.066255～0.091159。
- 4 worker: TX Mpps 范围 0.082581～0.097918。
- 本次结果由 Python generator 与 capture 同机运行；worker 增加不保证吞吐上升。
- observed_bottleneck 的逐 case 判断保存在 CSV；任何下降都按 TAP/kernel、
  generator/capture、scheduler、shared mempool 与 shared lookup cache 的组合开销解释。
- explicit TAP rte_flow RSS：skipped；本实验环境没有 clang，optional eBPF/toolchain 路径未启用。
- perf：未作为验收依赖；本次没有采集硬件 perf counters。
