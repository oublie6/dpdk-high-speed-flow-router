#include "dp_api.h"

#include <errno.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_version.h>

/* EAL is process-global, not a reusable instance. The Go wrapper serializes
 * access; C callers must honor the same single-thread lifecycle contract.
 */
static int attempted;
static int initialized;

int dp_runtime_init(int argc, char **argv)
{
    if (attempted)
        return -EALREADY;
    if (argc < 1 || argv == NULL)
        return -EINVAL;
    /* Even a failed EAL init can leave global state behind. Never retry. */
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
    /* Older system packages use the former name for the same EAL main lcore. */
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
    /* No worker or packet resources exist in Goal 001. Future workers must
     * stop and join before this point; cleanup is the last DPDK call.
     */
    ret = rte_eal_cleanup();
    initialized = 0; /* Cleanup is terminal, including its error path. */
    /* Unlike init, cleanup already returns a negative errno (e.g. -EFAULT).
     * Reading rte_errno here could report an unrelated, stale error.
     */
    return ret;
}
