#ifndef WELYOS_ULIB_H
#define WELYOS_ULIB_H
/*
 * ulib.h - 使用者程式共用的小工具
 *
 * 使用者程式沒有 C 函式庫可用(printf、strlen 這些都沒有),一切自己來。
 * 這裡只放各程式都會用到的幾個函式,全部是 static inline:
 * 每個程式只會帶走它實際用到的部分。
 */
#include "usys.h"

static inline unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* 十進位字串轉成數字(不處理正負號與溢位) */
static inline long uatoi(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* 輸出字串到標準輸出(1 號檔案描述符) */
static inline void puts_(const char *s)
{
    sys_write(1, s, ustrlen(s));
}

/* 把字串與十進位數字組進緩衝區,最後一次輸出,避免多條執行緒的輸出互相穿插 */
struct line {
    char buf[160];
    unsigned len;
};

static inline void line_str(struct line *l, const char *s)
{
    while (*s && l->len < sizeof(l->buf) - 1)
        l->buf[l->len++] = *s++;
}

static inline void line_num(struct line *l, long v)
{
    char tmp[24];
    int n = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

    if (v < 0)
        line_str(l, "-");
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n > 0 && l->len < sizeof(l->buf) - 1)
        l->buf[l->len++] = tmp[--n];
}

static inline void line_out(struct line *l)
{
    sys_write(1, l->buf, l->len);
    l->len = 0;
}
#endif
