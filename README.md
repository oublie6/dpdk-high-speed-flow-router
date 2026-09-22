# DPDK High-Speed Flow Router

一个采用 **Go 控制面 + thin cgo + C/DPDK 数据面** 的高速 **L2-L4 用户态 Flow Router**。

这个项目不是普通的 DPDK API demo，也不是应用层 proxy。目标是实现一个小而完整、可测量、可解释的数据面系统，并能够从 queue、mbuf、parser、lookup、rewrite 一直讲到 overload 和 benchmark。

## 当前状态

**Goal 002 / Goal 002R 已验收通过，当前进入 Goal 003 Parser 阶段。**

当前已经完成：

- DPDK 25.11.3 `/usr/local` 安装与精确版本检查；
- `control/dataplane` 纯 Go lifecycle manager；
- `dataplane/native` 唯一 cgo package 和 package-local C 源码；
- long-running EAL owner thread 与 graceful stop；
- 两个 TAP Virtual PMD、一个 C-owned mbuf pool；
- RX port/RXQ0 -> fixed lcore RTC -> TX port/TXQ0 原样转发；
- TX partial return 的 zero-retry/free 尾部 ownership 策略；
- EtherType `0x88b5` 与 marker `dpdk-flow-router-goal002` 的自动端到端验证；
- port close -> mempool free -> EAL cleanup 的真实证据；
- Teardown 失败后禁止进入 EAL cleanup，并在 C 层拒绝清理仍有存活资源的 runtime；
- cgo argv 通过小型 C helper 装配，Go 不再计算 `char **` 元素地址；
- DPDK 安装不再修改用户级 Go 环境，构建 allowlist 由 Makefile 局部提供。

尚未实现：

- packet parser；
- flow/route table；
- rewrite；
- multi-queue / RSS；
- benchmark；
- Web API / frontend。

详细边界见 [架构与 ownership](docs/architecture.md)，实现与验收证据见
[Goal 002](docs/goals/002-dpdk-25-11-3-tap-rtc-forwarding.md) 和
[Goal 002R 修复记录](docs/goals/002r-cleanup-cgo-readability.md)。

## 当前重要约束

项目当前构建基线固定为 DPDK 25.11.3。源码和运行时都会检查版本，
`scripts/check_env.sh` 遇到其他版本直接失败。系统自带的 19.11 package 可以保留，
但项目必须从 `/usr/local/lib/pkgconfig/libdpdk.pc` 解析到 25.11.3。

当前仍然只做 software/simulation dataplane：

- 不 bind/unbind 真实 NIC；
- 不使用管理网卡做 VFIO；
- 不修改默认路由或防火墙；
- 不宣称真实 NIC、hardware RSS 或 NUMA 性能。

## Goal 002：构建与 TAP RTC 验证

在 Ubuntu/Debian root 环境安装官方 DPDK 25.11.3：

~~~sh
./scripts/install_dpdk.sh
./scripts/check_env.sh
pkg-config --modversion libdpdk
make build
~~~

安装脚本从 DPDK 官方 release 下载并校验源码，使用 Meson/Ninja release build
安装到 `/usr/local`，仅启用本 Goal 需要的 vdev、ring mempool 和 TAP driver。
它不会绑定 PCI、配置 VFIO、修改网络、GRUB、HugePage 或用户级 Go 环境。
Makefile 在项目进程内提供旧版 cgo 所需的精确 allowlist，只允许 DPDK
pkg-config 实际输出的 `-include`、`rte_config.h` 与 `-mrtm`。

EAL 一次性回归：

~~~sh
EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
./bin/flow-router --probe -- --lcores="0@${EAL_CPU}" \
  --no-huge --no-pci --no-shconf --no-telemetry -m 64
~~~

成功时会打印：

~~~text
DPDK version: ...
EAL init succeeded: ...
EAL cleanup succeeded
~~~

TAP RTC 端到端验证：

~~~sh
./scripts/verify_tap_forwarding.sh
~~~

脚本动态生成两张短名称 TAP，只把本次接口设为 UP；它注入一个确定性 Ethernet
frame，并在 TX TAP 精确比较完整 frame、EtherType 和 marker。结束时发送 SIGTERM，
等待 worker 退出、port close、mempool free 与 EAL cleanup，并确认不遗留进程、
接口和临时文件。它不配置 IP、route 或 firewall。

推荐通过项目入口构建和测试：

~~~sh
make build
make test
make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
bash -n scripts/start_codex_tmux.sh
python3 -m py_compile scripts/verify_tap_forwarding.py

FLOW_ROUTER_TEST_CPU="$EAL_CPU" make test
~~~

需要直接运行 Go 命令时，显式提供项目局部环境变量：

~~~sh
CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 ./...
CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go vet ./...
FLOW_ROUTER_TEST_CPU="$EAL_CPU" \
  CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
    ./control/dataplane ./dataplane/native
~~~

普通测试不初始化 EAL；设置 `FLOW_ROUTER_TEST_CPU` 后会运行真实 EAL 回归和
TX partial-return ownership 测试。这里的结果只证明 software/TAP 功能正确性，
不证明真实 NIC DMA、hardware RSS、NUMA cost、line-rate 或吞吐性能。

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

Goal 002 / Goal 002R 已正式验收通过：

[Goal 002：统一 DPDK 25.11.3、收敛 native C 构建，并跑通 TAP RTC 转发](docs/goals/002-dpdk-25-11-3-tap-rtc-forwarding.md)

本阶段已经完成：

1. DPDK 25.11.3 安装和版本锁定；
2. package-local native C 构建组织；
3. 双 TAP Virtual PMD；
4. mempool + 单 RXQ/TXQ；
5. single-lcore RTC 原样 forwarding；
6. exact marker 端到端验证。

当前 Goal：

[Goal 003：Ethernet / IPv4 / TCP / UDP Parser 与 Packet Metadata](docs/goals/003-packet-parser-metadata.md)

本阶段只做 parser + metadata：parse 成功仍原样 forwarding，unsupported/malformed packet drop。
不做 flow/route lookup、rewrite、RSS 或多核。仍然只做软件仿真，不绑定真实 NIC。

开发协作规则见 [AGENTS.md](AGENTS.md)。
