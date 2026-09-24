#include "dp_lookup.h"

#include "dp_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <rte_errno.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#define DP_RULE_READER_ID 0U
#define DP_RULE_READER_CAPACITY RTE_MAX_LCORE

_Static_assert(sizeof(struct dp_flow_key) == 16,
               "flow key 必须包含确定的 16 字节布局");

static int
dp_validate_action(const struct dp_rule_action *action)
{
    const uint8_t valid_mask = DP_REWRITE_SRC_IPV4 | DP_REWRITE_DST_IPV4 |
                               DP_REWRITE_SRC_PORT | DP_REWRITE_DST_PORT;

    if (action->reserved != 0)
        return -EINVAL;
    if (action->type == DP_ACTION_DROP || action->type == DP_ACTION_FORWARD)
        return action->rewrite_mask == 0 ? 0 : -EINVAL;
    if (action->type != DP_ACTION_REWRITE)
        return -EINVAL;
    if (action->rewrite_mask == 0 || (action->rewrite_mask & ~valid_mask) != 0)
        return -EINVAL;
    return 0;
}

void
dp_flow_key_from_meta(const struct dp_packet_meta *meta,
                      struct dp_flow_key *key)
{
    /* reserved 是 hash key 的组成部分，必须连同其余 padding 一起确定化。 */
    memset(key, 0, sizeof(*key));
    key->src_ipv4 = meta->src_ipv4;
    key->dst_ipv4 = meta->dst_ipv4;
    key->src_port = meta->src_port;
    key->dst_port = meta->dst_port;
    key->l4_proto = meta->l4_proto;
}

static void
dp_snapshot_free(struct dp_rule_snapshot *snapshot, bool reclaimed,
                 bool record_resource_stats)
{
    if (!snapshot)
        return;
    if (snapshot->flow_table) {
        rte_hash_free(snapshot->flow_table);
        if (record_resource_stats)
            dp.stats.flow_table_freed = 1;
    }
    if (snapshot->route_table) {
        rte_lpm_free(snapshot->route_table);
        if (record_resource_stats)
            dp.stats.route_table_freed = 1;
    }
    if (snapshot->actions) {
        rte_free(snapshot->actions);
        if (record_resource_stats)
            dp.stats.action_store_freed = 1;
    }
    rte_free(snapshot);
    if (record_resource_stats)
        dp.stats.snapshot_freed = 1;
    if (reclaimed)
        dp.stats.rules_reclaimed++;
}

static int
dp_snapshot_build(const struct dp_flow_rule *flows, uint32_t flow_count,
                  const struct dp_route_rule *routes, uint32_t route_count,
                  uint64_t generation, struct dp_rule_snapshot **result)
{
    struct dp_rule_snapshot *snapshot;
    struct rte_hash_parameters hash_parameters = {0};
    struct rte_lpm_config lpm_config = {
        .max_rules = DP_MAX_ROUTE_RULES,
        .number_tbl8s = 256,
        .flags = 0,
    };
    char flow_name[32];
    char route_name[32];
    uint32_t total_actions;
    int ret = 0;

    *result = NULL;
    if (flow_count > DP_MAX_FLOW_RULES || route_count > DP_MAX_ROUTE_RULES)
        return -ENOSPC;
    if ((flow_count != 0 && !flows) || (route_count != 0 && !routes))
        return -EINVAL;

    snapshot = rte_zmalloc_socket("dfr_rule_snapshot", sizeof(*snapshot),
                                  RTE_CACHE_LINE_SIZE, dp.info.socket_id);
    if (!snapshot)
        return -(rte_errno ? rte_errno : ENOMEM);
    snapshot->generation = generation;
    total_actions = flow_count + route_count;
    if (total_actions != 0) {
        snapshot->actions = rte_zmalloc_socket(
            "dfr_rule_actions", sizeof(*snapshot->actions) * total_actions,
            RTE_CACHE_LINE_SIZE, dp.info.socket_id);
        if (!snapshot->actions) {
            ret = -(rte_errno ? rte_errno : ENOMEM);
            goto fail;
        }
        snapshot->action_count = total_actions;
    }

    if (flow_count != 0) {
        snprintf(flow_name, sizeof(flow_name), "dfr_f_%llu",
                 (unsigned long long)generation);
        hash_parameters.name = flow_name;
        hash_parameters.entries = DP_MAX_FLOW_RULES;
        hash_parameters.key_len = sizeof(struct dp_flow_key);
        hash_parameters.hash_func = rte_jhash;
        hash_parameters.socket_id = dp.info.socket_id;
        snapshot->flow_table = rte_hash_create(&hash_parameters);
        if (!snapshot->flow_table) {
            ret = -(rte_errno ? rte_errno : ENOMEM);
            goto fail;
        }
    }
    if (route_count != 0) {
        snprintf(route_name, sizeof(route_name), "dfr_r_%llu",
                 (unsigned long long)generation);
        snapshot->route_table = rte_lpm_create(route_name, dp.info.socket_id,
                                                &lpm_config);
        if (!snapshot->route_table) {
            ret = -(rte_errno ? rte_errno : ENOMEM);
            goto fail;
        }
    }

    for (uint32_t i = 0; i < flow_count; i++) {
        struct dp_flow_key key;
        void *existing;

        if ((flows[i].l4_proto != IPPROTO_TCP &&
             flows[i].l4_proto != IPPROTO_UDP) ||
            flows[i].reserved[0] != 0 || flows[i].reserved[1] != 0 ||
            flows[i].reserved[2] != 0) {
            ret = -EINVAL;
            goto fail;
        }
        ret = dp_validate_action(&flows[i].action);
        if (ret < 0)
            goto fail;
        memset(&key, 0, sizeof(key));
        key.src_ipv4 = flows[i].src_ipv4;
        key.dst_ipv4 = flows[i].dst_ipv4;
        key.src_port = flows[i].src_port;
        key.dst_port = flows[i].dst_port;
        key.l4_proto = flows[i].l4_proto;
        if (rte_hash_lookup_data(snapshot->flow_table, &key, &existing) >= 0) {
            ret = -EEXIST;
            goto fail;
        }
        snapshot->actions[i] = flows[i].action;
        ret = rte_hash_add_key_data(snapshot->flow_table, &key,
                                    &snapshot->actions[i]);
        if (ret < 0)
            goto fail;
    }

    for (uint32_t i = 0; i < route_count; i++) {
        uint32_t action_index = flow_count + i;
        uint32_t existing;
        uint32_t mask;

        if (routes[i].depth == 0 || routes[i].depth > 32 ||
            routes[i].reserved[0] != 0 || routes[i].reserved[1] != 0 ||
            routes[i].reserved[2] != 0) {
            ret = -EINVAL;
            goto fail;
        }
        mask = UINT32_MAX << (32 - routes[i].depth);
        if ((routes[i].prefix & mask) != routes[i].prefix) {
            ret = -EINVAL;
            goto fail;
        }
        ret = dp_validate_action(&routes[i].action);
        if (ret < 0)
            goto fail;
        ret = rte_lpm_is_rule_present(snapshot->route_table, routes[i].prefix,
                                      routes[i].depth, &existing);
        if (ret > 0) {
            ret = -EEXIST;
            goto fail;
        }
        if (ret < 0)
            goto fail;
        snapshot->actions[action_index] = routes[i].action;
        ret = rte_lpm_add(snapshot->route_table, routes[i].prefix,
                          routes[i].depth, action_index);
        if (ret < 0)
            goto fail;
    }

    *result = snapshot;
    return 0;

fail:
    dp_snapshot_free(snapshot, false, false);
    return ret;
}

static int
dp_qsbr_create(void)
{
    size_t size;

    if (dp.rules_qsbr)
        return 0;
    size = rte_rcu_qsbr_get_memsize(DP_RULE_READER_CAPACITY);
    if (size == 1)
        return -(rte_errno ? rte_errno : EINVAL);
    dp.rules_qsbr = rte_zmalloc_socket("dfr_rules_qsbr", size,
                                       RTE_CACHE_LINE_SIZE,
                                       dp.info.socket_id);
    if (!dp.rules_qsbr)
        return -(rte_errno ? rte_errno : ENOMEM);
    if (rte_rcu_qsbr_init(dp.rules_qsbr, DP_RULE_READER_CAPACITY) != 0) {
        rte_free(dp.rules_qsbr);
        dp.rules_qsbr = NULL;
        return -(rte_errno ? rte_errno : EINVAL);
    }
    return 0;
}

int
dp_configure_rules(const struct dp_flow_rule *flows, uint32_t flow_count,
                   const struct dp_route_rule *routes, uint32_t route_count)
{
    struct dp_rule_snapshot *snapshot;
    int ret;

    if (!dp.initialized)
        return -ENODEV;
    if (dp.rules_configured || dp.ready || dp.running || dp.ran)
        return -EALREADY;
    ret = dp_qsbr_create();
    if (ret < 0)
        return ret;
    ret = dp_snapshot_build(flows, flow_count, routes, route_count, 1,
                            &snapshot);
    if (ret < 0) {
        dp.stats.rules_publish_failed++;
        return ret;
    }
    atomic_store_explicit(&dp.active_rules, snapshot, memory_order_release);
    dp.rules_configured = true;
    dp.stats.rules_generation = 1;
    dp.stats.rules_publish_success = 1;
    return 0;
}

int
dp_publish_rules(const struct dp_flow_rule *flows, uint32_t flow_count,
                 const struct dp_route_rule *routes, uint32_t route_count,
                 uint64_t *generation)
{
    struct dp_rule_snapshot *new_snapshot;
    struct dp_rule_snapshot *old_snapshot;
    uint64_t next_generation;
    uint64_t token;
    int ret;

    if (!generation)
        return -EINVAL;
    pthread_mutex_lock(&dp.writer_lock);
    atomic_store_explicit(&dp.writer_active, true, memory_order_release);
    /* initialized/rules_configured/QSBR 在 writer_lock 覆盖的规则生命周期内
     * 保持稳定。这里不读取 worker 的非 atomic running/ready 字段，避免合法的
     * cross-thread publish 与 owner thread 形成数据竞争。
     */
    if (!dp.initialized || !dp.rules_configured || !dp.rules_qsbr ||
        !atomic_load_explicit(&dp.active_rules, memory_order_acquire)) {
        ret = -ENODEV;
        goto out;
    }
    next_generation = dp.stats.rules_generation + 1;
    ret = dp_snapshot_build(flows, flow_count, routes, route_count,
                            next_generation, &new_snapshot);
    if (ret < 0) {
        dp.stats.rules_publish_failed++;
        goto out;
    }

    /* release 发布完整构建结果，reader 的 acquire load 保证整个 burst 只看见
     * 同一代。交换完成后才启动 grace period，旧的 quiescent 不能提前计入。
     */
    old_snapshot = atomic_exchange_explicit(&dp.active_rules, new_snapshot,
                                            memory_order_acq_rel);
    dp.stats.rules_generation = next_generation;
    dp.stats.rules_publish_success++;
    token = rte_rcu_qsbr_start(dp.rules_qsbr);
    (void)rte_rcu_qsbr_check(dp.rules_qsbr, token, true);
    dp_snapshot_free(old_snapshot, true, true);
    *generation = next_generation;
    ret = 0;

out:
    atomic_store_explicit(&dp.writer_active, false, memory_order_release);
    pthread_mutex_unlock(&dp.writer_lock);
    return ret;
}

struct dp_rule_snapshot *
dp_rules_active_load(void)
{
    return atomic_load_explicit(&dp.active_rules, memory_order_acquire);
}

int
dp_rules_reader_register(void)
{
    int ret;

    if (!dp.rules_qsbr)
        return -ENODEV;
    ret = rte_rcu_qsbr_thread_register(dp.rules_qsbr, DP_RULE_READER_ID);
    if (ret != 0)
        return -(rte_errno ? rte_errno : EIO);
    rte_rcu_qsbr_thread_online(dp.rules_qsbr, DP_RULE_READER_ID);
    return 0;
}

void
dp_rules_reader_quiescent(void)
{
    rte_rcu_qsbr_quiescent(dp.rules_qsbr, DP_RULE_READER_ID);
}

void
dp_rules_reader_unregister(void)
{
    rte_rcu_qsbr_thread_offline(dp.rules_qsbr, DP_RULE_READER_ID);
    (void)rte_rcu_qsbr_thread_unregister(dp.rules_qsbr, DP_RULE_READER_ID);
}

enum dp_lookup_result
dp_lookup_packet(const struct dp_packet_meta *meta,
                 const struct dp_rule_snapshot *snapshot,
                 const struct dp_rule_action **action)
{
    struct dp_flow_key key;
    void *flow_action;
    uint32_t action_index;

    *action = NULL;
    if (snapshot && snapshot->flow_table) {
        dp_flow_key_from_meta(meta, &key);
        if (rte_hash_lookup_data(snapshot->flow_table, &key, &flow_action) >= 0) {
            *action = flow_action;
            return DP_LOOKUP_FLOW_HIT;
        }
    }
    if (snapshot && snapshot->route_table &&
        rte_lpm_lookup(snapshot->route_table, meta->dst_ipv4,
                       &action_index) == 0 &&
        action_index < snapshot->action_count) {
        *action = &snapshot->actions[action_index];
        return DP_LOOKUP_ROUTE_HIT;
    }
    return DP_LOOKUP_MISS;
}

void
dp_rules_teardown(void)
{
    struct dp_rule_snapshot *snapshot;

    pthread_mutex_lock(&dp.writer_lock);
    atomic_store_explicit(&dp.writer_active, true, memory_order_release);
    snapshot = atomic_exchange_explicit(&dp.active_rules, NULL,
                                        memory_order_acq_rel);
    dp_snapshot_free(snapshot, false, true);
    if (dp.rules_qsbr) {
        rte_free(dp.rules_qsbr);
        dp.rules_qsbr = NULL;
        dp.stats.qsbr_freed = 1;
    }
    dp.rules_configured = false;
    atomic_store_explicit(&dp.writer_active, false, memory_order_release);
    pthread_mutex_unlock(&dp.writer_lock);
}
