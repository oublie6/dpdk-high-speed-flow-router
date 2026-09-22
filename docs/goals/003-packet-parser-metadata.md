# Goal 003：Ethernet / IPv4 / TCP / UDP Parser 与 Packet Metadata

日期：2026-09-23  
状态：⬜ 待 Codex 实现

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