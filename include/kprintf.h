#ifndef WELYOS_KPRINTF_H
#define WELYOS_KPRINTF_H
#include <stdarg.h>

/* 支援: %c %s %d %i %u %x %X %p %%,旗標 0、寬度、修飾 l (long) */
int kprintf(const char *fmt, ...);
int kvprintf(const char *fmt, va_list ap);
#endif
