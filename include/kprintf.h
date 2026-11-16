#ifndef WELYOS_KPRINTF_H
#define WELYOS_KPRINTF_H
#include <stdarg.h>

/*
 * kernel 版 printf,透過 arch_putc 輸出到主控台。回傳輸出的字元數。
 * 格式: %[0][寬度][l|ll|z]轉換字元
 * 轉換字元: c s d i u x X p %
 *   l / ll / z 分別表示 long / long long / size_t 大小的整數
 *   %p 輸出 0x + 指標寬度的十六進位(32 位元 8 位、64 位元 16 位)
 * 不支援: 浮點、精度
 */
int kprintf(const char *fmt, ...);
int kvprintf(const char *fmt, va_list ap);   /* 已有 va_list 時使用(例如包裝 panic) */
#endif
