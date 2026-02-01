#include <lib/types.h>
#include <lib/debug.h>
#include <string.h>

void *
memset(void *s, int c, size_t n)
{
    char *p = s;
    while (n-- > 0)
        *p++ = c;
    return s;
} 