#ifndef DP_API_H
#define DP_API_H
#include <stdint.h>

/* 公共边界不暴露 mbuf/ethdev。Info 中 version 是借用指针，Go 在 cleanup 前复制。 */
struct dp_runtime_info {
    int initialized;
    unsigned int main_lcore, lcore_count;
    const char *version;
    uint16_t rx_port, tx_port, rx_desc, tx_desc;
    unsigned int nb_mbuf, cache_size;
    int socket_id;
};
struct dp_stats {
    uint64_t rx, parse_ok, parse_unsupported, parse_malformed;
    uint64_t tx_accepted, tx_unsent, drop;
    unsigned int ports_closed, pool_in_use;
    int pool_freed;
};

/* 除 request_stop 外，所有调用必须在同一个 owner OS thread 串行执行。
 * argv 和字符串属于 caller 的 C 内存，EAL 可以调整顺序，须保留到 cleanup。
 * 一个进程只允许一次 init 尝试。所有状态函数返回 0 或负 errno。
 */
int dp_runtime_init(int argc, char **argv);
int dp_runtime_get_info(struct dp_runtime_info *info);
int dp_dataplane_setup(const char *rx_device, const char *tx_device);
int dp_dataplane_run(void);
/* 唯一允许其他线程调用的入口：只写 C-owned atomic flag，不操作任何 queue。 */
void dp_dataplane_request_stop(void);
/* stats 只在 run 返回后读取，不提供与 hot path 并发的快照。 */
int dp_dataplane_get_stats(struct dp_stats *stats);
int dp_dataplane_teardown(void);
int dp_runtime_cleanup(void);
#endif
