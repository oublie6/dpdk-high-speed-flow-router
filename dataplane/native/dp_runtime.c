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
struct dp_state dp = {.stop = ATOMIC_VAR_INIT(false)};

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
        dp.started[0] || dp.started[1])
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
    struct rte_mempool *pool = dp.pool;
    int ret;

    if (resource != DP_TEST_LIVE_WORKER && resource != DP_TEST_LIVE_PORT &&
        resource != DP_TEST_LIVE_MEMPOOL)
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
    }

    ret = dp_runtime_cleanup();
    dp.initialized = initialized;
    dp.running = running;
    dp.owned[0] = owned;
    dp.pool = pool;
    return ret;
}
