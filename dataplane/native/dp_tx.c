#include "dp_internal.h"
#include "dp_test.h"
#include "dp_tx.h"

#include <errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

void
dp_complete_tx(struct rte_mbuf **pkts, uint16_t n, uint16_t sent,
               struct dp_stats *stats)
{
    stats->tx_accepted += sent;
    stats->tx_unsent += n - sent;
    stats->drop += n - sent;
    for (uint16_t i = sent; i < n; i++)
        rte_pktmbuf_free(pkts[i]);
}

int
dp_test_tx_partial_ownership(void)
{
    struct rte_mempool *pool;
    struct rte_mbuf *pkts[4];
    struct dp_stats stats = {
        .rx = 4,
        .parse_ok = 4,
        .flow_hit = 4,
        .action_forward = 4,
    };
    int ret = 0;

    if (!dp.initialized || dp.running || dp.pool)
        return -EBUSY;
    pool = rte_pktmbuf_pool_create("dp_partial_tx_test", 63, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE,
                                   rte_socket_id());
    if (!pool)
        return -ENOMEM;
    if (rte_pktmbuf_alloc_bulk(pool, pkts, 4) != 0) {
        rte_mempool_free(pool);
        return -ENOMEM;
    }

    /* 模拟 PMD 只接受前两个 mbuf。helper 必须只释放未接受的尾部。 */
    dp_complete_tx(pkts, 4, 2, &stats);
    if (stats.rx != 4 || stats.parse_ok != 4 || stats.flow_hit != 4 ||
        stats.action_forward != 4 || stats.tx_accepted != 2 ||
        stats.tx_unsent != 2 || stats.drop != 2 ||
        stats.rx != stats.parse_ok + stats.parse_unsupported +
                    stats.parse_malformed ||
        stats.parse_ok != stats.flow_hit + stats.route_hit + stats.lookup_miss ||
        stats.parse_ok != stats.action_drop + stats.action_forward +
                          stats.action_rewrite ||
        stats.action_forward + stats.action_rewrite !=
            stats.tx_accepted + stats.tx_unsent ||
        stats.drop != stats.parse_unsupported + stats.parse_malformed +
                      stats.action_drop + stats.tx_unsent ||
        rte_mempool_in_use_count(pool) != 2)
        ret = -EIO;

    /* 测试端模拟 PMD 最终释放已接受的两个 mbuf；此处也证明它们未被重复释放。 */
    rte_pktmbuf_free(pkts[0]);
    rte_pktmbuf_free(pkts[1]);
    if (rte_mempool_in_use_count(pool) != 0)
        ret = -EIO;
    rte_mempool_free(pool);
    return ret;
}
