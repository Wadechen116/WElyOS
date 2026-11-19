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

/*
 * 輸出字串到標準輸出(1 號檔案描述符),分成兩種:
 *   out_str   回傳 0 = 全部寫出,-1 = 失敗(例如管線另一端已經關閉、磁碟滿了)。
 *             輸出就是「工作成果」的地方用這個,並且要檢查 — 否則 `ps > 檔案` 寫不進去時仍然回報成功
 *   puts_     盡力而為,不回報。只用在提示、用法說明、錯誤訊息:它們寫不出去時,
 *             已經沒有別的地方可以回報了
 */
static inline int out_str(const char *s)
{
    long len = (long)ustrlen(s);
    return sys_write(1, s, (unsigned long)len) == len ? 0 : -1;
}

static inline void puts_(const char *s)
{
    if (out_str(s) != 0)
        return;
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

/* 把組好的一行寫出去。line_write 回傳 0 = 全部寫出、-1 = 失敗(輸出是工作成果時用它並檢查);
   line_out 盡力而為(訊息、測試的說明文字) */
static inline int line_write(struct line *l)
{
    long len = (long)l->len;
    l->len = 0;
    return sys_write(1, l->buf, (unsigned long)len) == len ? 0 : -1;
}

static inline void line_out(struct line *l)
{
    if (line_write(l) != 0)
        return;
}
#endif
