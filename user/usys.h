#ifndef WELYOS_USYS_H
#define WELYOS_USYS_H
/*
 * usys.h - 使用者程式這一側的系統呼叫介面
 *
 * 使用者程式不能直接呼叫 kernel 的函式(位址空間與權限都不同)。
 * 做法是把「呼叫編號」與參數放進約定好的暫存器,執行一條會觸發例外的
 * 特殊指令;CPU 因此切換到 kernel 模式並跳到 kernel 的例外入口,
 * kernel 依編號執行對應的功能,把結果放回暫存器後返回。
 *
 *              指令         編號   參數             回傳值
 *   i386      int $0x80    eax    ebx ecx edx      eax
 *   RISC-V    ecall        a7     a0  a1  a2       a0
 *   AArch64   svc #0       x8     x0  x1  x2       x0
 */

/* 編號必須與 kernel 的 include/proc.h 一致 */
enum {
    SYS_write = 1, SYS_exit, SYS_getpid, SYS_yield, SYS_sleep,
    SYS_thread_create, SYS_thread_exit, SYS_gettid, SYS_futex_wait, SYS_futex_wake,
    SYS_open, SYS_close, SYS_read, SYS_readdir, SYS_mkdir, SYS_unlink, SYS_stat,
    SYS_spawn, SYS_exec, SYS_wait, SYS_dup2,
};

/* 檔案相關的定義,必須與 kernel 的 include/fs.h 一致 */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define E_NOENT   2
#define E_NOEXEC  8
#define E_BADF    9
#define E_FAULT   14
#define E_BUSY    16
#define E_EXIST   17
#define E_TXTBSY  26
#define E_NOTEMPTY 39
enum { I_FILE = 1, I_DIR, I_DEV };
struct dirent { char name[60]; unsigned int type; };
struct stat { unsigned int type; unsigned int nlink; unsigned long long size; };

static inline long syscall3(long num, long a0, long a1, long a2)
{
#if defined(__i386__)
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a0), "c"(a1), "d"(a2)
                     : "memory");
    return ret;
#elif defined(__riscv)
    register long r_a0 __asm__("a0") = a0;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a7 __asm__("a7") = num;
    __asm__ volatile("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a7) : "memory");
    return r_a0;
#elif defined(__aarch64__)
    register long r_x0 __asm__("x0") = a0;
    register long r_x1 __asm__("x1") = a1;
    register long r_x2 __asm__("x2") = a2;
    register long r_x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(r_x0) : "r"(r_x1), "r"(r_x2), "r"(r_x8) : "memory");
    return r_x0;
#else
#error "unsupported architecture"
#endif
}

/* 檔案描述符:0 = 標準輸入,1 = 標準輸出,2 = 標準錯誤(行程建立時都指向主控台) */
static inline long sys_write(long fd, const void *buf, unsigned long len)
{
    return syscall3(SYS_write, fd, (long)buf, (long)len);
}
static inline long sys_read(long fd, void *buf, unsigned long len)
{
    return syscall3(SYS_read, fd, (long)buf, (long)len);
}
static inline long sys_open(const char *path, long flags)
{
    return syscall3(SYS_open, (long)path, flags, 0);
}
static inline long sys_close(long fd)             { return syscall3(SYS_close, fd, 0, 0); }
/* 讓 newfd 也指向 oldfd 所指的那次開啟(newfd 原本若開著會先被關閉) */
static inline long sys_dup2(long oldfd, long newfd) { return syscall3(SYS_dup2, oldfd, newfd, 0); }
static inline long sys_readdir(long fd, struct dirent *d)
{
    return syscall3(SYS_readdir, fd, (long)d, 0);
}
static inline long sys_mkdir(const char *path)    { return syscall3(SYS_mkdir, (long)path, 0, 0); }
static inline long sys_unlink(const char *path)   { return syscall3(SYS_unlink, (long)path, 0, 0); }
static inline long sys_stat(const char *path, struct stat *st)
{
    return syscall3(SYS_stat, (long)path, (long)st, 0);
}
__attribute__((noreturn))
static inline void sys_exit(int code)      { syscall3(SYS_exit, code, 0, 0); for (;;) ; }
static inline long sys_getpid(void)        { return syscall3(SYS_getpid, 0, 0, 0); }
static inline long sys_gettid(void)        { return syscall3(SYS_gettid, 0, 0, 0); }
static inline void sys_yield(void)         { syscall3(SYS_yield, 0, 0, 0); }
static inline long sys_sleep(long ms)      { return syscall3(SYS_sleep, ms, 0, 0); }
__attribute__((noreturn))
static inline void sys_thread_exit(void)   { syscall3(SYS_thread_exit, 0, 0, 0); for (;;) ; }
static inline long sys_thread_create(void (*entry)(long), long arg)
{
    return syscall3(SYS_thread_create, (long)entry, arg, 0);
}
/* 行程:argv 是以 NULL 結尾的字串陣列,argv[0] 依慣例是程式名稱 */
static inline long sys_spawn(const char *path, char **argv)
{
    return syscall3(SYS_spawn, (long)path, (long)argv, 0);
}
static inline long sys_exec(const char *path, char **argv)
{
    return syscall3(SYS_exec, (long)path, (long)argv, 0);
}
static inline long sys_wait(long pid)             { return syscall3(SYS_wait, pid, 0, 0); }
static inline long sys_futex_wait(volatile int *addr, int val)
{
    return syscall3(SYS_futex_wait, (long)addr, val, 0);
}
static inline long sys_futex_wake(volatile int *addr)
{
    return syscall3(SYS_futex_wake, (long)addr, 0, 0);
}

/*
 * 使用者模式的互斥鎖,建立在 futex 上。鎖是一個整數:
 *   0 = 沒人持有    1 = 有人持有,沒人在等    2 = 有人持有,而且有人在等
 * 沒有競爭時 lock / unlock 各只是一條原子指令,不進 kernel。
 * __sync_* 是編譯器內建的原子操作(i386 的 lock cmpxchg、RISC-V 的 lr/sc、
 * AArch64 的 ldxr/stxr),整個「比較並交換」不會被其他執行緒打斷。
 */
typedef volatile int umutex_t;

static inline void umutex_lock(umutex_t *m)
{
    if (__sync_bool_compare_and_swap(m, 0, 1))
        return;                              /* 快速路徑:原本沒人持有 */
    for (;;) {
        /* 標記「有人在等」。若鎖剛好被放開(值為 0),直接以 2 搶下:
           用 2 而不是 1,因為可能還有其他人在等,解鎖時要記得叫醒 */
        if (*m == 2 || __sync_bool_compare_and_swap(m, 1, 2))
            sys_futex_wait(m, 2);            /* 值仍為 2 才會真的睡 */
        if (__sync_bool_compare_and_swap(m, 0, 2))
            return;
    }
}

static inline void umutex_unlock(umutex_t *m)
{
    /* 把值設回 0,並看原本是不是 2;是的話才需要進 kernel 叫醒等待者 */
    if (__sync_lock_test_and_set(m, 0) == 2)
        sys_futex_wake(m);
}
#endif
