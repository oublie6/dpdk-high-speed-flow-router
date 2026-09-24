#ifndef DP_WORKER_H
#define DP_WORKER_H

#include "dp_api.h"
#include "dp_lookup.h"
#include <stdatomic.h>
#include <rte_common.h>

struct rte_mbuf;

/* 每个 context 恰好由一个 lcore 写。cache-line 对齐和整 cache-line 大小避免
 * 相邻 worker 的 packet counters 落入同一条 cache line。
 */
struct dp_worker_ctx {
    struct dp_worker_info info;
    struct dp_packet_stats stats;
    atomic_int result;
    atomic_bool launched;
    atomic_bool reader_registered;
} __rte_cache_aligned;

_Static_assert(sizeof(struct dp_worker_ctx) % RTE_CACHE_LINE_SIZE == 0,
               "worker context 必须占用完整 cache line");
_Static_assert(_Alignof(struct dp_worker_ctx) >= RTE_CACHE_LINE_SIZE,
               "worker context 必须按 cache line 对齐");

/* 返回 NULL 表示 packet 已由本函数 drop/free；非 NULL 仍由 caller 持有并进入 TX。 */
struct rte_mbuf *dp_process_packet(struct rte_mbuf *mbuf,
                                   struct dp_packet_stats *stats,
                                   const struct dp_rule_snapshot *snapshot);

#endif
