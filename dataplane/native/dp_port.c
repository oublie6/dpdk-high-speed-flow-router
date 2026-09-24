#include "dp_internal.h"
#include "dp_lookup.h"
#include <errno.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

static int setup_port(unsigned int index)
{
    uint16_t port = dp.ports[index];
    uint16_t rx_desc = 256, tx_desc = 256;
    struct rte_eth_conf config = {0};
    int ret;

    /* TAP 的 RX/TX 共用 queue FD，配置层为每个 port 建立对称 queue。
     * worker 仍只 poll ports[0]/RXQ0、发送 ports[1]/TXQ0。
     * offloads 保持为 0，禁止软件实验中意外改变包内容。
     */
    ret = rte_eth_dev_configure(port, 1, 1, &config);
    if (ret < 0)
        return ret;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc);
    if (ret < 0)
        return ret;
    ret = rte_eth_rx_queue_setup(port, 0, rx_desc, dp.info.socket_id, NULL, dp.pool);
    if (ret < 0)
        return ret;
    ret = rte_eth_tx_queue_setup(port, 0, tx_desc, dp.info.socket_id, NULL);
    if (ret < 0)
        return ret;
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

int dp_dataplane_setup(const char *rx_device, const char *tx_device)
{
    struct rte_eth_dev_info dev_info;
    int ret;
    if (!dp.initialized)
        return -ENODEV;
    if (dp.pool || dp.ready)
        return -EALREADY;
    if (!dp.rules_configured)
        return -EINVAL;
    if (!rx_device || !tx_device || dp.info.lcore_count != 1)
        return -EINVAL;
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
        ret = rte_eth_dev_info_get(dp.ports[i], &dev_info);
        if (ret < 0)
            return ret;
        if (!dev_info.driver_name || strcmp(dev_info.driver_name, "net_tap") != 0)
            return -ENOTSUP;
    }
    dp.owned[0] = dp.owned[1] = true;
    dp.info.rx_port = dp.ports[0];
    dp.info.tx_port = dp.ports[1];
    dp.info.nb_mbuf = DP_NB_MBUF;
    dp.info.cache_size = DP_CACHE_SIZE;
    dp.info.socket_id = rte_socket_id();
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
    if (dp.running)
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
