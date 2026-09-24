#include "dp_lookup.h"

#include "dp_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>

#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lpm.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

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

static int
dp_create_action_store(uint32_t action_count)
{
    if (action_count == 0)
        return 0;
    dp.actions = rte_zmalloc_socket("flow_router_actions",
                                    sizeof(*dp.actions) * action_count,
                                    RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!dp.actions)
        return -(rte_errno ? rte_errno : ENOMEM);
    dp.action_count = action_count;
    return 0;
}

static int
dp_create_flow_table(void)
{
    struct rte_hash_parameters parameters = {
        .name = "flow_router_exact_flows",
        .entries = DP_MAX_FLOW_RULES,
        .key_len = sizeof(struct dp_flow_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };

    dp.flow_table = rte_hash_create(&parameters);
    if (!dp.flow_table)
        return -(rte_errno ? rte_errno : ENOMEM);
    return 0;
}

static int
dp_create_route_table(void)
{
    struct rte_lpm_config config = {
        .max_rules = DP_MAX_ROUTE_RULES,
        .number_tbl8s = 256,
        .flags = 0,
    };

    dp.route_table = rte_lpm_create("flow_router_ipv4_routes", rte_socket_id(),
                                    &config);
    if (!dp.route_table)
        return -(rte_errno ? rte_errno : ENOMEM);
    return 0;
}

int
dp_configure_rules(const struct dp_flow_rule *flows, uint32_t flow_count,
                       const struct dp_route_rule *routes,
                       uint32_t route_count)
{
    uint32_t total_actions;
    int ret;

    if (!dp.initialized)
        return -ENODEV;
    if (dp.rules_configured || dp.ready || dp.running || dp.ran)
        return -EALREADY;
    if (flow_count > DP_MAX_FLOW_RULES || route_count > DP_MAX_ROUTE_RULES)
        return -ENOSPC;
    if ((flow_count != 0 && !flows) || (route_count != 0 && !routes))
        return -EINVAL;

    total_actions = flow_count + route_count;
    ret = dp_create_action_store(total_actions);
    if (ret < 0)
        return ret;
    if (flow_count != 0) {
        ret = dp_create_flow_table();
        if (ret < 0)
            return ret;
    }
    if (route_count != 0) {
        ret = dp_create_route_table();
        if (ret < 0)
            return ret;
    }

    for (uint32_t i = 0; i < flow_count; i++) {
        struct dp_flow_key key;
        void *existing;

        if ((flows[i].l4_proto != IPPROTO_TCP &&
             flows[i].l4_proto != IPPROTO_UDP) ||
            flows[i].reserved[0] != 0 || flows[i].reserved[1] != 0 ||
            flows[i].reserved[2] != 0)
            return -EINVAL;
        ret = dp_validate_action(&flows[i].action);
        if (ret < 0)
            return ret;

        memset(&key, 0, sizeof(key));
        key.src_ipv4 = flows[i].src_ipv4;
        key.dst_ipv4 = flows[i].dst_ipv4;
        key.src_port = flows[i].src_port;
        key.dst_port = flows[i].dst_port;
        key.l4_proto = flows[i].l4_proto;
        if (rte_hash_lookup_data(dp.flow_table, &key, &existing) >= 0)
            return -EEXIST;
        dp.actions[i] = flows[i].action;
        ret = rte_hash_add_key_data(dp.flow_table, &key, &dp.actions[i]);
        if (ret < 0)
            return ret;
    }

    for (uint32_t i = 0; i < route_count; i++) {
        uint32_t action_index = flow_count + i;
        uint32_t existing;

        if (routes[i].depth == 0 || routes[i].depth > 32 ||
            routes[i].reserved[0] != 0 ||
            routes[i].reserved[1] != 0 || routes[i].reserved[2] != 0)
            return -EINVAL;
        ret = dp_validate_action(&routes[i].action);
        if (ret < 0)
            return ret;
        ret = rte_lpm_is_rule_present(dp.route_table, routes[i].prefix,
                                      routes[i].depth, &existing);
        if (ret > 0)
            return -EEXIST;
        if (ret < 0)
            return ret;
        dp.actions[action_index] = routes[i].action;
        ret = rte_lpm_add(dp.route_table, routes[i].prefix, routes[i].depth,
                          action_index);
        if (ret < 0)
            return ret;
    }

    dp.rules_configured = true;
    return 0;
}

enum dp_lookup_result
dp_lookup_packet(const struct dp_packet_meta *meta,
                 const struct dp_rule_action **action)
{
    struct dp_flow_key key;
    void *flow_action;
    uint32_t action_index;

    *action = NULL;
    if (dp.flow_table) {
        dp_flow_key_from_meta(meta, &key);
        if (rte_hash_lookup_data(dp.flow_table, &key, &flow_action) >= 0) {
            *action = flow_action;
            return DP_LOOKUP_FLOW_HIT;
        }
    }
    if (dp.route_table &&
        rte_lpm_lookup(dp.route_table, meta->dst_ipv4, &action_index) == 0) {
        if (action_index < dp.action_count) {
            *action = &dp.actions[action_index];
            return DP_LOOKUP_ROUTE_HIT;
        }
    }
    return DP_LOOKUP_MISS;
}

void
dp_rules_teardown(void)
{
    if (dp.flow_table) {
        rte_hash_free(dp.flow_table);
        dp.flow_table = NULL;
        dp.stats.flow_table_freed = 1;
    }
    if (dp.route_table) {
        rte_lpm_free(dp.route_table);
        dp.route_table = NULL;
        dp.stats.route_table_freed = 1;
    }
    if (dp.actions) {
        rte_free(dp.actions);
        dp.actions = NULL;
        dp.action_count = 0;
        dp.stats.action_store_freed = 1;
    }
    dp.rules_configured = false;
}
