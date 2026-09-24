#ifndef DP_INTERNAL_H
#define DP_INTERNAL_H
#include "dp_api.h"
#include <sys/types.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <rte_mempool.h>
#include <rte_rcu_qsbr.h>
#include "dp_lookup.h"

#define DP_NB_MBUF 4096
#define DP_CACHE_SIZE 128
#define DP_BURST_SIZE 32

/* worker 只读 active_rules 和 stop。writer_lock 只用于低频控制路径。 */
struct dp_state {
    bool attempted, initialized, rules_configured, ready, running, ran;
    atomic_bool stop;
    struct rte_mempool *pool;
    _Atomic(struct dp_rule_snapshot *) active_rules;
    struct rte_rcu_qsbr *rules_qsbr;
    pthread_mutex_t writer_lock;
    atomic_bool writer_active;
    uint16_t ports[2];
    bool owned[2], started[2];
    struct dp_runtime_info info;
    struct dp_stats stats;
};
extern struct dp_state dp;
#endif
