/*
 * string.c - 最小字串與記憶體函式
 *
 * memcpy / memset 是 kernel 裡最常被呼叫、處理資料量最大的函式(清空頁面、
 * 複製程式映像、建立頁表),所以值得做快一點:位址對齊時一次處理一個
 * 機器字組(32 位元架構 4 位元組、64 位元架構 8 位元組),而不是一個位元組。
 *
 * 注意:CMake 加了 -fno-tree-loop-distribute-patterns,避免編譯器把這裡的
 * 迴圈又「最佳化」成對 memcpy/memset 自己的呼叫(無限遞迴)。
 */
#include <stdint.h>
#include "kstring.h"

/*
 * may_alias:告訴編譯器「這個型別的指標可能指向任何型別的資料」。
 * C 規定不同型別的指標不會指向同一塊記憶體(strict aliasing),編譯器會依此
 * 做最佳化;用 unsigned long * 去讀寫別人的資料若不加這個屬性,結果可能出錯。
 */
typedef unsigned long __attribute__((may_alias)) word_t;
#define WSIZE sizeof(word_t)
#define WMASK (WSIZE - 1)

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    /* 兩個位址相對於字組邊界的偏移相同,才有可能同時對齊。
       不能假設未對齊的字組存取可行:AArch64 在 MMU 開啟前會因此出錯 */
    if ((((uintptr_t)d ^ (uintptr_t)s) & WMASK) == 0) {
        while (n && ((uintptr_t)d & WMASK)) {            /* 先逐位元組走到對齊 */
            *d++ = *s++;
            n--;
        }
        word_t *dw = (word_t *)d;
        const word_t *sw = (const word_t *)s;
        for (; n >= WSIZE; n -= WSIZE)                   /* 主體:一次一個字組 */
            *dw++ = *sw++;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--)                                          /* 剩下的尾巴(或無法對齊時的全部) */
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {                     /* 目的在前:由前往後複製,memcpy 正是這個方向 */
        return memcpy(dst, src, n);
    } else if (d > s) {              /* 目的在後且可能重疊:由後往前複製 */
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;

    while (n && ((uintptr_t)d & WMASK)) {                /* 先逐位元組走到對齊 */
        *d++ = b;
        n--;
    }
    /* 把一個位元組複製成整個字組:0xAB -> 0xABAB...AB。
       ~0UL / 0xFF 是 0x0101...01,乘上位元組的值即可 */
    word_t w = (word_t)b * ((word_t)~0UL / 0xFF);
    word_t *dw = (word_t *)d;
    for (; n >= WSIZE; n -= WSIZE)
        *dw++ = w;
    d = (unsigned char *)dw;
    while (n--)
        *d++ = b;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            break;
    }
    return 0;
}
