#ifndef DP_BINDING_H
#define DP_BINDING_H

#include <stddef.h>

/* 这些 helper 只服务 cgo argv 装配，把 C 数组布局细节留在 C 中。 */
char **dp_argv_alloc(size_t count);
void dp_argv_set(char **argv, size_t index, char *value);
void dp_argv_free_array(char **argv);

#endif
