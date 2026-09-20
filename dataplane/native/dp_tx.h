#ifndef DP_TX_H
#define DP_TX_H
#include "dp_api.h"
#include <sys/types.h>
#include <rte_mbuf.h>

/* RX 返回的 n 个 mbuf 归 application；TX 接受 [0,sent) 后归 PMD。
 * zero-retry 策略只释放未被接受的尾部，永远不触碰已转移的 ownership。
 * 真实 worker 与 partial-return 测试执行同一个函数，避免测试复制实现。
 */
void dp_complete_tx(struct rte_mbuf **pkts, uint16_t n, uint16_t sent,
                    struct dp_stats *stats);
#endif
