#include "dp_internal.h"
#include "dp_parser.h"
#include "dp_tx.h"
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
            struct dp_packet_meta meta;
            enum dp_parse_result result = dp_parse_packet(pkts[i], &meta);

            if (result == DP_PARSE_OK) {
                dp.stats.parse_ok++;
                /* 复用 RX 数组原地压缩，不为每个 burst 分配第二个 pointer array。 */
                pkts[tx_count++] = pkts[i];
                continue;
            }

            if (result == DP_PARSE_UNSUPPORTED)
                dp.stats.parse_unsupported++;
            else
                dp.stats.parse_malformed++;
            dp.stats.drop++;
            rte_pktmbuf_free(pkts[i]);
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
