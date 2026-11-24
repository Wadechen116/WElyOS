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

/*
 * 系統呼叫的編號、傳遞的結構、錯誤碼、訊號編號…都和 kernel 引入同一份檔案(include/uapi/)。
 * 以相對路徑引入:使用者程式的編譯因此不需要、也看不到 kernel 內部的標頭。
 */
#include "../include/uapi/errno.h"
#include "../include/uapi/syscall.h"
#include "../include/uapi/fs.h"
#include "../include/uapi/proc.h"
#include "../include/uapi/net.h"
#include "../include/uapi/blackbox.h"

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
/* 高精度時鐘(bench 用);沒有可用的時鐘時回傳 -E_NOSYS */
static inline long sys_clock(struct clockinfo *info) { return syscall3(SYS_clock, (long)info, 0, 0); }
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
/*
 * 網路。位址是 32 位元的整數,最高位元組在前:10.0.2.15 = NET_IP(10, 0, 2, 15)。
 * 這台機器固定是 10.0.2.15,閘道 10.0.2.2,名稱伺服器 10.0.2.3(模擬器提供的私有網路)。
 */
/* ping:回傳來回的毫秒數;逾時回傳 -E_TIMEDOUT;沒有網路回傳 -E_NETDOWN */
static inline long sys_ping(unsigned ip, long timeout_ms) { return syscall3(SYS_ping, (long)ip, timeout_ms, 0); }
/* UDP:先以 sys_udp_open 取得一個描述符(port 0 = 自動挑一個),用完以 sys_close 關閉 */
static inline long sys_udp_open(long port)        { return syscall3(SYS_udp_open, port, 0, 0); }
static inline long sys_udp_send(long fd, unsigned ip, unsigned port, const void *data, unsigned len)
{
    struct udpmsg m = { ip, (unsigned short)port, 0, len, 0, (unsigned long)data };
    return syscall3(SYS_udp_send, fd, (long)&m, 0);
}
/* 等一個封包(最多 timeout_ms 毫秒)。回傳長度,*ip 與 *port 是來源;沒有等到回傳 -E_TIMEDOUT */
static inline long sys_udp_recv(long fd, void *buf, unsigned cap, unsigned *ip, unsigned *port,
                                long timeout_ms)
{
    struct udpmsg m = { 0, 0, 0, cap, 0, (unsigned long)buf };
    long n = syscall3(SYS_udp_recv, fd, (long)&m, timeout_ms);
    if (n >= 0) {
        if (ip)
            *ip = m.ip;
        if (port)
            *port = m.port;
    }
    return n;
}
static inline long sys_netinfo(struct netinfo *info) { return syscall3(SYS_netinfo, (long)info, 0, 0); }
/*
 * 黑盒子:kernel 記在磁碟上、重新開機後還在的事件紀錄(開機、關機、panic、看門狗、
 * 程式因例外被終止、磁碟錯誤、受監督的程式被重新啟動、註記)。
 */
/* 讀取第 index 筆(0 = 最新的)。0 = 取得,1 = 沒有這麼多筆 */
static inline long sys_bbread(long index, struct bb_record *r) { return syscall3(SYS_bbread, index, (long)r, 0); }
/* 留下一筆紀錄(type 為 BB_NOTE 或 BB_RESTART);需要系統管理的特權 */
static inline long sys_bblog(long type, const char *text) { return syscall3(SYS_bblog, type, (long)text, 0); }
/* 立刻把整顆磁碟巡檢一遍:直接從磁碟讀每個區塊,核對檢查碼。需要系統管理的特權 */
static inline long sys_scrub(struct scrubinfo *info) { return syscall3(SYS_scrub, (long)info, 0, 0); }
/*
 * 週期性的即時工作(需要 CAP_ADMIN)。
 * sys_rt_set:每 period_ms 毫秒做一次,每次最多用 budget_ms 毫秒的 CPU;在預算內優先於所有一般的行程。
 *   -E_BUSY = 全系統的即時工作合計會超過 CPU 的 70%,不接受;period_ms 為 0 = 取消。
 * sys_rt_wait:這一次做完了,睡到下一個週期。錯過期限、超出預算都會被計數(見 struct rt_stats)。
 */
static inline long sys_rt_set(unsigned period_ms, unsigned budget_ms)
{
    return syscall3(SYS_rt_set, (long)period_ms, (long)budget_ms, 0);
}
static inline long sys_rt_wait(struct rt_stats *st) { return syscall3(SYS_rt_wait, (long)st, 0, 0); }
/*
 * 把檔案寫回儲存媒體。回傳 0 才表示資料已經保存;-E_IO = 寫入失敗;
 * 管線、主控台、socket 回傳 -E_INVAL(它們沒有「寫回」這件事)。
 * 記憶體檔案系統上的檔案回傳 0,但那只表示沒有待寫的東西 — 內容不會保存到下次開機。
 *
 * 和 sys_close 的關係:sys_close 對可寫的檔案也會回報寫入失敗(-E_IO),但它只保證資料交給了裝置;
 * 要確定資料到了媒體上,先 sys_fsync 再 sys_close,兩個都要檢查。
 * sys_close 不論回傳什麼,那個描述符都已經關閉:不可以對同一個編號再關一次。
 */
static inline long sys_fsync(long fd)             { return syscall3(SYS_fsync, fd, 0, 0); }
/*
 * 關閉一個「結果不需要確認」的描述符:唯讀開啟的檔案、管線的一端、socket、暫時 dup 出來的編號,
 * 或是已經在失敗路徑上、正要回報別的錯誤時的清理。
 * 寫過資料、而且要確定資料存好了的檔案不可以用這個:那要檢查 sys_fsync 與 sys_close 的回傳值。
 * fd 是負的(沒有開成)就不做事。
 */
static inline void close_quiet(long fd)
{
    if (fd >= 0 && syscall3(SYS_close, fd, 0, 0) != 0)
        return;
}
/* 故障注入(測試用,需要 CAP_ADMIN):把行程 pid 的記憶體 addr 處那個位元組的第 bit 個位元翻轉,
   模擬宇宙射線造成的單一位元錯誤。在對方下一次返回使用者模式之前生效 */
static inline long sys_seu(long pid, const volatile void *addr, int bit)
{
    return syscall3(SYS_seu, pid, (long)addr, bit);
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
static inline long sys_failinj(long op, long site, long n) { return syscall3(SYS_failinj, op, site, n); }
/* CPU 親和性(I-344):tid 0 = 自己 */
static inline long sys_setaffinity(long tid, unsigned long mask) { return syscall3(SYS_setaffinity, tid, (long)mask, 0); }
static inline long sys_getaffinity(long tid)                     { return syscall3(SYS_getaffinity, tid, 0, 0); }
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
