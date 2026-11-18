/*
 * libc.c - 使用者程式的小型 C 函式庫(說明見 libc.h)
 */
#include "libc.h"

/* ------------------------------------------------------------------ */
/* 字串                                                                */
/* ------------------------------------------------------------------ */

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
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {                                     /* 重疊而且目的在後面:從尾端開始搬 */
        while (n--)
            d[n] = s[n];
    }
    return dst;
}

int atoi(const char *s)
{
    int v = 0, neg = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

void gmtime_(unsigned long long epoch, int offset_hours, struct datetime *out)
{
    /* 先轉成 32 位元再做除法(i386 沒有 64 位元除法可用);可以表示到西元 2106 年 */
    unsigned long t = (unsigned long)epoch + (unsigned long)(offset_hours * 3600);
    unsigned long days = t / 86400, rem = t % 86400;

    out->hour = (int)(rem / 3600);
    out->min = (int)(rem % 3600 / 60);
    out->sec = (int)(rem % 60);
    out->wday = (int)((days + 4) % 7);           /* 1970-01-01 是星期四 */

    /* 天數換算成年月日(公曆;算法出自 Howard Hinnant 的 civil_from_days)。
       把一年當成從 3 月開始:閏日就落在一年的最後一天,規則變得單純 */
    unsigned long z = days + 719468;
    unsigned long era = z / 146097, doe = z - era * 146097;
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;

    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year = (int)(yoe + era * 400 + (out->mon <= 2));
}

void exit(int code)
{
    sys_exit(code);
}

void abort(void)
{
    sys_exit(134);
}

/* ------------------------------------------------------------------ */
/* 格式化輸出                                                          */
/* ------------------------------------------------------------------ */

/* 輸出的去處:一塊緩衝區;滿了的話,有描述符就先寫出去,沒有(snprintf)就丟掉多的 */
struct out {
    char *buf;
    size_t cap, len;                             /* len:緩衝區裡目前有幾個字元 */
    size_t total;                                /* 總共產生了幾個字元(回傳值) */
    long fd;                                     /* -1 = 只寫進緩衝區 */
};

static void out_char(struct out *o, char c)
{
    o->total++;
    if (o->len == o->cap) {
        if (o->fd < 0)
            return;
        sys_write(o->fd, o->buf, o->len);
        o->len = 0;
    }
    o->buf[o->len++] = c;
}

/* 把 64 位元的數字除以 10:回傳商,*rem 得到餘數。
   不直接寫 v / 10:i386 上 64 位元的除法需要編譯器的輔助函式(libgcc),這裡沒有連結。
   做法是每次處理 16 個位元,被除數因此不超過 32 位元,只需要一般的除法指令 */
static unsigned long long div10(unsigned long long v, unsigned *rem)
{
    unsigned long long q = 0;
    unsigned r = 0;

    for (int shift = 48; shift >= 0; shift -= 16) {
        unsigned part = (r << 16) | (unsigned)((v >> shift) & 0xFFFF);
        q |= (unsigned long long)(part / 10) << shift;
        r = part % 10;
    }
    *rem = r;
    return q;
}

static void out_number(struct out *o, unsigned long long v, unsigned base, int upper, int neg,
                       int width, int left, int zero)
{
    char tmp[24];
    int n = 0;

    do {
        unsigned digit;
        if (base == 16) {
            digit = (unsigned)(v & 15);
            v >>= 4;
        } else {
            v = div10(v, &digit);
        }
        tmp[n++] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
    } while (v);

    int pad = width - n - neg;
    if (!left && !zero)
        for (; pad > 0; pad--)
            out_char(o, ' ');
    if (neg)
        out_char(o, '-');
    if (!left && zero)
        for (; pad > 0; pad--)
            out_char(o, '0');
    while (n > 0)
        out_char(o, tmp[--n]);
    for (; pad > 0; pad--)                       /* 靠左對齊:補在後面 */
        out_char(o, ' ');
}

static void format(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(o, *fmt);
            continue;
        }
        int left = 0, zero = 0, width = 0, longs = 0;

        fmt++;
        for (; *fmt == '-' || *fmt == '0'; fmt++) {
            if (*fmt == '-')
                left = 1;
            else
                zero = 1;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (width < 0) {
            left = 1;
            width = -width;
        }
        for (; *fmt == 'l' || *fmt == 'z'; fmt++)
            longs += *fmt == 'z' ? 1 : 1;

        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = longs >= 2 ? va_arg(ap, long long) : longs == 1 ? va_arg(ap, long)
                                                                          : va_arg(ap, int);
            unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            out_number(o, u, 10, 0, v < 0, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long u = longs >= 2 ? va_arg(ap, unsigned long long)
                                 : longs == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            out_number(o, u, *fmt == 'u' ? 10 : 16, *fmt == 'X', 0, width, left, zero);
            break;
        }
        case 'p':
            out_char(o, '0');
            out_char(o, 'x');
            out_number(o, (unsigned long)va_arg(ap, void *), 16, 0, 0, 0, 0, 0);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            for (int pad = width - 1; !left && pad > 0; pad--)
                out_char(o, ' ');
            out_char(o, c);
            for (int pad = width - 1; left && pad > 0; pad--)
                out_char(o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int pad = width - (int)strlen(s);
            for (; !left && pad > 0; pad--)
                out_char(o, ' ');
            while (*s)
                out_char(o, *s++);
            for (; left && pad > 0; pad--)
                out_char(o, ' ');
            break;
        }
        case '%':
            out_char(o, '%');
            break;
        case '\0':
            return;                              /* 格式字串結束在 % 之後 */
        default:                                 /* 不認得的:原樣印出來,方便發現寫錯的格式 */
            out_char(o, '%');
            out_char(o, *fmt);
        }
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o = { buf, cap ? cap - 1 : 0, 0, 0, -1 };     /* 留一格給結尾的 0 */

    format(&o, fmt, ap);
    if (cap)
        buf[o.len] = 0;
    return (int)o.total;                         /* 和標準相同:回傳「完整輸出會有多長」 */
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

static int vfdprintf(long fd, const char *fmt, va_list ap)
{
    char buf[256];                               /* 通常整段一次寫出,不會和別的行程的輸出交錯 */
    struct out o = { buf, sizeof(buf), 0, 0, fd };

    format(&o, fmt, ap);
    if (o.len)
        sys_write(fd, buf, o.len);
    return (int)o.total;
}

int printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vfdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(long fd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vfdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    return printf("%s\n", s);
}

int putchar(int c)
{
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

/* ------------------------------------------------------------------ */
/* malloc                                                              */
/* ------------------------------------------------------------------ */

/*
 * heap 是一段連續的記憶體,從 kernel 給的起點往後長(sys_sbrk)。裡面一塊接一塊,
 * 每一塊前面有一個小標頭,記著這一塊多大、是不是空的:
 *
 *   [標頭|......使用中......][標頭|...空的...][標頭|....使用中....]  <- heap 的結尾
 *
 * malloc:從頭找第一個夠大的空塊(first fit)。太大就切成兩塊;都不夠就向 kernel 再要一段。
 * free  :標記為空的,並和後面相鄰的空塊合併(前面的在下一次 malloc 掃過時合併)。
 *         最後一塊如果是空的而且夠大,把它還給 kernel。
 *
 * 簡單而不快:每次配置都從頭掃。對這裡的小程式足夠;重點是正確與能偵測錯誤:
 * 標頭裡有一個固定的記號,free 時檢查。釋放不是 malloc 給的指標、或同一塊釋放兩次,
 * 都會被發現並立刻終止程式,而不是悄悄弄壞 heap(那種錯誤要很久以後才會以奇怪的方式出現)。
 *
 * 多條執行緒可以同時呼叫:整個 heap 由一把鎖保護。
 */
struct block {
    size_t size;                                 /* 這一塊可用的位元組數(不含標頭) */
    unsigned magic;                              /* BLOCK_USED 或 BLOCK_FREE */
} __attribute__((aligned(16)));                  /* 標頭固定 16 位元組(32 位元的架構上也是):
                                                    標頭之後的位址才會對齊 16,這是各架構對 malloc 的要求 */
_Static_assert(sizeof(struct block) == 16, "malloc block header must be 16 bytes");
#define BLOCK_USED 0xA110C8EDu
#define BLOCK_FREE 0xF4EEB10Cu
#define ALIGN16(n) (((n) + 15) & ~(size_t)15)
#define GROW_MIN   (64 * 1024)                   /* 每次至少向 kernel 要這麼多,減少系統呼叫 */
#define TRIM_MIN   (256 * 1024)                  /* 尾端的空塊超過這麼大才歸還 */

static struct block *heap_first;                 /* 第一塊;NULL = heap 還沒建立 */
static char *heap_end;                           /* 最後一塊之後的位址 = 目前的 break */
static umutex_t heap_lock;

static struct block *next_block(struct block *b)
{
    return (struct block *)((char *)(b + 1) + b->size);
}

static void heap_corrupt(const char *what)
{
    fprintf(2, "malloc: %s\n", what);
    abort();
}

void *malloc(size_t size)
{
    struct block *b, *last = NULL;

    if (size == 0)
        size = 1;
    if (size > ((size_t)1 << 30))
        return NULL;
    size = ALIGN16(size);

    umutex_lock(&heap_lock);
    for (b = heap_first; b && (char *)b < heap_end; b = next_block(b)) {
        if (b->magic != BLOCK_USED && b->magic != BLOCK_FREE)
            heap_corrupt("heap is corrupted (a block header was overwritten)");
        if (b->magic == BLOCK_FREE) {
            /* 順便把後面相鄰的空塊併進來 */
            for (struct block *n = next_block(b); (char *)n < heap_end && n->magic == BLOCK_FREE;
                 n = next_block(b))
                b->size += sizeof(*n) + n->size;
            if (b->size >= size)
                break;
        }
        last = b;
    }
    if (!b || (char *)b >= heap_end) {
        /* 沒有夠大的空塊:向 kernel 再要一段,接在最後(最後一塊是空的就直接加大它) */
        size_t have = last && last->magic == BLOCK_FREE ? last->size + sizeof(*last) : 0;
        size_t need = sizeof(*b) + size - have;
        size_t grow = need < GROW_MIN ? GROW_MIN : ALIGN16(need);
        long old = sys_sbrk((long)grow);
        if (old < 0) {
            umutex_unlock(&heap_lock);
            return NULL;                         /* 記憶體不足 */
        }
        if (!heap_first)
            heap_first = (struct block *)old;
        heap_end = (char *)old + grow;
        if (have) {
            b = last;
            b->size += grow;
        } else {
            b = (struct block *)old;
            b->size = grow - sizeof(*b);
        }
        b->magic = BLOCK_FREE;
    }
    /* 夠大的話切成兩塊,後半留著給別人用 */
    if (b->size >= size + sizeof(*b) + 32) {
        struct block *rest = (struct block *)((char *)(b + 1) + size);
        rest->size = b->size - size - sizeof(*rest);
        rest->magic = BLOCK_FREE;
        b->size = size;
    }
    b->magic = BLOCK_USED;
    umutex_unlock(&heap_lock);
    return b + 1;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = (struct block *)ptr - 1;

    umutex_lock(&heap_lock);
    if ((char *)b < (char *)heap_first || (char *)ptr >= heap_end || ((size_t)ptr & 15))
        heap_corrupt("free() of a pointer that did not come from malloc()");
    if (b->magic == BLOCK_FREE)
        heap_corrupt("the same block was freed twice");
    if (b->magic != BLOCK_USED)
        heap_corrupt("free() of an invalid pointer, or its header was overwritten");
    b->magic = BLOCK_FREE;
    for (struct block *n = next_block(b); (char *)n < heap_end && n->magic == BLOCK_FREE;
         n = next_block(b))
        b->size += sizeof(*n) + n->size;
    /* 這一塊在最尾端而且很大:還給 kernel,留下一小塊 */
    if ((char *)next_block(b) == heap_end && b->size >= TRIM_MIN) {
        size_t give = (b->size - GROW_MIN) & ~(size_t)4095;
        if (sys_sbrk(-(long)give) >= 0) {
            b->size -= give;
            heap_end -= give;
        }
    }
    umutex_unlock(&heap_lock);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > ((size_t)1 << 30) / size)
        return NULL;                             /* 相乘會太大(或溢位) */
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    struct block *b = (struct block *)ptr - 1;
    if (b->magic != BLOCK_USED)
        heap_corrupt("realloc() of an invalid pointer");
    if (size <= b->size)
        return ptr;                              /* 原本就夠大 */
    void *p = malloc(size);
    if (!p)
        return NULL;
    memcpy(p, ptr, b->size);
    free(ptr);
    return p;
}
