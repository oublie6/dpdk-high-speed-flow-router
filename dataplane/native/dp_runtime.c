#include "dp_internal.h"
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
    if (dp.running)
        return -EBUSY;
    /* teardown 失败也会报告错误并终止进程；不尝试第二次 EAL 生命周期。 */
    dp.initialized = false;
    return rte_eal_cleanup();
}
