# Goal 001：项目骨架与 Go -> cgo -> C/DPDK EAL 最小闭环

日期：2026-09-19
状态：✅ 已实现并完成 Codex 验证；待 ChatGPT 验收

## 1. 背景

本项目最终目标是：

~~~
Web Frontend
    ↓
Go Backend / Control Plane
    ↓
thin cgo
    ↓
C / DPDK Dataplane
~~~

但第一阶段不做前端、不做 RX/TX、不做流表，也不追求功能数量。

本 Goal 只解决一件事：

> 建立一个可维护的工程骨架，并在真实开发环境中证明 Go -> cgo -> thin C API -> DPDK EAL init/cleanup 可以稳定编译、运行、退出。

完成后，后续 RX/TX、parser、flow table、RCU/QSBR 都应建立在这个边界之上。

## 2. 本次目标

完成以下最小闭环：

~~~
Go main
  ↓
Go dataplane wrapper
  ↓
cgo
  ↓
public C API
  ↓
DPDK EAL init
  ↓
status / version
  ↓
DPDK EAL cleanup
  ↓
Go process exits cleanly
~~~

同时形成后续开发可复用的：

- 基础目录结构；
- 构建入口；
- 环境检查脚本；
- 最小配置/参数传递模型；
- Go/C ownership 边界说明；
- 可重复运行文档。

## 3. 必须遵守的架构边界

### 3.1 Go 是控制面

Go 只负责：

- process lifecycle；
- 配置与参数准备；
- 调用粗粒度 C API；
- 接收状态/错误；
- 后续承载 REST/gRPC/管理面。

### 3.2 C/DPDK 是数据面

C 层负责：

- DPDK EAL；
- 后续 port/queue/mempool/worker；
- 后续 packet hot path。

### 3.3 禁止逐包跨 cgo

本 Goal 即使还没有 packet，也必须把接口设计成未来不会走向：

~~~
packet -> C -> Go -> C -> packet
~~~

推荐接口风格：

~~~
dp_runtime_init(...)
dp_runtime_get_info(...)
dp_runtime_cleanup(...)
~~~

本阶段不要求未来所有 API 一次设计完，但当前 C API 必须是粗粒度的项目级边界。

## 4. 建议目录

可以基于实际构建需要微调，但保持职责边界清晰：

~~~
.
├── cmd/
│   └── flow-router/
│       └── main.go
├── control/
│   └── dataplane/
│       ├── dataplane.go
│       └── cgo_linux.go
├── dataplane/
│   ├── core/
│   │   └── dp_runtime.c
│   └── include/
│       └── dp_api.h
├── scripts/
│   ├── start_codex_tmux.sh
│   └── check_env.sh
├── docs/
│   ├── architecture.md
│   └── goals/
│       └── 001-bootstrap-go-cgo-dpdk.md
├── Makefile
└── go.mod
~~~

不要为了“看起来完整”创建大量空目录。

## 5. 具体实现要求

### 5.1 Go CLI

实现一个最小可执行程序。命令形式可以合理设计，但必须满足：

- Go 程序是最终入口；
- 能把 EAL 参数传到 C；
- C 返回错误时 Go 有明确错误信息；
- 正常退出时执行 cleanup；
- 不把 DPDK C API 大面积暴露到 Go 业务代码。

### 5.2 thin cgo wrapper

要求：

- cgo 只集中在一个很小的 package；
- Go 其他 package 不直接 import C；
- 不长期保存未经设计的 Go pointer 到 C；
- 字符串/argv 转换有明确分配与释放；
- C 返回值统一转换成 Go error/status。

### 5.3 C public API

至少提供一个稳定的项目级 API，而不是 Go 直接调用 rte_eal_init()。

建议语义：

~~~c
struct dp_runtime_info {
    int initialized;
    unsigned int main_lcore;
};

int dp_runtime_init(int argc, char **argv);
int dp_runtime_get_info(struct dp_runtime_info *info);
int dp_runtime_cleanup(void);
~~~

名称可以调整，但职责必须类似。

### 5.4 DPDK EAL

必须实际调用 DPDK：

- rte_eal_init()；
- 获取至少一项 runtime 信息，例如 main lcore / lcore count / DPDK version；
- rte_eal_cleanup()。

不要用 mock 假装 EAL 成功。

### 5.5 构建

优先使用系统安装的 DPDK 与：

~~~
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
~~~

构建方式需要：

- 一条明确命令完成 build；
- 支持 go build / Makefile；
- 不硬编码单机绝对路径；
- DPDK 未安装时给出清晰错误。

### 5.6 环境检查

新增 scripts/check_env.sh，只读检查：

- Go；
- gcc/clang；
- pkg-config；
- libdpdk pkg-config；
- DPDK version；
- HugePage 基本状态；
- CPU/lcore；
- NUMA（如果系统提供）。

本脚本不能：

- bind/unbind PCI device；
- 修改 host route；
- 修改 firewall；
- 加载/卸载危险内核模块；
- 修改 HugePage 数量；
- 修改系统网络配置。

### 5.7 文档

更新 docs/architecture.md，至少说明：

~~~
Go ownership
C ownership
cgo boundary
EAL lifecycle
future worker lifecycle
future rule-update path
~~~

并在 README 的 Current Status 中记录实际完成状态。

## 6. 当前环境约束

当前开发可能运行在云服务器/软件实验环境。

因此本 Goal：

- 不要求真实 DPDK NIC；
- 不做 vfio-pci binding；
- 不触碰服务器管理网卡；
- 可以使用 --no-huge 完成最小 EAL 验证；
- 如果已有 HugePage，可以记录但不自动修改。

所有真实 NIC / IOMMU / NUMA benchmark 延后。

## 7. 明确非目标

本次 Codex 不要实现：

- RX/TX queue；
- mempool；
- rte_eth_rx_burst()；
- rte_eth_tx_burst()；
- Ethernet/IP/TCP/UDP parser；
- flow table；
- route table；
- rewrite；
- RCU/QSBR；
- multi-queue；
- RSS；
- Web API；
- Web frontend；
- VPP；
- benchmark optimization。

即使这些很容易顺手加，也不要扩大本 Goal。

## 8. 验收标准

Codex 完成后，至少需要提供并实际执行以下证据。

### A. 静态检查

~~~
git status --short
go test ./...
go vet ./...
bash -n scripts/check_env.sh
bash -n scripts/start_codex_tmux.sh
~~~

如果某项受环境限制失败，需要给出真实原因，不得伪造通过。

### B. 构建

至少：

~~~
make build
~~~

或等价的一条明确构建命令成功。

### C. 环境检查

~~~
./scripts/check_env.sh
~~~

输出真实环境信息。

### D. EAL 最小运行

实际运行 Go 入口，证明：

~~~
Go
-> cgo
-> project C API
-> rte_eal_init
-> read DPDK runtime info
-> rte_eal_cleanup
-> clean exit
~~~

日志至少能看到：

- DPDK version；
- EAL init 成功；
- main lcore 或 lcore count；
- cleanup 成功。

### E. 边界检查

验收时应能确认：

- 只有 thin wrapper package 使用 cgo；
- Go 没有直接调用 per-packet DPDK API；
- C API 是项目自己的稳定边界；
- 没有引入 RX/TX/flow-table 等超 scope 功能。

## 9. Codex 交付要求

完成任务后：

1. 自己检查 diff；
2. 运行能运行的验收命令；
3. 把真实命令和结果摘要写入最终报告；
4. 更新 README Current Status；
5. 不删除或弱化 AGENTS.md 中的项目约束；
6. 不提交 secrets、host-sensitive data、巨大日志；
7. 创建一个 focused commit。

建议 commit message：

~~~
bootstrap: establish Go cgo DPDK EAL skeleton
~~~

## 10. 给 Codex 的执行提示

开始前先阅读：

1. AGENTS.md
2. README.md
3. 本文件

然后只完成 Goal 001。

不要提前实现 Goal 002 的 RX/TX dataplane。

## 11. 完成后由 ChatGPT 验收

Codex 完成并提交后，下一步不是立即继续开发。

先由 ChatGPT 检查：

~~~
commit / diff
-> project layout
-> Go/cgo/C boundary
-> ownership/lifetime
-> build/test evidence
-> EAL runtime evidence
-> scope compliance
~~~

验收通过后，再共同设计 Goal 002：

> mempool + virtual PMD/port + single RXQ/TXQ + 最小 RTC forwarding。


## 12. 实现与验收记录（2026-09-19）

本次只实现 Goal 001：Go CLI、thin cgo wrapper、项目 C API、真实 EAL
init/info/cleanup、Makefile、只读环境检查，以及 ownership/lifecycle 文档。
关键线程、argv、错误转换和 cleanup 边界均有代码注释。
未实现第 7 节列出的任何后续 dataplane 功能；没有性能测试或性能结论。

### 环境与构建修正

验证环境：Linux amd64 / Ubuntu 20.04，Go 1.13.8，GCC 9.4.0，
pkg-config 0.29.1，系统 DPDK 19.11.14。环境检查实际报告 8 个可用 CPU、
1 个 NUMA node，HugePages_Total=0；Clang 和 numactl 未安装，NUMA 信息从
sysfs 读取。只验证软件环境中的 EAL 生命周期，不代表真实 NIC 或 NUMA 性能。

初始环境缺少构建依赖，安装系统开发包后验证。首次 `make build` 和
`go test ./...` 真实失败，原因是 Go 1.13 的 cgo 拒绝 pkg-config 输出的
`-include rte_config.h`。修正为仅允许这两个 token：Makefile 设置
`CGO_CFLAGS_ALLOW`，直接 Go 命令使用下面的 export；修正后全部验收通过。
没有修改系统 DPDK 文件，也没有放宽为允许任意编译参数。

### 实际执行的验收命令与结果

以下命令在仓库根目录执行：

```sh
export CGO_CFLAGS_ALLOW='-include|rte_config.h'
git status --short
go test ./...
go vet ./...
bash -n scripts/check_env.sh
bash -n scripts/start_codex_tmux.sh
make build
./scripts/check_env.sh
```

结果：修正后的 build、test、vet、两个 Bash 语法检查、环境检查均退出 0。
普通 `go test ./...` 运行 NUL 参数拒绝测试；真实 EAL 集成测试默认跳过，
通过下面的显式命令另外运行。`start_codex_tmux.sh` 仅做语法检查，没有执行
其安装或配置操作。提交前 `git status --short` 显示本次预期变更。

```sh
EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
./bin/flow-router -- --lcores="0@${EAL_CPU}" --no-huge --no-pci --no-shconf -m 64
FLOW_ROUTER_TEST_CPU="$EAL_CPU" go test -count=1 -v ./control/dataplane
```

两条命令均退出 0。实际 CLI 关键输出：

```text
DPDK version: DPDK 19.11.14
EAL init succeeded: initialized=true main_lcore=0 lcore_count=1
EAL cleanup succeeded
```

EAL 自身报告探测到 8 个 lcore、1 个 NUMA node；应用只启用 1 个 logical
lcore。DPDK 的 VFIO 支持探测日志不表示执行了设备绑定；此处禁用了 PCI
probe，没有绑定网卡、修改网络配置或 HugePage 数量。
集成测试使用独立进程，真实调用 EAL：成功路径与非法参数路径均通过，
并确认两种情况下同一进程再次初始化都会被拒绝；NUL 拒绝测试也通过。

额外执行重复启动和负向检查：

```sh
for attempt in 1 2 3; do
    ./bin/flow-router -- --lcores="0@${EAL_CPU}" --no-huge --no-pci --no-shconf -m 64 || exit 1
done
./bin/flow-router -- --lcores="0@${EAL_CPU}" --no-huge --no-pci --no-shconf -m 64 --flow-router-invalid-eal-option
test "$?" -eq 1
PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= make build
test "$?" -eq 2
```

3 次独立启动均 init/cleanup 成功并退出 0。非法 EAL 参数的 CLI 退出 1，
明确报告 `EAL init: invalid argument (status -22)`。隐藏 pkg-config 搜索目录
后，`make build` 按预期退出 2，给出安装 `libdpdk-dev` 或设置
`PKG_CONFIG_PATH` 的说明；这两项是预期失败测试，退出码断言均通过。

### 边界与交付审查

- 已检查完整 diff 和 `git diff --check`。
- 源码搜索确认只有 `control/dataplane/cgo_linux.go` 导入 C。
- DPDK 调用仅在 C runtime 中，为 EAL init/info/cleanup；没有逐包 API。
- 公共 API 位于 `dataplane/include/dp_api.h`，Go 业务代码只使用 Go wrapper。
- 未修改 AGENTS.md 或现有 tmux 脚本；未提交凭据、管理地址、原始环境日志、
  二进制或大型生成文件，`bin/` 已忽略。
- README 已记录实际状态、依赖、运行方法、测试和下一步；详细 ownership、
  失败时的内存生命周期与未来 worker/rule-update 边界见 `docs/architecture.md`。

下一步仅为 ChatGPT 验收本次 focused commit，验收通过后再讨论 Goal 002。
路由/流表语义、rewrite、虚拟拓扑、queue/lcore 模型和 benchmark baseline
仍待共同设计，不属于本次已实现能力。
