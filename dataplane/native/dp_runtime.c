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
    if (!stats)
        return -EINVAL;
    if (dp.running)
        return -EBUSY;
    *stats = dp.stats;
    return 0;
}

int dp_runtime_cleanup(void)
{
    if (!dp.initialized)
        return -ENODEV;
    /* teardown 未完整结束时，EAL 仍可能被 worker、ethdev 或 mempool 引用。 */
    if (dp.running || dp.pool || dp.owned[0] || dp.owned[1] ||
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
    int ret;

    if (resource != DP_TEST_LIVE_WORKER && resource != DP_TEST_LIVE_PORT &&
        resource != DP_TEST_LIVE_MEMPOOL &&
        resource != DP_TEST_LIVE_ACTIVE_RULES &&
        resource != DP_TEST_LIVE_QSBR &&
        resource != DP_TEST_LIVE_WRITER)
        return -EINVAL;

    dp.initialized = true;
    switch (resource) {
    case DP_TEST_LIVE_WORKER:
        dp.running = true;
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
    return ret;
}
