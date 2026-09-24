#ifndef DP_LOOKUP_H
#define DP_LOOKUP_H

#include "dp_api.h"
#include "dp_packet.h"

struct dp_flow_key {
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t l4_proto;
    uint8_t reserved[3];
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
                 const struct dp_rule_action **action);
void dp_rules_teardown(void);

#endif
