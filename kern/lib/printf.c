#include <lib/types.h>
#include <lib/debug.h>
#include <string.h>

int
vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct sprintbuf b = {buf, buf+n-1, 0};

    if (buf == NULL || n < 1)
        return -E_INVAL;

    // print the string to the buffer
    vprintfmt((void*)sprintputch, &b, fmt, ap);

    // null terminate the buffer
    *b.buf = '\0';

    return b.cnt;
} 