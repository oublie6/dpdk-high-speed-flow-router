#include "dp_internal.h"
#include "dp_lookup.h"
#include <errno.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

static atomic_int test_queue_setup_failure = ATOMIC_VAR_INIT(-1);

static int validate_queue_capacity(unsigned int max_rx, unsigned int max_tx,
                                   unsigned int workers)
{
    return max_rx >= workers && max_tx >= workers ? 0 : -ENOSPC;
}

static int validate_worker_socket(int baseline_socket, int worker_socket)
{
    return baseline_socket == worker_socket ? 0 : -EXDEV;
}

void dp_test_inject_queue_setup_failure(int queue_id)
{
    atomic_store_explicit(&test_queue_setup_failure, queue_id,
                          memory_order_relaxed);
}

int dp_test_queue_capacity(unsigned int max_rx, unsigned int max_tx,
                           unsigned int workers)
{
    return validate_queue_capacity(max_rx, max_tx, workers);
}

int dp_test_worker_socket(int baseline_socket, int worker_socket)
{
    return validate_worker_socket(baseline_socket, worker_socket);
}

static int setup_port(unsigned int index)
{
    uint16_t port = dp.ports[index];
    uint16_t rx_desc = 256, tx_desc = 256;
    struct rte_eth_conf config = {0};
    int ret;

    /* TAP 的 RX/TX 共用 queue FD，两个 port 都建立 worker_count 个对称
     * queue。worker i 只 poll RX port/RXQi，只发送 TX port/TXQi。
     * offloads 保持为 0，禁止软件实验中意外改变包内容。
     */
    ret = rte_eth_dev_configure(port, dp.worker_count, dp.worker_count,
                                &config);
    if (ret < 0)
        return ret;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc);
    if (ret < 0)
        return ret;
    for (uint16_t queue = 0; queue < dp.worker_count; queue++) {
        ret = rte_eth_rx_queue_setup(port, queue, rx_desc, dp.info.socket_id,
                                     NULL, dp.pool);
        if (ret < 0)
            return ret;
        if (index == 0 &&
            atomic_load_explicit(&test_queue_setup_failure,
                                 memory_order_relaxed) == (int)queue)
            return -EIO;
        ret = rte_eth_tx_queue_setup(port, queue, tx_desc, dp.info.socket_id,
                                     NULL);
        if (ret < 0)
            return ret;
    }
    ret = rte_eth_dev_start(port);
    if (ret < 0)
        return ret;
    dp.started[index] = true;
    if (index == 0)
        dp.info.rx_desc = rx_desc;
    else
        dp.info.tx_desc = tx_desc;
    return 0;
}

static int setup_workers(unsigned int worker_count)
{
    unsigned int next = 1;
    unsigned int lcore_id;

    if (worker_count == 0 || worker_count > DP_MAX_WORKERS ||
        dp.info.lcore_count != worker_count)
        return -EINVAL;

    memset(dp.workers, 0, sizeof(dp.workers));
    dp.worker_count = worker_count;
    dp.info.worker_count = worker_count;

    /* main lcore 同时执行 worker0；其余 enabled lcore 按 DPDK 枚举顺序映射。
     * queue、worker 与 QSBR reader 使用同一个连续 id，便于审计 ownership。
     */
    dp.workers[0].info.worker_id = 0;
    dp.workers[0].info.lcore_id = dp.info.main_lcore;
    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_id == dp.info.main_lcore)
            continue;
        if (next >= worker_count)
            return -EINVAL;
        dp.workers[next].info.worker_id = next;
        dp.workers[next].info.lcore_id = lcore_id;
        next++;
    }
    if (next != worker_count)
        return -EINVAL;

    for (unsigned int i = 0; i < worker_count; i++) {
        int socket = (int)rte_lcore_to_socket_id(dp.workers[i].info.lcore_id);

        if (validate_worker_socket(dp.info.socket_id, socket) < 0)
            return -EXDEV;
        dp.workers[i].info.rx_queue_id = (uint16_t)i;
        dp.workers[i].info.tx_queue_id = (uint16_t)i;
        dp.info.workers[i] = dp.workers[i].info;
        atomic_init(&dp.workers[i].result, 0);
        atomic_init(&dp.workers[i].launched, false);
        atomic_init(&dp.workers[i].reader_registered, false);
    }
    return 0;
}

int dp_dataplane_setup(const char *rx_device, const char *tx_device,
                       unsigned int worker_count)
{
    struct rte_eth_dev_info dev_info[2];
    int ret;
    if (!dp.initialized)
        return -ENODEV;
    if (dp.pool || dp.ready)
        return -EALREADY;
    if (!dp.rules_configured)
        return -EINVAL;
    if (!rx_device || !tx_device)
        return -EINVAL;
    ret = setup_workers(worker_count);
    if (ret < 0)
        return ret;
    /* port id 是 EAL 的分配结果，不能把 vdev 顺序当成固定的 0/1。 */
    ret = rte_eth_dev_get_port_by_name(rx_device, &dp.ports[0]);
    if (ret < 0)
        return ret;
    ret = rte_eth_dev_get_port_by_name(tx_device, &dp.ports[1]);
    if (ret < 0)
        return ret;
    if (dp.ports[0] == dp.ports[1])
        return -EINVAL;
    for (unsigned int i = 0; i < 2; i++) {
        ret = rte_eth_dev_info_get(dp.ports[i], &dev_info[i]);
        if (ret < 0)
            return ret;
        if (!dev_info[i].driver_name ||
            strcmp(dev_info[i].driver_name, "net_tap") != 0)
            return -ENOTSUP;
        if (validate_queue_capacity(dev_info[i].max_rx_queues,
                                    dev_info[i].max_tx_queues,
                                    worker_count) < 0)
            return -ENOSPC;
    }
    dp.owned[0] = dp.owned[1] = true;
    dp.info.rx_port = dp.ports[0];
    dp.info.tx_port = dp.ports[1];
    dp.info.nb_mbuf = DP_NB_MBUF;
    dp.info.cache_size = DP_CACHE_SIZE;
    /* 单 owner、单向转发只需要一个 pool；无需跨核传递或多个 pool。 */
    dp.pool = rte_pktmbuf_pool_create("flow_router_pool", DP_NB_MBUF,
        DP_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, dp.info.socket_id);
    if (!dp.pool)
        return -(rte_errno ? rte_errno : ENOMEM);
    for (unsigned int i = 0; i < 2; i++) {
        ret = setup_port(i);
        if (ret < 0)
            return ret; /* caller 无论 setup 成败都执行 teardown。 */
    }
    dp.ready = true;
    return 0;
}

int dp_dataplane_teardown(void)
{
    int first_error = 0;
    if (dp.running || !dp_workers_stopped())
        return -EBUSY;
    dp.ready = false;
    /* worker 已停止后，先撤销其只读 lookup/action snapshot。即使 port close
     * 随后失败，这些资源也不再被任何执行路径引用。
     */
    dp_rules_teardown();
    for (unsigned int i = 0; i < 2; i++) {
        int ret;
        if (!dp.owned[i])
            continue;
        if (dp.started[i]) {
            ret = rte_eth_dev_stop(dp.ports[i]);
            if (ret < 0 && !first_error)
                first_error = ret;
            if (ret == 0)
                dp.started[i] = false;
        }
        ret = rte_eth_dev_close(dp.ports[i]);
        if (ret < 0 && !first_error)
            first_error = ret;
        if (ret == 0) {
            dp.owned[i] = false;
            dp.stats.ports_closed++;
        }
    }
    /* TAP RX queue 自己也持有 mbuf。必须先 close 两个 port，再检查和释放 pool。
     * close 失败时保留 pool 到进程退出，不能冒险产生 PMD use-after-free。
     */
    if (dp.pool && !dp.owned[0] && !dp.owned[1]) {
        dp.stats.pool_in_use = rte_mempool_in_use_count(dp.pool);
        if (dp.stats.pool_in_use != 0)
            return first_error ? first_error : -EBUSY;
        rte_mempool_free(dp.pool);
        dp.pool = NULL;
        dp.stats.pool_freed = 1;
    }
    return first_error;
}
