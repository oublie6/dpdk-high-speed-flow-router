#include "dp_binding.h"

#include <stdlib.h>
#include <string.h>

char **dp_argv_alloc(size_t count)
{
    return calloc(count, sizeof(char *));
}

void dp_argv_set(char **argv, size_t index, char *value)
{
    argv[index] = value;
}

void dp_argv_free_array(char **argv)
{
    free(argv);
}

struct dp_flow_rule *dp_flow_rules_alloc(size_t count)
{
    return calloc(count, sizeof(struct dp_flow_rule));
}

static void
dp_rule_action_set(struct dp_rule_action *action, uint8_t type,
                   uint8_t rewrite_mask, uint32_t src_ipv4,
                   uint32_t dst_ipv4, uint16_t src_port, uint16_t dst_port)
{
    memset(action, 0, sizeof(*action));
    action->type = type;
    action->rewrite_mask = rewrite_mask;
    action->src_ipv4 = src_ipv4;
    action->dst_ipv4 = dst_ipv4;
    action->src_port = src_port;
    action->dst_port = dst_port;
}

void dp_flow_rule_set(struct dp_flow_rule *rules, size_t index,
                      uint32_t src_ipv4, uint32_t dst_ipv4,
                      uint16_t src_port, uint16_t dst_port, uint8_t l4_proto,
                      uint8_t action_type, uint8_t rewrite_mask,
                      uint32_t rewrite_src_ipv4, uint32_t rewrite_dst_ipv4,
                      uint16_t rewrite_src_port, uint16_t rewrite_dst_port)
{
    struct dp_flow_rule *rule = &rules[index];
    memset(rule, 0, sizeof(*rule));
    rule->src_ipv4 = src_ipv4;
    rule->dst_ipv4 = dst_ipv4;
    rule->src_port = src_port;
    rule->dst_port = dst_port;
    rule->l4_proto = l4_proto;
    dp_rule_action_set(&rule->action, action_type, rewrite_mask,
                       rewrite_src_ipv4, rewrite_dst_ipv4,
                       rewrite_src_port, rewrite_dst_port);
}

struct dp_route_rule *dp_route_rules_alloc(size_t count)
{
    return calloc(count, sizeof(struct dp_route_rule));
}

void dp_route_rule_set(struct dp_route_rule *rules, size_t index,
                       uint32_t prefix, uint8_t depth,
                       uint8_t action_type, uint8_t rewrite_mask,
                       uint32_t rewrite_src_ipv4, uint32_t rewrite_dst_ipv4,
                       uint16_t rewrite_src_port, uint16_t rewrite_dst_port)
{
    struct dp_route_rule *rule = &rules[index];
    memset(rule, 0, sizeof(*rule));
    rule->prefix = prefix;
    rule->depth = depth;
    dp_rule_action_set(&rule->action, action_type, rewrite_mask,
                       rewrite_src_ipv4, rewrite_dst_ipv4,
                       rewrite_src_port, rewrite_dst_port);
}

void dp_rule_array_free(void *rules)
{
    free(rules);
}
