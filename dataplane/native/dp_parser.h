#ifndef DP_PARSER_H
#define DP_PARSER_H

#include "dp_packet.h"

struct rte_mbuf;

/* parser 不修改也不释放 mbuf。只有返回 DP_PARSE_OK 时 meta 才可供后续阶段使用。 */
enum dp_parse_result dp_parse_packet(struct rte_mbuf *mbuf,
                                     struct dp_packet_meta *meta);

#endif
