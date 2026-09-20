#ifndef DP_TEST_H
#define DP_TEST_H

/* package 内测试钩子：模拟 partial return 并验证 mbuf ownership。
 * 它不属于项目公共 runtime API，也不会进入 packet hot path。
 */
int dp_test_tx_partial_ownership(void);

#endif
