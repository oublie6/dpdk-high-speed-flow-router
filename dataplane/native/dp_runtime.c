#include "dp_internal.h"
#include "dp_test.h"
#include <errno.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_version.h>

/* 明确锁定当前基线；不再维护 19.11 compatibility branch。 */
#if RTE_VER_YEAR != 25 || RTE_VER_MONTH != 11 || RTE_VER_MINOR != 3
#error "This project requires DPDK 25.11.3 headers"
#endif
struct dp_state dp = {
    .stop = ATOMIC_VAR_INIT(false),
    .active_rules = ATOMIC_VAR_INIT(NULL),
    .writer_lock = PTHREAD_MUTEX_INITIALIZER,
    .writer_active = ATOMIC_VAR_INIT(false),
};

bool dp_workers_stopped(void)
{
    if (atomic_load_explicit(&dp.active_workers, memory_order_acquire) != 0 ||
        atomic_load_explicit(&dp.registered_readers, memory_order_acquire) != 0)
        return false;
    for (unsigned int i = 0; i < dp.worker_count; i++) {
        if (atomic_load_explicit(&dp.workers[i].launched,
                                 memory_order_acquire) ||
            atomic_load_explicit(&dp.workers[i].reader_registered,
                                 memory_order_acquire))
            return false;
    }
    return true;
}

static void
dp_packet_stats_add(struct dp_packet_stats *total,
                    const struct dp_packet_stats *local)
{
    total->rx += local->rx;
    total->parse_ok += local->parse_ok;
    total->parse_unsupported += local->parse_unsupported;
    total->parse_malformed += local->parse_malformed;
    total->flow_hit += local->flow_hit;
    total->route_hit += local->route_hit;
    total->lookup_miss += local->lookup_miss;
    total->action_drop += local->action_drop;
    total->action_forward += local->action_forward;
    total->action_rewrite += local->action_rewrite;
    total->tx_accepted += local->tx_accepted;
    total->tx_unsent += local->tx_unsent;
    total->drop += local->drop;
    total->rx_bursts += local->rx_bursts;
    total->rx_empty_polls += local->rx_empty_polls;
    total->tx_bursts += local->tx_bursts;
}

int dp_runtime_init(int argc, char **argv)
{
    if (dp.attempted)
        return -EALREADY;
    if (argc < 1 || !argv)
        return -EINVAL;
    dp.attempted = true;
    /* 同时检查动态链接到的库，防止头文件新、运行库旧。 */
    if (strcmp(rte_version(), "DPDK 25.11.3") != 0)
        return -ENOTSUP;
    if (rte_eal_init(argc, argv) < 0)
        return -(rte_errno ? rte_errno : EIO);
    dp.initialized = true;
    dp.info.initialized = 1;
    dp.info.main_lcore = rte_get_main_lcore();
    dp.info.lcore_count = rte_lcore_count();
    dp.info.version = rte_version();
    /* 后续 publish 可能来自普通 Go goroutine，builder 必须使用这里保存的
     * EAL socket，而不能依赖调用线程的 rte_socket_id()。
     */
    dp.info.socket_id = rte_socket_id();
    return 0;
}

int dp_runtime_get_info(struct dp_runtime_info *info)
{
    if (!info)
        return -EINVAL;
    if (!dp.initialized)
        return -ENODEV;
    *info = dp.info;
    return 0;
}

int dp_dataplane_get_stats(struct dp_stats *stats)
{
    struct dp_packet_stats total = {0};

    if (!stats)
        return -EINVAL;
    if (dp.running)
        return -EBUSY;
    *stats = dp.stats;
    stats->rx = 0;
    stats->parse_ok = 0;
    stats->parse_unsupported = 0;
    stats->parse_malformed = 0;
    stats->flow_hit = 0;
    stats->route_hit = 0;
    stats->lookup_miss = 0;
    stats->action_drop = 0;
    stats->action_forward = 0;
    stats->action_rewrite = 0;
    stats->tx_accepted = 0;
    stats->tx_unsent = 0;
    stats->drop = 0;
    stats->rx_bursts = 0;
    stats->rx_empty_polls = 0;
    stats->tx_bursts = 0;
    stats->worker_count = dp.worker_count;
    for (unsigned int i = 0; i < dp.worker_count; i++) {
        const struct dp_packet_stats *local = &dp.workers[i].stats;

        stats->workers[i].info = dp.workers[i].info;
        stats->workers[i].packets = *local;
        dp_packet_stats_add(&total, local);
    }
    stats->rx = total.rx;
    stats->parse_ok = total.parse_ok;
    stats->parse_unsupported = total.parse_unsupported;
    stats->parse_malformed = total.parse_malformed;
    stats->flow_hit = total.flow_hit;
    stats->route_hit = total.route_hit;
    stats->lookup_miss = total.lookup_miss;
    stats->action_drop = total.action_drop;
    stats->action_forward = total.action_forward;
    stats->action_rewrite = total.action_rewrite;
    stats->tx_accepted = total.tx_accepted;
    stats->tx_unsent = total.tx_unsent;
    stats->drop = total.drop;
    stats->rx_bursts = total.rx_bursts;
    stats->rx_empty_polls = total.rx_empty_polls;
    stats->tx_bursts = total.tx_bursts;
    return 0;
}

int dp_test_worker_stats_layout(void)
{
    struct dp_worker_ctx contexts[2];
    struct dp_packet_stats total = {0};

    memset(contexts, 0, sizeof(contexts));
    if ((uintptr_t)&contexts[0] % RTE_CACHE_LINE_SIZE != 0 ||
        (uintptr_t)&contexts[1] - (uintptr_t)&contexts[0] < RTE_CACHE_LINE_SIZE)
        return -EIO;
    contexts[0].stats.rx = 4;
    contexts[0].stats.parse_ok = 4;
    contexts[0].stats.flow_hit = 4;
    contexts[0].stats.action_forward = 4;
    contexts[0].stats.tx_accepted = 4;
    contexts[0].stats.rx_bursts = 10;
    contexts[0].stats.rx_empty_polls = 8;
    contexts[0].stats.tx_bursts = 4;
    contexts[1].stats.rx = 6;
    contexts[1].stats.parse_ok = 4;
    contexts[1].stats.parse_unsupported = 1;
    contexts[1].stats.parse_malformed = 1;
    contexts[1].stats.route_hit = 2;
    contexts[1].stats.lookup_miss = 2;
    contexts[1].stats.action_forward = 1;
    contexts[1].stats.action_rewrite = 1;
    contexts[1].stats.action_drop = 2;
    contexts[1].stats.tx_accepted = 1;
    contexts[1].stats.tx_unsent = 1;
    contexts[1].stats.drop = 5;
    contexts[1].stats.rx_bursts = 20;
    contexts[1].stats.rx_empty_polls = 15;
    contexts[1].stats.tx_bursts = 2;
    dp_packet_stats_add(&total, &contexts[0].stats);
    dp_packet_stats_add(&total, &contexts[1].stats);
    if (total.rx != 10 || total.parse_ok != 8 ||
        total.parse_unsupported != 1 || total.parse_malformed != 1 ||
        total.flow_hit != 4 || total.route_hit != 2 ||
        total.lookup_miss != 2 || total.action_forward != 5 ||
        total.action_rewrite != 1 || total.action_drop != 2 ||
        total.tx_accepted != 5 || total.tx_unsent != 1 || total.drop != 5 ||
        total.rx_bursts != 30 || total.rx_empty_polls != 23 ||
        total.tx_bursts != 6 ||
        total.rx != total.parse_ok + total.parse_unsupported +
                    total.parse_malformed ||
        total.parse_ok != total.flow_hit + total.route_hit +
                          total.lookup_miss ||
        total.parse_ok != total.action_drop + total.action_forward +
                          total.action_rewrite ||
        total.action_forward + total.action_rewrite !=
            total.tx_accepted + total.tx_unsent ||
        total.drop != total.parse_unsupported + total.parse_malformed +
                      total.action_drop + total.tx_unsent)
        return -EIO;
    return 0;
}

int dp_runtime_cleanup(void)
{
    if (!dp.initialized)
        return -ENODEV;
    /* teardown 未完整结束时，EAL 仍可能被 worker、ethdev 或 mempool 引用。 */
    if (dp.running || !dp_workers_stopped() ||
        dp.pool || dp.owned[0] || dp.owned[1] ||
        dp.started[0] || dp.started[1] ||
        atomic_load_explicit(&dp.active_rules, memory_order_acquire) ||
        dp.rules_qsbr ||
        atomic_load_explicit(&dp.writer_active, memory_order_acquire) ||
        dp.rules_configured)
        return -EBUSY;
    /* teardown 失败也会报告错误并终止进程；不尝试第二次 EAL 生命周期。 */
    dp.initialized = false;
    return rte_eal_cleanup();
}

int dp_test_runtime_cleanup_guard(int resource)
{
    bool initialized = dp.initialized;
    bool running = dp.running;
    bool owned = dp.owned[0];
    bool rules_configured = dp.rules_configured;
    struct rte_mempool *pool = dp.pool;
    struct dp_rule_snapshot *active_rules =
        atomic_load_explicit(&dp.active_rules, memory_order_relaxed);
    struct rte_rcu_qsbr *rules_qsbr = dp.rules_qsbr;
    bool writer_active =
        atomic_load_explicit(&dp.writer_active, memory_order_relaxed);
    unsigned int active_workers =
        atomic_load_explicit(&dp.active_workers, memory_order_relaxed);
    unsigned int registered_readers =
        atomic_load_explicit(&dp.registered_readers, memory_order_relaxed);
    int ret;

    if (resource != DP_TEST_LIVE_WORKER && resource != DP_TEST_LIVE_PORT &&
        resource != DP_TEST_LIVE_MEMPOOL &&
        resource != DP_TEST_LIVE_ACTIVE_RULES &&
        resource != DP_TEST_LIVE_QSBR &&
        resource != DP_TEST_LIVE_WRITER &&
        resource != DP_TEST_LIVE_READER)
        return -EINVAL;

    dp.initialized = true;
    switch (resource) {
    case DP_TEST_LIVE_WORKER:
        atomic_store_explicit(&dp.active_workers, 1, memory_order_relaxed);
        break;
    case DP_TEST_LIVE_PORT:
        dp.owned[0] = true;
        break;
    case DP_TEST_LIVE_MEMPOOL:
        /* cleanup guard 只比较 NULL，不会解引用这个测试哨兵。 */
        dp.pool = (struct rte_mempool *)1;
        break;
    case DP_TEST_LIVE_ACTIVE_RULES:
        atomic_store_explicit(&dp.active_rules,
                              (struct dp_rule_snapshot *)1,
                              memory_order_relaxed);
        break;
    case DP_TEST_LIVE_QSBR:
        dp.rules_qsbr = (struct rte_rcu_qsbr *)1;
        break;
    case DP_TEST_LIVE_WRITER:
        atomic_store_explicit(&dp.writer_active, true,
                              memory_order_relaxed);
        break;
    case DP_TEST_LIVE_READER:
        atomic_store_explicit(&dp.registered_readers, 1,
                              memory_order_relaxed);
        break;
    }

    ret = dp_runtime_cleanup();
    dp.initialized = initialized;
    dp.running = running;
    dp.owned[0] = owned;
    dp.pool = pool;
    dp.rules_configured = rules_configured;
    atomic_store_explicit(&dp.active_rules, active_rules,
                          memory_order_relaxed);
    dp.rules_qsbr = rules_qsbr;
    atomic_store_explicit(&dp.writer_active, writer_active,
                          memory_order_relaxed);
    atomic_store_explicit(&dp.active_workers, active_workers,
                          memory_order_relaxed);
    atomic_store_explicit(&dp.registered_readers, registered_readers,
                          memory_order_relaxed);
    return ret;
}
