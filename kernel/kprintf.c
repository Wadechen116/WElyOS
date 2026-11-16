/*
 * kprintf.c - kernel 的格式化輸出
 *
 * 沒有 libc 可用,所以自行實作精簡版 printf。<stdarg.h>、<stdint.h>、
 * <stddef.h> 是編譯器內建的 freestanding 標頭,不需要作業系統支援。
 *
 * 注意:i386 沒有 32 位元版 libgcc,64 位元的 / 與 % 會呼叫不存在的
 * __udivdi3,因此十進位轉換改用自行實作的位移-相減除法(udivmod)。
 */
#include <stdint.h>
#include <stddef.h>
#include "arch.h"
#include "kprintf.h"

/* 輸出單一字元。回傳 1(輸出字元數) */
static int out_char(char c)
{
    if (c == '\n')
        arch_putc('\r');        /* 序列終端機需要 CRLF */
    arch_putc(c);
    return 1;
}

/* 64 位元無號除法(商與餘數),只用位移與比較,不產生除法指令 */
static void udivmod(uint64_t v, unsigned base, uint64_t *q, unsigned *r)
{
    uint64_t quo = 0, rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((v >> i) & 1);
        if (rem >= base) {
            rem -= base;
            quo |= (uint64_t)1 << i;
        }
    }
    *q = quo;
    *r = (unsigned)rem;
}

/*
 * 輸出數字。v 為絕對值,neg 表示原值為負(要補 '-')。
 *   base  進位(10 或 16);upper 為 1 時十六進位用大寫
 *   width 最小寬度,不足以 pad 補齊(' ' 靠右對齊,'0' 補零)
 * 回傳輸出的字元數。
 */
static int out_num(uint64_t v, unsigned base, int upper, int width,
                   char pad, int neg)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    int n = 0, count = 0;

    /* 由低位到高位取出各位數(順序是反的),do-while 保證 v=0 也印 "0" */
    do {
        if (base == 16) {                /* 16 進位用位移即可,較快 */
            buf[n++] = digits[v & 15];
            v >>= 4;
        } else {
            unsigned r;
            udivmod(v, base, &v, &r);
            buf[n++] = digits[r];
        }
    } while (v);

    /* 補零時負號要在零之前(-0007);補空白時負號緊貼數字(   -7) */
    if (neg && pad == '0')
        count += out_char('-');
    for (int i = n + (neg ? 1 : 0); i < width; i++)
        count += out_char(pad);
    if (neg && pad != '0')
        count += out_char('-');
    while (n)                            /* 倒序輸出,恢復正確順序 */
        count += out_char(buf[--n]);
    return count;
}

/*
 * 核心格式化函式。格式規格:%[0][寬度][l|ll|z]轉換字元
 */
int kvprintf(const char *fmt, va_list ap)
{
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            count += out_char(*fmt);
            continue;
        }
        fmt++;                           /* 跳過 '%',開始解析規格 */

        char pad = ' ';
        int width = 0, lcount = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {   /* l=1, ll=2;z 視為 long */
            lcount++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            count += out_char((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            for (int i = len; i < width; i++)
                count += out_char(' ');
            while (*s)
                count += out_char(*s++);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v;
            if (lcount >= 2)
                v = va_arg(ap, long long);
            else if (lcount == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);
            /* 0 - v 以無號運算取絕對值,最小負值也不會溢位 */
            uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            count += out_num(u, 10, 0, width, pad, v < 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            if (lcount >= 2)
                v = va_arg(ap, unsigned long long);
            else if (lcount == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            count += out_num(v, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, pad, 0);
            break;
        }
        case 'p':
            count += out_char('0');
            count += out_char('x');
            count += out_num((uint64_t)(uintptr_t)va_arg(ap, void *),
                             16, 0, (int)(2 * sizeof(void *)), '0', 0);
            break;
        case '%':
            count += out_char('%');
            break;
        case '\0':
            return count;                /* 格式字串以 % 結尾 */
        default:                         /* 未知轉換:原樣輸出 */
            count += out_char('%');
            count += out_char(*fmt);
            break;
        }
    }
    return count;
}

/* 可變參數版本:包裝 kvprintf */
int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
