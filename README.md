# DPDK High-Speed Flow Router

一个采用 **Go 控制面 + thin cgo + C/DPDK 数据面** 的高速 **L2-L4 用户态 Flow Router**。

这个项目不是普通的 DPDK API demo，也不是应用层 proxy。目标是实现一个小而完整、可测量、可解释的数据面系统，并能够从 queue、mbuf、parser、lookup、rewrite 一直讲到 overload 和 benchmark。

## 当前状态

**Goal 001 已验收通过：Go -> cgo -> project C API -> DPDK EAL init/info/cleanup。**

当前已经完成：

- Go CLI 作为程序入口；
- thin cgo wrapper；
- 项目级 C API；
- 真实 DPDK EAL init / runtime info / cleanup；
- EAL argv 的 C memory ownership；
- EAL 生命周期固定在一个 locked OS thread；
- build / test / vet / EAL integration evidence；
- 只读环境检查脚本。

尚未实现：

- mempool；
- port / virtual PMD；
- RX/TX queue；
- packet parser；
- flow/route table；
- rewrite；
- worker；
- multi-queue / RSS；
- benchmark；
- Web API / frontend。

详细边界见 [架构与 ownership](docs/architecture.md)，Goal 001 的实现与验收证据见 [Goal 001](docs/goals/001-bootstrap-go-cgo-dpdk.md)。

## 当前重要约束

Goal 001 的实际环境使用了 DPDK 19.11.14，但既有软件实验主线已经使用 DPDK 25.11.3。

因此进入 Goal 002 前先做两件事：

1. **把新项目统一到 DPDK 25.11.3**；
2. **确定稳定的 native C dataplane 构建方式**，不让后续大量 C 文件继续依赖 `#include *.c + go build -a` 的过渡模式。

当前仍然只做 software/simulation dataplane：

- 不 bind/unbind 真实 NIC；
- 不使用管理网卡做 VFIO；
- 不修改默认路由或防火墙；
- 不宣称真实 NIC、hardware RSS 或 NUMA 性能。

## Goal 001：构建与 EAL 验证

依赖 Linux、Go+cgo、GCC/Clang、make、pkg-config 和 DPDK development files。

~~~sh
./scripts/check_env.sh
make build
~~~

Goal 001 的旧验证环境需要：

~~~sh
export CGO_CFLAGS_ALLOW='-include|rte_config.h'
~~~

然后选择当前进程允许使用的 CPU：

~~~sh
EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
./bin/flow-router -- --lcores="0@${EAL_CPU}" --no-huge --no-pci --no-shconf -m 64
~~~

成功时会打印：

~~~text
DPDK version: ...
EAL init succeeded: ...
EAL cleanup succeeded
~~~

这条链路只证明 EAL 和 Go/cgo/C 边界，不代表 dataplane 收发性能。

测试：

~~~sh
export CGO_CFLAGS_ALLOW='-include|rte_config.h'
go test ./...
go vet ./...
bash -n scripts/check_env.sh
bash -n scripts/start_codex_tmux.sh

FLOW_ROUTER_TEST_CPU="$EAL_CPU" go test -count=1 -v ./control/dataplane
~~~

## 为什么做这个项目

我们真正想验证的是下面这些工程问题：

~~~text
RSS / multi-queue
        ↓
RXQ single ownership
        ↓
fixed lcore
        ↓
burst processing
        ↓
packet parsing
        ↓
route / flow lookup
        ↓
action / rewrite
        ↓
bounded TX
        ↓
benchmark
~~~

最终项目要能够回答：

- 为什么 RXQ 通常由一个固定 lcore owner 处理？
- mbuf ownership 在 RX、应用和 TX 之间怎么转移？
- 什么时候 RTC 比 cross-core pipeline 更合适？
- RSS 和 flow affinity 如何配合？
- TX short return 怎么处理才不会泄漏或无限 retry？
- 哪些状态应该做 per-lcore？
- NUMA 对 queue/lcore/mempool 有什么影响？
- 哪些优化对整条 datapath 真的有效？

## 目标架构

~~~text
                       Go Control Plane
                config / rules / lifecycle
                          / stats
                             |
                     coarse cgo API
                             |
                             v
+----------------------------------------------------------------+
|                       C / DPDK Dataplane                        |
|                                                                |
|  NIC / virtual PMD                                             |
|        |                                                       |
|       RSS                                                      |
|        |                                                       |
|       RXQ                                                      |
|        |                                                       |
|   fixed lcore                                                  |
|        |                                                       |
|   rte_eth_rx_burst()                                           |
|        |                                                       |
|   Ethernet -> IPv4 -> TCP/UDP parser                           |
|        |                                                       |
|   route / flow / policy lookup                                 |
|        |                                                       |
|   DROP / FORWARD / REWRITE                                     |
|        |                                                       |
|   bounded TX policy                                            |
|        |                                                       |
|   rte_eth_tx_burst()                                           |
|        |                                                       |
|       TXQ -> NIC                                               |
+----------------------------------------------------------------+
~~~

packet hot path 全部留在 C/DPDK。Go 不逐包处理。

## v0.1 范围

### 协议

- Ethernet
- IPv4
- TCP
- UDP

### 核心能力

- L2/L3/L4 parse；
- route 和/或 exact flow lookup；
- packet drop；
- output-port forwarding；
- 基础 IPv4/TCP/UDP rewrite；
- RX/TX burst；
- fixed RXQ -> lcore ownership；
- multi-queue architecture；
- RSS-aware flow affinity；
- Run-To-Completion fast path；
- per-lcore stats；
- bounded TX retry/drop；
- 可重复 functional/performance test。

### Go 控制面

Go 后续负责：

- configuration；
- route/rule lifecycle；
- dataplane lifecycle；
- stats access；
- 后续 immutable config/rule publication；
- 再后续 REST/gRPC 和 Web frontend。

cgo 必须保持薄且粗粒度。

## v0.1 明确不做

第一版暂不做：

- IPv6；
- NAT；
- conntrack；
- TCP termination；
- TCP stream reassembly；
- user-space TCP stack；
- Kafka/DDS/HTTP/SFTP 等 L7 parsing；
- DPI；
- 大型 wildcard ACL engine；
- crypto；
- generic plugin framework；
- VPP-like graph engine；
- 强制 cross-core `rte_ring` pipeline；
- distributed control plane。

先把 baseline dataplane 做完整、测清楚，再决定后续能力。

## 设计原则

### single owner

默认：

~~~text
RSS -> RXQ -> fixed lcore -> RTC -> TXQ
~~~

优先保持 ownership 和 cache locality，不在软件里无意义地重新分发。

### RTC first

一个 packet 默认由同一个 lcore 完成 parse、lookup、rewrite、TX。

只有 benchmark 证明某个 heavy/slow stage 需要拆分时，才引入 `rte_ring` pipeline。

### NUMA-aware

真实 NIC 环境下尽量让：

~~~text
NIC
+ RX/TX queues
+ worker lcores
+ mempool
+ hot flow/route state
~~~

位于同一 NUMA node。

当前 software lab 不用于证明真实 NIC NUMA 性能。

### ownership 明确

~~~text
RX burst 返回 mbuf
        ↓
application owns it
        ↓
TX 接受 mbuf
        ↓
PMD/TX owns accepted packet

TX 未接受 mbuf
        ↓
application 仍然 owns it
        ↓
bounded retry / drop / free
~~~

### evidence-driven

没有 workload、环境和测量，不做性能结论。

## 开发路线

~~~text
Goal 001  Go/cgo/C + EAL                         ✅
        ↓
环境基线  DPDK 25.11.3 + native C build
        ↓
Goal 002  mempool + virtual PMD + RXQ/TXQ + RTC
        ↓
Goal 003  Ethernet / IPv4 / TCP / UDP parser
        ↓
Goal 004  route / flow lookup
        ↓
Goal 005  DROP / FORWARD / REWRITE
        ↓
Goal 006  Go 动态规则管理
        ↓
Goal 007  QSBR / RCU 热更新
        ↓
Goal 008  multi-queue / RSS / fixed lcore
        ↓
Goal 009  benchmark / profiling
        ↓
Goal 010  API + Web frontend
~~~

## Benchmark 目标

至少记录：

- Mpps / Gbps；
- RX/TX/drop；
- CPU；
- cycles/packet（可行时）；
- packet size / flow count；
- queue count / burst size；
- workload duration；
- DPDK version；
- PMD/NIC 类型。

software PMD/TAP/PCAP 结果只标记为软件实验。

## 与 VPP 的关系

本仓库坚持 **DPDK-first / from-scratch**。

后续会单独建立 VPP / GoVPP 项目：

~~~text
P4 / P4Runtime
        ↓
DPDK High-Speed Flow Router
        ↓
VPP / GoVPP
        ↓
Cloud Native / Cloud Network Dataplane
~~~

这个项目负责证明“自己从底层构建数据面”的能力；VPP 项目负责学习和实践成熟工业级框架。

## 当前下一步

Goal 002 已定义，当前等待 Codex 执行：

[Goal 002：统一 DPDK 25.11.3、收敛 native C 构建，并跑通 TAP RTC 转发](docs/goals/002-dpdk-25-11-3-tap-rtc-forwarding.md)

本阶段将在新机器完成：

1. DPDK 25.11.3 安装和版本锁定；
2. package-local native C 构建组织；
3. 双 TAP Virtual PMD；
4. mempool + 单 RXQ/TXQ；
5. single-lcore RTC 原样 forwarding；
6. exact marker 端到端验证。

仍然只做软件仿真，不绑定真实 NIC。

开发协作规则见 [AGENTS.md](AGENTS.md)。
