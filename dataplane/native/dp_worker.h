#ifndef DP_WORKER_H
#define DP_WORKER_H

#include "dp_api.h"
#include "dp_lookup.h"

struct rte_mbuf;

/* 返回 NULL 表示 packet 已由本函数 drop/free；非 NULL 仍由 caller 持有并进入 TX。 */
struct rte_mbuf *dp_process_packet(struct rte_mbuf *mbuf,
                                   struct dp_stats *stats,
                                   const struct dp_rule_snapshot *snapshot);

#endif
