#include "dp_internal.h"
#include "dp_action.h"
#include "dp_lookup.h"
#include "dp_parser.h"
#include "dp_tx.h"
#include "dp_worker.h"
#include "dp_test.h"
#include <errno.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_mbuf.h>

static atomic_int test_launch_failure_worker = ATOMIC_VAR_INIT(-1);
static atomic_int test_worker_failure_worker = ATOMIC_VAR_INIT(-1);

void dp_test_inject_remote_launch_failure(int worker_id)
{
    atomic_store_explicit(&test_launch_failure_worker, worker_id,
                          memory_order_relaxed);
}

void dp_test_inject_worker_failure(int worker_id)
{
    atomic_store_explicit(&test_worker_failure_worker, worker_id,
                          memory_order_relaxed);
}

void dp_dataplane_request_stop(void)
{
    /* 这里只传递停止请求，不发布其他数据，因此 relaxed ordering 已足够。
     * 不在 setup 中重置，避免丢失初始化期间到达的 stop 请求。
     */
    atomic_store_explicit(&dp.stop, true, memory_order_relaxed);
}

struct rte_mbuf *
dp_process_packet(struct rte_mbuf *mbuf, struct dp_packet_stats *stats,
                  const struct dp_rule_snapshot *snapshot)
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

    lookup_result = dp_lookup_packet(&meta, snapshot, &action);
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

static int
dp_worker_run(void *argument)
{
    struct dp_worker_ctx *worker = argument;
    struct rte_mbuf *pkts[DP_BURST_SIZE];
    int ret;

    atomic_fetch_add_explicit(&dp.active_workers, 1, memory_order_relaxed);
    if (atomic_load_explicit(&test_worker_failure_worker,
                             memory_order_relaxed) ==
        (int)worker->info.worker_id) {
        ret = -EIO;
        atomic_store_explicit(&worker->result, ret, memory_order_release);
        atomic_store_explicit(&dp.stop, true, memory_order_relaxed);
        atomic_fetch_sub_explicit(&dp.active_workers, 1, memory_order_relaxed);
        return ret;
    }
    ret = dp_rules_reader_register(worker->info.worker_id);
    if (ret < 0) {
        atomic_store_explicit(&worker->result, ret, memory_order_release);
        atomic_store_explicit(&dp.stop, true, memory_order_relaxed);
        atomic_fetch_sub_explicit(&dp.active_workers, 1, memory_order_relaxed);
        return ret;
    }
    atomic_store_explicit(&worker->reader_registered, true,
                          memory_order_release);

    while (!atomic_load_explicit(&dp.stop, memory_order_relaxed)) {
        uint16_t n = rte_eth_rx_burst(dp.ports[0], worker->info.rx_queue_id,
                                      pkts, DP_BURST_SIZE);
        uint16_t tx_count = 0;
        struct dp_rule_snapshot *snapshot = dp_rules_active_load();

        worker->stats.rx_bursts++;
        if (n == 0) {
            worker->stats.rx_empty_polls++;
            /* idle 也是完整的 read-side 周期。每个 online reader 都必须推进，
             * 否则同步 publish 会一直等待低流量 queue。
             */
            dp_rules_reader_quiescent(worker->info.worker_id);
            continue;
        }

        worker->stats.rx += n;
        for (uint16_t i = 0; i < n; i++) {
            struct rte_mbuf *candidate =
                dp_process_packet(pkts[i], &worker->stats, snapshot);
            if (candidate) {
                /* 复用 RX 数组原地压缩，不为每个 burst 分配第二个 pointer array。 */
                pkts[tx_count++] = candidate;
            }
        }

        if (tx_count != 0) {
            uint16_t sent = rte_eth_tx_burst(dp.ports[1],
                                              worker->info.tx_queue_id,
                                              pkts, tx_count);
            worker->stats.tx_bursts++;
            dp_complete_tx(pkts, tx_count, sent, &worker->stats);
        }
        /* 本 burst 的全部 lookup/action 和 TX ownership 都已结束，之后不再
         * 解引用 snapshot，因此这里是安全且明确的 quiescent point。
         */
        dp_rules_reader_quiescent(worker->info.worker_id);
    }

    /* 每次 burst 都已转移或释放全部 mbuf；返回时没有 pending packet ownership。 */
    dp_rules_reader_unregister(worker->info.worker_id);
    atomic_store_explicit(&worker->reader_registered, false,
                          memory_order_release);
    atomic_store_explicit(&worker->result, 0, memory_order_release);
    atomic_fetch_sub_explicit(&dp.active_workers, 1, memory_order_relaxed);
    return 0;
}

int dp_dataplane_run(void)
{
    unsigned int launched = 0;
    int first_error = 0;

    if (!dp.ready)
        return -ENODEV;
    if (dp.ran)
        return -EALREADY;
    dp.ran = true;
    dp.running = true;

    /* main lcore 留给 worker0；其余 worker 先 remote launch。若中途失败，
     * 统一 stop 并 join 已成功启动者，绝不把 remote lcore 留给 cleanup。
     */
    for (unsigned int i = 1; i < dp.worker_count; i++) {
        int ret;

        if (atomic_load_explicit(&test_launch_failure_worker,
                                 memory_order_relaxed) == (int)i)
            ret = -EIO;
        else
            ret = rte_eal_remote_launch(dp_worker_run, &dp.workers[i],
                                        dp.workers[i].info.lcore_id);
        if (ret != 0) {
            first_error = ret < 0 ? ret : -EIO;
            atomic_store_explicit(&dp.stop, true, memory_order_relaxed);
            break;
        }
        atomic_store_explicit(&dp.workers[i].launched, true,
                              memory_order_release);
        launched++;
    }

    if (first_error == 0) {
        int ret;

        atomic_store_explicit(&dp.workers[0].launched, true,
                              memory_order_release);
        ret = dp_worker_run(&dp.workers[0]);
        atomic_store_explicit(&dp.workers[0].launched, false,
                              memory_order_release);
        if (ret < 0)
            first_error = ret;
    }

    for (unsigned int i = 1; i <= launched; i++) {
        int ret = rte_eal_wait_lcore(dp.workers[i].info.lcore_id);

        atomic_store_explicit(&dp.workers[i].launched, false,
                              memory_order_release);
        if (ret < 0 && first_error == 0)
            first_error = ret;
    }
    dp.running = false;
    return first_error;
}
