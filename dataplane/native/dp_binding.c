#include "dp_binding.h"

#include <stdlib.h>

char **dp_argv_alloc(size_t count)
{
    return calloc(count, sizeof(char *));
}

void dp_argv_set(char **argv, size_t index, char *value)
{
    argv[index] = value;
}

void dp_argv_free_array(char **argv)
{
    free(argv);
}
