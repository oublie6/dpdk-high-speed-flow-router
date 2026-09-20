# Goal 002：统一 DPDK 25.11.3、收敛 native C 构建，并跑通 TAP RTC 转发

日期：2026-09-19  
状态：Codex 已完成，待 ChatGPT 验收

## 1. 背景

Goal 001 已经完成并通过验收：

~~~text
Go CLI
-> thin cgo
-> project C API
-> DPDK EAL init/info/cleanup
~~~

但 Goal 001 所在的新机器实际使用的是：

~~~text
Ubuntu 20.04
Go 1.13.8
DPDK 19.11.14
~~~

而本项目后续开发基线已经决定统一为：

~~~text
DPDK 25.11.3
Go control plane
thin cgo
C/DPDK dataplane
software / virtual PMD simulation
~~~

本 Goal 同时解决三个问题：

1. 在**当前这台新机器**上安装并切换到 DPDK 25.11.3；
2. 把 Goal 001 的临时 `#include *.c + go build -a` 方式收敛成稳定的 package-local native C 构建组织；
3. 在不绑定真实 NIC 的前提下，使用两个 TAP Virtual PMD 跑通第一条真实 RTC packet path。

本 Goal 完成后，项目第一次真正具备：

~~~text
TAP RX
-> DPDK RXQ
-> fixed lcore
-> rte_eth_rx_burst()
-> 原样 forwarding
-> rte_eth_tx_burst()
-> TAP TX
~~~

这仍然只是软件仿真，不代表真实 NIC line-rate 能力。

---

## 2. 最终目标

完成下面这条端到端链路：

~~~text
Host raw packet injector
        |
        v
Linux TAP: <rx iface>
        |
        v
DPDK TAP PMD
        |
      RXQ 0
        |
   fixed lcore
        |
rte_eth_rx_burst()
        |
   RTC forwarding
        |
rte_eth_tx_burst()
        |
      TXQ 0
        |
DPDK TAP PMD
        |
        v
Linux TAP: <tx iface>
        |
        v
Host capture / marker assertion
~~~

Go 只负责：

- EAL / dataplane 生命周期；
- 参数和配置；
- start/stop；
- 最终 stats 展示；
- 后续控制面扩展。

C/DPDK 负责：

- EAL；
- vdev/port；
- mempool；
- RX/TX queue；
- worker loop；
- mbuf ownership；
- packet forwarding。

**禁止逐包跨 cgo。**

---

# 3. Gate A：先统一新机器上的 DPDK 版本

在写 Goal 002 packet path 前，必须先把新机器的开发基线统一到：

~~~text
DPDK 25.11.3
~~~

如果 Gate A 没有通过，不得继续实现 TAP forwarding。

## 3.1 新增安装脚本

新增：

~~~text
scripts/install_dpdk.sh
~~~

目标：

- 安装 DPDK 25.11.3 的必要 build dependencies；
- 从官方 release source 安装 DPDK 25.11.3；
- 源码建议放在：
  `/opt/src/dpdk-25.11.3`
- 安装 prefix 建议：
  `/usr/local`
- 使用 Meson/Ninja release build；
- 安装完成后执行必要的 `ldconfig`；
- 最终保证：
  `pkg-config --modversion libdpdk`
  返回 **25.11.3**。

可以根据当前 Ubuntu 20.04 的实际 Meson/Ninja 版本补齐或升级构建工具，但必须记录真实操作。

### 安全限制

安装脚本可以：

- apt 安装 build dependencies；
- 下载和编译官方 DPDK source；
- 安装到 /usr/local；
- 执行 ldconfig。

安装脚本不得：

- bind/unbind PCI device；
- 调用 dpdk-devbind.py 修改真实 NIC；
- 修改默认路由；
- 修改 firewall；
- 修改管理网卡；
- 修改 Kubernetes/Cilium；
- 配置 VFIO 真实设备；
- 永久修改 GRUB；
- 为本 Goal 强制配置 HugePage。

当前 TAP 仿真允许使用 `--no-huge`。

## 3.2 处理旧版 DPDK 冲突

当前机器可能仍存在 distro DPDK 19.11.14。

要求：

- 不要为了省事继续兼容 19.11；
- 不要在源码里继续增加兼容旧 DPDK 的 `#if RTE_VERSION`，除非 25.11.3 自身确有必要；
- 优先让 /usr/local 安装的 DPDK 25.11.3 成为本项目 `pkg-config libdpdk` 的实际解析结果；
- 不要无理由卸载系统 package；如果存在 pkg-config 搜索路径冲突，应通过清晰、可重复的环境/构建配置解决；
- README 和环境检查脚本必须显示最终实际解析到的版本和路径。

## 3.3 更新环境检查

更新：

~~~text
scripts/check_env.sh
~~~

至少检查：

- Go；
- gcc/clang；
- pkg-config；
- meson；
- ninja；
- DPDK pkg-config version；
- DPDK prefix/pkg-config path（可获取时）；
- `/dev/net/tun`；
- `ip` 命令；
- Python3；
- CPU / allowed affinity；
- HugePage 当前状态；
- NUMA 信息。

**本项目当前要求 DPDK 精确为 25.11.3。**

如果：

~~~text
pkg-config --modversion libdpdk != 25.11.3
~~~

环境检查必须失败并给出明确说明。

## 3.4 Gate A 验收

必须真实执行：

~~~bash
./scripts/install_dpdk.sh
./scripts/check_env.sh
pkg-config --modversion libdpdk
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
~~~

验收必须能看到：

~~~text
25.11.3
~~~

并记录新机器的真实：

- distro；
- kernel；
- Go；
- GCC/Clang；
- Meson；
- Ninja；
- DPDK；
- CPU count；
- NUMA；
- HugePage；
- /dev/net/tun。

---

# 4. Gate B：收敛 Go/cgo/C 构建组织

Goal 001 当前存在临时方式：

~~~text
control/dataplane/runtime_linux.c
    #include "../../dataplane/core/dp_runtime.c"

make build
    -> go build -a
~~~

这个方式在 Goal 001 可接受，但不能继续扩展。

本 Goal 要改成：

~~~text
Go Control Plane
        |
        v
control/dataplane          # 纯 Go lifecycle / manager
        |
        v
dataplane/native           # 唯一 cgo package
        |
        +-- binding_linux.go
        +-- dp_api.h
        +-- dp_internal.h
        +-- dp_runtime.c
        +-- dp_port.c
        +-- dp_worker.c
        +-- ...
        |
        v
DPDK 25.11.3
~~~

## 4.1 核心要求

- 只有 `dataplane/native` package 可以 `import "C"`；
- 所有本项目 native `.c/.h` 文件与 cgo binding 放在同一个 package 目录；
- cgo 通过：
  `#cgo pkg-config: libdpdk`
  获取 DPDK flags；
- 不再通过 `#include "../../.../*.c"` 引入外部 C implementation；
- `make build` 不再依赖 `go build -a` 才能看到 C 源码变化；
- 修改任意 `dataplane/native/*.c` 后，普通 `go build` / `go test` 应能正确触发编译；
- 本 Goal 暂时**不要**额外引入 `libflowdp.a`、`libflowdp.so`、项目级 Meson build；
- 当 dataplane 明显变大后，再考虑独立 native library。

## 4.2 Go 控制面职责

`control/dataplane` 应变成纯 Go 层。

它负责：

- lifecycle；
- dedicated locked OS thread；
- config；
- ready / stop / wait；
- 对 `dataplane/native` 的粗粒度调用。

它不允许：

- import C；
- 接触 rte_mbuf；
- 调用 per-packet native API；
- 在 Go 中逐包转发。

---

# 5. long-running EAL / dataplane 生命周期

Goal 001 的 `Probe()` 是一次性 init/info/cleanup。

Goal 002 开始需要真正 long-running dataplane。

建议模型：

~~~text
Go main
   |
   v
control/dataplane
   |
dedicated goroutine
runtime.LockOSThread()
   |
   v
native.Init()
   |
native.Setup()
   |
signal ready
   |
native.Run()        # blocking C RTC loop
   |
stop requested
   |
native.Teardown()
   |
native.Cleanup()
   |
locked goroutine exits
~~~

另一个 Go goroutine / signal handler 可以请求 stop，但：

- stop API 只能修改 C-owned stop flag；
- stop path 不应从任意 Go OS thread 直接操作 ethdev/queue/mempool；
- port stop/close、mempool free、EAL cleanup 仍在 dataplane owner thread 上完成。

可以使用 C11 atomic stop flag 或等价简单机制。

## 5.1 最小项目级 native API

可以调整名字，但语义至少覆盖：

~~~c
dp_runtime_init(...)
dp_runtime_get_info(...)

dp_dataplane_setup(...)
dp_dataplane_run(...)
dp_dataplane_request_stop(...)
dp_dataplane_get_stats(...)
dp_dataplane_teardown(...)

dp_runtime_cleanup(...)
~~~

不要为每个 packet 暴露 C API。

---

# 6. TAP Virtual PMD 软件拓扑

本 Goal 只使用 software TAP PMD。

建议：

~~~text
DPDK vdev name: net_tap_rx
Linux iface:    动态生成 dfrx<PID>

DPDK vdev name: net_tap_tx
Linux iface:    动态生成 dftx<PID>
~~~

Linux interface 名不要硬编码成可能与其他实验冲突的全局固定名字。

必须：

- 使用两个独立 TAP vdev；
- 使用 `--no-pci`；
- 当前允许 `--no-huge`；
- 不 bind 真实 NIC；
- 不修改默认路由；
- 只允许操作本次测试自己创建的 TAP interface。

不要求 TAP RSS。

---

# 7. mempool / port / queue

## 7.1 mempool

使用 `rte_pktmbuf_pool_create()` 创建一个 C-owned packet mbuf pool。

要求：

- 参数适合软件实验，不要追求极限；
- 使用 `RTE_MBUF_DEFAULT_BUF_SIZE`；
- 记录 nb_mbuf、cache size、socket id；
- 说明为什么当前只使用一个 pool；
- cleanup 时必须在 ethdev stop/close 之后再 free pool。

可以采用例如 4096 mbufs / 128 cache 这样的保守值，也可以根据实际 DPDK 25.11.3 环境调整并记录理由。

## 7.2 port 识别

不要盲目假设：

~~~text
rx port == 0
tx port == 1
~~~

应通过 DPDK device name / vdev identity 找到对应 port id，并记录 mapping：

~~~text
net_tap_rx -> port X
net_tap_tx -> port Y
~~~

## 7.3 queue

Goal 002 只处理：

~~~text
1 个有效 RXQ
1 个有效 TXQ
1 个 dataplane owner lcore
~~~

worker 只：

~~~text
poll rx_port / RXQ0
send tx_port / TXQ0
~~~

如果 TAP PMD 配置层要求为每个 port 创建对称 queue，可以为兼容 driver 配置，但 hot path 只能有一个实际 RX source 和一个实际 TX destination，并在文档写清楚。

descriptor 数量使用保守值，并允许 `rte_eth_dev_adjust_nb_rx_tx_desc()` 调整。

当前不做 RSS/multi-queue。

---

# 8. 第一条 RTC forwarding hot path

本 Goal 不做 parser，不做 flow table。

worker 逻辑只有：

~~~text
while (!stop) {
    n = rte_eth_rx_burst(rx_port, 0, pkts, BURST_SIZE);

    if (n == 0)
        continue;

    sent = rte_eth_tx_burst(tx_port, 0, pkts, n);

    if (sent < n) {
        free pkts[sent:n];
        count tx_unsent/drop;
    }
}
~~~

要求：

- RX burst 后，application owns 返回的 mbuf；
- TX 接受的 `[0, sent)` ownership 转给 PMD；
- TX 未接受的 `[sent, n)` 仍属于 application，必须 free；
- 禁止无限 TX retry；
- Goal 002 可以使用最简单的 zero-retry/drop policy；
- 不要逐包调用 Go；
- 不要在 hot loop 中打印日志；
- stats 至少包括：
  - rx；
  - tx_accepted；
  - tx_unsent；
  - drop；
  - burst count（可选）。

Goal 002 的目标是 ownership 正确，不是做最优 TX buffering。

---

# 9. 自动化 TAP 端到端验证

新增自动化验证脚本，建议：

~~~text
scripts/verify_tap_forwarding.sh
scripts/verify_tap_forwarding.py
~~~

具体文件数可调整，但必须自动化完成。

## 9.1 验证流程

验证脚本应：

1. 选择当前进程允许使用的一个 CPU；
2. 生成本次测试唯一的短 TAP iface 名；
3. 启动 `bin/flow-router`；
4. 传入两个 TAP vdev；
5. 等待 dataplane ready；
6. 只把本次创建的 TAP interface 设置为 UP；
7. 在 RX TAP 上通过 AF_PACKET/raw socket 注入一个 deterministic Ethernet frame；
8. EtherType 建议使用 `0x88b5`；
9. payload 使用固定 marker，例如：
   `dpdk-flow-router-goal002`
10. 在 TX TAP 上抓取；
11. 精确断言 EtherType + marker 一致；
12. 请求 router graceful stop；
13. 等待 cleanup；
14. 检查最终 stats；
15. 清理本次测试创建的 process/interface/temp files。

不要只看 link up 或 port counter 就宣称成功。

**真正验收依据必须是 exact marker 从 RX TAP 到 TX TAP。**

## 9.2 测试隔离

- interface 名必须有本次测试唯一 suffix；
- cleanup 只能删除本测试自己创建的 TAP；
- 不删除任意其他 tap/tun；
- 不碰默认路由；
- 不碰 host firewall；
- 不碰管理 NIC；
- 不碰 WireGuard/Cilium/Kubernetes。

## 9.3 超时

自动化验证必须有整体 timeout。

如果 forwarding 没成功：

- 自动停止 router；
- 自动清理本次资源；
- 返回非零；
- 输出足够诊断信息。

不能无限卡住。

---

# 10. CLI / 日志

CLI 可以合理调整，但至少支持清晰传入：

- EAL args；
- rx vdev/device name；
- tx vdev/device name；
- 或等价配置。

启动成功后输出一次低频信息，例如：

~~~text
DPDK version: 25.11.3
main_lcore: ...
rx device: net_tap_rx -> port ...
tx device: net_tap_tx -> port ...
mempool: ...
dataplane ready
~~~

退出后输出最终 stats：

~~~text
rx=...
tx_accepted=...
tx_unsent=...
drop=...
~~~

hot path 不打印逐包日志。

---

# 11. 文档要求

所有新增文档与代码注释遵守 AGENTS.md：

**默认使用中文。**

更新：

- `README.md`
- `docs/architecture.md`
- 本 Goal 实现记录

至少写清楚：

- 新机器 DPDK 25.11.3 的实际安装方式；
- 为什么不再兼容 19.11；
- 为什么本阶段使用 package-local native C，而不是 `libflowdp.a/.so`；
- long-running EAL thread ownership；
- port/mempool/queue ownership；
- RX/TX mbuf ownership；
- TAP simulation 的能力边界；
- 当前仍不能证明真实 NIC DMA/RSS/NUMA/line-rate。

---

# 12. 明确非目标

本 Goal 不实现：

- Ethernet parser；
- IPv4 parser；
- TCP/UDP parser；
- flow table；
- route table；
- DROP rule；
- rewrite；
- checksum rewrite；
- RCU/QSBR；
- Go 动态 AddFlow/DeleteFlow；
- REST/gRPC；
- Web frontend；
- RSS；
- multi-queue；
- multi-lcore；
- rte_ring pipeline；
- NAT；
- conntrack；
- IPv6；
- real NIC / VFIO；
- hardware RSS；
- NUMA performance benchmark；
- throughput optimization。

原样 forwarding 之外的 packet semantics 留给 Goal 003+。

---

# 13. 验收标准

## A. DPDK 版本

~~~bash
pkg-config --modversion libdpdk
~~~

必须精确输出：

~~~text
25.11.3
~~~

项目不接受“仍然链接 19.11，但代码兼容 25.11”的结果。

## B. 构建组织

检查：

~~~bash
grep -R 'import "C"' -n --include='*.go' .
~~~

应只有：

~~~text
dataplane/native
~~~

且不得再出现通过 `#include "../../.../*.c"` 引入 dataplane implementation 的方式。

普通：

~~~bash
make build
go test ./...
go vet ./...
~~~

应成功。

`make build` 不应依赖 `go build -a` 作为正确性的必要条件。

## C. Shell / Python 基础检查

至少：

~~~bash
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
~~~

如果实际文件布局不同，运行对应检查。

## D. EAL 回归

Goal 001 的 EAL init/info/cleanup 能力不能回归。

至少验证：

- init 成功；
- version = 25.11.3；
- cleanup 成功；
- invalid EAL args 失败；
- 同一进程重复 init 被拒绝或由新 lifecycle API 明确禁止。

## E. TAP forwarding

自动执行：

~~~bash
./scripts/verify_tap_forwarding.sh
~~~

必须真实证明：

~~~text
host inject marker
-> RX TAP
-> DPDK TAP PMD
-> RXQ0
-> fixed lcore RTC
-> TXQ0
-> DPDK TAP PMD
-> TX TAP
-> host exact marker capture
~~~

脚本最终退出 0。

## F. ownership / cleanup

验收时必须确认：

- RX burst 返回后 application owns mbuf；
- TX accepted mbuf 不被 application free；
- TX unsent mbuf 被 application free；
- ethdev stop/close 在 mempool free 前；
- worker stop/join 在 EAL cleanup 前；
- 失败路径不会遗留本次 router process；
- 不遗留本次 TAP interfaces；
- 没有触碰真实 NIC 和系统默认网络配置。

## G. scope

源码中不应出现 Goal 003+ 的：

- parser；
- route/flow lookup；
- rewrite；
- RCU/QSBR；
- RSS/multi-queue；
- Web API。

---

# 14. Codex 交付要求

完成后：

1. 自己检查完整 diff；
2. 运行所有可以运行的验收命令；
3. 记录真实环境和命令结果；
4. 更新 Goal 状态为“Codex 已完成，待 ChatGPT 验收”；
5. 更新 README 当前状态；
6. 更新架构文档；
7. 不提交二进制、大型日志、pcap、secret、host-sensitive data；
8. 创建一个 focused commit。

建议 commit message：

~~~text
dataplane: add DPDK 25.11.3 TAP RTC forwarding
~~~

---

# 15. 完成后由 ChatGPT 验收

Codex 完成提交后先停止继续开发。

ChatGPT 将检查：

~~~text
DPDK 25.11.3 installation evidence
-> pkg-config resolution
-> native C build organization
-> Go/cgo boundary
-> long-running EAL thread ownership
-> mempool/port/queue lifecycle
-> RX/TX mbuf ownership
-> exact TAP marker forwarding
-> cleanup/error paths
-> scope compliance
~~~

只有 Goal 002 验收通过后，再设计 Goal 003：

> Ethernet / IPv4 / TCP / UDP parser 与 packet metadata。

---

# 16. Codex 实现记录（2026-09-20）

## 16.1 Gate A：实际环境与安装结果

任务开始时工作区为空、分支为 `main`；`git fetch origin` 后通过
`git pull --ff-only origin main` 从 `5060e14` 快进到远端最新 `c3d4e00`，
没有覆盖或丢弃本地修改。随后完整阅读 AGENTS、README、architecture 与本 Goal。

初始 `pkg-config --modversion libdpdk` 为 `19.11.14`，因此先停止 dataplane
实现。新增并实际执行 `./scripts/install_dpdk.sh`：从官方 release 下载
`dpdk-25.11.3.tar.xz`，SHA-256 为
`3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490`，
源码解压到 `/opt/src/dpdk-25.11.3`，通过 Meson/Ninja release shared build
安装到 `/usr/local` 并执行 `ldconfig`。系统 19.11 package 保留，没有卸载。

首次配置发现 Meson 使用系统 Python、无法导入 venv 中的 `pyelftools`；修正为
Meson 与 `pyelftools` 共用 `/opt/dpdk-build-tools` venv 后，安装成功。最终：

~~~text
Ubuntu 20.04.4 LTS
kernel 5.4.0-216-generic
Go 1.13.8
GCC 9.4.0
Clang unavailable（GCC 已满足构建要求）
Meson 1.5.2
Ninja 1.10.0
Python 3.8.10
DPDK 25.11.3
pkg-config directory /usr/local/lib/pkgconfig
CPU 8 / allowed 0-7
NUMA nodes 1
HugePages_Total 0
/dev/net/tun 存在且为 character device
~~~

实际执行并通过：

~~~bash
./scripts/install_dpdk.sh
./scripts/check_env.sh
pkg-config --modversion libdpdk
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
~~~

`pkg-config --modversion libdpdk` 精确输出 `25.11.3`；prefix 为 `/usr/local`。
安装脚本还通过 `go env -w` 写入 anchored `CGO_CFLAGS_ALLOW`，只放行该版本
pkg-config 实际输出的 `-include`、`rte_config.h` 与 `-mrtm`，因此普通
`go build/test/vet` 不依赖当前 shell 的手工 export。
安装与测试没有 bind/unbind PCI、调用 `dpdk-devbind.py`、配置真实 VFIO、修改
route/firewall/管理网卡/Kubernetes/Cilium/GRUB 或 HugePage。

## 16.2 Gate B：实现内容

- cgo 与所有项目 native `.c/.h` 收敛到 `dataplane/native`；删除 Goal 001 的
  `#include ../../.../*.c` adapter，普通 `go build` 不再使用 `-a`。
- `control/dataplane` 为纯 Go lifecycle manager；一个 locked OS thread 完成
  init、setup、blocking RTC run、teardown、stats 与 EAL cleanup。
- 创建 4096 mbuf / cache 128 / default buffer size 的单个 C-owned pool。
- 按 vdev name 查找两个 TAP port；每个 port 为 TAP compatibility 配置 RXQ0/TXQ0，
  hot path 只 poll RX port/RXQ0，并只发送到 TX port/TXQ0。
- worker 执行 burst RX -> 单次 burst TX；TX 未接受尾部立即 free，zero retry，
  hot path 不打印日志，也不跨 cgo 逐包调用。
- C11 atomic stop 只传递停止请求；worker 返回后依次 stop/close port、验证 pool
  in-use 为 0、free pool、cleanup EAL。
- 自动验证脚本生成唯一 TAP 名，注入 EtherType `0x88b5` 与固定 marker，精确比较
  TX TAP 的完整 60-byte frame，并在成功、setup 失败与 timeout 路径清理资源。
- 使用真实 EAL/mempool 模拟 4 个 mbuf 的 partial return（sent=2），验证尾部被
  application free、前两个仍归 PMD，模拟 PMD 完成后 pool in-use 回到 0。

本机 Go runtime 已占用 realtime signals，TAP PMD 输出 `No Rx trigger signal
available`，并退回非阻塞 polling。exact marker 与 cleanup 均正常；这是软件环境
限制，不构成 forwarding correctness 失败，也不用于性能结论。

## 16.3 实际验收命令与结果

~~~bash
grep -R 'import "C"' -n --include='*.go' .
make build
go test -count=1 ./...
go vet ./...
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
bash -n scripts/start_codex_tmux.sh
python3 -m py_compile scripts/verify_tap_forwarding.py

EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
FLOW_ROUTER_TEST_CPU="$EAL_CPU" go test -count=1 -v \
  ./control/dataplane ./dataplane/native
./scripts/verify_tap_forwarding.sh
~~~

build、普通 test、vet、Shell/Python 语法检查均退出 0。源码搜索只在
`dataplane/native/binding_linux.go` 找到 `import "C"`。真实 EAL 回归确认
25.11.3 init/info/cleanup 成功、未知参数非零退出、同进程第二次 init 被拒绝；
partial-return ownership 测试通过。

另外先清空 Go build cache，再直接运行普通 `go test -count=1 ./...`，结果通过；
随后修改 `dataplane/native/dp_worker.c` 中的注释并执行普通 `go build -x`，trace
明确出现 `gcc ... -c dp_worker.c`。这证明 C source change 由 package dependency
正常触发编译，不依赖 `go build -a`。

TAP 验证退出 0，关键输出为：

~~~text
exact marker captured: EtherType=0x88b5 marker=dpdk-flow-router-goal002 frame_bytes=60
DPDK version: DPDK 25.11.3
rx device: net_tap_rx -> port 0 RXQ0 desc=256
tx device: net_tap_tx -> port 1 TXQ0 desc=256
stats: rx=2 tx_accepted=2 tx_unsent=0 drop=0
teardown: ports_closed=2 pool_in_use=0 pool_freed=true
EAL cleanup succeeded
PASS: exact TAP forwarding, graceful cleanup, no test interfaces/process/temp files remain
~~~

`rx=2` 包含 host 在 TAP link-up 后产生的背景 frame；验收依据是捕获到完整一致的
EtherType + marker frame，而不是计数器恰好等于 1。统计仍满足
`rx == tx_accepted + tx_unsent` 与 `drop == tx_unsent`。

另外执行 `--bad-device` setup 失败与 `--skip-injection --timeout 1` 超时负向测试，
两者均按预期非零退出；随后检查没有 `flow-router` process，也没有任何
`dfrx*` / `dftx*` interface。没有生成或提交 pcap、二进制、巨大日志、secret
或 host-specific credential。

## 16.4 当前边界与下一步

本次只证明 software/TAP functional path，不证明真实 NIC DMA、hardware RSS、
NUMA performance、line-rate、latency 或 throughput。Goal 003+ 的 parser、
flow/route、rewrite、RCU/QSBR、RSS/multi-queue/multi-lcore、API/Web、NAT 与
conntrack 均未实现。下一步仅为 ChatGPT 验收本 focused commit；不继续 Goal 003。

## 16.5 Goal 002R 验收修复

Goal 002 验收发现的 cleanup 顺序、cgo argv 可读性和用户级 Go 环境副作用，已由
[Goal 002R](002r-cleanup-cgo-readability.md) 单独修复并保留完整实现与测试记录。
本节只链接修复记录，不改写 Goal 002 的原始实现历史。
