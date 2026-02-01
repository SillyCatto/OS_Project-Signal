#include <lib/types.h>
#include <lib/debug.h>
#include <string.h>

int
strncmp(const char *p, const char *q, size_t n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (int)((unsigned char)*p - (unsigned char)*q);
} 