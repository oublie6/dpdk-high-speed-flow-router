#ifndef DP_TEST_H
#define DP_TEST_H

/* package 内测试钩子：模拟 partial return 并验证 mbuf ownership。
 * 它不属于项目公共 runtime API，也不会进入 packet hot path。
 */
int dp_test_tx_partial_ownership(void);

enum dp_test_live_resource {
    DP_TEST_LIVE_WORKER = 1,
    DP_TEST_LIVE_PORT = 2,
    DP_TEST_LIVE_MEMPOOL = 3,
};

/* 临时构造一种存活资源并直接调用 cleanup；返回值应为 -EBUSY。 */
int dp_test_runtime_cleanup_guard(int resource);

#endif
