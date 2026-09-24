#ifndef DP_TEST_H
#define DP_TEST_H

#include "dp_packet.h"

/* package 内测试钩子：模拟 partial return 并验证 mbuf ownership。
 * 它不属于项目公共 runtime API，也不会进入 packet hot path。
 */
int dp_test_tx_partial_ownership(void);

enum dp_test_live_resource {
    DP_TEST_LIVE_WORKER = 1,
    DP_TEST_LIVE_PORT = 2,
    DP_TEST_LIVE_MEMPOOL = 3,
    DP_TEST_LIVE_ACTIVE_RULES = 4,
    DP_TEST_LIVE_QSBR = 5,
    DP_TEST_LIVE_WRITER = 6,
};

/* 临时构造一种存活资源并直接调用 cleanup；返回值应为 -EBUSY。 */
int dp_test_runtime_cleanup_guard(int resource);

enum dp_test_parse_fixture {
    DP_TEST_PARSE_VALID_TCP = 1,
    DP_TEST_PARSE_VALID_UDP,
    DP_TEST_PARSE_IPV4_OPTIONS,
    DP_TEST_PARSE_TCP_OPTIONS,
    DP_TEST_PARSE_NON_IPV4,
    DP_TEST_PARSE_UNSUPPORTED_L4,
    DP_TEST_PARSE_IPV4_FRAGMENT,
    DP_TEST_PARSE_ETHERNET_TRUNCATED,
    DP_TEST_PARSE_IPV4_TRUNCATED,
    DP_TEST_PARSE_INVALID_IPV4_VERSION,
    DP_TEST_PARSE_INVALID_IHL,
    DP_TEST_PARSE_INVALID_TOTAL_LENGTH,
    DP_TEST_PARSE_TOTAL_LENGTH_BEYOND_FRAME,
    DP_TEST_PARSE_TCP_TRUNCATED,
    DP_TEST_PARSE_INVALID_TCP_OFFSET,
    DP_TEST_PARSE_TCP_HEADER_BEYOND_PAYLOAD,
    DP_TEST_PARSE_UDP_TRUNCATED,
    DP_TEST_PARSE_INVALID_UDP_LENGTH,
    DP_TEST_PARSE_UDP_BEYOND_PAYLOAD,
    DP_TEST_PARSE_MULTI_SEGMENT,
};

/* deterministic fixture 只供 package 内单元测试调用，不进入生产控制面。 */
int dp_test_parse_fixture(int fixture, struct dp_packet_meta *meta);

/* 在真实 EAL allocator 上覆盖 rte_hash、rte_lpm、action 与 mbuf ownership。 */
int dp_test_static_lookup_actions(void);
int dp_test_dynamic_rules_qsbr(void);

#endif
