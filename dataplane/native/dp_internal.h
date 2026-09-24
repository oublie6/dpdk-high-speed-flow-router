#ifndef DP_INTERNAL_H
#define DP_INTERNAL_H
#include "dp_api.h"
#include <sys/types.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <rte_hash.h>
#include <rte_lpm.h>
#include <rte_mempool.h>

#define DP_NB_MBUF 4096
#define DP_CACHE_SIZE 128
#define DP_BURST_SIZE 32

/* 除 stop 外的 mutable state 都只有 owner 线程访问，不需要热路径锁。 */
struct dp_state {
    bool attempted, initialized, rules_configured, ready, running, ran;
    atomic_bool stop;
    struct rte_mempool *pool;
    struct rte_hash *flow_table;
    struct rte_lpm *route_table;
    struct dp_rule_action *actions;
    uint32_t action_count;
    uint16_t ports[2];
    bool owned[2], started[2];
    struct dp_runtime_info info;
    struct dp_stats stats;
};
extern struct dp_state dp;
#endif
