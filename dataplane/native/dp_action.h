#ifndef DP_ACTION_H
#define DP_ACTION_H

#include "dp_api.h"
#include "dp_packet.h"

struct rte_mbuf;

/* DROP 的 free 仍由 worker 显式完成；本函数只执行需要修改 packet 的动作。 */
int dp_apply_action(struct rte_mbuf *mbuf, const struct dp_packet_meta *meta,
                    const struct dp_rule_action *action);

#endif
