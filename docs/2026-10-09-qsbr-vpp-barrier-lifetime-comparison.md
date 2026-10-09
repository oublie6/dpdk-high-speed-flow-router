# 2026-10-09 DPDK QSBR 与 VPP adjacency 生命周期对照（跨仓库学习索引）

> 本文件只记录知识关联，**不修改 DPDK 代码、Goal 范围或 v0.1 封板状态**。VPP 的版本限定为 **24.10-release**；其结论以匹配 tag 的官方源码和 VPP 项目实测为依据。VPP 项目最新接续笔记：[Goal001 runtime/DPO 并发](https://github.com/oublie6/vpp-cloud-native-service-gateway/blob/main/docs/2026-10-09-goal001-vpp-runtime-dpo-concurrency-handoff.md)。

## 本仓库已经实现的 QSBR 模型

- Go 控制面变更规则，native C 生成新的 immutable snapshot / generation；
- atomic publish 后，lcore fast path lock-free 读取已发布 generation；
- 多 reader 在 quiescent point 报告进度，writer 可异步判定 grace period、批量 reclaim 旧 generation；
- 原有 1/2/4-worker software TAP 多队列和 QSBR 动态规则 regression 已验收；未声称 hardware RSS/真实 NIC 性能。

## VPP24.10 的对照（不是在 DPDK 项目里引入 VPP）

| 目的 | 本仓库 DPDK QSBR | VPP adjacency |
| --- | --- | --- |
| 更新入口 | generation 的 atomic pointer publish | LB bucket 内的 `dpo_id_t`（≤64-bit）整体 publish |
| 活跃 reader | lcore 不停包，按 QSBR reader ID 过安全点 | worker 正常完成当前 main-loop graph work，再响应 barrier |
| 长期 ownership | snapshot/gen 生命周期和可达性由规则发布/回收算法管理 | DPO object type 的 lock/unlock/refcount 管理长期控制面引用 |
| 旧对象安全释放 | 等所有 reader 过 QSBR grace period | 对 adjacency：最后一个长期锁归零后 barrier，再 pool_put |
| 修改多字段共享结构 | 构造全新 snapshot 避免原地修改 | adjacency MAC/rewrite 原地修改会使用 worker barrier；类型改变时还有 back-walk/drop/restack |

**两个不同的问题：**
1. 新 reader 是否仍能经由旧 forwarding graph/规则 snapshot 获得对象？必须先消除长期引用/旧可达路径。
2. 已经获得旧值的 in-flight reader 是否都完成？必须经过 quiescent/grace period 后再 reclaim。

refcount 与 barrier 不能互相替代；也不能把 `DPO ID` 当成“每个 handle 自带引用计数”。VPP24.10 `src/vnet/dpo/dpo.c::dpo_copy()` 在 64-bit 赋值之后调用新 DPO lock、旧 DPO unlock；Adj 最后引用离开会进 `src/vnet/adj/adj.c::adj_last_lock_gone()` 的 worker barrier，再 pool_put。属于 **VPP adjacency 具体实现**，不能套到全部 DPO 类型。

### 注意差异

- QSBR 的 grace period 允许 reader 继续执行，更新者常可异步回收；VPP worker barrier 是 main 发请求，让 worker 在 main-loop boundary 同步 park。
- VPP VLIB graph edge 不等于跨核 pipeline；worker 默认 run-to-completion，explicit handoff 才跨线程。
- VPP 普通 IPv4 lookup 的 LB bucket selection 可能融入 `ip4-lookup` node，不能把 LB DPO 与 `ip4-load-balance` node 强行 1:1 对应。

## 一手资料 / 工程材料

- [本仓库动态规则/QSBR Goal](goals/006-007-dynamic-rules-qsbr.md)
- [本仓库 multi-queue/benchmark Goal](goals/008-009-multiqueue-rss-benchmark.md)
- [VPP v24.10 dpo.c](https://github.com/FDio/vpp/blob/v24.10/src/vnet/dpo/dpo.c)
- [VPP v24.10 adj.c](https://github.com/FDio/vpp/blob/v24.10/src/vnet/adj/adj.c)
- [VPP v24.10 adj_nbr.c](https://github.com/FDio/vpp/blob/v24.10/src/vnet/adj/adj_nbr.c)
- [VPP v24.10 main.c](https://github.com/FDio/vpp/blob/v24.10/src/vlib/main.c)
- [VPP v24.10 threads.h](https://github.com/FDio/vpp/blob/v24.10/src/vlib/threads.h)

## 下一步

DPDK 仓库维持 v0.1 已验收、已封板；新的学习和功能开发在 `oublie6/vpp-cloud-native-service-gateway` 继续（Goal001 讲解收尾 → Goal002）。
