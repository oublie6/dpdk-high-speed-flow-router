#ifndef DP_API_H
#define DP_API_H

struct dp_runtime_info {
    int initialized;
    unsigned int main_lcore;
    unsigned int lcore_count;
    const char *version; /* Borrowed static string; never free. */
};

/* Single process, single init attempt. Serialize on the same OS thread.
 * argv is mutable C-owned memory and must live until cleanup (or process exit
 * after failed init). Errors are negative errno values; success is zero.
 */
/* argc includes the program name. argv has argc valid strings plus a NULL
 * terminator; EAL may modify both strings and array order. The caller owns all
 * allocations. A negative result is terminal for this process.
 */
int dp_runtime_init(int argc, char **argv);

/* Fill caller-owned output only while initialized. Copy the borrowed version
 * string before cleanup if it must survive the EAL lifetime.
 */
int dp_runtime_get_info(struct dp_runtime_info *info);

/* Call once after successful init, even if a later info read fails. After this
 * call (success or failure), no further DPDK calls are permitted.
 */
int dp_runtime_cleanup(void);

#endif
