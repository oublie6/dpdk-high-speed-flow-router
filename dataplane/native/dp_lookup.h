#ifndef DP_LOOKUP_H
#define DP_LOOKUP_H

#include "dp_api.h"
#include "dp_packet.h"
#include <sys/types.h>
#include <rte_hash.h>
#include <rte_lpm.h>

struct dp_flow_key {
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t l4_proto;
    uint8_t reserved[3];
};

/* 一个 generation 的 lookup table 与 action store 只能整体发布和回收。 */
struct dp_rule_snapshot {
    struct rte_hash *flow_table;
    struct rte_lpm *route_table;
    struct dp_rule_action *actions;
    uint32_t action_count;
    uint64_t generation;
};

enum dp_lookup_result {
    DP_LOOKUP_FLOW_HIT = 0,
    DP_LOOKUP_ROUTE_HIT,
    DP_LOOKUP_MISS,
};

void dp_flow_key_from_meta(const struct dp_packet_meta *meta,
                           struct dp_flow_key *key);
enum dp_lookup_result
dp_lookup_packet(const struct dp_packet_meta *meta,
                 const struct dp_rule_snapshot *snapshot,
                 const struct dp_rule_action **action);
struct dp_rule_snapshot *dp_rules_active_load(void);
int dp_rules_reader_register(unsigned int reader_id);
void dp_rules_reader_quiescent(unsigned int reader_id);
void dp_rules_reader_unregister(unsigned int reader_id);
void dp_rules_teardown(void);

#endif
