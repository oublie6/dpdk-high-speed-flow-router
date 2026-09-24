# Goal 004-005：Static Lookup + DROP / FORWARD / REWRITE

日期：2026-09-24  
状态：⬜ 待 Codex 实现

## 1. 背景

Goal 003 已验收通过。为了压缩开发周期，本 Goal 合并原计划 Goal 004（route / exact flow lookup）与 Goal 005（DROP / FORWARD / REWRITE）。

目标路径：

~~~text
RX burst
-> parse
-> exact 5-tuple flow lookup
-> flow miss 时 IPv4 LPM route lookup
-> action
   ├─ DROP
   ├─ FORWARD
   └─ REWRITE
-> TX burst
~~~

本 Goal 仍只做 software/TAP 功能验证。

## 2. Lookup 语义

固定优先级：

~~~text
1. exact flow
2. flow miss -> IPv4 LPM route
3. route miss -> default DROP
~~~

Flow hit 后不得继续查 route。

## 3. Exact Flow

使用 DPDK rte_hash。key 为固定 5-tuple，推荐：

~~~c
struct dp_flow_key {
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t l4_proto;
    uint8_t reserved[3];
};
~~~

要求：

- 所有字段沿用 Goal 003 metadata 的 host byte order；
- reserved/padding 必须显式清零，禁止未初始化字节参与 hash；
- 支持 TCP/UDP exact match；
- 不做 wildcard/range/ACL；
- hot path 无分配、无锁。

## 4. IPv4 Route

使用 DPDK rte_lpm。route key 为 IPv4 prefix + prefix length，lookup 使用 meta.dst_ipv4。

必须测试 longest-prefix：同时存在 10.0.0.0/8 与 10.1.2.0/24 时，10.1.2.99 必须命中 /24。

## 5. Action Store

flow 和 route 统一引用 immutable action：

~~~c
enum dp_action_type {
    DP_ACTION_DROP = 0,
    DP_ACTION_FORWARD,
    DP_ACTION_REWRITE,
};
~~~

建议 flow -> action pointer/index，route -> action index。action store 在 Run 期间不可修改，因此本 Goal 不引入 RCU/QSBR。

## 6. 启动前静态规则

新增启动参数：

~~~text
--rules-file <json>
~~~

只在启动前加载一次。建议 JSON：

~~~json
{
  "flows": [
    {
      "src_ipv4": "192.0.2.1",
      "dst_ipv4": "198.51.100.2",
      "src_port": 12345,
      "dst_port": 23456,
      "protocol": "udp",
      "action": {
        "type": "rewrite",
        "dst_ipv4": "203.0.113.9",
        "dst_port": 34567
      }
    }
  ],
  "routes": [
    {
      "prefix": "198.51.100.0/24",
      "action": {"type": "forward"}
    }
  ]
}
~~~

要求：

- Go 负责 JSON 和 IPv4/CIDR 校验，native 不解析 JSON；
- 使用 net/netip 或等价标准库；
- Go -> C 只发布已经规范化的数值 snapshot；
- Run 开始后禁止 Add/Delete/Replace；
- 不做 file watch/hot reload；
- 没有 rules file 时允许空规则集，parse OK packet 因 lookup miss 默认 DROP。

## 7. Lifecycle

建议：

~~~text
native.Init
-> native.GetInfo
-> native.ConfigureRules(static snapshot)
-> native.Setup
-> native.Run
-> native.Teardown
-> native.Cleanup
~~~

ConfigureRules/Setup 中途失败也必须由 Teardown 清理已创建成功的 flow table、LPM、action store 等资源。

dp_runtime_cleanup() guard 需要补充：flow table、route table、action store 任一仍存活时返回 EBUSY，不得进入 rte_eal_cleanup。

## 8. REWRITE 范围

只做基础 L3/L4 rewrite：

~~~text
src IPv4
dst IPv4
src TCP/UDP port
dst TCP/UDP port
~~~

建议用 mask 标记字段：REWRITE_SRC_IPV4 / REWRITE_DST_IPV4 / REWRITE_SRC_PORT / REWRITE_DST_PORT。

未设置字段必须保持不变；REWRITE 至少包含一个字段。

本 Goal 不做 MAC rewrite、TTL decrement、DSCP、VLAN、payload rewrite、NAT state、conntrack。

## 9. Checksum

任何 L3/L4 rewrite 后必须软件重算：

1. IPv4 header checksum；
2. TCP checksum；
3. UDP checksum。

优先使用 rte_ipv4_cksum() 与 rte_ipv4_udptcp_cksum()，调用前把对应 checksum 字段清零。

本 Goal 不依赖 TX checksum offload。

## 10. Worker 主路径

~~~text
RX
-> parse
-> parse failure -> free/drop
-> exact flow
   ├─ hit -> action
   └─ miss -> route LPM
              ├─ hit -> action
              └─ miss -> default DROP
-> execute action
   ├─ DROP    -> free
   ├─ FORWARD -> TX candidate
   └─ REWRITE -> in-place rewrite/checksum -> TX candidate
-> 原地 compact
-> one TX burst
-> existing partial-TX ownership
~~~

仍保持 single-lcore、single RXQ/TXQ、RTC、zero-retry，不使用 rte_ring，不逐包跨 cgo。

## 11. Stats

保留：rx / parse_ok / parse_unsupported / parse_malformed / tx_accepted / tx_unsent / drop。

新增：

~~~text
flow_hit
route_hit
lookup_miss
action_drop
action_forward
action_rewrite
~~~

必须满足：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = flow_hit + route_hit + lookup_miss
parse_ok = action_drop + action_forward + action_rewrite
action_forward + action_rewrite = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + action_drop + tx_unsent
~~~

lookup miss 使用 default DROP，因此同时增加 lookup_miss、action_drop、drop。

## 12. 单元测试

必须至少覆盖：

### Flow
- TCP exact hit；
- UDP exact hit；
- src port / dst port / protocol 任一不同都 miss；
- padding/reserved 清零后 key 稳定。

### Route
- /24 hit；
- /8 hit；
- /24 与 /8 同时存在时 longest-prefix 选择 /24；
- route miss；
- 如实现 /0，则覆盖 default route。

### Precedence
- 同一 packet：flow 命中 DROP，route 同时命中 FORWARD，最终必须 DROP。

### Action
- DROP ownership 正确；
- FORWARD 完整 packet 不变；
- UDP REWRITE；
- TCP REWRITE；
- 只改一个字段时其他字段保持；
- rewrite 后 IPv4 checksum 正确；
- rewrite 后 TCP/UDP checksum 正确。

### Config
- 合法 JSON；
- 非 IPv4 地址；
- 非法 CIDR；
- 非 tcp/udp protocol；
- 非法 action；
- rewrite 无字段；
- duplicate flow；
- duplicate route prefix/depth；
- 超过 table capacity。

无效配置必须在 Run 前失败。

## 13. TAP E2E

verify_tap_forwarding.py 在临时目录生成 rules JSON，并至少注入：

### A：Flow DROP
exact flow -> DROP，同时 dst IP 匹配 route FORWARD。不得从 TX 出现，用来证明 flow precedence。

### B：Flow REWRITE
exact flow -> REWRITE，至少修改 dst IPv4 + dst UDP port。payload marker 保持；TX packet 必须匹配预期，并验证 IPv4/UDP checksum。

### C：Route FORWARD
flow miss -> /24 route hit -> FORWARD，完整 frame 字节不变。

### D：Lookup Miss
flow miss + route miss -> default DROP，不得从 TX 出现。

### E：Goal003 regression
继续注入 malformed IPv4，必须 parse_malformed/drop。

TAP background packet 允许存在，不允许写死 raw RX 数量；最终检查全部 stats 守恒。

## 14. 模块边界

建议新增：

~~~text
dataplane/native/dp_lookup.c/.h
dataplane/native/dp_action.c/.h
control/dataplane/rules.go
~~~

具体文件可以调整，但 parser != lookup != action != binding。不要把 lookup/action 全塞进 dp_worker.c。

## 15. Teardown

worker 停止之后才允许销毁它可能读取的 flow/LPM/action 资源。所有新增资源必须显式释放，正常路径不能依赖进程退出兜底。

## 16. 非目标

本 Goal 不做：runtime Add/Delete、hot reload、RCU/QSBR、multi-writer hash、wildcard ACL、neighbor/ARP、MAC rewrite、IPv6、NAT/conntrack、multi-port、multi-queue/RSS、multi-lcore、benchmark、REST/gRPC、Web。

FORWARD/REWRITE 当前仍发送到现有唯一 TX port。

## 17. 可读性

- 不提前做 bulk hash lookup；
- parser/lookup/action 分层；
- flow key 显式清零；
- ownership/lifecycle 可见；
- Go/C static snapshot 转换留在 thin binding；
- hot path 不逐包日志；
- 清晰 > 可解释 > 可测试 > 微优化。

## 18. 验收

开始前：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

基础命令：

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

同时继续运行 Goal002/002R/003 的 EAL、partial TX、cleanup guard、parser 回归。

验收结果必须证明：

~~~text
flow exact hit
route LPM longest-prefix hit
flow precedence over route
lookup miss default drop
DROP works
FORWARD unchanged
UDP REWRITE + checksum valid
TCP REWRITE + checksum valid
stats conservation PASS
partial TX ownership PASS
Goal003 parser regression PASS
ports_closed=2
pool_in_use=0
pool_freed=true
lookup/action resources freed
EAL cleanup succeeded
~~~

不得遗留 flow-router、TAP、temporary rules JSON 或 lookup/action allocation。

## 19. 交付

完成后更新 README、docs/architecture.md 和本 Goal 实现记录；状态改为“✅ Codex 已完成，待 ChatGPT 验收”；创建 focused commit；不要开始后续 Goal。

建议 commit：

~~~text
dataplane: add static lookup and packet actions
~~~

最终报告：commit SHA、flow key、LPM 设计、precedence、action 结构、JSON static config、checksum 策略、lifecycle、unit tests、TAP E2E、Goal002/003 回归和未解决问题。