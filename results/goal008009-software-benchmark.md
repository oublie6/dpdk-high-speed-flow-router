# Goal 008-009 Software Benchmark 结果

本结果是 **software TAP / kernel / raw-socket end-to-end benchmark**。
它包含 Linux kernel、TAP PMD、Python raw-socket generator/capture 与调度开销，
不证明 physical NIC DMA、hardware RSS/RETA、NUMA NIC locality 或 line-rate。

每个 case 使用 1.0s warmup + 5.0s measurement；由于运行期统计只在 workers 停止并 join 后聚合，表中 duration 与 counters 覆盖二者的连续总区间。

| workers | bytes | flows | offered pps | RX Mpps | TX Mpps | Gbps | CPU % | worker flow hits |
|---:|---:|---:|---:|---:|---:|---:|---:|:---|
| 1 | 64 | 1 | 97227.411 | 0.090002 | 0.090001 | 0.046080 | 99.813 | 540115 |
| 1 | 64 | 1024 | 95870.649 | 0.089337 | 0.089336 | 0.045740 | 100.000 | 536016 |
| 1 | 1500 | 1 | 80378.729 | 0.077983 | 0.077981 | 0.935777 | 100.000 | 467889 |
| 1 | 1500 | 1024 | 76919.558 | 0.075492 | 0.075491 | 0.905898 | 99.792 | 453138 |
| 2 | 64 | 1 | 95854.099 | 0.091480 | 0.091479 | 0.046837 | 200.000 | 0;548875 |
| 2 | 64 | 1024 | 81283.104 | 0.081099 | 0.081098 | 0.041522 | 199.833 | 237572;249017 |
| 2 | 1500 | 1 | 80381.652 | 0.076668 | 0.076667 | 0.920002 | 199.667 | 0;460001 |
| 2 | 1500 | 1024 | 89391.152 | 0.088850 | 0.088849 | 1.066184 | 200.000 | 259718;273374 |
| 4 | 64 | 1 | 104093.147 | 0.093512 | 0.093510 | 0.047877 | 399.833 | 0;0;561063;0 |
| 4 | 64 | 1024 | 94049.118 | 0.093652 | 0.093651 | 0.047949 | 400.000 | 138133;136848;148049;138875 |
| 4 | 1500 | 1 | 117527.117 | 0.104209 | 0.104208 | 1.250495 | 399.666 | 0;0;625248;0 |
| 4 | 1500 | 1024 | 90092.158 | 0.089837 | 0.089836 | 1.078030 | 399.664 | 132478;131034;143328;132178 |

## 环境

- git commit: `97359e2103a8ea9d21f71c7bbcb9efa804e76c49`（worktree_dirty=false）
- DPDK: `25.11.3`
- kernel: `5.4.0-216-generic`
- CPU: `Intel(R) Xeon(R) CPU E5-2698 v4 @ 2.20GHz`
- allowed CPUs: `0,1,2,3,4,5,6,7`
- NUMA: `nodes=0`
- PMD/load path: `net_tap (software TAP/kernel/raw-socket)`

## 观察

- 1 worker: TX Mpps 范围 0.075491～0.090001。
- 2 worker: TX Mpps 范围 0.076667～0.091479。
- 4 worker: TX Mpps 范围 0.089836～0.104208。
- 本次结果由 Python generator 与 capture 同机运行；worker 增加不保证吞吐上升。
- observed_bottleneck 的逐 case 判断保存在 CSV；任何下降都按 TAP/kernel、
  generator/capture、scheduler、shared mempool 与 shared lookup cache 的组合开销解释。
- explicit TAP rte_flow RSS：skipped；本实验环境没有 clang，optional eBPF/toolchain 路径未启用。
- perf：未作为验收依赖；本次没有采集硬件 perf counters。
