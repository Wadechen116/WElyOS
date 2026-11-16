#ifndef WELYOS_KSTRING_H
#define WELYOS_KSTRING_H
#include <stddef.h>

/* 精簡版字串/記憶體函式(實作在 lib/string.c)。
 * 編譯器可能隱含產生 memcpy/memset 呼叫,所以這些符號必須存在。 */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
#endif
