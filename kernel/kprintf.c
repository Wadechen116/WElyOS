#include <stdint.h>
#include <stddef.h>
#include "kprintf.h"
#include "serial.h"

static int out_char(char c)
{
    if (c == '\n')
        serial_putc('\r');      /* 序列終端機需要 CRLF */
    serial_putc(c);
    return 1;
}

/* 輸出無號數:base 進位、至少 width 位、不足時補 pad */
static int out_num(uint32_t v, unsigned base, int upper, int width,
                   char pad, int neg)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int n = 0, count = 0;

    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);

    if (neg && pad == '0')
        count += out_char('-');
    for (int i = n + (neg ? 1 : 0); i < width; i++)
        count += out_char(pad);
    if (neg && pad != '0')
        count += out_char('-');
    while (n)
        count += out_char(buf[--n]);
    return count;
}

int kvprintf(const char *fmt, va_list ap)
{
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            count += out_char(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l')                 /* i386 上 long 與 int 同為 32 位元 */
            fmt++;

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
            int32_t v = va_arg(ap, int32_t);
            uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
            count += out_num(u, 10, 0, width, pad, v < 0);
            break;
        }
        case 'u':
            count += out_num(va_arg(ap, uint32_t), 10, 0, width, pad, 0);
            break;
        case 'x':
            count += out_num(va_arg(ap, uint32_t), 16, 0, width, pad, 0);
            break;
        case 'X':
            count += out_num(va_arg(ap, uint32_t), 16, 1, width, pad, 0);
            break;
        case 'p':
            count += out_char('0');
            count += out_char('x');
            count += out_num((uint32_t)(uintptr_t)va_arg(ap, void *),
                             16, 0, 8, '0', 0);
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

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
