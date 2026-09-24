#include "dp_internal.h"
#include "dp_action.h"
#include "dp_lookup.h"
#include "dp_parser.h"
#include "dp_tx.h"
#include "dp_worker.h"
#include <errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

void dp_dataplane_request_stop(void)
{
    /* 这里只传递停止请求，不发布其他数据，因此 relaxed ordering 已足够。
     * 不在 setup 中重置，避免丢失初始化期间到达的 stop 请求。
     */
    atomic_store_explicit(&dp.stop, true, memory_order_relaxed);
}

struct rte_mbuf *
dp_process_packet(struct rte_mbuf *mbuf, struct dp_stats *stats)
{
    struct dp_packet_meta meta;
    const struct dp_rule_action *action;
    enum dp_parse_result parse_result;
    enum dp_lookup_result lookup_result;

    parse_result = dp_parse_packet(mbuf, &meta);
    if (parse_result != DP_PARSE_OK) {
        if (parse_result == DP_PARSE_UNSUPPORTED)
            stats->parse_unsupported++;
        else
            stats->parse_malformed++;
        stats->drop++;
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    stats->parse_ok++;

    lookup_result = dp_lookup_packet(&meta, &action);
    if (lookup_result == DP_LOOKUP_FLOW_HIT)
        stats->flow_hit++;
    else if (lookup_result == DP_LOOKUP_ROUTE_HIT)
        stats->route_hit++;
    else {
        /* lookup miss 的唯一隐式策略是 default DROP。 */
        stats->lookup_miss++;
        stats->action_drop++;
        stats->drop++;
        rte_pktmbuf_free(mbuf);
        return NULL;
    }

    if (action->type == DP_ACTION_DROP) {
        stats->action_drop++;
        stats->drop++;
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    if (action->type == DP_ACTION_FORWARD) {
        stats->action_forward++;
        return mbuf;
    }
    if (action->type == DP_ACTION_REWRITE &&
        dp_apply_action(mbuf, &meta, action) == 0) {
        stats->action_rewrite++;
        return mbuf;
    }

    /* 配置阶段已拒绝非法 action；保留防御分支以确保异常时 ownership 不泄漏。 */
    stats->action_drop++;
    stats->drop++;
    rte_pktmbuf_free(mbuf);
    return NULL;
}

int dp_dataplane_run(void)
{
    struct rte_mbuf *pkts[DP_BURST_SIZE];
    if (!dp.ready)
        return -ENODEV;
    if (dp.ran)
        return -EALREADY;
    dp.ran = dp.running = true;
    while (!atomic_load_explicit(&dp.stop, memory_order_relaxed)) {
        uint16_t n = rte_eth_rx_burst(dp.ports[0], 0, pkts, DP_BURST_SIZE);
        uint16_t tx_count = 0;
        if (n == 0)
            continue;

        dp.stats.rx += n;
        for (uint16_t i = 0; i < n; i++) {
            struct rte_mbuf *candidate = dp_process_packet(pkts[i], &dp.stats);
            if (candidate) {
                /* 复用 RX 数组原地压缩，不为每个 burst 分配第二个 pointer array。 */
                pkts[tx_count++] = candidate;
            }
        }

        if (tx_count != 0) {
            uint16_t sent = rte_eth_tx_burst(dp.ports[1], 0, pkts, tx_count);
            dp_complete_tx(pkts, tx_count, sent, &dp.stats);
        }
    }
    /* 每次 burst 都已转移或释放全部 mbuf；返回时没有 pending packet ownership。 */
    dp.running = false;
    return 0;
}
