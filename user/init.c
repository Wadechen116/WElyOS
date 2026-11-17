/*
 * init.c - 內建的使用者程式
 *
 * 這個檔案編譯後在「使用者模式」執行:沒有特權、看不到 kernel 的記憶體,
 * 只能透過 usys.h 的系統呼叫與 kernel 互動。它沒有連結 kernel 的任何程式碼,
 * 連 kprintf 都沒有,輸出要自己組字串再呼叫 sys_write。
 *
 * kernel 以不同的 mode 參數啟動它,用來測試使用者模式的各種行為。
 * mode 的數值必須與 kernel/main.c 的 UMODE_* 一致。
 */
#include "usys.h"

enum {
    UMODE_HELLO = 0,     /* 正常執行:輸出、多執行緒、結束碼 */
    UMODE_READ_KERNEL,   /* 讀取 kernel 的記憶體 */
    UMODE_WRITE_TEXT,    /* 寫入自己的程式碼 */
    UMODE_PRIVILEGED,    /* 執行特權指令 */
    UMODE_NULL,          /* 解參考 NULL */
    UMODE_BAD_POINTER,   /* 把 kernel 的位址傳給系統呼叫 */
    UMODE_SPIN,          /* 無窮迴圈,永不讓出 CPU */
    UMODE_STACK,         /* 使用者 stack 溢位 */
    UMODE_ISOLATION,     /* 檢查自己的資料不受其他行程影響 */
    UMODE_BAD_SYSCALL,   /* 不存在的系統呼叫編號 */
    UMODE_FUTEX,         /* 多執行緒以使用者模式的 mutex 保護共用資料 */
};

/* 各架構 kernel 映像所在的位址(使用者模式不應該讀得到) */
#if defined(__i386__)
#define KERNEL_ADDR 0x00100000UL
#elif defined(__riscv)
#define KERNEL_ADDR 0x80200000UL
#else
#define KERNEL_ADDR 0x40080000UL
#endif

/*
 * 編譯器在最佳化時可能自行產生對 memset / memcpy 的呼叫(例如把結構清為 0)。
 * 使用者程式沒有連結任何函式庫,也不能用 kernel 裡的那一份,所以要自己提供。
 */
void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* ---- 極簡的輸出函式:使用者程式沒有 C 函式庫,一切自己來 ---- */

static unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static void puts_(const char *s)
{
    sys_write(s, ustrlen(s));
}

/* 把字串與十進位數字組進緩衝區,最後一次輸出,避免多條執行緒的輸出互相穿插 */
struct line {
    char buf[96];
    unsigned len;
};

static void line_str(struct line *l, const char *s)
{
    while (*s && l->len < sizeof(l->buf) - 1)
        l->buf[l->len++] = *s++;
}

static void line_num(struct line *l, long v)
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

static void line_out(struct line *l)
{
    sys_write(l->buf, l->len);
    l->len = 0;
}

/* ---- UMODE_HELLO ---- */

static volatile long worker_result[2];     /* 兩條執行緒各自回報的結果 */
static volatile int worker_done;

static void worker(long idx)
{
    /* 做一點計算:1 加到 100*(idx+1) */
    long sum = 0;
    for (long i = 1; i <= 100 * (idx + 1); i++)
        sum += i;
    worker_result[idx] = sum;

    struct line l = { .len = 0 };
    line_str(&l, "  [user] thread tid=");
    line_num(&l, sys_gettid());
    line_str(&l, " computed ");
    line_num(&l, sum);
    line_str(&l, "\n");
    line_out(&l);

    worker_done++;                           /* 示範用;兩條執行緒不會同時執行到這裡以外的共用資料 */
    sys_thread_exit();
}

static int mode_hello(void)
{
    struct line l = { .len = 0 };

    line_str(&l, "  [user] hello from user mode, pid=");
    line_num(&l, sys_getpid());
    line_str(&l, "\n");
    line_out(&l);

    /* 同一個行程內再建立兩條執行緒:它們共用這個行程的記憶體(全域變數) */
    if (sys_thread_create(worker, 0) < 0 || sys_thread_create(worker, 1) < 0)
        return 1;
    while (worker_done < 2)
        sys_sleep(1);

    /* 5050 + 20100 = 25150 */
    line_str(&l, "  [user] both threads finished, total=");
    line_num(&l, worker_result[0] + worker_result[1]);
    line_str(&l, "\n");
    line_out(&l);
    return worker_result[0] + worker_result[1] == 25150 ? 42 : 2;
}

/* ---- UMODE_FUTEX ---- */

#define FUTEX_THREADS 3
#define FUTEX_ROUNDS  20000

static umutex_t counter_lock;
static volatile long shared_counter;         /* 受 counter_lock 保護 */
static volatile int finished;                /* 完成的執行緒數;也當作 futex 讓 main 等待 */

static void futex_worker(long idx)
{
    (void)idx;
    for (int i = 0; i < FUTEX_ROUNDS; i++) {
        umutex_lock(&counter_lock);
        long tmp = shared_counter;           /* 讀出、加 1、寫回:沒有鎖的話會互相覆蓋 */
        shared_counter = tmp + 1;
        umutex_unlock(&counter_lock);
    }
    __sync_fetch_and_add(&finished, 1);
    sys_futex_wake(&finished);
    sys_thread_exit();
}

static int mode_futex(void)
{
    for (long i = 0; i < FUTEX_THREADS; i++)
        if (sys_thread_create(futex_worker, i) < 0)
            return 1;

    /* 睡在 finished 上,每有一條執行緒完成才被叫醒一次,不需要輪詢 */
    int n;
    while ((n = finished) < FUTEX_THREADS)
        sys_futex_wait(&finished, n);

    struct line l = { .len = 0 };
    line_str(&l, "  [user] 3 threads x 20000 locked increments = ");
    line_num(&l, shared_counter);
    line_str(&l, "\n");
    line_out(&l);
    return shared_counter == (long)FUTEX_THREADS * FUTEX_ROUNDS ? 11 : 1;
}

/* ---- 故意犯錯的幾種模式 ---- */

static long recurse(long depth)
{
    volatile char pad[256];
    for (unsigned i = 0; i < sizeof(pad); i++)
        pad[i] = (char)depth;
    if (depth > 1000000)                     /* 遠超過 stack 能容納的深度;實際上到不了 */
        return 0;
    return recurse(depth + 1) + pad[0];
}

static volatile long isolation_value;        /* 每個行程都有自己的一份,位於相同的虛擬位址 */

static int mode_isolation(void)
{
    long pid = sys_getpid();

    isolation_value = pid * 1000 + 7;
    for (int i = 0; i < 5; i++) {
        sys_sleep(1);                        /* 期間其他行程也在寫「同一個位址」 */
        if (isolation_value != pid * 1000 + 7)
            return 1;                        /* 被別的行程改到了:隔離失敗 */
    }
    return 0;
}

void _start(long mode);

static int umain(long mode)
{
    switch (mode) {
    case UMODE_HELLO:
        return mode_hello();

    case UMODE_READ_KERNEL:
        puts_("  [user] reading kernel memory...\n");
        return (int)*(volatile long *)KERNEL_ADDR;       /* 應在這裡被終止 */

    case UMODE_WRITE_TEXT:
        puts_("  [user] writing to my own code...\n");
        *(volatile char *)(unsigned long)_start = 0;     /* 程式碼頁是唯讀的 */
        return 1;

    case UMODE_PRIVILEGED:
        puts_("  [user] executing a privileged instruction...\n");
#if defined(__i386__)
        __asm__ volatile("cli");                         /* 關中斷:只有 ring 0 可以 */
#elif defined(__riscv)
        __asm__ volatile("csrr t0, sstatus");            /* 讀取 S-mode 的 CSR */
#else
        __asm__ volatile("mrs x0, sctlr_el1");           /* 讀取 EL1 的系統暫存器 */
#endif
        return 1;

    case UMODE_NULL:
        puts_("  [user] dereferencing NULL...\n");
        return (int)*(volatile long *)0;

    case UMODE_BAD_POINTER: {
        /* 請 kernel 把「kernel 自己的記憶體」印出來。kernel 必須拒絕,
           否則使用者就能藉由系統呼叫讀到任何東西 */
        long r = sys_write((const char *)KERNEL_ADDR, 16);
        puts_(r < 0 ? "  [user] write(kernel address) was rejected\n"
                    : "  [user] write(kernel address) SUCCEEDED?!\n");
        return r < 0 ? 7 : 1;
    }

    case UMODE_SPIN:
        puts_("  [user] spinning forever...\n");
        for (;;)
            ;

    case UMODE_STACK:
        puts_("  [user] overflowing my stack...\n");
        return (int)recurse(0);

    case UMODE_ISOLATION:
        return mode_isolation();

    case UMODE_FUTEX:
        return mode_futex();

    case UMODE_BAD_SYSCALL:
        return syscall3(9999, 1, 2, 3) < 0 ? 9 : 1;      /* 應回傳錯誤而不是出事 */
    }
    return 3;
}

/*
 * 進入點。kernel 把 CPU 降到使用者模式後從這裡開始執行,mode 是第一個參數。
 * 放在 .text.start 區段,linker script 會把它排在程式碼的最前面。
 * umain 返回後必須呼叫 sys_exit:使用者程式沒有「上一層」可以返回。
 */
__attribute__((section(".text.start"), noreturn))
void _start(long mode)
{
    sys_exit(umain(mode));
}
