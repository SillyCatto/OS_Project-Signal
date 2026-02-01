#ifndef _KERN_LIB_PRINTFMT_H_
#define _KERN_LIB_PRINTFMT_H_

#include <lib/types.h>
#include <lib/stdarg.h>

typedef void (*putch_t)(int, void *);

void vprintfmt(putch_t putch, void *putdat, const char *fmt, va_list ap);

#endif /* _KERN_LIB_PRINTFMT_H_ */ 