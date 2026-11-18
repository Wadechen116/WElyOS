#ifndef WELYOS_LIBC_H
#define WELYOS_LIBC_H
/*
 * libc.h - 使用者程式的小型 C 函式庫(實作在 libc.c,每個程式都會連結)
 *
 * 名稱與行為沿用標準 C 函式庫中最常用的一小部分,所以一般的 C 程式寫法可以直接用:
 *   記憶體   malloc / calloc / realloc / free      (建立在 sys_sbrk 之上)
 *   輸出     printf / fprintf / snprintf / puts / putchar
 *   字串     strlen / strcmp / strncmp / strcpy / strncpy / strchr / strrchr /
 *            memcmp / memmove / memcpy / memset / atoi
 *   其他     exit / abort
 * 沒有用到的函式不會被連結進程式(每個函式各自一個區段,連結器把沒人用的丟掉)。
 *
 * 和標準不同的地方:
 *   - 檔案以「檔案描述符」(整數)表示,沒有 FILE *;fprintf 的第一個參數是描述符
 *   - printf 不緩衝:每次呼叫組好一整段就寫出去
 *   - 支援的格式:%d %i %u %x %X %c %s %p %%,長度修飾 l / ll / z,
 *     旗標 - 與 0,以及寬度(可以用 *)。不支援浮點數
 */
#include <stdarg.h>
#include <stddef.h>
#include "usys.h"

void *malloc(size_t size);               /* 失敗回傳 NULL */
void *calloc(size_t count, size_t size); /* 內容清為 0;相乘溢位時回傳 NULL */
void *realloc(void *ptr, size_t size);   /* 失敗回傳 NULL,原本的區塊不動 */
void  free(void *ptr);                   /* NULL 沒有作用;重複釋放會被偵測到並終止程式 */

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(long fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int puts(const char *s);                 /* 輸出字串加上換行 */
int putchar(int c);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
int    memcmp(const void *a, const void *b, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memcpy(void *dst, const void *src, unsigned long n);      /* 定義在 crt0.c */
void  *memset(void *dst, int c, unsigned long n);                /* 定義在 crt0.c */
int    atoi(const char *s);

/* 日期時間:把「1970-01-01 00:00:00 UTC 至今的秒數」換成年月日時分秒。
   offset_hours 是和 UTC 的時差(台灣是 +8);wday 為星期幾(0 = 星期日) */
struct datetime { int year, mon, day, hour, min, sec, wday; };
void gmtime_(unsigned long long epoch, int offset_hours, struct datetime *out);

void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));  /* 以結束碼 134 結束(128 + SIGABRT 的 6,Unix 的慣例) */
#endif
