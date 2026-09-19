#include "dp_api.h"

#include <errno.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_version.h>

/* EAL 是 process-global runtime，并不是可重复创建的普通对象。
 * Go wrapper 会串行化生命周期；其他 C caller 也必须遵守同样约束。
 */
static int attempted;
static int initialized;

int dp_runtime_init(int argc, char **argv)
{
    if (attempted)
        return -EALREADY;
    if (argc < 1 || argv == NULL)
        return -EINVAL;

    /* 即使 rte_eal_init() 失败，也可能已经留下部分全局状态。
     * 因此一次失败后当前进程不再 retry。
     */
    attempted = 1;
    if (rte_eal_init(argc, argv) < 0)
        return -(rte_errno ? rte_errno : EIO);

    initialized = 1;
    return 0;
}

int dp_runtime_get_info(struct dp_runtime_info *info)
{
    if (info == NULL)
        return -EINVAL;
    if (!initialized)
        return -ENODEV;

    info->initialized = initialized;

    /* 较老 DPDK 使用 master_lcore 命名；较新版本使用 main_lcore。 */
#if RTE_VERSION >= RTE_VERSION_NUM(20, 11, 0, 0)
    info->main_lcore = rte_get_main_lcore();
#else
    info->main_lcore = rte_get_master_lcore();
#endif
    info->lcore_count = rte_lcore_count();
    info->version = rte_version();
    return 0;
}

int dp_runtime_cleanup(void)
{
    int ret;

    if (!initialized)
        return -ENODEV;

    /* Goal 001 还没有 worker 或 packet resource。
     * 后续版本必须先 stop/join worker 并释放 queue/mbuf ownership，
     * 最后才能执行 EAL cleanup。
     */
    ret = rte_eal_cleanup();
    initialized = 0; /* cleanup 是终止状态，即使返回 error 也不再继续使用 DPDK。 */

    /* rte_eal_cleanup() 自己已经返回 negative errno。
     * 这里不读取 rte_errno，避免把无关的 stale errno 当成 cleanup 错误。
     */
    return ret;
}
