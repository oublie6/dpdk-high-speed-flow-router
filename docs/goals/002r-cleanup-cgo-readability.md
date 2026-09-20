# Goal 002R：修复 cleanup 安全边界、移除复杂 unsafe，并收敛 Go 构建环境

日期：2026-09-20  
状态：✅ Codex 已完成，待 ChatGPT 复验

## 1. 背景

Goal 002 已由 Codex 实现并提交：

~~~text
f6c02c23e79c9fb51d2698516bd90a86478536dd
dataplane: add DPDK 25.11.3 TAP RTC forwarding
~~~

ChatGPT 验收确认 Goal 002 主体方向正确，但正式通过前还需要修复三个问题：

1. \`Teardown()\` 失败后 Go owner 仍继续调用 \`EAL Cleanup()\`，破坏资源依赖顺序；
2. cgo argv 装配仍使用复杂 \`unsafe.Pointer/uintptr\` 指针运算，不符合最新 AGENTS.md 可读性约束；
3. \`scripts/install_dpdk.sh\` 使用 \`go env -w CGO_CFLAGS_ALLOW=...\` 修改用户级全局 Go 环境，不够项目隔离。

本 Goal 只修复以上三项，不新增 dataplane 功能。

---

## 2. 执行前必须同步最新代码

开始实现前必须执行：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

要求：

- 必须基于远端最新 \`main\`；
- 如果存在未提交修改，不允许覆盖或丢弃；
- 如果 \`git pull --ff-only\` 失败、出现 divergence/conflict，停止并报告；
- 禁止 \`git reset --hard\`、强制覆盖、未经确认的 rebase；
- 同步成功后重新阅读最新的 \`AGENTS.md\`、\`README.md\`、\`docs/architecture.md\`、Goal 002 主文档和本文件。

---

# 3. 修复一：Teardown 失败后禁止继续 EAL Cleanup

## 3.1 当前问题

当前 Go owner 的语义是：

~~~text
Teardown
-> 即使失败
-> GetStats
-> 仍然 Cleanup
~~~

但 C teardown 已经明确采用：

~~~text
port close 失败
或 pool_in_use != 0
-> 不冒险继续释放
-> 保留资源到进程退出
~~~

两层语义冲突。

## 3.2 Go owner 必须修改

要求显式保存 \`teardownErr\`，保持控制流直白。

推荐语义：

~~~text
Run 返回
  ↓
Teardown
  ├─ 成功
  │    ↓
  │  GetStats
  │    ↓
  │  Cleanup
  │
  └─ 失败
       ↓
     GetStats（如果当前状态允许）
       ↓
     不调用 EAL Cleanup
       ↓
     返回错误并让进程退出兜底
~~~

不要把该逻辑压缩成复杂表达式。

## 3.3 C Cleanup 增加防御

\`dp_runtime_cleanup()\` 必须拒绝资源仍存活时进入 \`rte_eal_cleanup()\`。

至少检查：

~~~text
dp.running == true
dp.pool != NULL
dp.owned[0] == true
dp.owned[1] == true
~~~

可以根据实际 state 补充最小必要检查，但不要堆无意义条件。

目标：

> 只有 worker、port、mempool 生命周期已经安全结束，才允许 EAL cleanup。

## 3.4 测试

新增简单、可控、仅测试使用的 failure injection，证明：

~~~text
Teardown failure
-> Go 不调用 native.Cleanup()
~~~

并直接验证：

~~~text
资源仍存活
-> dp_runtime_cleanup()
-> 返回 -EBUSY 或等价明确错误
~~~

不要依赖 TAP driver 偶发 close failure 才能测试。

---

# 4. 修复二：移除 argv 装配中的复杂 unsafe 指针运算

## 4.1 当前问题

当前 \`binding_linux.go\` 中手工计算 C \`char **\` 数组元素地址：

~~~text
unsafe.Pointer
-> uintptr
-> index * sizeof(pointer)
-> cast
-> dereference
~~~

功能可以工作，但初始化路径不是 hot path，不值得牺牲可读性。

## 4.2 改造原则

把 argv array 的：

~~~text
allocate
set argv[i]
free array
~~~

封装成命名清晰的小 C helper。

推荐语义：

~~~c
char **dp_argv_alloc(size_t count);
void dp_argv_set(char **argv, size_t index, char *value);
void dp_argv_free_array(char **argv);
~~~

具体命名可调整。

Go 侧应能直接读成：

~~~text
allocate argv
set argv[i]
~~~

不得再出现：

~~~text
uintptr(pointer) + index * sizeof(pointer)
~~~

## 4.3 unsafe 允许范围

\`C.CString()\` 最终通过：

~~~go
C.free(unsafe.Pointer(p))
~~~

这种简单、局部、标准的 cgo free conversion 可以保留。

不要为了“零 unsafe”反而引入更复杂设计。

---

# 5. 修复三：不要通过 go env -w 修改用户全局 Go 环境

## 5.1 当前问题

当前安装脚本执行：

~~~bash
go env -w 'CGO_CFLAGS_ALLOW=...'
~~~

它会修改用户级 persistent Go environment，并可能影响其他 Go 项目。

## 5.2 目标

\`scripts/install_dpdk.sh\` 只负责：

- 安装/构建 DPDK 25.11.3；
- 验证 pkg-config；
- 不修改用户级 Go persistent environment。

删除 \`go env -w\`。

## 5.3 项目局部构建环境

旧 Go/cgo 需要的 \`CGO_CFLAGS_ALLOW\` 应由项目构建入口提供。

优先保持简单：

- Makefile 统一 export 项目需要的 \`CGO_CFLAGS_ALLOW\`；
- 增加 \`make test\`、\`make vet\` 等 target；
- README 推荐 \`make build / make test / make vet\`；
- 需要直接执行 Go 命令时，在命令前显式设置该环境变量。

不要为了这件事引入复杂构建系统。

## 5.4 环境检查

\`scripts/check_env.sh\` 可以输出当前 \`CGO_CFLAGS_ALLOW\` 用于诊断，但：

- 不要求它已经被写入 GOENV；
- 不自动修改它；
- 不把用户全局配置当作项目正确运行的必要条件。

---

# 6. 可读性要求

严格遵守 AGENTS.md：

- 控制流程显式；
- 非 hot path 优先可读性；
- 不使用复杂语法糖；
- 不引入深层闭包或过度抽象；
- helper 名称要体现实际语义；
- 新增注释使用中文；
- API、DPDK、Go/C 原生术语可以保留英文。

---

# 7. 明确非目标

本修复 Goal 不实现：

- Ethernet/IPv4/TCP/UDP parser；
- flow/route table；
- rewrite；
- RCU/QSBR；
- RSS/multi-queue/multi-lcore；
- 新 benchmark；
- REST/gRPC；
- frontend；
- NAT/conntrack；
- real NIC/VFIO。

不要顺手重构 Goal 002 其他已经工作的代码。

---

# 8. 验收标准

## A. 同步与 diff

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

完成后：

~~~bash
git diff --check
git status --short
~~~

## B. 可读性 / unsafe

确认：

- 不再存在 argv array 的 \`uintptr + offset\` 指针运算；
- \`unsafe\` 只保留必要、简单的 C memory free/conversion；
- argv helper 小而清晰；
- 只有 \`dataplane/native\` 使用 cgo。

## C. cleanup 安全

必须有自动测试证明：

~~~text
Teardown 失败
-> 不进入正常 EAL Cleanup
~~~

同时直接测试 C guard：

~~~text
资源未释放
-> dp_runtime_cleanup()
-> 返回 -EBUSY 或等价错误
~~~

正常路径仍必须：

~~~text
Run stop
-> Teardown
-> ports closed
-> pool free
-> EAL cleanup
~~~

## D. 构建环境

检查：

~~~bash
grep -R 'go env -w' -n scripts Makefile README.md docs || true
~~~

项目执行路径中不应再通过安装脚本写 persistent Go environment。

至少通过：

~~~bash
make build
make test
make vet
GOENV=off make build
GOENV=off make test
GOENV=off make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
~~~

如果保留直接 Go 命令验收，显式使用项目局部 env：

~~~bash
CGO_CFLAGS_ALLOW='^(-include|rte_config\\.h|-mrtm)$' go test -count=1 ./...
CGO_CFLAGS_ALLOW='^(-include|rte_config\\.h|-mrtm)$' go vet ./...
~~~

## E. Goal 002 回归

再次真实执行：

~~~bash
./scripts/check_env.sh
./scripts/verify_tap_forwarding.sh
~~~

确认：

- DPDK 仍为 25.11.3；
- exact marker 成功；
- RX/TX ownership stats 正常；
- ports_closed=2；
- pool_in_use=0；
- pool_freed=true；
- EAL cleanup 成功；
- 不遗留 TAP/process/temp files。

## F. scope

不得出现 Goal 003+ 功能。

---

# 9. 文档更新

完成后：

- 更新 \`README.md\`；
- 更新 \`docs/architecture.md\`；
- 在本文件记录真实实现和测试证据；
- 将状态改为：**✅ Codex 已完成，待 ChatGPT 复验**；
- Goal 002 主文档只补充本修复记录链接，不重写历史。

---

# 10. Codex 交付要求

完成后停止继续开发，不进入 Goal 003。

创建 focused commit。

建议 commit message：

~~~text
fix: harden Goal 002 cleanup and cgo boundary
~~~

最终报告：

- commit SHA；
- 修改文件；
- teardown failure 如何测试；
- C cleanup guard 如何验证；
- argv unsafe 如何移除；
- \`go env -w\` 如何移除；
- 全部实际验收命令和结果；
- TAP exact marker 回归结果；
- 是否还有未解决问题。

---

# 11. Codex 实现记录（2026-09-20）

## 11.1 同步结果

修改前依次执行了：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

工作区为空、分支为 `main`；远端从 `f6c02c2` 快进到 `29bbfa2`，没有 divergence
或 conflict。同步后重新完整阅读 AGENTS、README、架构、Goal 002 和本 Goal。

## 11.2 Teardown 与 Cleanup

Go owner 新增直白的 `finishLifecycle()` 顺序：显式保存 `teardownErr`，随后在状态
允许时读取 stats；只有 Teardown 成功才调用 Cleanup。Teardown 失败时保留错误并
让进程退出兜底，不再进入正常 EAL cleanup。

`TestTeardownFailureSkipsCleanup` 使用可控 fake 返回固定 teardown error，并记录
Cleanup 调用次数。测试断言错误被返回、stats 仍被保存且 Cleanup 调用次数严格为 0，
不依赖 TAP PMD 的偶发 close failure。

C 的 `dp_runtime_cleanup()` 现在检查 worker running、两个 port 的 owned/started
状态和 pool 指针，任一资源仍存活都返回 `-EBUSY`。package 内测试钩子分别临时
构造 worker、port、mempool 存活状态并直接调用该函数；
`TestRuntimeCleanupRejectsLiveResources` 的三个子测试都确认 Go error 可匹配
`syscall.EBUSY`。

## 11.3 argv 与项目构建环境

新增 `dp_binding.c/.h`，用 `dp_argv_alloc()`、`dp_argv_set()` 和
`dp_argv_free_array()` 封装 `char **` 数组布局。Go binding 现在只负责分配、逐项
设置和释放；已移除 `unsafe.Pointer -> uintptr -> offset` 运算。`unsafe` 仅保留
`C.CString()` 对应的标准 `C.free(unsafe.Pointer(p))` 转换。

安装脚本已删除用户级 persistent Go environment 写入。Makefile 统一 export 精确
`CGO_CFLAGS_ALLOW`，并新增 `make test`、`make vet`；README 推荐使用三个 make
入口，直接 Go 命令则显式传入项目局部变量。`check_env.sh` 只报告当前进程变量，
不读取或修改 GOENV。

## 11.4 实际验收记录

以下命令均实际执行并退出 0：

~~~bash
git diff --check
make build
make test
make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py

CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 ./...
CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go vet ./...
./scripts/check_env.sh

CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
  ./control/dataplane -run '^TestTeardownFailureSkipsCleanup$'
CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
  ./dataplane/native -run '^TestRuntimeCleanupRejectsLiveResources$'

EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
FLOW_ROUTER_TEST_CPU="$EAL_CPU" \
  CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
    ./control/dataplane ./dataplane/native

./scripts/verify_tap_forwarding.sh
grep -R 'go env -w' -n scripts Makefile README.md docs || true
~~~

真实 EAL 回归确认 init/info/cleanup、非法参数拒绝和同进程重复 init 限制均未回归；
partial TX ownership 测试通过。环境检查确认 DPDK 精确为 25.11.3，且未设置进程级
allowlist 时只给出诊断，不失败。关闭用户级 GOENV 后三个 make 入口也全部通过，
证明项目构建不依赖 persistent Go environment。

TAP exact marker 回归退出 0，关键结果为：

~~~text
exact marker captured: EtherType=0x88b5 marker=dpdk-flow-router-goal002 frame_bytes=60
stats: rx=2 tx_accepted=2 tx_unsent=0 drop=0
teardown: ports_closed=2 pool_in_use=0 pool_freed=true
EAL cleanup succeeded
PASS: exact TAP forwarding, graceful cleanup, no test interfaces/process/temp files remain
~~~

脚本退出后又独立检查，没有残留 `flow-router` 进程、`dfrx*` / `dftx*` TAP 或
`dfr-goal002-*` 临时目录。grep 只命中 Goal 文档对旧行为及本检查命令的历史说明；
`scripts/install_dpdk.sh`、Makefile 和 README 的当前执行流程均不再写用户级环境。

## 11.5 当前边界

本修复没有增加 parser、flow/route、rewrite、RCU/QSBR、RSS、multi-queue、
multi-lcore、API、frontend、NAT、conntrack、real NIC/VFIO 或 Goal 003 内容。
当前没有已知未解决问题；下一步仅等待 ChatGPT 复验。
