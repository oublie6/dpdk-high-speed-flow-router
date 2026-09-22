# Goal 003：Ethernet / IPv4 / TCP / UDP Parser 与 Packet Metadata

日期：2026-09-23  
状态：✅ ChatGPT 验收通过

## 1. 背景与目标

Goal 001、Goal 002、Goal 002R 已验收通过。当前 dataplane 已具备：

~~~text
TAP RX -> RXQ0 -> rte_eth_rx_burst() -> single-lcore RTC
       -> rte_eth_tx_burst() -> TXQ0 -> TAP TX
~~~

但 worker 目前完全不理解 packet 内容。本 Goal 将主路径改为：

~~~text
RX burst
-> Ethernet parse
-> IPv4 parse
-> TCP / UDP parse
-> packet metadata
-> parse OK：原样 TX
-> unsupported / malformed：drop + free
~~~

本 Goal 只做 parser 与 metadata，不做 lookup、rewrite、动态规则、RSS 或多核。

## 2. 模块边界

保持 package-local native C 结构，建议新增：

~~~text
dataplane/native/
├── dp_packet.h      metadata / parse result
├── dp_parser.h      parser API
├── dp_parser.c      Ethernet/IPv4/TCP/UDP parser
└── ...              现有 runtime/port/worker/tx
~~~

要求：

- parser 全部在 C/DPDK hot path；
- 不新增项目级 libflowdp.a/.so、Meson/CMake；
- 不把 parser 放到 Go 或 binding_linux.go；
- 禁止逐包跨 cgo；
- parser 不修改 packet，也不负责 free mbuf。

## 3. Packet Metadata

建议结构至少包含：

~~~c
struct dp_packet_meta {
    uint16_t ether_type;
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint8_t  l4_proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    uint16_t l4_offset;
};
~~~

可以小幅调整字段，但不要加入 route result、flow action、next hop、NAT/conntrack state 或 C pointer。

EtherType、IPv4 address、TCP/UDP port 在 metadata 中统一保存为 host byte order，显式使用 DPDK endian helper。后续 lookup 不允许再猜字段字节序。

## 4. Parse Result

建议定义：

~~~c
enum dp_parse_result {
    DP_PARSE_OK = 0,
    DP_PARSE_UNSUPPORTED,
    DP_PARSE_MALFORMED,
};
~~~

语义：

- OK：Ethernet -> IPv4 -> TCP/UDP 且长度/header 合法；
- UNSUPPORTED：非 IPv4、非 TCP/UDP、IPv4 fragment、multi-segment 等当前不支持但 packet 不一定错误；
- MALFORMED：header 截断、version/IHL/length/data offset 等字段非法。

## 5. Ethernet

- 只支持 Ethernet II；
- 先验证 Ethernet header 长度；
- 读取 EtherType；
- 0x0800 才进入 IPv4 parser；
- 其他 EtherType 返回 UNSUPPORTED；
- 本 Goal 不做 VLAN/QinQ/MPLS/ARP/IPv6。

## 6. IPv4

必须解析并验证：version、IHL、total_length、fragment flags/offset、protocol、src/dst address。

### 6.1 IHL

不能假设 IPv4 header 永远 20 bytes：

~~~text
ihl_words = version_ihl & 0x0f
ihl_bytes = ihl_words * 4
ihl_words >= 5
~~~

支持合法 IHL > 5，但不解析 option 内容，只正确跳过。

### 6.2 total_length

必须保证：

~~~text
total_length >= ihl_bytes
实际 packet 长度 >= Ethernet header + total_length
~~~

Ethernet padding 允许存在，因此实际 frame 可以大于 IPv4 total_length。

### 6.3 fragment

本 Goal 不做 IP reassembly。任何 MF=1 或 fragment offset != 0 都返回 UNSUPPORTED，不尝试解析 TCP/UDP。

### 6.4 checksum

本 Goal 不验证 IPv4/TCP/UDP checksum，避免 parser correctness 与后续 offload 语义混在一起。

## 7. TCP

- IPv4 payload 至少包含最小 TCP header；
- 读取 src/dst port；
- 读取 data offset；
- data offset >= 5；
- TCP header length 不得超过 IPv4 payload；
- data offset > 5 时正确计算 header length，但不解析 option 语义；
- 不做 TCP state、stream reassembly、checksum。

## 8. UDP

- IPv4 payload 至少包含 8-byte UDP header；
- 读取 src/dst port；
- UDP length >= 8；
- UDP length 不得超过 IPv4 payload；
- 不做 UDP checksum 或 L7 payload parser。

## 9. Multi-segment 策略

当前明确采用：

~~~text
nb_segs == 1  -> 正常 parse
nb_segs > 1   -> DP_PARSE_UNSUPPORTED -> drop + free
~~~

本 Goal 不引入 rte_pktmbuf_read()、linearize、跨 segment copy 或 scatter/gather parser。

## 10. Parser API

推荐：

~~~c
enum dp_parse_result
dp_parse_packet(struct rte_mbuf *mbuf, struct dp_packet_meta *meta);
~~~

要求：hot path 无 heap allocation、无锁、无逐包日志；parser 失败不 free mbuf，ownership 始终由 worker 管理；只有 DP_PARSE_OK 时 metadata 保证可消费。

## 11. Worker 集成与 ownership

Goal 002：

~~~text
RX burst -> TX burst
~~~

Goal 003：

~~~text
RX burst
-> stats.rx += n
-> 逐包 parse
   OK          -> compact 到待发送数组
   UNSUPPORTED -> free + drop
   MALFORMED   -> free + drop
-> 对 parse OK packet 执行一次 TX burst
-> TX short return 继续使用现有 ownership 规则
~~~

优先复用 RX pointer array 做 in-place compaction，不要为 burst 额外 heap allocation。

重要：Goal 002 的 dp_complete_tx() 当前会增加 rx 统计。进入 parser 后必须把 rx 统计移到 RX burst 返回之后，否则 parser 提前 drop 的 packet 会漏计。

TX helper 只负责 TX accepted/unsent 与 unsent free；parser drop 在 worker 路径 free，禁止 double free。

## 12. Stats

新增：

~~~text
parse_ok
parse_unsupported
parse_malformed
~~~

保留：rx、tx_accepted、tx_unsent、drop。

当前 Goal 应满足：

~~~text
rx = parse_ok + parse_unsupported + parse_malformed
parse_ok = tx_accepted + tx_unsent
drop = parse_unsupported + parse_malformed + tx_unsent
~~~

## 13. 单元测试

必须使用 deterministic fixtures，至少覆盖：

1. 正常 Ethernet + IPv4 + TCP；
2. 正常 Ethernet + IPv4 + UDP；
3. IPv4 IHL > 5；
4. TCP data offset > 5；
5. 非 IPv4 EtherType；
6. IPv4 protocol 非 TCP/UDP；
7. IPv4 fragment；
8. Ethernet truncated；
9. IPv4 truncated；
10. IPv4 version != 4；
11. IPv4 IHL < 5；
12. IPv4 total_length 非法；
13. TCP truncated；
14. TCP data offset < 5；
15. TCP header length 超出 IPv4 payload；
16. UDP truncated；
17. UDP length < 8；
18. UDP length 超出 IPv4 payload；
19. multi-segment mbuf。

valid case 必须检查 metadata 值、host byte order、offset/header length；failure case 必须证明不越界、不崩溃。

允许 package-private test C helper，但不得暴露为生产 control-plane API。

## 14. TAP 端到端回归

Goal 002 使用 EtherType 0x88b5 marker；Goal 003 后该包会成为 UNSUPPORTED，因此必须更新 fixture。

### 正向 frame

构造合法：

~~~text
Ethernet -> IPv4 -> UDP -> payload: dpdk-flow-router-goal003
~~~

固定 src/dst IP 与 UDP port。TX TAP 必须捕获到完整、字节完全一致的 frame，证明 packet 真正经过 parser 后仍原样转发。

### 负向 frame

再注入 deterministic malformed IPv4 frame，例如 version=4 但 IHL=4。该 frame 不应转发，最终 stats 至少证明 parse_malformed >= 1。

TAP link-up 可能产生 background frame，因此不要求 rx 恰好为固定值，以 exact Goal003 marker + parser stats 守恒为验收依据。

## 15. CLI / 输出

最终 stats 至少输出：

~~~text
rx parse_ok parse_unsupported parse_malformed
tx_accepted tx_unsent drop
~~~

hot path 不打印 packet 日志。

## 16. 明确非目标

本 Goal 不实现：VLAN/QinQ、ARP、IPv6、IP reassembly、checksum validation、TCP state/reassembly、route/flow table、ACL、可配置 DROP/FORWARD action、rewrite、NAT、conntrack、RCU/QSBR、动态规则、RSS、multi-queue、multi-lcore、rte_ring pipeline、REST/gRPC、Web、real NIC/VFIO、benchmark 优化。

注意：parser failure 的 drop 只是 error policy，不是未来可配置 DROP action。

## 17. 可读性要求

- parser 拆成小而直接的函数；
- 不把 Ethernet/IPv4/TCP/UDP 全塞进一个巨型函数；
- 不用宏隐藏控制流；
- offset/length 校验必须容易读懂；
- 不为了几个 cycles 在没有 benchmark 前牺牲明显可读性；
- 中文注释说明边界和原因。

## 18. 验收标准

开始前必须：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

构建与基础检查：

~~~bash
git diff --check
make build
make test
make vet
bash -n scripts/install_dpdk.sh
bash -n scripts/check_env.sh
bash -n scripts/verify_tap_forwarding.sh
python3 -m py_compile scripts/verify_tap_forwarding.py
~~~

还必须验证：

- 第 13 节 parser fixtures 全部通过；
- Goal 002/002R 的 EAL、partial TX ownership、teardown failure、cleanup guard 回归通过；
- scripts/verify_tap_forwarding.sh 捕获 exact Goal003 IPv4/UDP marker；
- parse_malformed >= 1；
- stats 三条守恒关系成立；
- ports_closed=2、pool_in_use=0、pool_freed=true、EAL cleanup succeeded；
- 不遗留 flow-router、dfrx*/dftx* TAP 或临时文件；
- diff 中没有 Goal 004+ 的 route/flow/rewrite/RSS 等功能。

## 19. 文档与交付

完成后：

- 更新 README.md；
- 更新 docs/architecture.md；
- 在本 Goal 记录真实实现、测试和 TAP 证据；
- 状态改为：✅ Codex 已完成，待 ChatGPT 验收；
- 不改写 Goal 002/002R 历史；
- 完成后停止，不进入 Goal 004。

建议 focused commit：

~~~text
dataplane: add IPv4 TCP UDP packet parser
~~~

最终报告：commit SHA、修改文件、metadata 结构、parse result、multi-segment 策略、parser tests、stats ownership 调整、Goal002/002R 回归、TAP Goal003 exact marker/malformed drop、未解决问题。

---

## 20. Codex 实现记录（2026-09-22）

### 20.1 同步与范围

修改前依次执行：

~~~bash
git status --short
git branch --show-current
git fetch origin
git pull --ff-only origin main
~~~

工作区为空、分支为 `main`；远端从 `0a3fd00` 快进到 `551bccb`，没有 divergence
或 conflict。同步后完整阅读 AGENTS、README、架构、Goal 002、Goal 002R 和本 Goal。
本次没有加入 lookup、route/flow table、rewrite、RSS、multi-queue、multi-lcore 或
其他 Goal 004+ 能力。

### 20.2 Parser 与 metadata

新增 `dp_packet.h`、`dp_parser.h`、`dp_parser.c`。最终 metadata 为：

~~~c
struct dp_packet_meta {
    uint16_t ether_type;
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint8_t l4_proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    uint16_t l4_offset;
};
~~~

EtherType、IPv4 address、TCP/UDP port 均通过 DPDK endian helper 转为 host byte
order。parse result 使用 `DP_PARSE_OK`、`DP_PARSE_UNSUPPORTED`、
`DP_PARSE_MALFORMED` 三态。

parser 拆为 packet/Ethernet、IPv4、TCP、UDP 小函数。IPv4 IHL 与 TCP data offset
按字段计算；IPv4 total length 同时检查不得小于 IHL 且不得超出实际连续 frame。
IPv4 fragment、非 IPv4、非 TCP/UDP 和 multi-segment 返回 unsupported；截断、
非法 version/IHL/total length/TCP offset/UDP length 返回 malformed。不验证 checksum。

本阶段只支持 `nb_segs == 1`。parser 在任何 header 解引用前先拒绝 multi-segment，
并用 single segment `data_len` 限制所有 offset/length；没有使用
`rte_pktmbuf_read()`、linearize 或跨 segment copy。parser 不修改、不 free mbuf，
hot path 没有 heap allocation、锁或逐包日志。

### 20.3 Worker、stats 与 ownership

worker 在 RX burst 后立即执行 `stats.rx += n`，逐包 parse；OK mbuf 复用 RX pointer
array 原地压缩后进入一次 TX burst，unsupported/malformed mbuf 由 worker 立即 free
并增加对应分类与 drop。`dp_complete_tx()` 不再增加 RX，只处理 TX accepted/unsent、
unsent free 与 drop，因此 parser drop 不会从 RX 统计中消失，也不会 double free。

stats 增加 `parse_ok`、`parse_unsupported`、`parse_malformed`，CLI 同步输出这些字段。
partial TX 测试也改为按 Goal 003 语义预置 RX/parse_ok，再验证 TX 未接受尾部 ownership
及三条守恒中的 RX 分类和 parse_ok/TX 关系。

### 20.4 Deterministic parser tests

新增 package-private `dp_parser_test.c` fixture helper，不需要 EAL 或 mempool，不暴露为
生产 control-plane API。实际测试覆盖：valid TCP、valid UDP、IPv4 IHL > 5、TCP
data offset > 5、非 IPv4、unsupported L4、IPv4 fragment、Ethernet truncated、IPv4
truncated、非法 IPv4 version、IHL < 5、total length < IHL、total length 超出 frame、
TCP truncated、TCP offset < 5、TCP header 超出 IPv4 payload、UDP truncated、UDP
length < 8、UDP length 超出 IPv4 payload 和 multi-segment，共 20 个 fixture。

四个 valid case 对完整 metadata 做断言，包括固定 src/dst IPv4、src/dst port、
EtherType、L4 protocol、header length、L4 offset 和 host byte order。

### 20.5 Goal003 TAP 回归

验证器先注入一个 `version=4, IHL=4` 的 60-byte malformed IPv4 frame，再注入固定
地址/端口、payload 为 `dpdk-flow-router-goal003` 的 66-byte Ethernet/IPv4/UDP
frame。TX TAP 对合法 frame 做完整逐字节比较，并把捕获到 malformed frame 视为失败。

一次真实运行的关键证据为：

~~~text
exact marker captured: EtherType=0x0800 IPv4/UDP marker=dpdk-flow-router-goal003 frame_bytes=66
stats: rx=3 parse_ok=1 parse_unsupported=1 parse_malformed=1 tx_accepted=1 tx_unsent=0 drop=2
teardown: ports_closed=2 pool_in_use=0 pool_freed=true
EAL cleanup succeeded
PASS: exact Goal003 forwarding, malformed drop, stats conservation, graceful cleanup, no test interfaces/process/temp files remain
~~~

其中一个 unsupported packet 来自 TAP link-up 背景流量。结果满足三条守恒关系，
并证明 deterministic malformed frame 被 drop；脚本没有把 RX 固定为某个值。

### 20.6 实际验收记录

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

CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
  ./dataplane/native -run '^TestPacketParser'

EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
FLOW_ROUTER_TEST_CPU="$EAL_CPU" \
  CGO_CFLAGS_ALLOW='^(-include|rte_config\.h|-mrtm)$' go test -count=1 -v \
    ./control/dataplane ./dataplane/native
~~~

真实 EAL 回归确认 init/info/cleanup、invalid EAL args 和同进程 repeated init rejection
通过；partial TX ownership、Teardown failure skips Cleanup、C cleanup worker/port/mempool
guard 全部通过。DPDK 精确为 25.11.3。TAP 结束后无 `flow-router`、`dfrx*` / `dftx*`
或 `dfr-goal003-*` 残留。当前没有已知未解决问题；下一步只等待 ChatGPT 验收。


---

## 21. ChatGPT 验收结论（2026-09-23）

验收 commit：

~~~text
4c58e566f0104bb7c411a750719047173a026d87
dataplane: add IPv4 TCP UDP packet parser
~~~

验收结果：**通过。**

确认：

- parser 仅位于 C/DPDK hot path，没有逐包跨 cgo；
- metadata 字段保持小而明确，EtherType、IPv4 地址和 TCP/UDP 端口统一为 host byte order；
- Ethernet、IPv4、TCP、UDP 长度检查顺序正确，IHL 和 TCP data offset 均按报文字段计算；
- IPv4 fragment、非 IPv4、非 TCP/UDP、multi-segment 明确返回 unsupported；
- truncated/invalid version/IHL/total length/TCP offset/UDP length 明确返回 malformed；
- parser 不修改、不释放 mbuf，ownership 仍由 worker 管理；
- worker 在 RX burst 后立即计 RX，parse failure 立即 free，parse OK 原地 compact 后只做一次 TX burst；
- dp_complete_tx() 不再增加 RX，只管理 TX accepted/unsent ownership；
- 三条 stats 守恒关系与 partial TX ownership 保持一致；
- 20 个 deterministic parser fixture 覆盖 Goal 003 约定的核心 case；
- Goal003 TAP 正向 IPv4/UDP exact marker 与 malformed drop 均有实际运行证据；
- Goal 002/002R 的 EAL、partial TX、teardown/cleanup guard 回归证据仍通过；
- 本 commit 未加入 route/flow lookup、rewrite、RSS/multi-queue 等 Goal 004+ 功能。

当前实现仍只证明 software/TAP 环境下的功能正确性，不代表真实 NIC、hardware RSS、NUMA 或 line-rate 性能。

Goal 003 正式完成，可以进入 Goal 004 的设计阶段。
