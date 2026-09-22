#ifndef DP_PACKET_H
#define DP_PACKET_H

#include <stdint.h>

/* parser 只产出后续 lookup 所需的协议字段和边界信息，不携带 action 或指针。
 * 所有多字节协议字段都已经转换成 host byte order。
 */
struct dp_packet_meta {
    uint16_t ether_type;
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint8_t l4_proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    uint16_t l4_offset;
};

enum dp_parse_result {
    DP_PARSE_OK = 0,
    DP_PARSE_UNSUPPORTED,
    DP_PARSE_MALFORMED,
};

#endif
