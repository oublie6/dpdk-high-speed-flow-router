#include "dp_action.h"
#include "dp_internal.h"
#include "dp_lookup.h"
#include "dp_parser.h"
#include "dp_test.h"
#include "dp_tx.h"
#include "dp_worker.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#define TEST_L2_LEN 14
#define TEST_IPV4_LEN 20
#define TEST_PAYLOAD_LEN 12

static const uint8_t test_payload[TEST_PAYLOAD_LEN] =
    {'s', 't', 'a', 't', 'i', 'c', '-', 'r', 'u', 'l', 'e', 's'};

static struct dp_rule_action
test_action(uint8_t type, uint8_t mask, uint32_t src_ipv4,
            uint32_t dst_ipv4, uint16_t src_port, uint16_t dst_port)
{
    struct dp_rule_action action = {0};
    action.type = type;
    action.rewrite_mask = mask;
    action.src_ipv4 = src_ipv4;
    action.dst_ipv4 = dst_ipv4;
    action.src_port = src_port;
    action.dst_port = dst_port;
    return action;
}

static void
test_set_flow(struct dp_flow_rule *rule, uint32_t src_ipv4, uint32_t dst_ipv4,
              uint16_t src_port, uint16_t dst_port, uint8_t protocol,
              struct dp_rule_action action)
{
    memset(rule, 0, sizeof(*rule));
    rule->src_ipv4 = src_ipv4;
    rule->dst_ipv4 = dst_ipv4;
    rule->src_port = src_port;
    rule->dst_port = dst_port;
    rule->l4_proto = protocol;
    rule->action = action;
}

static struct rte_mbuf *
test_build_packet(struct rte_mempool *pool, uint8_t protocol,
                  uint32_t src_ipv4, uint32_t dst_ipv4,
                  uint16_t src_port, uint16_t dst_port)
{
    uint16_t l4_len = protocol == IPPROTO_UDP ? sizeof(struct rte_udp_hdr) :
                                                sizeof(struct rte_tcp_hdr);
    uint16_t frame_len = TEST_L2_LEN + TEST_IPV4_LEN + l4_len + TEST_PAYLOAD_LEN;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    struct rte_ether_hdr *ethernet;
    struct rte_ipv4_hdr *ipv4;
    uint8_t *data;
    void *l4;

    if (!mbuf)
        return NULL;
    data = (uint8_t *)rte_pktmbuf_append(mbuf, frame_len);
    if (!data) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    memset(data, 0, frame_len);
    ethernet = (struct rte_ether_hdr *)data;
    ethernet->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    ipv4 = (struct rte_ipv4_hdr *)(data + TEST_L2_LEN);
    ipv4->version_ihl = 0x45;
    ipv4->total_length = rte_cpu_to_be_16(TEST_IPV4_LEN + l4_len +
                                           TEST_PAYLOAD_LEN);
    ipv4->packet_id = rte_cpu_to_be_16(0x4505);
    ipv4->time_to_live = 64;
    ipv4->next_proto_id = protocol;
    ipv4->src_addr = rte_cpu_to_be_32(src_ipv4);
    ipv4->dst_addr = rte_cpu_to_be_32(dst_ipv4);
    l4 = data + TEST_L2_LEN + TEST_IPV4_LEN;
    if (protocol == IPPROTO_UDP) {
        struct rte_udp_hdr *udp = l4;
        udp->src_port = rte_cpu_to_be_16(src_port);
        udp->dst_port = rte_cpu_to_be_16(dst_port);
        udp->dgram_len = rte_cpu_to_be_16(l4_len + TEST_PAYLOAD_LEN);
        memcpy((uint8_t *)l4 + l4_len, test_payload, TEST_PAYLOAD_LEN);
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4, udp);
    } else {
        struct rte_tcp_hdr *tcp = l4;
        tcp->src_port = rte_cpu_to_be_16(src_port);
        tcp->dst_port = rte_cpu_to_be_16(dst_port);
        tcp->data_off = 0x50;
        memcpy((uint8_t *)l4 + l4_len, test_payload, TEST_PAYLOAD_LEN);
        tcp->cksum = rte_ipv4_udptcp_cksum(ipv4, tcp);
    }
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
    return mbuf;
}

static int
test_lookup_semantics(void)
{
    struct dp_packet_meta base = {
        .src_ipv4 = 0xc0000201,
        .dst_ipv4 = 0xcb00710a,
        .src_port = 12345,
        .dst_port = 443,
        .l4_proto = IPPROTO_TCP,
    };
    struct dp_packet_meta meta;
    struct dp_flow_key first;
    struct dp_flow_key second;
    const struct dp_rule_action *action;

    memset(&first, 0xaa, sizeof(first));
    memset(&second, 0x55, sizeof(second));
    dp_flow_key_from_meta(&base, &first);
    dp_flow_key_from_meta(&base, &second);
    if (memcmp(&first, &second, sizeof(first)) != 0 || first.reserved[0] != 0 ||
        first.reserved[1] != 0 || first.reserved[2] != 0)
        return -EIO;

    if (dp_lookup_packet(&base, dp_rules_active_load(), &action) != DP_LOOKUP_FLOW_HIT ||
        action->type != DP_ACTION_FORWARD)
        return -EIO;
    meta = base;
    meta.src_ipv4++;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;
    meta = base;
    meta.dst_ipv4++;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;
    meta = base;
    meta.src_port++;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;
    meta = base;
    meta.dst_port++;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;
    meta = base;
    meta.l4_proto = IPPROTO_UDP;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;

    meta = base;
    meta.src_ipv4 = 0xc0000202;
    meta.dst_ipv4 = 0xcb007114;
    meta.src_port = 3000;
    meta.dst_port = 4000;
    meta.l4_proto = IPPROTO_UDP;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_FLOW_HIT ||
        action->type != DP_ACTION_REWRITE)
        return -EIO;

    meta.dst_ipv4 = 0x0a090807;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_ROUTE_HIT ||
        action->type != DP_ACTION_DROP)
        return -EIO;
    meta.dst_ipv4 = 0x0a010263;
    meta.src_port = 9999;
    meta.dst_port = 9998;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_ROUTE_HIT ||
        action->type != DP_ACTION_FORWARD)
        return -EIO;

    /* exact flow DROP 与 /24 FORWARD 同时匹配时，必须在 flow hit 后停止。 */
    meta.src_ipv4 = 0xc0000201;
    meta.dst_ipv4 = 0x0a010263;
    meta.src_port = 1000;
    meta.dst_port = 2000;
    meta.l4_proto = IPPROTO_UDP;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_FLOW_HIT ||
        action->type != DP_ACTION_DROP)
        return -EIO;
    meta.dst_ipv4 = 0xc6336401;
    if (dp_lookup_packet(&meta, dp_rules_active_load(), &action) != DP_LOOKUP_MISS)
        return -EIO;
    return 0;
}

static int
test_rewrite_packet(struct rte_mbuf *mbuf, uint8_t protocol,
                    uint32_t src_ipv4, uint32_t dst_ipv4,
                    uint16_t src_port, uint16_t dst_port)
{
    uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(data + TEST_L2_LEN);
    void *l4 = data + TEST_L2_LEN + TEST_IPV4_LEN;

    if (rte_be_to_cpu_32(ipv4->src_addr) != src_ipv4 ||
        rte_be_to_cpu_32(ipv4->dst_addr) != dst_ipv4 ||
        rte_ipv4_udptcp_cksum_verify(ipv4, l4) != 0 ||
        memcmp((uint8_t *)l4 + (protocol == IPPROTO_UDP ?
               sizeof(struct rte_udp_hdr) : sizeof(struct rte_tcp_hdr)),
               test_payload, TEST_PAYLOAD_LEN) != 0)
        return -EIO;
    if (protocol == IPPROTO_UDP) {
        struct rte_udp_hdr *udp = l4;
        if (rte_be_to_cpu_16(udp->src_port) != src_port ||
            rte_be_to_cpu_16(udp->dst_port) != dst_port)
            return -EIO;
    } else {
        struct rte_tcp_hdr *tcp = l4;
        if (rte_be_to_cpu_16(tcp->src_port) != src_port ||
            rte_be_to_cpu_16(tcp->dst_port) != dst_port)
            return -EIO;
    }

    {
        rte_be16_t checksum = ipv4->hdr_checksum;
        ipv4->hdr_checksum = 0;
        if (rte_ipv4_cksum(ipv4) != checksum)
            return -EIO;
        ipv4->hdr_checksum = checksum;
    }
    return 0;
}

static int
test_packet_actions(struct rte_mempool *pool)
{
    struct rte_mbuf *candidates[4];
    struct dp_packet_stats stats = {0};
    struct rte_mbuf *mbuf;
    uint8_t before[128];
    uint16_t before_len;
    uint16_t candidate_count = 0;

#define PROCESS_PACKET(packet)                                                     \
    do {                                                                           \
        stats.rx++;                                                                \
        struct rte_mbuf *processed = dp_process_packet((packet), &stats, dp_rules_active_load());          \
        if (processed)                                                             \
            candidates[candidate_count++] = processed;                             \
    } while (0)

    mbuf = test_build_packet(pool, IPPROTO_UDP, 0xc0000201, 0x0a010263,
                             1000, 2000);
    if (!mbuf)
        return -ENOMEM;
    PROCESS_PACKET(mbuf);
    if (rte_mempool_in_use_count(pool) != 0)
        return -EIO;

    mbuf = test_build_packet(pool, IPPROTO_TCP, 0xc0000201, 0xcb00710a,
                             12345, 443);
    if (!mbuf)
        return -ENOMEM;
    before_len = rte_pktmbuf_pkt_len(mbuf);
    memcpy(before, rte_pktmbuf_mtod(mbuf, uint8_t *), before_len);
    PROCESS_PACKET(mbuf);
    if (candidate_count != 1 ||
        memcmp(before, rte_pktmbuf_mtod(candidates[0], uint8_t *), before_len) != 0)
        return -EIO;

    mbuf = test_build_packet(pool, IPPROTO_UDP, 0xc0000202, 0xcb007114,
                             3000, 4000);
    if (!mbuf)
        return -ENOMEM;
    PROCESS_PACKET(mbuf);
    if (candidate_count != 2 ||
        test_rewrite_packet(candidates[1], IPPROTO_UDP, 0xc0000216,
                            0xcb007163, 3001, 4001) < 0)
        return -EIO;

    mbuf = test_build_packet(pool, IPPROTO_TCP, 0xc0000203, 0xcb00711e,
                             5000, 6000);
    if (!mbuf)
        return -ENOMEM;
    PROCESS_PACKET(mbuf);
    if (candidate_count != 3 ||
        test_rewrite_packet(candidates[2], IPPROTO_TCP, 0xc0000203,
                            0xcb00711e, 5000, 6001) < 0)
        return -EIO;

    mbuf = test_build_packet(pool, IPPROTO_UDP, 0xc0000204, 0x0a010232,
                             7000, 8000);
    if (!mbuf)
        return -ENOMEM;
    before_len = rte_pktmbuf_pkt_len(mbuf);
    memcpy(before, rte_pktmbuf_mtod(mbuf, uint8_t *), before_len);
    PROCESS_PACKET(mbuf);
    if (candidate_count != 4 ||
        memcmp(before, rte_pktmbuf_mtod(candidates[3], uint8_t *), before_len) != 0)
        return -EIO;

    mbuf = test_build_packet(pool, IPPROTO_UDP, 0xc0000205, 0xc6336401,
                             9000, 9001);
    if (!mbuf)
        return -ENOMEM;
    PROCESS_PACKET(mbuf);

    dp_complete_tx(candidates, candidate_count, candidate_count, &stats);
    if (stats.rx != 6 || stats.parse_ok != 6 || stats.flow_hit != 4 ||
        stats.route_hit != 1 || stats.lookup_miss != 1 ||
        stats.action_drop != 2 || stats.action_forward != 2 ||
        stats.action_rewrite != 2 || stats.tx_accepted != 4 ||
        stats.tx_unsent != 0 || stats.drop != 2 ||
        stats.rx != stats.parse_ok + stats.parse_unsupported +
                    stats.parse_malformed ||
        stats.parse_ok != stats.flow_hit + stats.route_hit + stats.lookup_miss ||
        stats.parse_ok != stats.action_drop + stats.action_forward +
                          stats.action_rewrite ||
        stats.action_forward + stats.action_rewrite !=
            stats.tx_accepted + stats.tx_unsent ||
        stats.drop != stats.parse_unsupported + stats.parse_malformed +
                      stats.action_drop + stats.tx_unsent)
        return -EIO;

    /* accepted ownership 在测试中由模拟 PMD 最终释放。 */
    for (uint16_t i = 0; i < candidate_count; i++)
        rte_pktmbuf_free(candidates[i]);
    if (rte_mempool_in_use_count(pool) != 0)
        return -EIO;
    return 0;
#undef PROCESS_PACKET
}

int
dp_test_static_lookup_actions(void)
{
    struct dp_flow_rule flows[4];
    struct dp_flow_rule invalid_flow;
    struct dp_route_rule routes[2] = {0};
    struct rte_mempool *pool;
    int ret;

    if (!dp.initialized || dp.running || dp.pool || dp.rules_configured)
        return -EBUSY;

    /* builder 遇到非法 rule 时必须立即清掉 partial snapshot，active generation
     * 保持为空；QSBR 由统一 teardown 回收。
     */
    test_set_flow(&invalid_flow, 1, 2, 3, 4, IPPROTO_ICMP,
                  test_action(DP_ACTION_DROP, 0, 0, 0, 0, 0));
    ret = dp_configure_rules(&invalid_flow, 1, NULL, 0);
    if (ret != -EINVAL || dp_rules_active_load() || !dp.rules_qsbr)
        return -EIO;
    dp_rules_teardown();
    if (dp_rules_active_load() || dp.rules_qsbr || dp.rules_configured)
        return -EIO;
    dp.stats.flow_table_freed = 0;
    dp.stats.action_store_freed = 0;

    test_set_flow(&flows[0], 0xc0000201, 0xcb00710a, 12345, 443,
                  IPPROTO_TCP, test_action(DP_ACTION_FORWARD, 0, 0, 0, 0, 0));
    test_set_flow(&flows[1], 0xc0000201, 0x0a010263, 1000, 2000,
                  IPPROTO_UDP, test_action(DP_ACTION_DROP, 0, 0, 0, 0, 0));
    test_set_flow(&flows[2], 0xc0000202, 0xcb007114, 3000, 4000,
                  IPPROTO_UDP,
                  test_action(DP_ACTION_REWRITE,
                              DP_REWRITE_SRC_IPV4 | DP_REWRITE_DST_IPV4 |
                              DP_REWRITE_SRC_PORT | DP_REWRITE_DST_PORT,
                              0xc0000216, 0xcb007163, 3001, 4001));
    test_set_flow(&flows[3], 0xc0000203, 0xcb00711e, 5000, 6000,
                  IPPROTO_TCP,
                  test_action(DP_ACTION_REWRITE, DP_REWRITE_DST_PORT,
                              0, 0, 0, 6001));
    routes[0].prefix = 0x0a000000;
    routes[0].depth = 8;
    routes[0].action = test_action(DP_ACTION_DROP, 0, 0, 0, 0, 0);
    routes[1].prefix = 0x0a010200;
    routes[1].depth = 24;
    routes[1].action = test_action(DP_ACTION_FORWARD, 0, 0, 0, 0, 0);

    ret = dp_configure_rules(flows, 4, routes, 2);
    if (ret < 0)
        goto out_rules;
    if (dp_configure_rules(NULL, 0, NULL, 0) != -EALREADY) {
        ret = -EIO;
        goto out_rules;
    }
    ret = test_lookup_semantics();
    if (ret < 0)
        goto out_rules;

    pool = rte_pktmbuf_pool_create("dp_lookup_action_test", 255, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE,
                                   rte_socket_id());
    if (!pool) {
        ret = -ENOMEM;
        goto out_rules;
    }
    ret = test_packet_actions(pool);
    if (rte_mempool_in_use_count(pool) != 0 && ret == 0)
        ret = -EIO;
    /* failure case 由进程退出兜底，避免错误路径重复 free 未知 ownership。 */
    if (rte_mempool_in_use_count(pool) == 0)
        rte_mempool_free(pool);

out_rules:
    dp_rules_teardown();
    if (ret == 0 && (dp_rules_active_load() || dp.rules_qsbr ||
                     dp.rules_configured || !dp.stats.flow_table_freed ||
                     !dp.stats.route_table_freed ||
                     !dp.stats.action_store_freed ||
                     !dp.stats.snapshot_freed || !dp.stats.qsbr_freed))
        ret = -EIO;
    return ret;
}

struct test_publish_context {
    const struct dp_flow_rule *flows;
    uint32_t flow_count;
    const struct dp_route_rule *routes;
    uint32_t route_count;
    uint64_t generation;
    int result;
    atomic_bool done;
};

struct test_idle_reader_context {
    atomic_bool stop;
    unsigned int reader_id;
};

static void *
test_publish_thread(void *argument)
{
    struct test_publish_context *context = argument;

    context->result = dp_publish_rules(
        context->flows, context->flow_count,
        context->routes, context->route_count, &context->generation);
    atomic_store_explicit(&context->done, true, memory_order_release);
    return NULL;
}

static void *
test_idle_reader_thread(void *argument)
{
    struct test_idle_reader_context *context = argument;

    /* 模拟 worker 的 RX=0 分支：没有 packet 可处理时仍持续报告 quiescent。 */
    while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
        dp_rules_reader_quiescent(context->reader_id);
        rte_pause();
    }
    return NULL;
}

int
dp_test_dynamic_rules_qsbr(void)
{
    const struct dp_rule_action drop =
        test_action(DP_ACTION_DROP, 0, 0, 0, 0, 0);
    const struct dp_rule_action forward =
        test_action(DP_ACTION_FORWARD, 0, 0, 0, 0, 0);
    const struct dp_rule_action rewrite =
        test_action(DP_ACTION_REWRITE, DP_REWRITE_DST_PORT,
                    0, 0, 0, 9000);
    struct dp_flow_rule initial;
    struct dp_flow_rule updated;
    struct dp_flow_rule invalid;
    struct dp_packet_meta meta = {
        .src_ipv4 = 0xc0000201,
        .dst_ipv4 = 0xc6336402,
        .src_port = 1234,
        .dst_port = 5678,
        .l4_proto = IPPROTO_UDP,
    };
    struct test_publish_context context = {.done = ATOMIC_VAR_INIT(false)};
    struct test_idle_reader_context idle[2] = {
        {.stop = ATOMIC_VAR_INIT(false), .reader_id = 0},
        {.stop = ATOMIC_VAR_INIT(false), .reader_id = 1},
    };
    const struct dp_rule_action *found;
    struct dp_rule_snapshot *active;
    pthread_t writer;
    pthread_t idle_readers[2];
    uint64_t generation;
    uint64_t reclaimed_before;
    int ret = 0;

    if (!dp.initialized || dp.running || dp.pool || dp.rules_configured)
        return -EBUSY;
    test_set_flow(&initial, meta.src_ipv4, meta.dst_ipv4,
                  meta.src_port, meta.dst_port, meta.l4_proto, drop);
    test_set_flow(&updated, meta.src_ipv4, meta.dst_ipv4,
                  meta.src_port, meta.dst_port, meta.l4_proto, rewrite);
    test_set_flow(&invalid, meta.src_ipv4, meta.dst_ipv4,
                  meta.src_port, meta.dst_port, IPPROTO_ICMP, forward);

    ret = dp_configure_rules(&initial, 1, NULL, 0);
    if (ret < 0)
        goto out;
    active = dp_rules_active_load();
    if (!active || active->generation != 1 ||
        dp_lookup_packet(&meta, active, &found) != DP_LOOKUP_FLOW_HIT ||
        found->type != DP_ACTION_DROP) {
        ret = -EIO;
        goto out;
    }

    dp.ready = true;
    ret = dp_rules_reader_register(0);
    if (ret < 0)
        goto out;
    ret = dp_rules_reader_register(1);
    if (ret < 0) {
        dp_rules_reader_unregister(0);
        goto out;
    }
    context.flows = &updated;
    context.flow_count = 1;
    reclaimed_before = dp.stats.rules_reclaimed;
    if (pthread_create(&writer, NULL, test_publish_thread, &context) != 0) {
        ret = -EIO;
        dp_rules_reader_unregister(1);
        dp_rules_reader_unregister(0);
        goto out;
    }

    /* writer 应先完成 pointer exchange，再被尚未 quiescent 的 reader 阻塞。 */
    for (uint32_t spin = 0; spin < 100000000; spin++) {
        active = dp_rules_active_load();
        if (active && active->generation == 2)
            break;
        rte_pause();
    }
    active = dp_rules_active_load();
    if (!active || active->generation != 2 ||
        dp_lookup_packet(&meta, active, &found) != DP_LOOKUP_FLOW_HIT ||
        found->type != DP_ACTION_REWRITE ||
        dp.stats.rules_reclaimed != reclaimed_before ||
        atomic_load_explicit(&context.done, memory_order_acquire)) {
        ret = -EIO;
    }
    /* reader0 单独 quiescent 后，reader1 仍持有旧 generation；writer 必须
     * 继续阻塞，不能把“任意 reader 已推进”误当成完整 grace period。
     */
    dp_rules_reader_quiescent(0);
    for (uint32_t spin = 0; spin < 1000000; spin++)
        rte_pause();
    if (dp.stats.rules_reclaimed != reclaimed_before ||
        atomic_load_explicit(&context.done, memory_order_acquire))
        ret = -EIO;
    dp_rules_reader_quiescent(1);
    if (pthread_join(writer, NULL) != 0)
        ret = -EIO;
    if (ret == 0 && (context.result != 0 || context.generation != 2 ||
                     dp.stats.rules_reclaimed != reclaimed_before + 1))
        ret = -EIO;
    if (ret < 0)
        goto out_reader;

    /* reader 保持 online，但只执行 RX=0 quiescent；同步 publish 必须能够完成。 */
    updated.action = forward;
    if (pthread_create(&idle_readers[0], NULL, test_idle_reader_thread,
                       &idle[0]) != 0) {
        ret = -EIO;
        goto out_reader;
    }
    if (pthread_create(&idle_readers[1], NULL, test_idle_reader_thread,
                       &idle[1]) != 0) {
        atomic_store_explicit(&idle[0].stop, true, memory_order_release);
        (void)pthread_join(idle_readers[0], NULL);
        ret = -EIO;
        goto out_reader;
    }
    ret = dp_publish_rules(&updated, 1, NULL, 0, &generation);
    for (unsigned int i = 0; i < 2; i++)
        atomic_store_explicit(&idle[i].stop, true, memory_order_release);
    for (unsigned int i = 0; i < 2; i++) {
        if (pthread_join(idle_readers[i], NULL) != 0)
            ret = -EIO;
    }
    if (ret == 0 && (generation != 3 || dp.stats.rules_reclaimed != 2))
        ret = -EIO;

out_reader:
    dp_rules_reader_unregister(1);
    dp_rules_reader_unregister(0);
    if (ret < 0)
        goto out;

    active = dp_rules_active_load();
    if (dp_publish_rules(&invalid, 1, NULL, 0, &generation) != -EINVAL ||
        dp_rules_active_load() != active || active->generation != 3 ||
        dp.stats.rules_generation != 3 ||
        dp_lookup_packet(&meta, active, &found) != DP_LOOKUP_FLOW_HIT ||
        found->type != DP_ACTION_FORWARD) {
        ret = -EIO;
        goto out;
    }

    /* reader 已 offline；50 次同步 publish 应立即完成并逐代回收。 */
    for (uint32_t i = 0; i < 50; i++) {
        updated.action = (i & 1U) == 0 ? forward : rewrite;
        ret = dp_publish_rules(&updated, 1, NULL, 0, &generation);
        if (ret < 0)
            goto out;
    }
    active = dp_rules_active_load();
    if (!active || generation != 53 || active->generation != 53 ||
        dp.stats.rules_generation != 53 ||
        dp.stats.rules_publish_success != 53 ||
        dp.stats.rules_publish_failed != 1 ||
        dp.stats.rules_reclaimed != 52)
        ret = -EIO;

out:
    dp.ready = false;
    dp_rules_teardown();
    if (ret == 0 && (dp_rules_active_load() || dp.rules_qsbr ||
                     !dp.stats.snapshot_freed || !dp.stats.qsbr_freed))
        ret = -EIO;
    return ret;
}
