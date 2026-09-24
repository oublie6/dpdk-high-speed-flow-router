#ifndef DP_BINDING_H
#define DP_BINDING_H

#include <stddef.h>
#include <stdint.h>

#include "dp_api.h"

/* 这些 helper 只服务 cgo argv 装配，把 C 数组布局细节留在 C 中。 */
char **dp_argv_alloc(size_t count);
void dp_argv_set(char **argv, size_t index, char *value);
void dp_argv_free_array(char **argv);

/* rule array 的布局和元素写入都留在 C，Go binding 不做 pointer arithmetic。 */
struct dp_flow_rule *dp_flow_rules_alloc(size_t count);
void dp_flow_rule_set(struct dp_flow_rule *rules, size_t index,
                      uint32_t src_ipv4, uint32_t dst_ipv4,
                      uint16_t src_port, uint16_t dst_port, uint8_t l4_proto,
                      uint8_t action_type, uint8_t rewrite_mask,
                      uint32_t rewrite_src_ipv4, uint32_t rewrite_dst_ipv4,
                      uint16_t rewrite_src_port, uint16_t rewrite_dst_port);
struct dp_route_rule *dp_route_rules_alloc(size_t count);
void dp_route_rule_set(struct dp_route_rule *rules, size_t index,
                       uint32_t prefix, uint8_t depth,
                       uint8_t action_type, uint8_t rewrite_mask,
                       uint32_t rewrite_src_ipv4, uint32_t rewrite_dst_ipv4,
                       uint16_t rewrite_src_port, uint16_t rewrite_dst_port);
void dp_rule_array_free(void *rules);

#endif
