# Goal 006-007：Dynamic Rule Publication + RCU/QSBR

日期：2026-09-25  
状态：✅ ChatGPT 验收通过

## 1. 目标

Goal 004-005 已验收通过。当前 flow hash、IPv4 LPM 与 action store 只在启动前构建，Run 期间完全 immutable。

为了压缩进度，本 Goal 合并原 Goal 006（Go 动态规则管理）与 Goal 007（RCU/QSBR 热更新），一次完成：

~~~text
Go control plane
-> Add/Delete/Replace rules
-> build a brand-new immutable native snapshot
-> atomic publish
-> worker keeps lock-free reads
-> QSBR grace period
-> reclaim old snapshot
~~~

本 Goal 不在运行中的 rte_hash/rte_lpm 上原地修改；所有更新都采用 whole-snapshot replacement。

## 2. 核心架构

把 Goal 004-005 的分散全局资源收敛成一个 generation-owned snapshot：

~~~c
struct dp_rule_snapshot {
    struct rte_hash *flow_table;
    struct rte_lpm *route_table;
    struct dp_rule_action *actions;
    uint32_t action_count;
    uint64_t generation;
};
~~~

`dp_state` 保存：

~~~text
_Atomic(struct dp_rule_snapshot *) active_rules
struct rte_rcu_qsbr *rules_qsbr
writer serialization state
generation counter
~~~

flow hash 中的 action pointer 必须指向同一个 snapshot 的 action store；route action index 也只能解释到同一个 snapshot 中。

禁止让 packet 同时看到 generation N 的 flow table 与 generation N+1 的 action store。

## 3. Read Side

当前仍然只有一个 worker，但实现必须为下一 Goal 多 worker 留出自然扩展点。

初始化 QSBR 时 reader capacity 不要写死为 1，建议按 `RTE_MAX_LCORE` 或明确的项目级最大 worker 数预留。

当前 worker 使用 reader ID 0：

~~~text
register reader 0
-> online
-> loop
   -> acquire-load active_rules once per RX burst
   -> whole burst uses the same snapshot
   -> parse / lookup / action / TX
   -> quiescent
-> offline
-> unregister
~~~

非常重要：`rte_eth_rx_burst()` 返回 0 时也必须报告 quiescent，不能因为 idle traffic 让 writer 永久等不到 grace period。

当前 loop 不包含 blocking call；未来如果 reader 要进入 blocking path，必须先 offline，返回后再 online。

worker 不加 mutex，不逐包做 refcount，不逐包跨 cgo。

## 4. Atomic Publication

reader 使用 C11 atomic acquire load 获取 snapshot pointer。

writer 构建新 snapshot 完全成功后才允许发布：

~~~text
build generation N+1 completely
-> atomic exchange active_rules
-> old snapshot becomes retired
-> wait QSBR grace period
-> free old flow hash / LPM / actions / snapshot
~~~

writer 的 atomic exchange 使用 acquire/release 或等价正确内存序。

关键顺序要求：先完成 pointer publish，再开始等待后续 grace period；不能在 publish 前就把已经发生的 quiescent state 当作旧 snapshot 的安全回收证据。

当前控制面更新频率很低，可以让 publish 同步等待一个 QSBR grace period；不要求实现复杂 deferred reclamation queue。控制路径允许等待，但 packet hot path 不得等待 writer。

## 5. Snapshot Builder

把 Goal 004-005 的 flow/LPM/action 构建逻辑改为针对一个独立 snapshot 工作，不直接先写入 active global resources。

要求：

- 新 snapshot build 失败时，必须释放新 generation 的 partial resources；
- build 失败不得改变 active_rules；
- build 失败不得改变当前 generation；
- 每个 generation 的 rte_hash/rte_lpm object name 必须唯一，因为旧 generation 在 grace period 完成前仍可能存在；
- 动态 publish 可能从非 EAL Go control goroutine 进入 C，内存/table NUMA socket 必须使用初始化时保存的 `dp.info.socket_id`，不能依赖调用线程的 `rte_socket_id()`；
- flow key、LPM、action、checksum 等 Goal004-005 语义保持不变。

建议短对象名，例如：

~~~text
dfr_f_<generation>
dfr_r_<generation>
~~~

注意 DPDK object name 长度限制。

## 6. Go Dynamic Rule Manager

`Runtime` 保存一份 Go-owned 当前 RuleSnapshot，并使用 mutex 串行化 writer。

至少提供：

~~~go
func (r *Runtime) ReplaceRules(snapshot RuleSnapshot) error
func (r *Runtime) AddFlow(rule FlowRule) error
func (r *Runtime) DeleteFlow(key FlowKey) error
func (r *Runtime) AddRoute(rule RouteRule) error
func (r *Runtime) DeleteRoute(key RouteKey) error
~~~

可以小幅调整命名，但必须明确区分 key 与 rule/action。

语义：

- Add duplicate -> error；
- Delete missing -> error；
- ReplaceRules 使用现有严格 validateRules；
- 每次 mutation 都先 clone Go snapshot，在 Go 中完成变更和校验；
- 调用 native PublishRules(newSnapshot)；
- native publish 成功后才更新 Runtime 自己保存的 Go snapshot；
- native publish 失败时 Go 当前 snapshot 保持旧版本；
- 多个并发 writer 必须被串行化；
- caller 修改传入 slice/object 后不能影响已发布版本。

本 Goal 不要求高频单条规则原地更新。1024 规模下每次 CRUD 重建完整 immutable snapshot 是刻意设计。

## 7. Native API Threading Contract

Goal 004-005 的规则是 owner thread 启动前 ConfigureRules。本 Goal 新增一个明确例外：

~~~text
dp_publish_rules(...)
~~~

允许从 Go control goroutine 在 dataplane Run 期间调用。

因此 API 注释必须更新：

- request_stop：跨线程允许；
- publish_rules：跨线程允许，单 writer 序列化；
- packet worker：只读 atomic snapshot；
- 其他 lifecycle API 仍由 owner OS thread 串行调用。

C 侧也必须有防御性 writer serialization 或明确保证所有入口最终只经 Runtime writer mutex；优先让 native 边界本身不容易被误用。

## 8. QSBR Lifecycle

建议：

~~~text
EAL Init
-> create/init QSBR
-> build/publish generation 1
-> Setup
-> worker register + online
-> Run
   -> dynamic generations 2..N
-> worker offline + unregister
-> Run returns
-> detach active snapshot
-> free current snapshot
-> free QSBR
-> port/mempool teardown
-> EAL cleanup
~~~

实际 teardown 顺序可以按依赖调整，但必须保证 worker 已经不再引用 rules 后才能释放 current snapshot。

`dp_runtime_cleanup()` guard 至少增加：

~~~text
active_rules != NULL
QSBR still live
writer/update still in progress
-> EBUSY
~~~

## 9. Generation / Stats

新增至少：

~~~text
rules_generation
rules_publish_success
rules_reclaimed
rules_publish_failed   （可选但推荐）
~~~

generation 1 表示初始启动 snapshot。每次成功 Replace/Add/Delete 后 generation +1。

`rules_reclaimed` 只在旧 native snapshot 的 hash/LPM/action/snapshot 确实释放后增加。

不要在 packet hot path 为每个包增加 generation stats；worker 只按 burst 取得 snapshot。

原有 packet stats 五条守恒关系必须继续成立。

## 10. Minimal Runtime Reload Surface

为了有真实端到端动态更新证据，而又不提前做 REST/Web，本 Goal 可以为 CLI 增加显式 SIGHUP reload：

~~~text
--rules-file rules.json
Run
修改 rules.json
kill -HUP <pid>
-> Go reload + validate
-> Runtime.ReplaceRules
-> success: log one control-plane line with new generation
~~~

要求：

- 只有显式 SIGHUP 才 reload，不做 file watch；
- reload JSON/validation/publish 失败时旧 snapshot 必须继续工作；
- failure 只记录 control-plane error，不得让 packet worker 崩溃；
- SIGTERM/SIGINT 仍负责 stop；
- 不在 hot path 打日志。

如果实现者有更简单且同等可重复的真实 runtime publish E2E 入口，可以替代 SIGHUP，但禁止引入 HTTP/Web。

## 11. Deterministic Tests：Go CRUD

至少覆盖：

1. ReplaceRules valid；
2. ReplaceRules invalid leaves old Go snapshot unchanged；
3. AddFlow success；
4. AddFlow duplicate；
5. DeleteFlow success；
6. DeleteFlow missing；
7. AddRoute success；
8. AddRoute duplicate；
9. DeleteRoute success；
10. DeleteRoute missing；
11. caller 修改原始 slice 不影响 Runtime snapshot；
12. 两个并发 writer 最终串行发布，不产生部分状态。

纯 Go snapshot mutation/validation 尽量不依赖 EAL。

## 12. Deterministic Tests：QSBR / Native

必须有真实 DPDK QSBR 证据，至少证明：

1. reader register / online / quiescent / offline / unregister 完整；
2. active snapshot atomic swap 后新 lookup 立即使用新 generation；
3. reader 尚未经过 publish 之后的 quiescent state 时，旧 snapshot 不得被 free；
4. reader 报告 quiescent 后，writer 才能完成 grace period 并回收旧 snapshot；
5. idle reader（RX=0）仍报告 quiescent，publish 不会无限阻塞；
6. repeated publish 至少 50 次，不泄漏 hash/LPM/action snapshot；
7. build failure 保留旧 generation 和旧行为；
8. teardown 后 active snapshot / QSBR 均清理。

可以使用 package-private C test helper；不要把 QSBR test API 暴露给生产控制面。

## 13. Runtime Packet Behavior Test

必须证明同一 packet 在不停止 dataplane 的情况下随 generation 改变行为。

建议 TAP E2E：

~~~text
generation 1:
exact flow -> DROP
inject packet X -> TX 看不到 X

generation 2:
同一 exact flow -> REWRITE
runtime publish, 不重启 process
inject 同一个 packet X -> TX 捕获 rewritten X + checksum valid

generation 3:
删除 exact flow，保留匹配 route -> FORWARD
runtime publish, 不重启 process
inject 同一个 packet X -> TX 捕获原包 unchanged
~~~

全过程：

- 同一个 flow-router PID；
- port/mempool 不重建；
- EAL 不重新 init；
- 每次 publish 后 generation 单调增加；
- 最终 packet stats 守恒；
- stop 后旧 generation 全部已回收、current snapshot 已释放。

继续保留 Goal003 malformed regression 与 Goal004-005 lookup/rewrite 基础回归。

## 14. Failure Semantics

动态 publish 必须是 all-or-nothing：

~~~text
new snapshot build/validate fail
-> active old snapshot unchanged
-> old packet behavior unchanged
-> no partial new resources leak
~~~

publish 已经 atomic swap 后，如 grace-period wait 遇到内部错误，不能把已被 reader 看见的新 snapshot 悄悄回滚成旧 snapshot。此类异常必须安全保留 ownership 并明确返回错误；正常设计应尽量让 grace-period path 无失败分支。

## 15. Non-goals

本 Goal 不做：

- multi-queue / multi-lcore dataplane；
- RSS / RETA；
- per-lcore stats；
- benchmark/profiling；
- REST/gRPC/Web；
- database/persistence；
- file watch；
- wildcard ACL；
- NAT/conntrack；
- ARP/neighbor；
- IPv6；
- bulk lookup；
- in-place concurrent rte_hash/rte_lpm mutation；
- generic config framework。

虽然当前只有一个 worker，QSBR 设计必须能自然扩展到下一 Goal 多 reader。

## 16. 可读性

- 明确区分 snapshot builder / publisher / reader / reclaimer；
- 不把 QSBR、lookup、action 全塞进 worker；
- atomic pointer 与 memory order 写清楚；
- 注释解释为什么 quiescent 点安全；
- control path 可以有 mutex，packet path 不允许 mutex；
- 不为了少几行把 ownership/reclamation 写成难读的宏；
- 清晰 > 可解释 > 可测试 > 微优化。

## 17. 验收命令

开始前：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

同步后完整阅读：

- AGENTS.md
- README.md
- docs/architecture.md
- Goal 003
- Goal 004-005
- 本 Goal

基础：

~~~bash
git diff --check
make build
make test
make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
./scripts/check_env.sh
./scripts/verify_tap_forwarding.sh
~~~

如果新增 reload/dynamic 专用脚本，也必须做 shell/python syntax check 并实际运行。

继续完整回归 Goal002/002R/003/004-005。

## 18. 验收结果必须证明

~~~text
initial generation published
runtime generation 1 -> 2 without restart
runtime generation 2 -> 3 without restart
same packet: DROP -> REWRITE -> route FORWARD
new generation visible after publish
old generation not freed before quiescent
old generation reclaimed after grace period
idle worker does not block publish
50+ repeated publishes leak-free
invalid publish keeps old behavior
packet stats conservation PASS
Goal003 parser regression PASS
Goal004-005 lookup/action/checksum regression PASS
current snapshot freed on teardown
QSBR freed on teardown
ports/pool cleanup PASS
EAL cleanup succeeded
~~~

## 19. 交付

完成后：

- 更新 README.md；
- 更新 docs/architecture.md；
- 本 Goal 追加真实实现、测试和 runtime publish 证据；
- 状态改为 `✅ Codex 已完成，待 ChatGPT 验收`；
- 创建 focused commit；
- 停止，不进入 RSS/multi-queue/benchmark。

建议 commit：

~~~text
dataplane: add dynamic rules with QSBR
~~~

最终报告：commit SHA、snapshot 结构、atomic publish 顺序、QSBR reader/quiescent 设计、Go CRUD API、writer serialization、runtime E2E、reclamation evidence、50+ publish stress、failure semantics、旧 Goal 回归与未解决问题。

---

## 20. Codex 实现记录（2026-09-24）

### 20.1 Snapshot 与 atomic publish

native 已将原来的分散全局 `flow_table`、`route_table`、`actions` 收敛为：

~~~c
struct dp_rule_snapshot {
    struct rte_hash *flow_table;
    struct rte_lpm *route_table;
    struct dp_rule_action *actions;
    uint32_t action_count;
    uint64_t generation;
};
~~~

`dp_state.active_rules` 是 C11 atomic pointer。初始 generation 1 使用 release store；动态
publish 在 C `writer_lock` 内完整构建 N+1 后使用 `memory_order_acq_rel` exchange。worker
每个 RX burst 使用 `memory_order_acquire` load 一次，整个 burst 只使用该 pointer。

exchange 完成后才调用 `rte_rcu_qsbr_start()` 和 blocking `rte_rcu_qsbr_check()`，随后
整体释放旧代 hash、LPM、action store 与 snapshot。新旧 generation 使用
`dfr_f_<generation>`、`dfr_r_<generation>` 唯一短名称。所有 builder allocation 使用
EAL init 保存的 `dp.info.socket_id`，不依赖 publish goroutine 所在线程。

### 20.2 QSBR reader lifecycle

QSBR 按 `RTE_MAX_LCORE` reader capacity 分配，当前 worker 使用 reader ID 0：

~~~text
register -> online -> worker loop -> offline -> unregister
~~~

非空 burst 在 parse/lookup/action/TX ownership 全部结束后报告 quiescent；RX=0 的 idle
分支也报告 quiescent。packet hot path 没有 mutex、refcount、allocation 或 cgo crossing。

### 20.3 Go Runtime CRUD 与 reload

`Runtime` 实现 `ReplaceRules`、`AddFlow`、`DeleteFlow`、`AddRoute`、`DeleteRoute` 和
`RulesGeneration`。`FlowKey`/`RouteKey` 与携带 action 的 rule 明确分离。每次 mutation
都在 Go `rulesMu` 内 clone、修改、校验、调用 native publish，成功后才更新 Go-owned
current snapshot。duplicate Add、missing Delete、invalid Replace 与 native publish failure
均保持旧 Go snapshot。传入 slice 会深拷贝；两个并发 writer 由 mutex 串行化。

`dp_publish_rules()` 是除 stop 外新增的 cross-thread API，并在 C 边界再次用 mutex
防御性串行化。其他 lifecycle API 仍只允许 owner OS thread。Run 返回后 owner 先关闭
Go publish 入口，再执行 Teardown。

CLI 的 `--rules-file` 同时提供 generation 1 与显式 SIGHUP reload。reload 的 JSON 读取、
校验或 publish 失败只输出 control-plane error；worker 和旧 packet behavior 保持运行。

### 20.4 Failure、stats 与 cleanup

builder failure 会释放 partial new snapshot，不交换 active pointer，不增加 generation。
动态统计新增：

~~~text
rules_generation
rules_publish_success
rules_publish_failed
rules_reclaimed
~~~

`rules_reclaimed` 只在旧 native generation 全部 free 后增加。Teardown 在 worker
offline/unregister 后 detach 并释放 current snapshot，再释放 QSBR。cleanup guard 已覆盖
active snapshot、QSBR 与 writer/update in progress；最终输出 `snapshot_freed` 和
`qsbr_freed`。

### 20.5 Deterministic native 与 Go 测试证据

真实 DPDK native test 完成以下顺序：reader 0 register/online，普通 pthread writer 构建
并交换 generation 2；测试观察到新代 lookup 已可见，同时 publisher 尚未返回且
`rules_reclaimed` 未增加；reader quiescent 后 writer 才返回并回收 generation 1。之后
非法 protocol build 返回 `EINVAL`，active pointer、generation 与 lookup action 都保持；
随后用只执行 RX=0 quiescent 的 idle reader 线程完成一次同步 publish；reader
offline/unregister 后再连续 publish 50 次，最终：

~~~text
generation=53
publish_success=53
publish_failed=1
reclaimed=52
current snapshot freed=true
QSBR freed=true
~~~

Go deterministic tests覆盖 valid/invalid Replace、flow/route Add/Delete、duplicate/missing、
caller slice ownership、native publish failure rollback 和两个并发 writer 串行化。

### 20.6 同一 PID runtime E2E

TAP verifier 的一次真实运行输出：

~~~text
same PID generation 1 DROP -> generation 2 REWRITE + checksum -> generation 3 route FORWARD unchanged PASS; invalid reload kept generation 2 behavior PASS
stats: rx=9 parse_ok=5 parse_unsupported=3 parse_malformed=1 flow_hit=3 route_hit=1 lookup_miss=1 action_drop=2 action_forward=1 action_rewrite=2 tx_accepted=3 tx_unsent=0 drop=6
rules: generation=3 publish_success=3 publish_failed=0 reclaimed=2
teardown: ports_closed=2 pool_in_use=0 pool_freed=true flow_table_freed=true route_table_freed=true action_store_freed=true snapshot_freed=true qsbr_freed=true
EAL cleanup succeeded
PASS: Goal006-007 same-PID dynamic snapshot/QSBR publication, old-generation reclaim, regressions, graceful cleanup, no test interfaces/process/temp files remain
~~~

generation 1 DROP 与 `/24` FORWARD 同时匹配，证明 flow precedence；generation 2 对同一
原始 frame 改写 destination IPv4/UDP port，并验证 IPv4/UDP checksum；一次非法 JSON
reload 后仍保持 generation 2 rewrite；generation 3 删除 exact flow，同一 frame 经 route
逐字节不变地 FORWARD。日志中 EAL init 与 port setup 都只有一次，publish 前存在 idle
窗口，因此同时覆盖 RX=0 quiescent。

### 20.7 实际验收命令

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

./scripts/check_env.sh
./scripts/verify_tap_forwarding.sh

EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
FLOW_ROUTER_TEST_CPU="$EAL_CPU" make test
FLOW_ROUTER_TEST_CPU="$EAL_CPU" \
  CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' \
  go test -count=1 -v ./control/dataplane ./dataplane/native

gcc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdpdk) \
  -Idataplane/native -fsyntax-only \
  dataplane/native/dp_lookup.c dataplane/native/dp_worker.c \
  dataplane/native/dp_runtime.c dataplane/native/dp_port.c \
  dataplane/native/dp_lookup_action_test.c
~~~

旧 Goal 回归继续覆盖 EAL lifecycle、partial TX ownership、Teardown failure、cleanup
guard、20 个 parser fixture、malformed drop、exact hash、LPM longest-prefix、flow precedence、
DROP/FORWARD/REWRITE、TCP/UDP checksum 与 packet stats 守恒。当前没有进入 RSS、
multi-queue、multi-lcore、per-lcore stats 或 benchmark，也没有已知的本 Goal 未解决问题。


---

## 21. ChatGPT 验收结论（2026-09-25）

验收 commit：

~~~text
5813b906814215b31fbbc17ed587dacab5767d79
dataplane: add dynamic rules with QSBR
~~~

验收结果：**通过。**

确认：

- native 规则资源已收敛为 generation-owned immutable snapshot，flow hash、LPM、action store 生命周期不再拆分；
- active snapshot 使用 C11 atomic pointer，reader acquire-load，writer acq_rel exchange；
- dynamic publish 先完整 build 新 generation，再 atomic exchange，之后才启动 QSBR grace period；
- worker 每个 RX burst 只读取一次 snapshot，整个 burst 使用同一 generation；
- non-empty burst 在 lookup/action/TX ownership 完成后报告 quiescent；
- RX=0 idle 分支同样报告 quiescent，不会让低流量 dataplane 阻塞 writer；
- reader register/online/quiescent/offline/unregister 生命周期完整；
- QSBR capacity 按 RTE_MAX_LCORE 预留，当前 reader ID=0，为下一 Goal 多 worker 留有明确扩展点；
- native deterministic test 真实证明：新 pointer 已可见时 publisher 仍被旧 reader 阻塞，reader quiescent 后旧 generation 才 reclaim；
- dynamic builder 使用 EAL 初始化阶段保存的 socket_id，不依赖普通 Go publish goroutine 的当前 socket；
- generation-specific rte_hash/rte_lpm object name 避免新旧 generation 共存时名称冲突；
- build failure 释放 partial new snapshot，不改变 active pointer、generation 或旧 packet behavior；
- Go Runtime 已提供 ReplaceRules、Add/Delete Flow、Add/Delete Route，并由 mutex 串行化 writer；
- Go-owned current snapshot 只在 native publish 成功后提交，失败时保持旧 snapshot；
- caller-owned slice 会被复制，不会反向修改已发布 generation；
- native 边界再次通过 writer mutex 串行化 publish，生命周期 API threading contract 已更新；
- SIGHUP reload 在同一 PID/EAL/port/mempool 中完成 generation 1 -> 2 -> 3；
- 同一个 packet 的行为真实完成 DROP -> REWRITE -> route FORWARD，rewrite checksum 有验证；
- invalid JSON reload 不触发 native publish，旧 generation 和 packet behavior 保持；
- native stress 连续 publish 50 次后 generation/reclaim 计数闭合，没有已知 snapshot/hash/LPM/action 泄漏；
- teardown 在 worker offline/unregister 后 detach current snapshot、释放 QSBR，并有 snapshot_freed / qsbr_freed 证据；
- cleanup guard 已覆盖 active snapshot、QSBR、writer active；
- Goal002/002R/003/004-005 的 lifecycle、parser、lookup、action、checksum、partial TX 和 stats regression 均有执行记录；
- 本 commit 未进入 RSS、multi-queue、multi-lcore、per-lcore stats 或 benchmark scope。

当前实现仍然只证明 software/TAP 环境下的功能与并发回收正确性，不代表真实 NIC、hardware RSS、NUMA 或 line-rate 性能。

Goal 006-007 正式完成。下一阶段只剩 Goal 008-009：multi-queue / RSS / multi-lcore + benchmark/profiling。
