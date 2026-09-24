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
    uint64_t flow_hit, route_hit, lookup_miss;
    uint64_t action_drop, action_forward, action_rewrite;
    uint64_t tx_accepted, tx_unsent, drop;
    uint64_t rules_generation, rules_publish_success;
    uint64_t rules_publish_failed, rules_reclaimed;
    unsigned int ports_closed, pool_in_use;
    int pool_freed, flow_table_freed, route_table_freed, action_store_freed;
    int snapshot_freed, qsbr_freed;
};

#define DP_MAX_FLOW_RULES 1024
#define DP_MAX_ROUTE_RULES 1024

enum dp_action_type {
    DP_ACTION_DROP = 0,
    DP_ACTION_FORWARD,
    DP_ACTION_REWRITE,
};

enum dp_rewrite_mask {
    DP_REWRITE_SRC_IPV4 = 1U << 0,
    DP_REWRITE_DST_IPV4 = 1U << 1,
    DP_REWRITE_SRC_PORT = 1U << 2,
    DP_REWRITE_DST_PORT = 1U << 3,
};

/* Go 已完成 JSON 与地址校验；跨边界结构只携带 host byte order 数值。 */
struct dp_rule_action {
    uint8_t type;
    uint8_t rewrite_mask;
    uint16_t reserved;
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
};

struct dp_flow_rule {
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t l4_proto;
    uint8_t reserved[3];
    struct dp_rule_action action;
};

struct dp_route_rule {
    uint32_t prefix;
    uint8_t depth;
    uint8_t reserved[3];
    struct dp_rule_action action;
};

/* request_stop 与 publish_rules 可以跨线程调用；publish_rules 在 native 边界
 * 内部串行化 writer。其余 lifecycle API 必须由 owner OS thread 串行调用。
 * argv 和字符串属于 caller 的 C 内存，EAL 可以调整顺序，须保留到 cleanup。
 * 一个进程只允许一次 init 尝试。所有状态函数返回 0 或负 errno。
 */
int dp_runtime_init(int argc, char **argv);
int dp_runtime_get_info(struct dp_runtime_info *info);
int dp_configure_rules(const struct dp_flow_rule *flows, uint32_t flow_count,
                       const struct dp_route_rule *routes,
                       uint32_t route_count);
int dp_publish_rules(const struct dp_flow_rule *flows, uint32_t flow_count,
                     const struct dp_route_rule *routes, uint32_t route_count,
                     uint64_t *generation);
int dp_dataplane_setup(const char *rx_device, const char *tx_device);
int dp_dataplane_run(void);
/* stop 也允许其他线程调用：只写 C-owned atomic flag，不操作任何 queue。 */
void dp_dataplane_request_stop(void);
/* stats 只在 run 返回后读取，不提供与 hot path 并发的快照。 */
int dp_dataplane_get_stats(struct dp_stats *stats);
int dp_dataplane_teardown(void);
int dp_runtime_cleanup(void);
#endif
