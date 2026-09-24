# 仓库协作说明

## 1. 仓库用途

本仓库实现一个基于 **DPDK 的高速 L2-L4 Flow Router**。

项目有两个主要目的：

1. 实现一个真实、可测量的用户态数据面，而不是再写一个 DPDK API demo；
2. 作为 **云原生容器网络 / Network Infra / 高性能网络研发** 方向的工程项目。

最终实现应能够证明作者可以理解、设计并实现：

~~~text
NIC / virtual PMD
-> RSS / RX queue
-> fixed lcore
-> burst RX
-> L2/L3/L4 parse
-> route / flow / policy lookup
-> action / rewrite
-> bounded TX
-> TX queue
~~~

配套学习仓库 `oublie6/high-performance-network-learning` 继续作为学习笔记、理论、实验、纠正记录和求职差距的 single source of truth。本仓库只负责 Flow Router 的产品代码、设计文档、benchmark 和工程决策。

---

## 2. 语言规范

**本仓库的文档和代码注释默认全部使用中文。**

具体约定：

- README、AGENTS、`docs/`、Goal 文档、设计说明、验收记录使用中文；
- Go/C/Shell/Makefile 中解释设计、ownership、生命周期、边界条件的注释使用中文；
- API 名、函数名、结构体名、变量名、协议名、DPDK 原生术语、命令、日志字段可以保留英文；
- `RXQ`、`lcore`、`mempool`、`mbuf`、`RSS`、`RTC`、`QSBR` 等行业术语不要求机械翻译；
- 不为了“中文化”修改稳定 API、协议字段或代码标识符；
- 新增英文长段落前应先判断是否真的有必要，默认优先中文。

---

## 3. 核心架构

目标架构：

~~~text
                Go Control Plane
        config / rules / lifecycle / stats
                       |
              coarse-grained cgo API
                       |
                       v
+--------------------------------------------------+
|                C / DPDK Dataplane                |
|                                                  |
| RXQ -> fixed lcore -> parse -> lookup -> action  |
|                         -> rewrite -> TXQ         |
+--------------------------------------------------+
~~~

packet hot path 必须留在 C/DPDK 中。

**禁止逐包跨越 Go/C 边界。**

Go 负责控制面和管理面；C/DPDK 负责逐包数据面。

---

## 4. v0.1 范围

第一版必须刻意保持小而完整。

### 支持协议

- Ethernet
- IPv4
- TCP
- UDP

### 支持的数据面能力

- packet parsing；
- route 和/或 exact flow lookup；
- `DROP`；
- `FORWARD`；
- 基础 L3/L4 `REWRITE`；
- RX/TX burst；
- RSS / multi-queue aware execution；
- fixed RXQ -> lcore ownership；
- Run-To-Completion；
- per-lcore statistics；
- 显式处理 TX partial return；
- bounded retry / drop policy；
- 可重复 benchmark。

### Go 控制面

后续应逐步提供：

- 配置加载；
- rule / route 管理；
- 数据面生命周期管理；
- stats 查询；
- 向数据面发布粗粒度配置。

第一阶段可以先使用静态配置，再演进到动态规则更新。

---

## 5. v0.1 明确不做

不要因为“有意思”就顺手加入：

- IPv6；
- NAT；
- conntrack；
- TCP termination；
- TCP stream reassembly；
- user-space TCP stack；
- HTTP/Kafka/DDS/SFTP 等应用层解析；
- 大型 wildcard ACL engine；
- DPI；
- crypto；
- multi-stage cross-core pipeline；
- generic plugin framework；
- VPP-like graph framework；
- distributed control plane；
- production-grade HA。

只有在 v0.1 已经真实运行、测试、benchmark 后，才讨论这些能力。

---

## 6. 数据面设计原则

### 6.1 single ownership 优先

默认采用：

~~~text
RSS
-> RXQ
-> fixed lcore
-> RTC processing
-> TXQ
~~~

除非测量证明必须改变，否则 fast path 中一个 queue 只有一个 owner。

尽量避免 packet path 上的 shared mutable state。

### 6.2 RTC 优先于 pipeline

默认不要在各处理阶段之间插入 `rte_ring`。

只有出现真实测量依据时才拆 pipeline，例如：

- 某个 heavy stage 可以水平扩展；
- 某类工作明显更慢或耗时波动很大；
- 存在 blocking/non-fast-path 工作；
- 需要明确隔离 slow path。

### 6.3 NUMA-aware

真实硬件允许时，尽量让：

~~~text
NIC / queue / lcore / mempool / hot state
~~~

位于同一个 NUMA node。

software/virtual-PMD 实验无法证明真实 NIC NUMA 性能，文档必须明确这一限制。

### 6.4 ownership 必须明确

每次 mbuf ownership 转移都必须能回答：

> 当前 mbuf 属于谁？谁负责释放？

TX short return 不能造成 mbuf 泄漏，也不能无限 retry。

### 6.5 优化 common path

hot path 要尽量短：

- cheap checks first；
- early drop；
- hot struct 尽量紧凑；
- 高频 mutable state 优先 per-lcore；
- 共享配置尽量 read-mostly；
- packet hot path 不做常规日志，debug/sampling 例外。

### 6.6 先测量，再增加复杂度

没有明确 workload 和测量数据，不接受性能结论。

---

## 7. Go / C 边界

优先使用粗粒度接口。

推荐：

~~~text
StartDataplane(config)
StopDataplane()
PublishRules(snapshot)
ReadStats()
~~~

禁止：

~~~text
Go -> C -> process one packet -> Go
~~~

不得让未经设计的 Go pointer 长期逃逸到 C 数据面状态。

所有跨边界共享结构都必须定义清楚 ownership 和 lifetime。

---

## 8. 仓库结构

在有真实内容时逐步形成：

~~~text
.
├── AGENTS.md
├── README.md
├── docs/
│   ├── architecture.md
│   ├── dataplane.md
│   ├── control-plane.md
│   └── benchmark.md
├── cmd/
│   └── flow-router/
├── control/
├── dataplane/
│   ├── core/
│   ├── parser/
│   ├── lookup/
│   └── action/
├── configs/
├── scripts/
├── benchmarks/
├── results/
└── tests/
~~~

不要为了让仓库“看起来很大”创建空目录。

---

## 9. 开发顺序

默认按照：

~~~text
goals / non-goals
-> architecture
-> build system
-> Go/C boundary
-> EAL init
-> port / virtual PMD init
-> mempool
-> RX/TX queue
-> single RXQ single lcore RTC loop
-> Ethernet/IPv4/TCP/UDP parser
-> action: drop/forward
-> rewrite
-> flow/route lookup
-> stats
-> bounded TX short-return handling
-> multi-queue
-> RSS / affinity
-> benchmark
-> optimization from evidence
~~~

除非实现证据要求调整，否则不要跳步把未来功能一次性堆进来。

---

## 10. 编码规则

### C / DPDK

- hot-path 函数保持小而直接；
- packet path 避免隐藏分配；
- common path 避免锁；
- 明确区分 segment data length 与 packet total length；
- 直接解引用 header 前先验证 multi-segment 假设；
- 显式处理 endian；
- ownership 转移在代码中必须可见；
- 尽量把 error/slow path 移出 common instruction stream。

### Go

- 控制面保持 idiomatic 和简单；
- 本项目禁止使用 Go 做逐包处理；
- 配置使用明确类型并进行显式校验；
- C binding 保持薄；
- cgo 必须封装在很小的 package 内，不向整个控制面扩散。

### 通用

- 禁止提交 secrets 和 host-specific credentials；
- 不硬编码管理 IP；
- 不提交巨大日志、pcap、生成物；
- 注释解释“为什么这样设计”和边界条件，不机械复述代码；
- 文档与代码注释遵循第 2 节中文规范。

### 可读性与语法复杂度

本项目优先保证代码可读、可解释、可调试，不追求“写得短”或“语法高级”。

要求：

- 优先使用直白、显式、容易单步调试的写法；
- 不为了减少几行代码而引入复杂语法糖；
- 不使用难以理解的链式调用、深层嵌套闭包或多层匿名函数；
- 初始化路径、控制路径和错误处理路径优先使用清晰的顺序式代码；
- 避免不必要的反射、元编程、泛型技巧和过度抽象；
- 能用一个小 helper 函数表达清楚时，不要把复杂逻辑压进一个表达式；
- 避免无必要的 `unsafe`；必须使用时，应把范围限制在很小的 binding/helper 层，并用中文说明为什么必须使用；
- 对 C pointer arithmetic、手工内存布局、位运算等低层代码，优先封装成命名清晰的 C helper，而不是在 Go 中堆叠复杂 `unsafe.Pointer/uintptr` 表达式；
- hot path 可以为性能做低层优化，但必须有 benchmark 或明确理由，并在注释中说明 trade-off；
- 非 hot path 不允许为了微不足道的性能收益牺牲明显的可读性；
- 新增代码应做到：熟悉 Go/C/DPDK 的工程师可以在较短时间内看懂控制流、ownership 和生命周期。

优先：

~~~text
清晰
> 可解释
> 可测试
> 再考虑微优化
~~~

只有性能证据证明必要时，才接受明显增加认知复杂度的实现。

---

## 11. Benchmark 规则

性能工作是项目本体的一部分，不是最后装饰。

每个 benchmark 至少记录：

- git commit；
- DPDK version；
- CPU / core count；
- NUMA topology（相关时）；
- NIC/PMD type；
- queue count；
- burst size；
- packet size；
- flow count/distribution；
- duration；
- forwarding behavior；
- offered load；
- RX/TX/drop counters。

优先指标：

- Mpps；
- Gbps；
- drop；
- cycles/packet（可行时）；
- CPU utilization；
- latency（环境允许时）；
- burst/queue 行为（必要时）。

software PMD/TAP/PCAP 的结果必须标明为软件实验，不得描述成真实 NIC line-rate 证据。

---

## 12. 测试规则

功能正确性测试与性能测试分开。

功能测试至少覆盖：

- Ethernet / IPv4 parse；
- TCP / UDP parse；
- malformed/truncated packet；
- match 行为；
- drop；
- forwarding decision；
- rewrite 和 checksum；
- 可模拟条件下的 TX partial-return ownership。

优先使用 deterministic packet fixtures 和小而聚焦的测试。

---

## 13. 文档规则

重要设计决策写入 `docs/`。

设计发生变化时记录：

~~~text
旧假设
-> 观察到的证据/问题
-> 新设计
-> trade-off
~~~

如果“纠正过程”本身有学习和工程价值，不要静默改写历史。

---

## 14. Goal / Codex / 验收工作流

### 14.1 每次执行前必须同步仓库

Codex 每次开始新的 Goal、修复任务或继续上一次任务前，必须先同步远端最新代码。

推荐流程：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

执行要求：

- 默认以远端最新 `main` 作为任务起点；
- 开始前先确认当前 branch 和工作区状态；
- 如果存在未提交修改，不允许直接 pull 覆盖，也不允许自行丢弃用户修改；
- 如果 `git pull --ff-only` 失败、出现 divergence/conflict，立即停止并报告；
- 不允许使用 `git reset --hard`、强制覆盖、自动 rebase 用户未确认的修改；
- 同步完成后，再重新阅读最新的 `AGENTS.md`、`README.md`、相关 Goal 文档；
- 任何任务都不能基于旧 clone/旧 commit 直接开始实现。

### 14.2 标准工作流

项目默认采用：

~~~text
ChatGPT 与用户讨论设计
-> ChatGPT 编写 Goal 文档和验收标准
-> 用户让 Codex 严格按 Goal 实现
-> Codex 自测并提交 focused commit
-> ChatGPT 检查 commit / diff / runtime evidence
-> 验收通过
-> 讨论下一个 Goal
~~~

Codex 不得主动越过 Goal scope。

---

## 15. 会话收尾

完成一个有意义的实现阶段后：

1. 更新对应设计文档；
2. 写清楚真正实现了什么；
3. 写清楚仍然只是计划的内容；
4. 有 benchmark 时记录真实证据；
5. 更新 README 当前状态；
6. 记录 Open Questions / 下一步；
7. 检查 secrets、host-specific data、巨大生成文件；
8. 提交 focused changes。

后续 Agent 应能仅靠 Git 恢复状态，不依赖聊天历史。

---

## 16. 当前项目状态

~~~text
Goal 001 ✅
Go -> cgo -> project C API -> DPDK EAL

Goal 002 ✅
DPDK 25.11.3
+ package-local native C
+ 双 TAP Virtual PMD
+ C-owned mempool
+ 单 RXQ/TXQ
+ single-lcore RTC
+ exact marker E2E

Goal 002R ✅
+ safe teardown / cleanup guard
+ argv C helper
+ project-local build env

Goal 003 ✅
Ethernet / IPv4 / TCP / UDP parser
+ host-order metadata
+ parser failure drop
+ deterministic parser tests

Goal 004-005 ✅
static rte_hash exact-flow
+ rte_lpm IPv4 route fallback
+ flow > route > default DROP
+ DROP / FORWARD / REWRITE
+ IPv4/TCP/UDP checksum
+ startup JSON rule snapshot
+ lookup/action lifecycle + E2E
~~~

当前仍然只证明 software/simulation dataplane 功能正确性，不绑定真实 NIC，
不宣称 hardware RSS、NUMA performance、line-rate 或吞吐性能。

## 17. 当前执行任务

当前没有进行中的实现 Goal。

下一步应先与用户确定收尾压缩方案。优先考虑：

~~~text
合并后续 A：动态规则发布 + RCU/QSBR
合并后续 B：multi-queue / RSS + benchmark
最后：是否保留 API/Web frontend，或先转 VPP
~~~

在新的 Goal 文档与验收标准明确之前，Codex 不应自行继续开发。
