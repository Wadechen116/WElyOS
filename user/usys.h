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
    SYS_spawn, SYS_exec, SYS_wait, SYS_dup2, SYS_fork, SYS_pipe,
    SYS_waitpid, SYS_kill, SYS_setpgid, SYS_tcsetfg, SYS_procinfo,
    SYS_sync, SYS_statfs, SYS_sysinfo, SYS_tcsetmode,
    SYS_lseek, SYS_rename, SYS_link, SYS_chdir, SYS_getcwd, SYS_sbrk,
    SYS_signal, SYS_sigreturn, SYS_time,
    SYS_getuid, SYS_getgid, SYS_setuid, SYS_capget, SYS_capdrop, SYS_chmod, SYS_chown,
    SYS_mmap, SYS_munmap, SYS_memlimit,
};
#define SYS_SIGRETURN_NUM 40 /* SYS_sigreturn 的數值:crt0.c 裡的組合語言要用(下面有檢查) */
_Static_assert(SYS_sigreturn == SYS_SIGRETURN_NUM, "update SYS_SIGRETURN_NUM");

/* sys_waitpid 的選項 */
#define WNOHANG   1          /* 不等待:還沒有子行程結束就立刻回傳 0 */
#define WUNTRACED 2          /* 子行程「被暫停」也回報:*status 會符合 WIFSTOPPED,行程不會被回收 */
#define WSTOPPED_BASE  0x80000000u
#define WIFSTOPPED(s)  (((unsigned)(s) & 0xFFFFFF00u) == WSTOPPED_BASE)
#define WSTOPSIG(s)    ((int)((unsigned)(s) & 0xFFu))

/* sys_procinfo 回報的內容,必須與 kernel 的 include/proc.h 一致 */
enum { PROC_RUNNING = 1, PROC_SLEEPING, PROC_ZOMBIE, PROC_STOPPED };
struct procinfo {
    int pid, ppid, pgid;
    int state;               /* PROC_RUNNING / PROC_SLEEPING / PROC_ZOMBIE / PROC_STOPPED */
    int nthreads;
    unsigned int cpu_ticks;  /* 累計用掉的 CPU 時間(tick,1 tick = 1ms) */
    unsigned int mem_kb;     /* 對映的記憶體(KB;含和其他行程共用的程式碼與尚未複製的頁) */
    unsigned int uid;        /* 行程的使用者編號 */
    char name[32];
};

/* sys_sysinfo 回報的內容,必須與 kernel 的 include/proc.h 一致 */
#define SYSINFO_MAX_CPUS 8
struct sysinfo {
    unsigned long long uptime_ticks;         /* 開機至今的 tick 數 */
    unsigned int hz;                         /* 每秒幾個 tick */
    unsigned int ncpu;
    unsigned int nprocs, nthreads;
    unsigned long long mem_total_kb, mem_free_kb;
    unsigned long long ctx_switches;         /* 累計的執行緒切換次數 */
    struct {
        unsigned long long busy, idle;       /* 這顆 CPU 忙碌 / 閒置的 tick 數(累計) */
    } cpu[SYSINFO_MAX_CPUS];
};

/* sys_tcsetmode 的模式(可以相加) */
#define CON_RAW    1         /* 不等整行:每個按鍵立刻交給讀取者,不做行編輯 */
#define CON_NOECHO 2         /* 不把打的字顯示出來 */

/* 訊號編號。終止:INT、KILL、TERM(被終止的行程結束碼為 128 + 編號);
   暫停:STOP、TSTP(Ctrl-Z)、TTIN(背景行程想讀鍵盤);繼續:CONT */
#define SIGINT  2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21

/* 檔案相關的定義,必須與 kernel 的 include/fs.h 一致 */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define E_PERM    1          /* 沒有這個特權 */
#define E_NOENT   2
#define E_ACCES   13         /* 檔案的權限不允許 */
#define E_INTR    4          /* 系統呼叫被訊號打斷(行程有登記處理函式時才會發生);通常再呼叫一次即可 */
#define E_SRCH    3
#define E_NOEXEC  8
#define E_BADF    9
#define E_CHILD   10
#define E_NOMEM   12
#define E_FAULT   14
#define E_INVAL   22
#define E_BUSY    16
#define E_EXIST   17
#define E_XDEV    18
#define E_NOTDIR  20
#define E_ISDIR   21
#define E_SPIPE   29
#define E_NAMETOOLONG 36
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#define E_TXTBSY  26
#define E_FBIG    27
#define E_NOSPC   28
#define E_PIPE    32
#define E_NOTEMPTY 39
enum { I_FILE = 1, I_DIR, I_DEV };
struct dirent { char name[60]; unsigned int type; };
struct stat {
    unsigned int type; unsigned int nlink; unsigned long long size;
    unsigned long long mtime;                /* 最後修改的時間(1970-01-01 UTC 至今的秒數);0 = 不知道 */
    unsigned int uid, gid;                   /* 擁有者與群組 */
    unsigned int mode;                       /* 權限位元,例如 0644(擁有者讀寫,其他人唯讀) */
};
/* 特權(sys_capget / sys_capdrop);說明見 kernel 的 include/cred.h */
#define CAP_FS     1u        /* 不受檔案權限限制;可以改任何檔案的擁有者與權限 */
#define CAP_KILL   2u        /* 可以對別人的行程送訊號 */
#define CAP_SETID  4u        /* 可以改變自己的 uid / gid */
#define CAP_ADMIN  8u        /* 系統管理(保留) */
#define CAP_ALL    15u
/* 現在的日期時間(sys_time) */
struct timeval { unsigned long long sec; unsigned int usec, pad; };
/* 檔案系統的用量(sys_statfs) */
struct fsinfo {
    unsigned int block_size, pad;            /* 一個區塊的位元組數 */
    unsigned long long blocks, free_blocks;  /* 可以放資料的區塊總數 / 還空著的 */
    unsigned long long inodes, free_inodes;  /* 檔案數的上限(0 = 沒有固定上限)/ 還可以建立幾個 */
    char type[8];                            /* "ramfs"、"wfs" */
};

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
/*
 * 訊號的處理方式。handler 可以是:
 *   SIG_DFL   預設(終止;SIGTSTP 是暫停)
 *   SIG_IGN   忽略
 *   函式      收到訊號時,行程的某一條執行緒會暫時放下手邊的事去執行 handler(sig),
 *             執行完再從被打斷的地方繼續。正在等待的系統呼叫(read、sleep、waitpid…)
 *             會提早返回 -E_INTR
 * 可以設定的訊號:SIGINT、SIGTERM、SIGUSR1、SIGUSR2、SIGTSTP。SIGKILL 與 SIGSTOP 不能改。
 * 回傳原本的處理方式;失敗回傳負的錯誤碼。
 * __sig_restorer 是處理函式返回時跳去的一小段程式(在 crt0.c),它呼叫 sigreturn
 * 請 kernel 還原被打斷時的暫存器。
 */
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
void __sig_restorer(void);
static inline long sys_signal(long sig, sighandler_t handler)
{
    return syscall3(SYS_signal, sig, (long)handler, (long)__sig_restorer);
}
/* 把 heap 的結尾移動 incr 個位元組(可為負),回傳移動之前的結尾;失敗回傳 -E_NOMEM。
   一般程式不直接用它,而是用 libc.h 的 malloc / free */
static inline long sys_sbrk(long incr)            { return syscall3(SYS_sbrk, incr, 0, 0); }
/*
 * 匿名對映:要一塊 len 位元組(進位到整頁)、內容為 0 的記憶體;用完以 sys_munmap(那個位址)整塊歸還。
 * 適合大塊、生命期獨立的記憶體(小塊請用 malloc)。失敗回傳負的錯誤碼(用 (long)p < 0 判斷)。
 */
static inline void *sys_mmap(unsigned long len)   { return (void *)syscall3(SYS_mmap, (long)len, 0, 0); }
static inline long sys_munmap(void *addr)         { return syscall3(SYS_munmap, (long)addr, 0, 0); }
/*
 * 記憶體配額(KB):這個行程的 heap 加上匿名對映最多可以要多少。kb 為 0 是查詢。
 * kernel 不會答應超過配額(或超過系統實際能提供)的要求:sbrk / mmap / malloc 會當場失敗,
 * 而不是先答應、等真的用到時才把程式終止。子行程沿用配額;調低隨時可以,調高需要特權。
 */
static inline long sys_memlimit(long kb)          { return syscall3(SYS_memlimit, kb, 0, 0); }
/* 改變讀寫位置,回傳新的位置。whence:SEEK_SET 從頭、SEEK_CUR 從目前位置、SEEK_END 從結尾 */
static inline long sys_lseek(long fd, long off, long whence)
{
    return syscall3(SYS_lseek, fd, off, whence);
}
/* 改名或搬移;目的已經存在時失敗(-E_EXIST),不會蓋掉它 */
static inline long sys_rename(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_rename, (long)oldpath, (long)newpath, 0);
}
static inline long sys_link(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_link, (long)oldpath, (long)newpath, 0);
}
/* 目前的工作目錄:不是以 "/" 開頭的路徑都從這裡算起。新行程沿用建立者的 */
static inline long sys_chdir(const char *path)    { return syscall3(SYS_chdir, (long)path, 0, 0); }
static inline long sys_getcwd(char *buf, unsigned long len)
{
    return syscall3(SYS_getcwd, (long)buf, (long)len, 0);
}
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
/* 整個系統的狀態(top 用) */
static inline long sys_sysinfo(struct sysinfo *info) { return syscall3(SYS_sysinfo, (long)info, 0, 0); }
/* 設定主控台的輸入模式。只有前景的行程可以設定;shell 每次回到提示符號時會設回一般模式 */
static inline long sys_tcsetmode(long mode)       { return syscall3(SYS_tcsetmode, mode, 0, 0); }
/*
 * 身分與權限。每個行程有使用者編號(uid;0 是 root)、群組編號(gid)與一組特權。
 * 特權只能減少:sys_setuid 改成非 root、或 sys_capdrop,都是拿不回來的。
 */
static inline long sys_getuid(void)               { return syscall3(SYS_getuid, 0, 0, 0); }
static inline long sys_getgid(void)               { return syscall3(SYS_getgid, 0, 0, 0); }
static inline long sys_setuid(long uid, long gid) { return syscall3(SYS_setuid, uid, gid, 0); }
static inline long sys_capget(void)               { return syscall3(SYS_capget, 0, 0, 0); }
static inline long sys_capdrop(long mask)         { return syscall3(SYS_capdrop, mask, 0, 0); }
static inline long sys_chmod(const char *path, long mode)
{
    return syscall3(SYS_chmod, (long)path, mode, 0);
}
static inline long sys_chown(const char *path, long uid, long gid)
{
    return syscall3(SYS_chown, (long)path, uid, gid);
}
/* 現在的日期時間:1970-01-01 00:00:00 UTC 至今的秒數與微秒(換成年月日見 libc.h 的 gmtime_) */
static inline long sys_time(struct timeval *tv)   { return syscall3(SYS_time, (long)tv, 0, 0); }
/* 把還留在記憶體裡的修改寫到磁碟上 */
static inline long sys_sync(void)                 { return syscall3(SYS_sync, 0, 0, 0); }
/* path 所在的檔案系統的用量 */
static inline long sys_statfs(const char *path, struct fsinfo *info)
{
    return syscall3(SYS_statfs, (long)path, (long)info, 0);
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
/* 等子行程結束。pid 為 -1 表示任何一個子行程。回傳結束的子行程的 pid,
   它的結束碼寫入 *status(可以傳 0 表示不需要);沒有子行程可等回傳 -E_CHILD。
   options 為 WNOHANG 時不等待:還沒有子行程結束就立刻回傳 0;
   加上 WUNTRACED 時,子行程被暫停也會回報(以 WIFSTOPPED(*status) 分辨) */
static inline long sys_waitpid(long pid, int *status, long options)
{
    return syscall3(SYS_waitpid, pid, (long)status, options);
}
/* 列出行程:把「編號大於 after 的下一個行程」的資訊寫入 *info。
   回傳 0 = 取得,1 = 沒有更多了。從 after = 0 開始,每次傳入上一次得到的 pid */
static inline long sys_procinfo(long after, struct procinfo *info)
{
    return syscall3(SYS_procinfo, after, (long)info, 0);
}
/* 送訊號。pid > 0:那個行程;pid < 0:行程群組 -pid 的所有行程 */
static inline long sys_kill(long pid, long sig)   { return syscall3(SYS_kill, pid, sig, 0); }
static inline long sys_setpgid(long pid, long pgid) { return syscall3(SYS_setpgid, pid, pgid, 0); }
static inline long sys_tcsetfg(long pgid)         { return syscall3(SYS_tcsetfg, pgid, 0, 0); }
/* 建立管線:fds[0] 是讀端,fds[1] 是寫端 */
static inline long sys_pipe(int fds[2])           { return syscall3(SYS_pipe, (long)fds, 0, 0); }
/* 複製目前的行程。回傳值:在原本的行程裡是新行程的 pid,在新行程裡是 0 */
static inline long sys_fork(void)                 { return syscall3(SYS_fork, 0, 0, 0); }
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
