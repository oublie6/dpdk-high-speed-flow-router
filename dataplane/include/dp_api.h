#ifndef DP_API_H
#define DP_API_H

struct dp_runtime_info {
    int initialized;
    unsigned int main_lcore;
    unsigned int lcore_count;
    const char *version; /* 借用 DPDK 的静态字符串，不允许 caller free。 */
};

/* 当前约束：单进程只允许一次 EAL init attempt。
 * argv 必须是可修改的 C-owned memory，并至少活到 cleanup；
 * init 失败时由进程退出统一回收。
 * 错误统一使用 negative errno，成功返回 0。
 *
 * argc 包含程序名；argv 包含 argc 个有效字符串以及末尾 NULL。
 * EAL 允许修改字符串和 pointer 顺序，allocation ownership 仍属于 caller。
 */
int dp_runtime_init(int argc, char **argv);

/* 仅在 initialized 状态读取 runtime info。
 * version 是 borrowed pointer；如果需要跨 cleanup 保存，caller 必须先复制。
 */
int dp_runtime_get_info(struct dp_runtime_info *info);

/* 成功 init 后调用一次。
 * Goal 001 约束 cleanup 后无论成功失败，都不再允许继续调用 DPDK。
 */
int dp_runtime_cleanup(void);

#endif
