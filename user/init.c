/*
 * init.c - 內建的使用者程式
 *
 * 這個檔案編譯後在「使用者模式」執行:沒有特權、看不到 kernel 的記憶體,
 * 只能透過 usys.h 的系統呼叫與 kernel 互動。它沒有連結 kernel 的任何程式碼,
 * 連 kprintf 都沒有,輸出要自己組字串再呼叫 sys_write。
 *
 * kernel 以不同的 mode 參數啟動它,用來測試使用者模式的各種行為。
 * mode 的數值(UMODE_*)定義在 include/uapi/selftest.h,和 kernel/main.c 共用。
 */
#include "usys.h"
#include "ulib.h"
#include "libc.h"

/* 測試模式的編號:見 include/uapi/selftest.h(kernel 啟動本程式時以參數傳入) */
#include "../include/uapi/selftest.h"

/* 各架構 kernel 映像所在的位址(使用者模式不應該讀得到) */
#if defined(__i386__)
#define KERNEL_ADDR 0x00100000UL
#elif defined(__riscv)
#define KERNEL_ADDR 0x80200000UL
#else
#define KERNEL_ADDR 0x40080000UL
#endif

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

/* ---- UMODE_THREADCHURN ---- */

/* I-330 / I-240 C3.2 (c1):反覆建立、結束執行緒 40 次(每個行程只有 16 格 stack)。
   每條執行緒都往下用自己的 stack 到 512KB 深(stack 成長的 fault 用預先承諾的票),
   寫入、讀回、結束;下一條要能拿到剛歸還的格。以前格子只增不減,第 16 條起就建立失敗 */
static volatile int churn_done, churn_bad;

static void churn_fn(long idx)
{
    volatile char deep[512 * 1024];
    for (unsigned long i = 0; i < sizeof(deep); i += 4096)
        deep[i] = (char)(idx + i / 4096);
    for (unsigned long i = 0; i < sizeof(deep); i += 4096)
        if (deep[i] != (char)(idx + i / 4096))
            churn_bad = 1;
    churn_done = 1;
    sys_thread_exit();
}

static int mode_threadchurn(void)
{
    for (int round = 0; round < 40; round++) {
        churn_done = 0;
        if (sys_thread_create(churn_fn, round) < 0)
            return 100 + round;                  /* 第幾輪建立失敗 */
        for (int w = 0; !churn_done; w++) {
            if (w > 5000)
                return 200 + round;              /* 等太久:那條執行緒沒做完 */
            sys_sleep(1);
        }
        sys_sleep(2);                            /* 讓它真正結束、歸還格子 */
    }
    return churn_bad ? 2 : 61;
}

/* ---- UMODE_THREADMIX ---- */

/* I-240 C3.2 補測:
   A. 4 條「產生者」執行緒同時各自反覆建立、等待 30 條工作執行緒(共 120 條),
      建立與結束在多顆 CPU 上交錯,搶同一組 stack 格;每條工作執行緒深用 128KB stack。
   B. fork 出的子行程:主執行緒建立 T 之後自己先結束;T(用第 2 格以後的 stack)再 fork,
      孫行程在繼承來的那格 stack 上深用 256KB,結束碼要正確傳回 */
static volatile int mix_done, mix_bad, mix_spawners;
static volatile int mix_mine[4];                 /* 每個產生者自己的工作執行緒完成數(只看全域數會被別人的完成誤觸發) */

static void mix_worker(long idx)
{
    volatile char deep[128 * 1024];
    for (unsigned long i = 0; i < sizeof(deep); i += 4096)
        deep[i] = (char)(idx + i / 4096);
    for (unsigned long i = 0; i < sizeof(deep); i += 4096)
        if (deep[i] != (char)(idx + i / 4096))
            mix_bad = 1;
    __atomic_fetch_add(&mix_mine[idx / 100], 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&mix_done, 1, __ATOMIC_SEQ_CST);
    sys_thread_exit();
}

static void mix_spawner(long id)
{
    for (int i = 0; i < 30; i++) {
        long r;
        int tries = 0;
        while ((r = sys_thread_create(mix_worker, id * 100 + i)) < 0) {
            if (r != -E_BUSY || ++tries > 2000) {      /* 格子暫時都在用:等別人歸還 */
                mix_bad = 2;
                goto out;
            }
            sys_sleep(1);
        }
        for (int w = 0; __atomic_load_n(&mix_mine[id], __ATOMIC_SEQ_CST) <= i; w++) {
            if (w > 5000) {
                mix_bad = 3;
                goto out;
            }
            sys_sleep(1);
        }
    }
out:
    __atomic_fetch_sub(&mix_spawners, 1, __ATOMIC_SEQ_CST);
    sys_thread_exit();
}

/* fork 只允許單一執行緒的行程;其他執行緒還在結束途中時回傳 -E_BUSY(暫時),有上限地重試 */
static long mix_fork(void)
{
    long pid;
    for (int tries = 0; (pid = sys_fork()) == -E_BUSY && tries < 5000; tries++)
        sys_sleep(1);
    return pid;
}

static void mix_forker(long unused)
{
    (void)unused;
    long pid = mix_fork();                       /* 主執行緒結束之前是 -E_BUSY */
    if (pid == 0) {
        volatile char deep[256 * 1024];
        for (unsigned long i = 0; i < sizeof(deep); i += 4096)
            deep[i] = (char)(i / 4096);
        for (unsigned long i = 0; i < sizeof(deep); i += 4096)
            if (deep[i] != (char)(i / 4096))
                sys_exit(5);
        sys_exit(33);
    }
    if (pid < 0)
        sys_exit(pid == -E_NOMEM ? 6 : 7);
    int st = 0;
    if (sys_waitpid(pid, &st, 0) != pid)
        sys_exit(8);
    sys_exit(st == 33 ? 34 : 9);
}

static int mode_threadmix(void)
{
    mix_spawners = 4;
    for (long i = 0; i < 4; i++)
        if (sys_thread_create(mix_spawner, i) < 0)
            return 1;
    for (int w = 0; __atomic_load_n(&mix_spawners, __ATOMIC_SEQ_CST) > 0; w++) {
        if (w > 60000)
            return 2;
        sys_sleep(1);
    }
    if (mix_bad)
        return 10 + mix_bad;
    if (mix_done != 120)
        return 3;
    long pid = mix_fork();                       /* 產生者真正結束之前是 -E_BUSY */
    if (pid == 0) {
        if (sys_thread_create(mix_forker, 0) < 0)
            sys_exit(4);
        sys_thread_exit();                       /* 主執行緒先結束;行程由 T 繼續 */
    }
    if (pid < 0)
        return pid == -E_BUSY ? 22 : 20;
    int st = 0;
    if (sys_waitpid(pid, &st, 0) != pid)
        return 21;
    return st == 34 ? 72 : 30 + st;
}

/* ---- UMODE_FAILSWEEP ---- */

/* I-335:配置失敗注入(SYS_failinj,只在 selftest 開機 + CAP_ADMIN 時有效)掃過行程相關的系統呼叫。
   對每個操作、每個地點(kmalloc / pmm),令第 N 次配置失敗(N = 1, 2, …),直到操作不再碰到注入。
   每次失敗:回傳 -E_NOMEM;收拾之後「可用記憶體」(pmm 空閒 + heap 空閒 + 執行緒池)、
   pmm 預留、自己 domain 的帳都和之前完全相同 */
static volatile int fsw_thread_ran;
static int fsw_warm;                             /* 暖身:不比較(有界保留的結構可能在這次建立) */

static void fsw_thread(long a)
{
    (void)a;
    fsw_thread_ran = 1;
    sys_thread_exit();
}

struct fsw_snap { long avail, resv, dom; };

/* 先前成功的 fork 讓自己的可寫頁變成 COW + 票;之後寫到其中一頁時退回一張票(預留數因此變動,
   不是洩漏)。拍快照前把測試會寫到的頁都寫一遍:全域資料與 bss,以及 stack 上一段**固定的**
   絕對範圍(mode_failsweep 進入時的 stack 位置往下 128KB)。範圍若跟著呼叫深度走,
   比較時呼叫得較深就會碰到新的 stack 頁(與它的頁表),看起來像洩漏 */
extern char __text_end[], __bss_end[];
static uintptr_t fsw_stack_hi;

static void fsw_settle(void)
{
    for (uintptr_t a = fsw_stack_hi - 128 * 1024; a < fsw_stack_hi; a += 4096) {
        volatile char *c = (volatile char *)a;
        *c = *c;
    }
    uintptr_t lo = ((uintptr_t)__text_end + 4095) & ~(uintptr_t)4095;
    for (uintptr_t a = lo; a < (uintptr_t)__bss_end; a += 4096) {
        volatile char *c = (volatile char *)a;
        *c = *c;
    }
}

/* 掃描開始時「還沒釋放的執行緒數」(只有這條測試執行緒時)。拍快照前先等它回到這個值:
   結束的執行緒(暖身的、fork / spawn 子行程的)在別顆 CPU 上分段歸還 stack 與票,
   最後才回收結構。主機負載高時,快照曾經拍在歸還到一半的時候(預留差 -123 / -187 / -251,
   正好是 stack 歸還的第 2 / 1 / 0 段之後),之後的數字永遠對不上 */
static long fsw_threads0;

static int fsw_quiesce(void)
{
    for (int w = 0; w < 5000; w++) {
        if (sys_failinj(FI_OP_UNFREED, 0, 0) == fsw_threads0)
            return 1;
        sys_sleep(1);                            /* 讓出 CPU;條件是計數,不是時間 */
    }
    return 0;
}

/* 快照不可信(等不到靜止,或計量的系統呼叫失敗)時設起,掃描以結束碼 5 失敗 —
   不能拿一個還在變動的基準繼續比(Codex 196) */
static int fsw_untrusted;

static void fsw_take(struct fsw_snap *x)
{
    if (!fsw_quiesce())
        fsw_untrusted = 1;
    fsw_settle();
    x->avail = sys_failinj(FI_OP_AVAIL_KB, 0, 0);
    x->resv = sys_failinj(FI_OP_RESERVED, 0, 0);
    x->dom = sys_failinj(FI_OP_DOMAIN, 0, 0);
    if (x->avail < 0 || x->resv < 0 || x->dom < 0)
        fsw_untrusted = 1;
}

static int fsw_report(const char *what, int site, int n, const char *why, long v)
{
    struct line l = { .len = 0 };
    line_str(&l, "  [user] failsweep ");
    line_str(&l, what);
    line_str(&l, site ? " pmm #" : " kmalloc #");
    line_num(&l, n);
    line_str(&l, ": ");
    line_str(&l, why);
    line_str(&l, " ");
    line_num(&l, v);
    line_str(&l, "\n");
    line_out(&l);
    return 0;
}

/* 等收拾完成:執行緒、子行程的資源由 kernel 非同步回收,最多等 2 秒讓數字穩定。
   check_resv = 0:fork 成功過(父的可寫頁變成 COW + 票,父多持有預留票,直到它寫到那些頁或結束才退;
   這是設計,不是洩漏 — 所有程式結束後預留回到原值由 user_test 另外核對) */
static int fsw_same(const struct fsw_snap *a, const char *what, int site, int n, int check_resv)
{
    if (fsw_warm)
        return 1;
    struct fsw_snap b;
    for (int w = 0; w < 2000; w++) {
        fsw_take(&b);
        if (b.avail == a->avail && (!check_resv || b.resv == a->resv) && b.dom == a->dom)
            return 1;
        sys_sleep(1);
    }
    fsw_report(what, site, n, "leaked avail_kb", b.avail - a->avail);
    fsw_report(what, site, n, "reserved", b.resv - a->resv);
    fsw_report(what, site, n, "domain pages", b.dom - a->dom);
    return 0;
}

enum { FSW_FORK, FSW_THREAD, FSW_EXEC, FSW_SPAWN, FSW_PIPE, FSW_NOPS };
static const char *const fsw_names[FSW_NOPS] = { "fork", "thread_create", "exec", "spawn", "pipe" };

/* 做一次操作(第 n 次配置失敗)。回傳 -1 = 錯誤(已印出),0 = 碰到注入且正確失敗,1 = 沒碰到注入 */
static int fsw_once(int op, int site, int n)
{
    static char *sleep_argv[] = { "sleep", "0", 0 };
    int st = 0, fds[2];
    long r;
    struct fsw_snap a;
    fsw_take(&a);
    if (op == FSW_EXEC) {
        /* exec 成功就不會回來:在子行程裡做,子行程自己核對「失敗時舊程式完好、帳不變」 */
        long pid = sys_fork();
        if (pid < 0)
            return fsw_report("exec(fork)", site, n, "fork failed", pid), -1;
        if (pid == 0) {
            long d0 = sys_failinj(FI_OP_DOMAIN, 0, 0);
            sys_failinj(FI_OP_ARM, site, n);
            r = sys_exec("/bin/sleep", sleep_argv);
            int fired = (int)sys_failinj(FI_OP_DISARM, 0, 0);
            if (!fired)
                sys_exit(90);                    /* exec 失敗卻沒有碰到注入 */
            if (r != -E_NOMEM)
                sys_exit(91);
            sys_exit(sys_failinj(FI_OP_DOMAIN, 0, 0) == d0 ? 92 : 93);
        }
        if (sys_waitpid(pid, &st, 0) != pid)
            return fsw_report("exec", site, n, "waitpid", 0), -1;
        if (st != 0 && st != 92)
            return fsw_report("exec", site, n, "child status", st), -1;
        if (!fsw_same(&a, "exec", site, n, 0))       /* 父這邊 fork 過 */
            return -1;
        return st == 0 ? 1 : 0;
    }
    if (sys_failinj(FI_OP_ARM, site, n) != 0)
        return fsw_report(fsw_names[op], site, n, "arm failed", 0), -1;
    switch (op) {
    case FSW_FORK:
        r = sys_fork();
        if (r == 0)
            sys_exit(0);
        break;
    case FSW_THREAD:
        fsw_thread_ran = 0;
        r = sys_thread_create(fsw_thread, 0);
        break;
    case FSW_SPAWN:
        r = sys_spawn("/bin/sleep", sleep_argv);
        break;
    default:
        r = sys_pipe(fds);
        break;
    }
    int fired = (int)sys_failinj(FI_OP_DISARM, 0, 0);
    if (fired && r != -E_NOMEM)
        return fsw_report(fsw_names[op], site, n, "returned", r), -1;
    if (!fired) {                                /* 成功:收拾乾淨,核對收拾之後也回到原值 */
        if (r < 0)
            return fsw_report(fsw_names[op], site, n, "failed without injection", r), -1;
        if (op == FSW_FORK || op == FSW_SPAWN) {
            if (sys_waitpid(r, &st, 0) != r || st != 0)
                return fsw_report(fsw_names[op], site, n, "child status", st), -1;
        } else if (op == FSW_THREAD) {
            for (int w = 0; !fsw_thread_ran; w++) {
                if (w > 5000)
                    return fsw_report("thread_create", site, n, "thread never ran", 0), -1;
                sys_sleep(1);
            }
        } else if (sys_close(fds[0]) != 0 || sys_close(fds[1]) != 0) {
            return fsw_report("pipe", site, n, "close", 0), -1;
        }
    }
    /* 失敗:父完全沒被動過,三項都必須回到原值。fork 成功時預留可以合理增加(見 fsw_same) */
    if (!fsw_same(&a, fsw_names[op], site, n, fired || op != FSW_FORK))
        return -1;
    return fired ? 0 : 1;
}

/* ---- I-335 第二輪:mmap / sbrk、管線讀寫、UDP ----
   這幾種操作分兩類:
   (1) 依設計不會碰到可注入的配置:mmap / sbrk 只做承諾(pmm_reserve 計數 + 頁表票),之後的 page fault
       用 pmm_alloc_reserved 與 WALK_ANON 的票,兩者都不注入;munmap / sbrk 縮小只歸還;管線讀寫只搬
       固定 4KB 緩衝區。對它們的核對是「整段啟用注入,也一次都不能觸發,而且全部成功」— 一旦有人在這些
       路徑加上一般配置,這裡會立刻失敗(那等於破壞了「已承諾的記憶體一定兌現」的保證)。
   (2) 真的會配置:udp_open(vfs_file_new 的 struct file)與 udp_recv / udp_send(sys_udp_io 的
       NET_UDP_MAX 暫存區;send 與 recv 共用同一個配置點,在分支之前,所以只掃 recv,timeout 0 不需要
       網路上有對端)。失敗時回傳 -E_NOMEM,帳回到原值。沒有網路(QEMU_NET=none)時 udp_open 在任何
       配置之前回傳 -E_NETDOWN:整個 UDP 部分略過並印出 */
enum { FSI_MEM, FSI_PIPEIO, FSI_UDPOPEN, FSI_UDPRECV, FSI_NOPS };
static const char *const fsi_names[FSI_NOPS] = { "mmap+sbrk", "pipe_io", "udp_open", "udp_recv" };
static int fsi_nonet;                            /* 沒有網路:UDP 兩項略過 */

/* 子行程裡做 mmap / 碰觸 / munmap 與 sbrk 長大 / 碰觸 / 縮回,全程啟用注入。
   在子行程做的原因:每次 mmap 的位址都往後移(mmap_next),碰觸時建立的中間頁表與它的票依設計保留到
   位址空間銷毀;在父行程裡做,跨過一張頁表的邊界時看起來就像洩漏。子行程結束 = 一切歸還,父再比較 */
static int fsi_mem_child(int site, int n)
{
    fsw_settle();                                /* fork 之後的 COW 頁先寫過:沒有票的 COW 複製走一般配置,會被注入 */
    sys_failinj(FI_OP_ARM, site, n);
    char *m = sys_mmap(4 * 4096);
    if ((long)m < 0)
        return 80;
    for (int i = 0; i < 4; i++)
        m[i * 4096] = (char)i;                   /* 兌現:用票,一定成功 */
    if (sys_munmap(m) != 0)
        return 81;
    long old = sys_sbrk(3 * 4096);
    if (old < 0)
        return 82;
    for (int i = 0; i < 3; i++)
        ((volatile char *)old)[i * 4096] = 1;
    if (sys_sbrk(-3 * 4096) != old + 3 * 4096)
        return 83;
    int fired = (int)sys_failinj(FI_OP_DISARM, 0, 0);
    return fired ? 84 : 0;                       /* 84 = 注入碰到了不該配置的路徑 */
}

/* 回傳 -1 = 錯誤(已印出),0 = 碰到注入且正確失敗,1 = 沒碰到注入(本操作的掃描結束) */
static int fsi_once(int op, int site, int n)
{
    static char iobuf[4096];
    int st = 0, fds[2] = { -1, -1 };
    long sock = -1, r;
    struct fsw_snap a;

    if (op == FSI_MEM) {
        fsw_take(&a);
        long pid = sys_fork();
        if (pid < 0)
            return fsw_report("mmap(fork)", site, n, "fork failed", pid), -1;
        if (pid == 0)
            sys_exit(fsi_mem_child(site, n));
        if (sys_waitpid(pid, &st, 0) != pid || st != 0)
            return fsw_report("mmap+sbrk", site, n, "child status", st), -1;
        return fsw_same(&a, "mmap+sbrk", site, n, 0) ? 1 : -1;   /* 父 fork 過:預留不比(見 fsw_same) */
    }
    /* 準備(不在注入範圍內):管線、socket 先建好,快照之後才啟用 */
    if (op == FSI_PIPEIO && sys_pipe(fds) != 0)
        return fsw_report("pipe_io", site, n, "pipe", 0), -1;
    if (op == FSI_UDPRECV && (sock = sys_udp_open(0)) < 0)
        return fsw_report("udp_recv", site, n, "udp_open", sock), -1;
    fsw_take(&a);
    if (sys_failinj(FI_OP_ARM, site, n) != 0)
        return fsw_report(fsi_names[op], site, n, "arm failed", 0), -1;
    switch (op) {
    case FSI_PIPEIO:
        r = sys_write(fds[1], iobuf, sizeof(iobuf));            /* 剛好填滿 4KB:不會擋住 */
        if (r == (long)sizeof(iobuf))
            r = sys_read(fds[0], iobuf, sizeof(iobuf));
        break;
    case FSI_UDPOPEN:
        r = sys_udp_open(0);
        break;
    default:
        r = sys_udp_recv(sock, iobuf, sizeof(iobuf), 0, 0, 0);  /* timeout 0:只看一次 */
        break;
    }
    int fired = (int)sys_failinj(FI_OP_DISARM, 0, 0);
    int ok;
    if (op == FSI_PIPEIO) {
        ok = !fired && r == (long)sizeof(iobuf);
        if (fired)
            fsw_report("pipe_io", site, n, "injection reached pipe read/write", r);
        if (sys_close(fds[0]) != 0 || sys_close(fds[1]) != 0)
            ok = 0;
        if (!ok)
            return fsw_report("pipe_io", site, n, "returned", r), -1;
        return 1;                                /* 收拾(close)在快照之後:不比較,下一輪重新建 */
    }
    if (fired && r != -E_NOMEM)
        return fsw_report(fsi_names[op], site, n, "returned", r), -1;
    if (!fired) {
        long want_ok = op == FSI_UDPOPEN ? r >= 0 : r == -E_TIMEDOUT;
        if (!want_ok)
            return fsw_report(fsi_names[op], site, n, "failed without injection", r), -1;
        if (op == FSI_UDPOPEN && sys_close(r) != 0)
            return fsw_report("udp_open", site, n, "close", 0), -1;
    }
    /* udp_recv:比較時 socket 仍開著(兩邊快照都含它),比較完才關 */
    int same = fsw_same(&a, fsi_names[op], site, n, 1);
    if (sock >= 0 && sys_close(sock) != 0)
        return fsw_report("udp_recv", site, n, "close", 0), -1;
    if (!same)
        return -1;
    return fired ? 0 : 1;
}

static int mode_failsweep(void)
{
    if (sys_failinj(FI_OP_AVAIL_KB, 0, 0) < 0)
        return 2;                                /* 沒有以 selftest 開機,或沒有 CAP_ADMIN */
    /* 基準:前面的測試結束的執行緒也可能還沒回收完 — 連續 50 次(每次隔 1ms)讀到完全相同的值
       才定下來;任何變動(不論變多或變少)都重新計算。5 秒內定不下來就失敗,不拿變動中的值當基準 */
    fsw_threads0 = sys_failinj(FI_OP_UNFREED, 0, 0);
    if (fsw_threads0 < 0)
        return 4;
    int still = 0;
    for (int w = 0; still < 50; w++) {
        if (w >= 5000)
            return 4;
        sys_sleep(1);
        long t = sys_failinj(FI_OP_UNFREED, 0, 0);
        if (t < 0)
            return 4;
        if (t == fsw_threads0) {
            still++;
        } else {
            fsw_threads0 = t;
            still = 0;
        }
    }
    volatile char here;
    fsw_stack_hi = (uintptr_t)&here & ~(uintptr_t)4095;   /* 這一頁以下 128KB 涵蓋整個掃描的 stack 用量 */
    int points[FSW_NOPS][2];
    for (int op = 0; op < FSW_NOPS; op++)
        for (int site = 0; site < 2; site++) {
            /* 暖身:先成功做一次,讓有界保留的結構(stack 格的中間頁表與它的票,保留到位址空間銷毀,
               上限 M)先建好。之後每個失敗點都必須讓帳完全回到原值 */
            fsw_warm = 1;
            int wr = fsw_once(op, site, 100000);
            fsw_warm = 0;
            if (wr != 1)
                return 40 + op * 2 + site;
            int n = 1, r;
            while ((r = fsw_once(op, site, n)) == 0)
                if (++n > 200)
                    return 3;
            if (r < 0)
                return 10 + op * 2 + site;      /* 哪個操作、哪個地點出錯(細節已印出) */
            if (fsw_untrusted)
                return 5;                        /* 等不到靜止或計量失敗:結果不可信 */
            points[op][site] = n - 1;
        }
    /* 第二輪(I-335):沒有網路時 udp_open 在配置之前就回 -E_NETDOWN,UDP 兩項略過 */
    long probe = sys_udp_open(0);
    if (probe == -E_NETDOWN)
        fsi_nonet = 1;
    else if (probe < 0 || sys_close(probe) != 0)
        return 60;
    int fpoints[FSI_NOPS][2] = { { 0 } };
    for (int op = 0; op < FSI_NOPS; op++)
        for (int site = 0; site < 2 && !(fsi_nonet && op >= FSI_UDPOPEN); site++) {
            fsw_warm = 1;
            int wr = fsi_once(op, site, 100000);
            fsw_warm = 0;
            if (wr != 1)
                return 61 + op * 2 + site;
            int n = 1, r;
            while ((r = fsi_once(op, site, n)) == 0)
                if (++n > 200)
                    return 3;
            if (r < 0)
                return 50 + op * 2 + site;
            if (fsw_untrusted)
                return 5;
            fpoints[op][site] = n - 1;
            /* 依設計不配置的兩項:失敗點必須是 0(fsi_once 在觸發時已經回報錯誤,這裡是第二道) */
            if ((op == FSI_MEM || op == FSI_PIPEIO) && n != 1)
                return 69;
        }
    struct line l = { .len = 0 };
    line_str(&l, "  [user] failsweep: failure points kmalloc+pmm:");
    for (int op = 0; op < FSW_NOPS; op++) {
        line_str(&l, " ");
        line_str(&l, fsw_names[op]);
        line_str(&l, " ");
        line_num(&l, points[op][0]);
        line_str(&l, "+");
        line_num(&l, points[op][1]);
    }
    line_str(&l, "\n");                          /* 分兩行:一行最多 160 字元,太長會被截斷 */
    line_out(&l);
    l.len = 0;
    line_str(&l, "  [user] failsweep: never or rarely allocating, kmalloc+pmm:");
    for (int op = 0; op < FSI_NOPS; op++) {
        line_str(&l, " ");
        line_str(&l, fsi_names[op]);
        if (fsi_nonet && op >= FSI_UDPOPEN) {
            line_str(&l, " skipped(no net)");
            continue;
        }
        line_str(&l, " ");
        line_num(&l, fpoints[op][0]);
        line_str(&l, "+");
        line_num(&l, fpoints[op][1]);
    }
    line_str(&l, "\n");
    line_out(&l);
    return 73;
}

/* ---- UMODE_SIGRACE ---- */

/* I-334:返回使用者模式時,kernel 先不上鎖讀「有沒有待處理訊號」,沒有的話再讀「是否被打斷」並清掉。
   訊號若剛好在這兩次讀取之間送達,打斷標記被清掉、訊號卻沒交付,接下來的睡眠就會睡滿。
   測試:父行程寫一個位元組到管線叫醒子行程(它睡 3ms 再送 SIGUSR1),接著啟用延遲注入 —
   這次系統呼叫返回時在兩次讀取之間忙等 15ms,訊號剛好落在這個空檔裡。父行程接著睡 2 秒:
   訊號在睡之前還沒交付,睡眠就必須被它叫醒;睡滿了 = 遺失喚醒 */
static volatile int sr_seen;
static volatile uint64_t sr_tick;               /* 處理函式執行時的 tick */

static void sr_handler(int sig)
{
    (void)sig;
    struct sysinfo t;
    if (sys_sysinfo(&t) == 0)
        sr_tick = t.uptime_ticks;
    sr_seen++;
}

static int mode_sigrace(void)
{
    long me = sys_getpid();
    if (sys_signal(SIGUSR1, sr_handler) < 0)
        return 2;
    long lw0 = sys_failinj(FI_OP_LOSTWAKE, 0, 0);
    long sp0 = sys_failinj(FI_OP_SLEEPPEND, 0, 0);
    int lost = 0, raced = 0, benign = 0;
    for (int k = 0; k < 60; k++) {
        int fds[2];
        if (sys_pipe(fds) != 0)
            return 3;
        int before = sr_seen;
        long pid = sys_fork();
        if (pid == 0) {
            char c;
            if (sys_read(fds[0], &c, 1) != 1)
                sys_exit(3);
            sys_sleep(3);                        /* 等父行程讀過「待處理訊號」、進入 15ms 的空檔中段 */
            sys_exit(sys_kill(me, SIGUSR1) == 0 ? 0 : 2);
        }
        if (pid < 0)
            return 4;
        if (sys_write(fds[1], "x", 1) != 1)      /* 叫醒子行程:它睡 3ms 後送訊號 */
            return 6;
        if (sys_failinj(FI_OP_SIGWIN, 0, 15) != 0)  /* 這次返回在兩次讀取之間忙等 15ms */
            return 5;
        if (sr_seen == before) {                 /* 還沒交付:接下來的睡眠必須被叫醒 */
            raced++;
            struct sysinfo t0, t1;
            if (sys_sysinfo(&t0) != 0)
                return 7;
            sys_sleep(2000);
            if (sys_sysinfo(&t1) != 0)
                return 8;
            /* 只有「睡滿了,而且處理函式在睡眠開始之後才執行」才算遺失喚醒。
               訊號若在 sr_seen 檢查之後、睡眠之前送達,它在 sysinfo 返回時就被交付了,
               接下來的睡眠睡滿是正確的(aarch64 上父行程在 CPU0、延遲期間關中斷時會發生) */
            if (t1.uptime_ticks - t0.uptime_ticks >= 1000) {
                if (sr_seen != before && sr_tick >= t0.uptime_ticks + 1000)
                    lost++;
                else
                    benign++;                    /* 處理函式在睡眠之前就執行了:睡滿是正確的 */
            }
        }
        int st = -1;
        while (sys_waitpid(pid, &st, 0) == -E_INTR)
            ;
        if (st != 0)
            return 9;
        for (int w = 0; sr_seen == before; w++) {    /* 最後一定要交付 */
            if (w > 3000)
                return 10;
            sys_sleep(1);
        }
        if (sys_close(fds[0]) != 0 || sys_close(fds[1]) != 0)
            return 11;
    }
    struct line l = { .len = 0 };
    line_str(&l, "  [user] sigrace: ");
    line_num(&l, raced);
    long bad = sys_failinj(FI_OP_LOSTWAKE, 0, 0) - lw0;
    line_str(&l, " of 60 rounds reached the sleep with the signal still pending, lost wakeups ");
    line_num(&l, lost);
    long sp = sys_failinj(FI_OP_SLEEPPEND, 0, 0) - sp0;
    line_str(&l, "\n");                          /* 分兩行:一行最多 160 字元 */
    line_out(&l);
    l.len = 0;
    line_str(&l, "  [user] sigrace: full sleeps after an earlier delivery ");
    line_num(&l, benign);
    line_str(&l, ", bad returns ");
    line_num(&l, bad);
    line_str(&l, ", sleeps with a deliverable signal ");
    line_num(&l, sp);
    line_str(&l, "\n");
    line_out(&l);
    if (bad)
        return 40 + (bad > 30 ? 30 : (int)bad);
    if (sp)
        return 30 + (sp > 9 ? 9 : (int)sp);
    return lost ? 20 + lost : 74;
}

/* ---- UMODE_FSQUOTA ---- */

/* I-240 C3.2 (d):ramfs 的 FS 配額。放棄 CAP_ADMIN 的子行程一直寫檔,必須停在 FS 上限、
   得到 -E_NOSPC(不是 -E_NOMEM,也沒被終止),此時 statfs 報告的空閒量是 0;
   管理者接著再寫 1MB 可以越過上限;全部刪掉之後 statfs 的空閒量回到原值 */
static char fsq_buf[65536];

static long fsq_fill(const char *prefix, int max_files, long max_chunks, long *err)
{
    char name[16];
    long chunks = 0;
    *err = 0;
    for (int f = 0; f < max_files && chunks < max_chunks && !*err; f++) {
        unsigned k = 0;
        for (const char *c = prefix; *c; c++)
            name[k++] = *c;
        name[k++] = (char)('0' + f / 10);
        name[k++] = (char)('0' + f % 10);
        name[k] = 0;
        long fd = sys_open(name, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            *err = fd;
            break;
        }
        for (int i = 0; i < 240 && chunks < max_chunks; i++) {     /* 每個檔案最多 15MB */
            long n = sys_write(fd, fsq_buf, sizeof(fsq_buf));
            if (n >= 0 && n < (long)sizeof(fsq_buf))
                n = sys_write(fd, fsq_buf, sizeof(fsq_buf));   /* 只寫進一部分:再寫一次,拿到真正的錯誤碼 */
            if (n != (long)sizeof(fsq_buf)) {
                *err = n < 0 ? n : -E_IO;
                break;
            }
            chunks++;
        }
        if (sys_close(fd) != 0 && !*err)
            *err = -E_IO;
    }
    return chunks;
}

static void fsq_clean(const char *prefix, int max_files)
{
    char name[16];
    for (int f = 0; f < max_files; f++) {
        unsigned k = 0;
        for (const char *c = prefix; *c; c++)
            name[k++] = *c;
        name[k++] = (char)('0' + f / 10);
        name[k++] = (char)('0' + f % 10);
        name[k] = 0;
        if (sys_unlink(name) != 0)
            break;
    }
}

static int mode_fsquota(void)
{
    struct fsinfo fi0, fi;
    if (sys_statfs("/", &fi0) != 0)
        return 1;
    long pid = sys_fork();
    if (pid == 0) {
        long err;
        if (sys_capdrop(CAP_ADMIN) < 0)
            sys_exit(2);
        fsq_fill("/fsu", 64, 1L << 30, &err);
        if (err != -E_NOSPC)
            sys_exit(err == -E_NOMEM ? 3 : 4);       /* 必須是 FS 上限,不是實體記憶體 */
        if (sys_statfs("/", &fi) != 0 || fi.free_blocks > 16)
            sys_exit(5);                             /* 到上限時,一般使用者看得到的空閒量是 0(容許取整) */
        sys_exit(0);
    }
    int st = -1;
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0)
        return 10 + st;
    long err;
    if (fsq_fill("/fsa", 1, 16, &err) != 16 || err != 0)
        return 20;                                   /* 管理者:越過 FS 上限再寫 1MB */
    fsq_clean("/fsa", 1);
    fsq_clean("/fsu", 64);
    if (sys_statfs("/", &fi) != 0 || fi.free_blocks + 16 < fi0.free_blocks)
        return 30;                                   /* 刪掉之後空閒量回來(容許 16 頁的 heap / 表的差異) */
    return 71;
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

/* 同樣的遞迴,但到指定的深度就返回;回傳實際到達的深度 */
static long recurse_limited(long depth, long limit)
{
    volatile char pad[256];
    for (unsigned i = 0; i < sizeof(pad); i++)
        pad[i] = (char)depth;
    if (depth >= limit)
        return depth;
    return recurse_limited(depth + 1, limit) + (pad[0] - (char)depth);
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

/* ---- UMODE_PROC ---- */

static void idle_worker(long arg)
{
    (void)arg;
    /* 每次只睡一下:行程結束時,睡在 kernel 裡的執行緒要等醒來才會結束,
       睡太久會讓整個行程的結束跟著拖延 */
    for (;;)
        sys_sleep(2);
}

/* 從使用者程式建立行程、傳參數、等它結束;以及各種應該失敗的情況 */
static int mode_proc(void)
{
    struct line l = { .len = 0 };
    char *echo_argv[] = { "args", "  [user] hello from a spawned /bin/args:", "one", "two", 0 };
    char *cat_argv[] = { "cat", "/etc/motd", 0 };
    char *ls_argv[] = { "ls", "/bin", 0 };
    long pid, code;

    /* 1. 建立行程並等待。args 的結束碼是它收到的參數個數 */
    pid = sys_spawn("/bin/args", echo_argv);
    if (pid <= 0)
        return 201;
    code = sys_wait(pid);
    if (code != 4)
        return 202;
    line_str(&l, "  [user] spawned pid ");
    line_num(&l, pid);
    line_str(&l, " exited with ");
    line_num(&l, code);
    line_str(&l, " (my pid is ");
    line_num(&l, sys_getpid());
    line_str(&l, ")\n");
    line_out(&l);

    /* 2. 其他小工具:顯示檔案內容、列出目錄 */
    puts_("  [user] running: cat /etc/motd\n");
    if (sys_wait(sys_spawn("/bin/cat", cat_argv)) != 0)
        return 203;
    puts_("  [user] running: ls /bin\n");
    if (sys_wait(sys_spawn("/bin/ls", ls_argv)) != 0)
        return 204;

    /* 3. 新行程繼承開啟中的檔案:先把自己的標準輸出換成一個檔案,
          再建立 echo。echo 照常寫入 1 號描述符,結果進了檔案 */
    long saved = sys_open("/dev/console", O_WRONLY);     /* 留一份主控台,等一下換回來 */
    sys_close(1);
    if (sys_open("/out.txt", O_CREAT | O_WRONLY | O_TRUNC) != 1)   /* 最小的空位就是 1 */
        return 210;
    char *redir_argv[] = { "args", "captured", "output", 0 };
    pid = sys_spawn("/bin/args", redir_argv);
    code = sys_wait(pid);
    if (sys_close(1) != 0)                               /* echo 寫進去的那個檔案 */
        return 2110;
    if (sys_open("/dev/console", O_WRONLY) != 1)         /* 換回主控台 */
        return 211;
    sys_close(saved);
    if (code != 3)
        return 212;
    char buf[64];
    long fd = sys_open("/out.txt", O_RDONLY);
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    sys_unlink("/out.txt");
    buf[n > 0 ? n : 0] = 0;
    if (!streq(buf, "captured output\n"))
        return 213;
    puts_("  [user] output of a child process was redirected into a file and read back\n");

    /* 4. 應該失敗的情況:每一種都只是回傳錯誤,自己照常繼續執行 */
    if (sys_spawn("/bin/nope", echo_argv) != -E_NOENT)
        return 220;
    if (sys_spawn("/etc/motd", echo_argv) != -E_ACCES)               /* 沒有執行權限 */
        return 221;
    /* 加上執行權限之後,才輪到內容的檢查:它不是合法的程式 */
    if (sys_chmod("/etc/motd", 0755) != 0 || sys_spawn("/etc/motd", echo_argv) != -E_NOEXEC ||
        sys_chmod("/etc/motd", 0644) != 0)
        return 226;
    if (sys_exec("/bin/nope", echo_argv) != -E_NOENT)                /* exec 失敗:原程式不受影響 */
        return 222;
    if (sys_exec("/etc/motd", echo_argv) != -E_ACCES)
        return 223;
    if (sys_spawn((const char *)KERNEL_ADDR, echo_argv) != -E_FAULT) /* 路徑指向 kernel */
        return 224;
    if (sys_spawn("/bin/echo", (char **)KERNEL_ADDR) != -E_FAULT)    /* 參數陣列指向 kernel */
        return 225;
    char *bad_argv[] = { "echo", (char *)KERNEL_ADDR, 0 };           /* 其中一個參數指向 kernel */
    if (sys_spawn("/bin/echo", bad_argv) != -E_FAULT)
        return 226;
    if (sys_wait(sys_getpid()) >= 0)                                 /* 等自己 */
        return 227;

    /* 5. 正在執行的程式檔不可以被修改(它的頁正對映在行程裡) */
    fd = sys_open("/bin/init", O_WRONLY);
    if (fd < 0)
        return 230;
    if (sys_write(fd, "x", 1) != -E_TXTBSY)
        return 231;
    sys_close(fd);
    if (sys_open("/bin/init", O_WRONLY | O_TRUNC) != -E_TXTBSY)
        return 232;

    puts_("  [user] process calls work as expected\n");
    return 12;
}

/* ---- UMODE_FORK ---- */

static volatile long fork_global = 100;      /* 在資料區段:fork 之後父子各有自己的一份 */
static volatile long cow_slot[3];            /* 三條執行緒各寫一格(三格在同一頁上) */
static volatile int cow_done;

static void cow_writer(long idx)
{
    for (long i = 1; i <= 2000; i++)
        cow_slot[idx] = i * (idx + 1);
    __sync_fetch_and_add(&cow_done, 1);
    sys_thread_exit();
}

/* 在 stack 上放一大塊資料(需要 stack 自動成長),fork 之後子行程改掉一半,
   父行程的那一份必須原封不動 */
static int fork_big_stack(void)
{
    volatile char big[60000];

    for (unsigned i = 0; i < sizeof(big); i++)
        big[i] = (char)(i * 3 + 1);
    long pid = sys_fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        for (unsigned i = 0; i < sizeof(big); i++)       /* 先確認複本和原本一樣 */
            if (big[i] != (char)(i * 3 + 1))
                sys_exit(51);
        for (unsigned i = 0; i < sizeof(big) / 2; i++)   /* 再改掉前半 */
            big[i] = 0x55;
        sys_exit(42);
    }
    if (sys_wait(pid) != 42)
        return 2;
    for (unsigned i = 0; i < sizeof(big); i++)
        if (big[i] != (char)(i * 3 + 1))
            return 3;
    return 0;
}

static int mode_fork(void)
{
    struct line l = { .len = 0 };
    volatile long local = 7;
    long pid, code;

    /* 1. 基本:子行程看到和父行程一樣的資料;它的修改不影響父行程 */
    pid = sys_fork();
    if (pid < 0)
        return 301;
    if (pid == 0) {
        if (fork_global != 100 || local != 7)
            sys_exit(50);
        fork_global = 200;                       /* 資料區段的頁:寫入時才複製 */
        local = 8;                               /* stack 的頁:同上 */
        line_str(&l, "  [user] child (pid ");
        line_num(&l, sys_getpid());
        line_str(&l, ") changed its copies to ");
        line_num(&l, fork_global);
        line_str(&l, " and ");
        line_num(&l, local);
        line_str(&l, "\n");
        line_out(&l);
        sys_exit(41);
    }
    code = sys_wait(pid);
    line_str(&l, "  [user] parent (pid ");
    line_num(&l, sys_getpid());
    line_str(&l, ") still sees ");
    line_num(&l, fork_global);
    line_str(&l, " and ");
    line_num(&l, local);
    line_str(&l, "; child exited with ");
    line_num(&l, code);
    line_str(&l, "\n");
    line_out(&l);
    if (code != 41 || fork_global != 100 || local != 7)
        return 302;

    /* 2. 傳統的 fork + exec:子行程把自己換成另一個程式 */
    pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { "args", "  [user] fork + exec:", "this is /bin/args", 0 };
        sys_exec("/bin/args", argv);
        sys_exit(99);                            /* exec 失敗才會到這裡 */
    }
    if (sys_wait(pid) != 3)
        return 303;

    /* 3. 連續 fork 很多次,每個子行程回報自己的編號 */
    for (long i = 0; i < 20; i++) {
        pid = sys_fork();
        if (pid < 0)
            return 304;
        if (pid == 0)
            sys_exit((int)(60 + i));
        if (sys_wait(pid) != 60 + i)
            return 305;
    }

    /* 4. 子行程再 fork(三代) */
    pid = sys_fork();
    if (pid == 0) {
        long g = sys_fork();
        if (g == 0)
            sys_exit(5);
        sys_exit((int)(sys_wait(g) + 10));
    }
    if (sys_wait(pid) != 15)
        return 306;

    /* 5. 大量資料(約 15 頁的 stack) */
    int r = fork_big_stack();
    if (r)
        return 310 + r;

    /* 6. fork 之後,子行程的三條執行緒同時寫入同一個(仍與父行程共用的)頁:
          三條可能同時為同一頁觸發寫入時複製,結果必須仍然正確 */
    pid = sys_fork();
    if (pid == 0) {
        for (long i = 0; i < 3; i++)
            sys_thread_create(cow_writer, i);
        while (cow_done < 3)
            sys_sleep(1);
        for (long i = 0; i < 3; i++)
            if (cow_slot[i] != 2000 * (i + 1))
                sys_exit(52);
        sys_exit(43);
    }
    if (sys_wait(pid) != 43)
        return 320;
    if (cow_slot[0] || cow_slot[1] || cow_slot[2] || cow_done)   /* 父行程的那一頁沒被動到 */
        return 321;

    puts_("  [user] fork works as expected\n");
    return 14;
}

/* ---- UMODE_SIGNAL ---- */

/* 建立一個永遠不會自己結束的子行程,回傳它的 pid */
static long fork_sleeper(void)
{
    long pid = sys_fork();
    if (pid == 0) {
        for (;;)
            sys_sleep(1000);
    }
    return pid;
}

static int mode_signal(void)
{
    struct line l = { .len = 0 };
    int st = 0;
    long a, b, pid;

    /* 1. 終止一個不會自己結束的子行程。它的結束碼是 128 + 訊號編號 */
    a = fork_sleeper();
    if (a <= 0)
        return 501;
    if (sys_kill(a, SIGKILL) != 0)
        return 502;
    if (sys_waitpid(a, &st, 0) != a || st != 128 + SIGKILL)
        return 503;
    line_str(&l, "  [user] killed child ");
    line_num(&l, a);
    line_str(&l, ", its exit status is ");
    line_num(&l, st);
    line_str(&l, " (128 + signal 9)\n");
    line_out(&l);

    /* 2. 等「任何一個」子行程:兩個子行程各自結束,兩次 waitpid(-1) 各收到一個 */
    a = sys_fork();
    if (a == 0)
        sys_exit(31);
    b = sys_fork();
    if (b == 0)
        sys_exit(32);
    long sum = 0;
    for (int i = 0; i < 2; i++) {
        pid = sys_waitpid(-1, &st, 0);
        if (!((pid == a && st == 31) || (pid == b && st == 32)))
            return 510;
        sum += pid;
    }
    if (sum != a + b)                            /* 同一個不能收到兩次 */
        return 511;
    if (sys_waitpid(-1, &st, 0) != -E_CHILD)        /* 已經沒有子行程了 */
        return 512;

    /* 3. 行程群組:把兩個子行程放進同一組,一次終止整組 */
    a = fork_sleeper();
    b = fork_sleeper();
    if (sys_setpgid(a, a) != 0 || sys_setpgid(b, a) != 0)
        return 520;
    if (sys_kill(-a, SIGTERM) != 0)
        return 521;
    if (sys_waitpid(a, &st, 0) != a || st != 128 + SIGTERM)
        return 522;
    if (sys_waitpid(b, &st, 0) != b || st != 128 + SIGTERM)
        return 523;

    /* 4. 孤兒行程:子行程建立孫行程後自己先結束。沒有人會 wait 那個孫行程,
          它結束時 kernel 要自動回收(測試最後的記憶體核對會確認這一點) */
    a = sys_fork();
    if (a == 0) {
        if (sys_fork() == 0) {
            sys_sleep(5);
            sys_exit(0);
        }
        sys_exit(33);
    }
    if (sys_waitpid(a, &st, 0) != a || st != 33)
        return 530;

    /* 5. 應該被拒絕的情況 */
    if (sys_kill(99999, SIGKILL) != -E_SRCH)     /* 沒有這個行程 */
        return 540;
    if (sys_kill(sys_getpid(), 7) >= 0)          /* 不支援的訊號編號 */
        return 541;
    if (sys_wait(1) != -E_CHILD)                 /* 不是自己的子行程:不能 wait */
        return 542;
    a = fork_sleeper();
    if (a <= 0 || sys_kill(a, SIGKILL) != 0)     /* 負的 pid 會被 kill 當成「整個群組」:先確認建立成功 */
        return 5430;
    if (sys_waitpid(a, (int *)KERNEL_ADDR, 0) != -E_FAULT)     /* 要求把結束碼寫到 kernel 的位址 */
        return 543;

    puts_("  [user] process relationships and signals work as expected\n");
    return 17;
}

/* fsync 與 close 的回傳值:一般檔案成功;管線與主控台沒有「寫回」這件事;關閉之後描述符不再有效 */
static int fsync_on(const char *path)
{
    int fds[2];
    long fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC);

    if (fd < 0 || sys_write(fd, "data", 4) != 4)
        return 1300;
    if (sys_fsync(fd) != 0)
        return 1301;
    if (sys_close(fd) != 0 || sys_close(fd) != -E_BADF || sys_fsync(fd) != -E_BADF)
        return 1302;                                     /* 關閉成功;同一個編號不能再用 */
    fd = sys_open(path, O_RDONLY);
    if (fd < 0 || sys_fsync(fd) != 0 || sys_close(fd) != 0)
        return 1303;                                     /* 唯讀的開啟也可以要求同步 */
    if (sys_fsync(1) != -E_INVAL || sys_fsync(99) != -E_BADF)
        return 1304;                                     /* 主控台;不存在的描述符 */
    if (sys_pipe(fds) != 0)
        return 1305;
    if (sys_fsync(fds[1]) != -E_INVAL || sys_close(fds[0]) != 0 || sys_close(fds[1]) != 0)
        return 1306;                                     /* 管線:不支援同步,但關閉照常成功 */
    return sys_unlink(path) == 0 ? 0 : 1307;
}

/* ---- UMODE_MEMRACE ---- */

/*
 * 對映的變更與「kernel 讀寫使用者記憶體」同時發生(第 48 步修正的兩個競態,Codex 以確定的交錯證實過):
 *   對映者  反覆 mmap 一塊 4 頁、前 3 頁填上 0x5A、公開位址、檢查內容、收回位址、munmap
 *   heap    反覆把 heap 長大 3 頁、填上 0xA5、檢查、再縮回去
 *   複製者  反覆對「目前公開的那一塊」做會讓 kernel 寫入(getcwd)與讀出(write 到 /dev/null)的系統呼叫
 * 要求:沒有當機;複製只會成功或回報 E_FAULT(那一塊剛好被收回了);
 *       任何人的內容都沒有被別人改掉 — 包括兩塊事先填好、從頭到尾不動的「哨兵」記憶體
 *       (被釋放的頁若被寫到了,下一個拿到那一頁的人就會看到不是自己寫的內容)。
 * 這是壓力測試:在真正的多顆 CPU 上以時間換取交錯的機會,不保證每次都碰到那個空檔;
 * 確定性的證明在主機端(tests/static/probe_memory_races.py)。
 */
static volatile int mr_stop, mr_running;
static volatile long mr_errors, mr_loops[3], mr_partial;
static volatile long mr_first_site, mr_first_value;     /* 第一個錯誤發生在哪裡、得到什麼值 */
#define MR_ERR(site, v) do { if (!mr_errors++) { mr_first_site = (site); mr_first_value = (long)(v); } } while (0)
static uint8_t *volatile mr_cur;
static long mr_null_fd, mr_file_fd;

static int mr_check(const uint8_t *p, unsigned long n, uint8_t v)
{
    for (unsigned long i = 0; i < n; i++)
        if (p[i] != v)
            return 1;
    return 0;
}

static void mr_mapper(long arg)
{
    (void)arg;
    while (!mr_stop) {
        uint8_t *p = sys_mmap(4 * 4096);
        if ((long)p < 0) {
            MR_ERR(1, p);
            break;
        }
        memset(p, 0x5A, 3 * 4096);
        mr_cur = p;
        sys_yield();
        if (mr_check(p, 3 * 4096, 0x5A))
            MR_ERR(2, 0);
        mr_cur = 0;
        long um = sys_munmap(p);
        if (um != 0)
            MR_ERR(3, um);
        mr_loops[0]++;
    }
    __sync_fetch_and_sub(&mr_running, 1);
    sys_thread_exit();
}

static void mr_heap(long arg)
{
    (void)arg;
    while (!mr_stop) {
        uint8_t *p = (uint8_t *)sys_sbrk(3 * 4096);
        if ((long)p < 0) {
            MR_ERR(4, p);
            break;
        }
        memset(p, 0xA5, 3 * 4096);
        sys_yield();
        if (mr_check(p, 3 * 4096, 0xA5))
            MR_ERR(5, 0);
        long sb = sys_sbrk(-3 * 4096);
        if (sb < 0)
            MR_ERR(6, sb);
        mr_loops[1]++;
    }
    __sync_fetch_and_sub(&mr_running, 1);
    sys_thread_exit();
}

static void mr_copier(long arg)
{
    (void)arg;
    while (!mr_stop) {
        uint8_t *p = mr_cur;
        if (p) {
            /* kernel 寫入使用者記憶體:從檔案讀一整頁到第 4 頁(只有這一頁不在對映者的檢查範圍內)。
               寫入的若是「已經被收回、配給別人」的頁,拿到那一頁的 heap 執行緒會看到不是 0xA5 的內容 */
            long r = sys_lseek(mr_file_fd, 0, SEEK_SET);
            if (r == 0)
                r = sys_read(mr_file_fd, p + 3 * 4096, 4096);
            if (r != -E_FAULT && (r <= 0 || r > 4096))
                MR_ERR(7, r);
            /* kernel 讀出使用者記憶體。合法的結果:全部寫出;E_FAULT(一開始就不合法);
               或寫出一部分 — 讀到一半那一塊被收回了,回報已寫出的量(和 POSIX 的 write 相同) */
            r = sys_write(mr_null_fd, p, 3 * 4096);
            if (r != -E_FAULT && (r <= 0 || r > 3 * 4096))
                MR_ERR(8, r);
            if (r > 0 && r < 3 * 4096)
                mr_partial++;
        }
        mr_loops[2]++;
        sys_yield();
    }
    __sync_fetch_and_sub(&mr_running, 1);
    sys_thread_exit();
}

static int mode_memrace(void)
{
    uint8_t *canary_map = sys_mmap(8 * 4096);
    uint8_t *canary_heap = (uint8_t *)sys_sbrk(8 * 4096);

    if ((long)canary_map < 0 || (long)canary_heap < 0)
        return 1;
    memset(canary_map, 0xC3, 8 * 4096);
    memset(canary_heap, 0x3C, 8 * 4096);
    if ((mr_null_fd = sys_open("/dev/null", O_WRONLY)) < 0 || (mr_file_fd = sys_open("/bin/init", O_RDONLY)) < 0)
        return 2;
    mr_running = 3;
    if (sys_thread_create(mr_mapper, 0) < 0 || sys_thread_create(mr_heap, 0) < 0 ||
        sys_thread_create(mr_copier, 0) < 0)
        return 3;
    sys_sleep(250);
    mr_stop = 1;
    while (mr_running > 0)
        sys_sleep(1);
    close_quiet(mr_null_fd);
    close_quiet(mr_file_fd);
    if (mr_check(canary_map, 8 * 4096, 0xC3) || mr_check(canary_heap, 8 * 4096, 0x3C))
        return 4;                                        /* 哨兵被改了:有人寫到了不屬於它的頁 */
    if (mr_errors) {
        printf("  [user] memrace: %ld error(s); first at site %ld, value %ld\n", mr_errors, mr_first_site,
               mr_first_value);
        return 5;
    }
    if (mr_loops[0] < 10 || mr_loops[1] < 10 || mr_loops[2] < 10)
        return 6;                                        /* 交錯的次數太少,測試沒有意義 */
    printf("  [user] concurrent mmap/munmap %ld, sbrk %ld, kernel copies %ld (%ld cut short by a munmap): "
           "no corruption\n", mr_loops[0], mr_loops[1], mr_loops[2], mr_partial);
    return 31;
}

/* ---- UMODE_TMR ---- */

#include "tmr.h"

static volatile unsigned char tmr_data[64];      /* 「計算」用到的資料:故障就注入在這裡 */
static volatile int tmr_hang;                    /* 被翻轉成非 0 的那一份會卡住不動 */

static int tmr_work(void *arg)
{
    char line[32];
    unsigned sum = 0;

    (void)arg;
    while (tmr_hang)
        ;
    for (unsigned i = 0; i < sizeof(tmr_data); i++)
        sum = sum * 31 + tmr_data[i];
    int n = snprintf(line, sizeof(line), "sum=%u\n", sum);
    return sys_write(1, line, (unsigned long)n) == n ? 7 : 1;
}

static int tmr_case;

static void tmr_inject(const long pid[TMR_N], void *arg)
{
    int *bad = arg;

    *bad = 0;
    if (tmr_case == 1)                           /* 第 2 份:資料裡翻轉一個位元 */
        *bad |= sys_seu(pid[1], &tmr_data[5], 3) != 0;
    if (tmr_case == 2)                           /* 第 3 份:卡住 */
        *bad |= sys_seu(pid[2], &tmr_hang, 0) != 0;
    if (tmr_case == 3) {                         /* 兩份各壞在不同的地方:三份都不一樣 */
        *bad |= sys_seu(pid[0], &tmr_data[1], 0) != 0;
        *bad |= sys_seu(pid[2], &tmr_data[9], 7) != 0;
    }
}

static int mode_tmr(void)
{
    struct tmr_result res[TMR_N];
    struct tmr_report rep;
    char want[32];
    unsigned sum = 0;
    int bad = 0, len, status = -1;

    for (unsigned i = 0; i < sizeof(tmr_data); i++) {
        tmr_data[i] = (unsigned char)(i * 7 + 3);
        sum = sum * 31 + tmr_data[i];
    }
    len = snprintf(want, sizeof(want), "sum=%u\n", sum);

    for (tmr_case = 0; tmr_case <= 3; tmr_case++) {
        int rc = tmr_run(tmr_work, 0, tmr_case == 2 ? 60 : 2000, tmr_inject, &bad, res, &rep);
        int base = 1200 + tmr_case * 10;
        const struct tmr_result *w = rep.winner >= 0 ? &res[rep.winner] : 0;
        int ok = !bad;

        if (tmr_case == 0)                       /* 沒有故障:三份相同 */
            ok = ok && rc == 0 && rep.agree == 3 && rep.odd == -1 && !rep.timed_out;
        if (tmr_case == 1)                       /* 一份算錯:被另外兩份否決,而且指得出是哪一份 */
            ok = ok && rc == 0 && rep.agree == 2 && rep.odd == 1 && !rep.timed_out &&
                 res[1].status == 7 && res[1].len > 0 && memcmp(res[1].out, want, (size_t)len) != 0;
        if (tmr_case == 2)                       /* 一份卡住:逾時被終止,結果仍然正確 */
            ok = ok && rc == 0 && rep.agree == 2 && rep.odd == 2 && rep.timed_out &&
                 res[2].status == 128 + SIGKILL;
        if (tmr_case == 3)                       /* 沒有兩份相同:拒絕給出結果 */
            ok = ok && rc == -1 && rep.agree == 0 && rep.winner == -1;
        if (tmr_case != 3)                       /* 有結果的話,一定是正確的那個 */
            ok = ok && w && w->status == 7 && w->len == (unsigned)len && memcmp(w->out, want, (size_t)len) == 0;
        for (int i = 0; i < TMR_N; i++)
            free(res[i].out);
        if (!ok)
            return base;
    }
    /* 注入只影響被指定的行程:我們自己的資料沒有變 */
    unsigned again = 0;
    for (unsigned i = 0; i < sizeof(tmr_data); i++)
        again = again * 31 + tmr_data[i];
    if (again != sum || tmr_hang)
        return 1250;
    /* 參數檢查與特權 */
    if (sys_seu(99999, &tmr_data[0], 0) != -E_SRCH || sys_seu(sys_getpid(), &tmr_data[0], 8) != -E_INVAL)
        return 1251;
    if (sys_seu(sys_getpid(), (void *)8, 0) != 0 || tmr_data[0] != 3)    /* 不合法的位址:什麼都不發生 */
        return 1252;
    if (sys_seu(sys_getpid(), &tmr_data[0], 2) != 0 || tmr_data[0] != (3 ^ 4))   /* 對自己:立刻生效 */
        return 1253;
    long pid = sys_fork();
    if (pid == 0) {
        sys_capdrop(CAP_ADMIN);
        sys_exit(sys_seu(sys_getpid(), &tmr_data[0], 0) == -E_PERM ? 0 : 1);
    }
    if (sys_waitpid(pid, &status, 0) != pid || status != 0)
        return 1254;
    return 30;
}

/* ---- UMODE_RT ---- */

static int mode_rt(void)
{
    struct rt_stats st;
    long pid;
    int status = -1;

    if (sys_rt_wait(&st) != -E_INVAL)                    /* 還不是週期性工作 */
        return 1;
    if (sys_rt_set(10, 10) != -E_INVAL || sys_rt_set(0, 5) != -E_INVAL)
        return 2;
    if (sys_rt_set(10, 8) != -E_BUSY)                    /* 80%:超過系統願意承諾的量 */
        return 3;
    if (sys_rt_set(10, 2) != 0)
        return 4;
    /* 另一個行程再要 60%:加上我們的 20% 超過上限,被拒絕;沒有特權的行程則根本不能要求 */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_rt_set(10, 6) != -E_BUSY || sys_rt_set(10, 4) != 0)
            sys_exit(1);
        sys_capdrop(CAP_ADMIN);
        sys_exit(sys_rt_set(20, 1) == -E_PERM ? 0 : 2);
    }
    struct sysinfo t0, t1;
    if (sys_sysinfo(&t0) != 0)
        return 10;
    for (int i = 0; i < 6; i++)                          /* 六個週期,幾乎不做事 */
        if (sys_rt_wait(&st) != 0)
            return 5;
    if (sys_sysinfo(&t1) != 0)
        return 11;
    /* 六個週期幾乎不做事:必須都準時、不超出預算。唯一的例外要量得到:這段期間計時器一次補記了
       >= 3 個 tick(預算是 2:虛擬 CPU 被停住、時間跳過去,補記的 tick 全算在當時正在執行的工作頭上)。
       一次補記 2 個是常態(中斷晚到 1 毫秒),不構成例外。每一次錯過期限都必須對應到一次量到的跳躍;
       超出預算則完全不允許 — 補記的 tick 不算成任何人的 CPU 時間(timer_tick_stolen),
       幾乎不做事的工作不可能超出預算。印出實際的數值 */
    unsigned jumped = 0;                                 /* 這段期間跳過 >= 3 個 tick 的次數 */
    for (int b = 1; b < 6; b++)
        jumped += t1.timer_burst[b] - t0.timer_burst[b];
    if (st.period_ms != 10 || st.budget_ms != 2 || st.releases + st.misses < 6 ||
        st.misses > jumped || st.overruns != 0) {
        printf("  [user] rt: releases %u misses %u overruns %u max used %u ms; timer jumps of 3+ ticks "
               "in the window: %u (ticks %llu..%llu)\n", st.releases, st.misses, st.overruns, st.max_used_ms,
               jumped, (unsigned long long)t0.uptime_ticks, (unsigned long long)t1.uptime_ticks);
        return 6;
    }
    if (sys_waitpid(pid, &status, 0) != pid || status != 0)
        return 7;
    if (sys_rt_wait(&st) != 0 || st.total_util != 200)   /* 子行程結束:它佔的 40% 已經還回來 */
        return 8;
    if (sys_rt_set(0, 0) != 0 || sys_rt_wait(&st) != -E_INVAL)
        return 9;
    return 29;
}

/* ---- UMODE_NET ---- */

#define NET_GW NET_IP(10, 0, 2, 2)

static int net_unprivileged(void)
{
    /* 沒有特權的行程不能綁定 1024 以下的埠;其他的可以 */
    if (sys_udp_open(69) != -E_ACCES)
        return 1;
    long s = sys_udp_open(5000);
    if (s < 0)
        return 2;
    return 0;                                            /* 不關閉就結束:kernel 要代為釋放那個埠 */
}

static int mode_net(void)
{
    static unsigned char pkt[600], motd[600];
    static const unsigned char rrq[] = "\0\1etc/motd\0octet";     /* TFTP 的讀取要求 */
    struct netinfo ni;
    unsigned ip = 0, port = 0;
    long s, s2, n, fd, len;

    if (sys_netinfo(&ni) != 0 || !ni.up || ni.ip != NET_IP(10, 0, 2, 15) || ni.gateway != NET_GW)
        return 1601;
    if (sys_netinfo((struct netinfo *)KERNEL_ADDR) != -E_FAULT)
        return 1602;

    /* ping:閘道會回答;同一個網路上不存在的位址等不到(第一個要先問到硬體位址,時間寬一點) */
    long ms = sys_ping(NET_GW, 1000);
    if (ms < 0 || ms > 1000)
        return 1603;
    if ((ms = sys_ping(NET_GW, 1000)) < 0 || ms > 200)
        return 1604;
    if (sys_ping(NET_IP(10, 0, 2, 99), 30) != -E_TIMEDOUT || sys_ping(NET_GW, -1) != -E_INVAL)
        return 1605;

    /* UDP socket 的基本規則 */
    if ((s = sys_udp_open(0)) < 0 || (s2 = sys_udp_open(6000)) < 0)
        return 1610;
    if (sys_udp_open(6000) != -E_BUSY || sys_udp_open(70000) != -E_INVAL)    /* 已被綁定;埠號不合理 */
        return 1611;
    if (sys_udp_send(1, NET_GW, 9, "x", 1) != -E_BADF || sys_udp_send(99, NET_GW, 9, "x", 1) != -E_BADF)
        return 1612;                                     /* 不是 socket;不存在的描述符 */
    if (sys_read(s, pkt, 4) != -E_INVAL || sys_write(s, "x", 1) != -E_INVAL || sys_lseek(s, 0, 0) != -E_SPIPE)
        return 1613;                                     /* socket 不能當成檔案讀寫 */
    if (sys_udp_recv(s, pkt, sizeof(pkt), &ip, &port, 5) != -E_TIMEDOUT)
        return 1614;
    if (sys_udp_send(s, NET_GW, 9, (void *)KERNEL_ADDR, 4) != -E_FAULT ||
        sys_udp_send(s, NET_GW, 9, pkt, 2000) != -E_INVAL)
        return 1615;
    if (sys_close(s2) != 0 || (s2 = sys_udp_open(6000)) < 0)                 /* 關閉後那個埠可以再用 */
        return 1616;
    sys_close(s2);

    /* 真的收送一次:向模擬器內建的 TFTP 伺服器要 etc/motd,第一塊資料應該就是整個檔案 */
    if ((fd = sys_open("/etc/motd", O_RDONLY)) < 0 || (len = sys_read(fd, motd, sizeof(motd))) <= 0)
        return 1620;
    sys_close(fd);
    n = -1;
    for (int tries = 0; tries < 3 && n < 0; tries++) {
        if (sys_udp_send(s, NET_GW, 69, rrq, sizeof(rrq)) != (long)sizeof(rrq))
            return 1621;
        n = sys_udp_recv(s, pkt, sizeof(pkt), &ip, &port, 500);
    }
    if (n != len + 4 || ip != NET_GW || port == 0 || pkt[0] != 0 || pkt[1] != 3 || pkt[3] != 1)
        return 1622;
    for (long i = 0; i < len; i++)
        if (pkt[4 + i] != motd[i])
            return 1623;                                 /* 內容和本機的那一份逐位元組相同 */
    unsigned char ack[4] = { 0, 4, 0, 1 };
    if (sys_udp_send(s, ip, port, ack, 4) != 4)
        return 1624;

    /* 特權與資源回收:子行程降為一般使用者 */
    int st = 1;
    long pid = sys_fork();
    if (pid == 0) {
        sys_setuid(1000, 1000);
        sys_exit(net_unprivileged());
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0)
        return 1630 + st;
    if ((s2 = sys_udp_open(5000)) < 0)                   /* 子行程沒關就結束的那個埠:已經被釋放 */
        return 1635;
    sys_close(s2);
    /* socket 的數量有上限;用完之後全部關掉,又可以再開 */
    long many[10];
    int got = 0;
    for (; got < 10; got++)
        if ((many[got] = sys_udp_open(0)) < 0)
            break;
    if (got != 7 || many[got] != -E_NOMEM)               /* 一共 8 個,s 佔了一個 */
        return 1636;
    for (int i = 0; i < got; i++)
        sys_close(many[i]);
    sys_close(s);
    if (sys_netinfo(&ni) != 0 || ni.udp_delivered < 1 || ni.tx_errors != 0)
        return 1637;

    printf("  [user] network: the gateway answers pings; a file fetched over UDP (TFTP) matches, %u frames in, %u out\n",
           ni.rx_frames, ni.tx_frames);
    return 28;
}

/* ---- UMODE_MEMQUOTA ---- */

static unsigned self_mem_kb(void);

/* 在子行程裡執行 fn,回傳它的結束碼(被終止時是負值或 128 以上) */
static int in_child(int (*fn)(void))
{
    int st = 0;
    long pid = sys_fork();

    if (pid == 0)
        sys_exit(fn());
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid)
        return 97;
    return st;
}

/* 配額 4MB:超過的要求當場失敗,行程照常繼續;答應過的每一頁都真的能用。
   第 56 步起配額也涵蓋每條執行緒預先承諾的整格 stack(255 頁)、頁表與 kernel 物件,
   一個剛 fork 出來的行程本身就佔約 1.1MB,所以配額從 2MB 改成 4MB(I-240 C3.2 (c1)) */
static int memq_limit_checks(void)
{
    static char *blk[64];
    int n = 0;

    if (sys_memlimit(4096) != 4096 || sys_memlimit(0) != 4096)
        return 1;
    if (sys_memlimit(8192) != -E_PERM)                   /* 調高需要特權(這個子行程已經放棄了) */
        return 2;
    long base = sys_sbrk(0);
    if (sys_sbrk(1 << 20) != base)                       /* 1MB:在配額內 */
        return 3;
    if (sys_sbrk(2 << 20) != -E_NOMEM || sys_sbrk(0) != base + (1 << 20))
        return 4;                                        /* 再要 2MB:拒絕,結尾不變 */
    for (long i = 0; i < (1 << 20); i += 4096)           /* 答應過的 1MB 每一頁都寫得進去 */
        ((char *)base)[i] = (char)i;
    if (sys_sbrk(-(1 << 20)) != base + (1 << 20))
        return 5;

    /* malloc 一直要到失敗為止:得到 NULL 而不是被終止;釋放之後又可以要 */
    while (n < 64 && (blk[n] = malloc(100 * 1024)) != 0) {
        memset(blk[n], n, 100 * 1024);
        n++;
    }
    if (n < 15 || n >= 40)                               /* 4MB 扣掉行程本身的約 1.1MB,大約放得下 29 塊 100KB */
        return 6;
    for (int i = 0; i < n; i++) {
        if (blk[i][0] != (char)i || blk[i][100 * 1024 - 1] != (char)i)
            return 7;
        free(blk[i]);
    }
    char *again = malloc(500 * 1024);
    if (!again)
        return 8;
    free(again);

    /* fork 需要和自己一樣多的承諾:配額沿用,子行程同樣受限 */
    long pid = sys_fork();
    if (pid == 0)
        sys_exit(sys_memlimit(0) == 4096 && sys_sbrk(4 << 20) == -E_NOMEM ? 0 : 1);
    int st = 1;
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0)
        return 9;
    return 0;
}

static int memq_touch_unmapped(void)
{
    char *m = sys_mmap(8192);

    if ((long)m < 0 || sys_munmap(m) != 0)
        return 1;
    m[0] = 1;                                            /* 已經歸還:不再屬於行程 */
    return 0;
}

static int memq_touch_gap(void)
{
    char *m = sys_mmap(4096);

    if ((long)m < 0)
        return 1;
    m[4096] = 1;                                         /* 這一塊之後留著一頁不對映:越界立刻出錯 */
    return 0;
}

/* exec 交易(I-240 C3.2 (c3)):配額只有 2MB,而一個程式本身就佔約 1.1MB(stack 255 頁等);
   exec 時新舊兩份同時存在約 2.2MB,超過配額 — 沒有交易時一定被拒絕。有交易時,新空間可以
   暫時超過上限、但不超過「舊空間會歸還的量」,換完之後回到上限內:exec 成功,args 以參數個數 3 結束 */
static int memq_exec_tight(void)
{
    char *argv[] = { "args", "x", "y", 0 };
    if (sys_memlimit(2048) != 2048)
        return 98;
    long rc = sys_exec("/bin/args", argv);               /* 成功就不會回來 */
    return rc == -E_NOMEM ? 99 : 97;                     /* 被配額拒絕 99,其他失敗 97 */
}

static int mode_memquota(void)
{
    long def = sys_memlimit(0);
    int rc;

    if (def != 32 * 1024 || sys_memlimit(-1) != -E_INVAL)
        return 1501;

    /* 1. 配額(在子行程裡把自己的配額調低、放棄特權) */
    long pid = sys_fork();
    int st = 0;
    if (pid == 0) {
        sys_capdrop(CAP_ADMIN);
        sys_exit(memq_limit_checks());
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0)
        return 1510 + st;
    if (sys_memlimit(0) != def)                          /* 子行程調低的是它自己的 */
        return 1509;
    if ((rc = in_child(memq_exec_tight)) != 3)           /* 接近配額上限的 exec(交易) */
        return 1540 + (rc == 99 ? 9 : rc == 98 ? 8 : 0);

    /* 2. 整個系統的上限:有特權的行程把配額調到 1GB,要 200MB — 這台機器沒有那麼多,
          當場拒絕(以前會先答應,等用到時才終止行程) */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_memlimit(1 << 20) != (1 << 20))
            sys_exit(1);
        sys_exit(sys_sbrk(200 << 20) == -E_NOMEM && (long)sys_mmap(200 << 20) == -E_NOMEM ? 0 : 2);
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0)
        return 1520 + st;

    /* 3. 匿名對映 */
    unsigned kb0 = self_mem_kb();
    char *a = sys_mmap(64 * 1024), *b = sys_mmap(1);
    if ((long)a < 0 || (long)b < 0 || a == b || ((unsigned long)a & 4095))
        return 1530;
    if (self_mem_kb() != kb0)                            /* 還沒碰:沒有配置 */
        return 1531;
    for (int i = 0; i < 64 * 1024; i++)
        if (a[i] != 0)
            return 1532;                                 /* 內容是 0 */
    memset(a, 0x5A, 64 * 1024);
    b[0] = 7;
    if (self_mem_kb() != kb0 + 64 + 4)
        return 1533;
    /* fork:子行程看到同樣的內容,改了不影響我們;它也可以歸還自己那一份 */
    pid = sys_fork();
    if (pid == 0) {
        int ok = a[100] == 0x5A && b[0] == 7;
        a[100] = 1;
        sys_exit(ok && sys_munmap(a) == 0 ? 0 : 1);
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != 0 || a[100] != 0x5A)
        return 1534;
    if (sys_munmap(a + 4096) != -E_INVAL || sys_munmap((void *)KERNEL_ADDR) != -E_INVAL)
        return 1535;                                     /* 只能整塊歸還,而且必須是自己的 */
    if (sys_munmap(a) != 0 || sys_munmap(a) != -E_INVAL || self_mem_kb() != kb0 + 4)
        return 1536;                                     /* 歸還後記憶體立刻減少;不能歸還兩次 */
    if (sys_munmap(b) != 0)
        return 1537;
    if ((long)sys_mmap(0) != -E_INVAL || (long)sys_mmap(1ul << 30) != -E_INVAL ||
        (long)sys_mmap(40 << 20) != -E_NOMEM)            /* 超過配額(32MB) */
        return 1538;
    /* 同時最多 16 塊 */
    void *many[17];
    int got = 0;
    for (; got < 17; got++)
        if ((long)(many[got] = sys_mmap(4096)) < 0)
            break;
    if (got != 16)
        return 1539;
    for (int i = 0; i < got; i++)
        if (sys_munmap(many[i]) != 0)
            return 1540;
    /* 碰已經歸還的對映、或越過一塊的結尾:行程被終止(各在子行程裡試) */
    if ((rc = in_child(memq_touch_unmapped)) != -1 || (rc = in_child(memq_touch_gap)) != -1)
        return 1541;
    if (self_mem_kb() != kb0)
        return 1542;

    puts_("  [user] memory quota: requests beyond the limit fail at once, nothing is killed later\n");
    return 27;
}

/* ---- UMODE_PERM ---- */

static int path_join(char *out, const char *dir, const char *name);

/* 以使用者 uid 的身分(沒有任何特權)執行 fn(base),回傳它的結果(0 = 全部符合預期) */
static int as_user(long uid, int (*fn)(const char *), const char *base)
{
    int st = 0;
    long pid = sys_fork();

    if (pid == 0) {
        if (sys_setuid(uid, uid) != 0)
            sys_exit(99);
        sys_exit(fn(base));
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid)
        return 98;
    return st;
}

/* 一般使用者(1000)面對 root 建立的東西 */
static int perm_user_checks(const char *base)
{
    char p[96];
    struct stat st;
    long fd;

    if (sys_getuid() != 1000 || sys_getgid() != 1000 || sys_capget() != 0)
        return 1;
    path_join(p, base, "/secret");                       /* 0600 root:完全不能碰 */
    if (sys_open(p, O_RDONLY) != -E_ACCES || sys_open(p, O_WRONLY) != -E_ACCES)
        return 2;
    if (sys_stat(p, &st) != 0 || st.uid != 0 || st.mode != 0600)   /* 看得到它存在與屬性 */
        return 3;
    path_join(p, base, "/pub");                          /* 0644 root:可讀不可寫 */
    if ((fd = sys_open(p, O_RDONLY)) < 0)
        return 4;
    sys_close(fd);
    if (sys_open(p, O_WRONLY) != -E_ACCES || sys_open(p, O_RDWR) != -E_ACCES ||
        sys_open(p, O_RDONLY | O_TRUNC) != -E_ACCES)     /* 清空也算寫入 */
        return 5;
    if (sys_chmod(p, 0666) != -E_PERM || sys_chown(p, 1000, 1000) != -E_PERM)
        return 6;                                        /* 不是擁有者;沒有特權 */
    if (sys_unlink(p) != -E_ACCES)                       /* 刪除看的是目錄的權限(0755 root) */
        return 7;
    path_join(p, base, "/mine");
    if (sys_open(p, O_CREAT | O_WRONLY) != -E_ACCES || sys_mkdir(p) != -E_ACCES)
        return 8;                                        /* 不能在別人的目錄裡建立東西 */
    path_join(p, base, "/priv/inside");                  /* priv 是 0700 root:連經過都不行 */
    if (sys_stat(p, &st) != -E_ACCES || sys_open(p, O_RDONLY) != -E_ACCES)
        return 9;
    path_join(p, base, "/priv");
    if (sys_chdir(p) != -E_ACCES || sys_open(p, O_RDONLY) != -E_ACCES)
        return 10;
    {
        char q[96];
        path_join(p, base, "/pub");
        path_join(q, base, "/pub2");
        if (sys_rename(p, q) != -E_ACCES || sys_link(p, q) != -E_ACCES)
            return 11;
    }

    /* open 是共用的目錄(0777):可以建立自己的檔案,它屬於自己,預設 0644 */
    path_join(p, base, "/open/note");
    if ((fd = sys_open(p, O_CREAT | O_RDWR)) < 0 || sys_write(fd, "hi", 2) != 2)
        return 12;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8001;
    if (sys_stat(p, &st) != 0 || st.uid != 1000 || st.gid != 1000 || st.mode != 0644)
        return 13;
    /* 自己的檔案可以改權限;改成 0000 之後連自己也打不開(擁有者那一組位元說了算) */
    if (sys_chmod(p, 0) != 0 || sys_open(p, O_RDONLY) != -E_ACCES || sys_chmod(p, 0600) != 0)
        return 14;
    if (sys_chown(p, 0, 0) != -E_PERM)                   /* 不能把檔案送給別人 */
        return 15;

    /* 沒有執行權限的檔案不能執行;加上之後才可以(內容不是程式,所以得到的是「不是程式」) */
    {
        char *argv[] = { "note", 0 };
        if (sys_spawn(p, argv) != -E_ACCES || sys_chmod(p, 0700) != 0 ||
            sys_spawn(p, argv) != -E_NOEXEC)
            return 16;
    }
    return 0;
}

/* 另一個一般使用者(1001)面對 1000 留下的檔案:0700,不是擁有者也不同群組 */
static int perm_other_checks(const char *base)
{
    char p[96];

    path_join(p, base, "/open/note");
    if (sys_open(p, O_RDONLY) != -E_ACCES || sys_chmod(p, 0777) != -E_PERM)
        return 1;
    if (sys_unlink(p) != 0)                              /* 但目錄是 0777:可以把它刪掉(沒有 sticky 位元) */
        return 2;
    return 0;
}

static int perm_on(const char *base)
{
    char p[96];
    struct stat st;
    long fd;
    int rc;

    path_join(p, base, "/open/note"); sys_unlink(p);     /* 上次留下的 */
    path_join(p, base, "/priv/inside"); sys_unlink(p);
    path_join(p, base, "/open"); sys_unlink(p);
    path_join(p, base, "/priv"); sys_unlink(p);
    path_join(p, base, "/secret"); sys_unlink(p);
    path_join(p, base, "/pub"); sys_unlink(p);
    sys_unlink(base);

    /* root 建立測試用的東西 */
    if (sys_mkdir(base) != 0 || sys_stat(base, &st) != 0 || st.mode != 0755 || st.uid != 0)
        return 1401;
    path_join(p, base, "/secret");
    if ((fd = sys_open(p, O_CREAT | O_WRONLY)) < 0 || sys_chmod(p, 0600) != 0)
        return 1402;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8054;
    path_join(p, base, "/pub");
    if ((fd = sys_open(p, O_CREAT | O_WRONLY)) < 0)
        return 1403;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8058;
    path_join(p, base, "/priv");
    if (sys_mkdir(p) != 0 || sys_chmod(p, 0700) != 0)
        return 1404;
    path_join(p, base, "/priv/inside");
    if ((fd = sys_open(p, O_CREAT | O_WRONLY)) < 0)
        return 1405;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8065;
    path_join(p, base, "/open");
    if (sys_mkdir(p) != 0 || sys_chmod(p, 0777) != 0 || sys_chmod(p, 01000) != 0 ||
        sys_stat(p, &st) != 0 || st.mode != 0 || sys_chmod(p, 0777) != 0)
        return 1406;                                     /* 權限以外的位元被忽略 */
    if (sys_chown("/no/such", 1, 1) != -E_NOENT || sys_chown(base, 70000, 1) != -E_INVAL)
        return 1407;

    if ((rc = as_user(1000, perm_user_checks, base)) != 0)
        return 1410 + rc;
    if ((rc = as_user(1001, perm_other_checks, base)) != 0)
        return 1440 + rc;

    /* root 可以改擁有者;改完之後原本被拒絕的人就可以用了 */
    path_join(p, base, "/secret");
    if (sys_chown(p, 1001, 1001) != 0 || sys_stat(p, &st) != 0 || st.uid != 1001 || st.gid != 1001)
        return 1450;

    path_join(p, base, "/priv/inside"); sys_unlink(p);
    path_join(p, base, "/open"); sys_unlink(p);
    path_join(p, base, "/priv"); sys_unlink(p);
    path_join(p, base, "/secret"); sys_unlink(p);
    path_join(p, base, "/pub"); sys_unlink(p);
    if (sys_unlink(base) != 0)
        return 1451;
    return 0;
}

static long perm_root_pid;                       /* 測試主行程(root)的編號,fork 之前記下 */

static int perm_drop_checks(const char *unused)
{
    (void)unused;
    /* 變成一般使用者之後:不能變回 root、不能變成別人;「改成自己」沒有作用但允許 */
    if (sys_setuid(0, 0) != -E_PERM || sys_setuid(1001, 1001) != -E_PERM || sys_setuid(1000, 1000) != 0)
        return 1;
    if (sys_capdrop(CAP_ALL) != 0)                       /* 本來就沒有:沒有東西可以放棄 */
        return 2;
    /* 不能對別人(這裡是 root 的父行程)送訊號;對自己可以 */
    if (sys_kill(perm_root_pid, SIGTERM) != -E_PERM || sys_kill(sys_getpid(), SIGCONT) != 0)
        return 3;
    return 0;
}

/* 行程這一側:特權只能減少、誰可以對誰送訊號 */
static int perm_process(void)
{
    struct stat st;
    int rc, status = 0;
    long pid, fd;

    if (sys_getuid() != 0 || sys_capget() != (long)CAP_ALL)
        return 1460;
    if (sys_setuid(70000, 0) != -E_INVAL || sys_setuid(-1, 0) != -E_INVAL)
        return 1461;
    perm_root_pid = sys_getpid();
    if ((rc = as_user(1000, perm_drop_checks, "")) != 0)
        return 1462 + rc;

    /* root 只放棄 CAP_FS(最小權限):仍然是 uid 0,但檔案權限開始對它生效 */
    sys_unlink("/pm-cap");
    if ((fd = sys_open("/pm-cap", O_CREAT | O_WRONLY)) < 0 || sys_chown("/pm-cap", 1000, 1000) != 0 ||
        sys_chmod("/pm-cap", 0600) != 0)
        return 1470;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8129;
    pid = sys_fork();
    if (pid == 0) {
        long ok = sys_open("/pm-cap", O_RDONLY);         /* 有 CAP_FS:別人的 0600 檔案也打得開 */
        if (ok < 0 || sys_capdrop(CAP_FS) != (long)(CAP_ALL & ~CAP_FS) || sys_getuid() != 0)
            sys_exit(1);
        if (sys_open("/pm-cap", O_RDONLY) != -E_ACCES || sys_chown("/pm-cap", 0, 0) != -E_PERM)
            sys_exit(2);                                 /* 放棄之後:和一般人一樣被擋下 */
        if (sys_capdrop(0) != (long)(CAP_ALL & ~CAP_FS))
            sys_exit(3);                                 /* 沒有任何呼叫可以把它拿回來 */
        sys_exit(0);
    }
    if (pid < 0 || sys_waitpid(pid, &status, 0) != pid || status != 0)
        return 1471 + status;
    if (sys_stat("/pm-cap", &st) != 0 || sys_unlink("/pm-cap") != 0)
        return 1475;

    /* 一般使用者的行程:別的使用者不能終止它,root(有 CAP_KILL)可以 */
    pid = sys_fork();
    if (pid == 0) {
        sys_setuid(1000, 1000);
        for (;;)
            sys_sleep(1000);
    }
    sys_sleep(3);
    long killer = sys_fork();
    if (killer == 0) {
        sys_setuid(1001, 1001);
        sys_exit(sys_kill(pid, SIGKILL) == -E_PERM && sys_kill(-pid, SIGKILL) != 0 ? 0 : 1);
    }
    if (killer < 0 || sys_waitpid(killer, &status, 0) != killer || status != 0)
        return 1480;
    if (sys_waitpid(pid, &status, WNOHANG) != 0)         /* 還活著 */
        return 1481;
    if (sys_kill(pid, SIGKILL) != 0 || sys_waitpid(pid, &status, 0) != pid || status != 128 + SIGKILL)
        return 1482;
    return 0;
}

/* ---- UMODE_TIME ---- */

static int same_date(unsigned long long epoch, int off, int y, int mo, int d, int h, int mi, int s, int wd)
{
    struct datetime dt;

    gmtime_(epoch, off, &dt);
    return dt.year == y && dt.mon == mo && dt.day == d && dt.hour == h && dt.min == mi &&
           dt.sec == s && dt.wday == wd;
}

static int mode_time(void)
{
    struct timeval a, b;
    struct datetime dt;
    struct stat st;

    /* 現在的時間:模擬器的時鐘跟著主機走,所以應該落在合理的範圍(2023 ~ 2100 年) */
    if (sys_time(&a) != 0 || a.sec < 1700000000ull || a.sec > 4102444800ull || a.usec >= 1000000)
        return 1301;
    if (sys_time((struct timeval *)KERNEL_ADDR) != -E_FAULT)
        return 1302;
    sys_sleep(30);
    if (sys_time(&b) != 0 || b.sec < a.sec || b.sec > a.sec + 5)
        return 1303;
    /* 30ms 之後,時間(換成微秒)往前走了至少 20ms */
    unsigned long da = (unsigned long)(b.sec - a.sec) * 1000000ul + b.usec - a.usec;
    if (da < 20000 || da > 3000000)
        return 1304;

    /* 秒數換成年月日:幾個知道答案的時間點,包括閏年的 2 月 29 日與世紀年 */
    if (!same_date(0, 0, 1970, 1, 1, 0, 0, 0, 4) ||
        !same_date(951782400, 0, 2000, 2, 29, 0, 0, 0, 2) ||          /* 2000 是閏年 */
        !same_date(951868799, 0, 2000, 2, 29, 23, 59, 59, 2) ||
        !same_date(951868800, 0, 2000, 3, 1, 0, 0, 0, 3) ||
        !same_date(1700000000, 0, 2023, 11, 14, 22, 13, 20, 2) ||
        !same_date(1700000000, 8, 2023, 11, 15, 6, 13, 20, 3) ||      /* 加 8 小時:跨到隔天 */
        !same_date(1709164800, 0, 2024, 2, 29, 0, 0, 0, 4) ||
        !same_date(4107542399u, 0, 2100, 2, 28, 23, 59, 59, 0) ||     /* 2100 不是閏年 */
        !same_date(4107542400u, 0, 2100, 3, 1, 0, 0, 0, 1))
        return 1305;

    /* 檔案的修改時間:建立時是現在;之後寫入會更新 */
    sys_unlink("/time-test");
    long fd = sys_open("/time-test", O_CREAT | O_RDWR);
    if (fd < 0 || sys_stat("/time-test", &st) != 0 || st.mtime < a.sec || st.mtime > b.sec + 5)
        return 1306;
    unsigned long long created = st.mtime;
    /* (時間以秒為單位;為了不讓測試多等一秒,這裡只確認寫入之後時間不會倒退、也不會超過現在) */
    if (sys_write(fd, "x", 1) != 1 || sys_stat("/time-test", &st) != 0 || st.mtime < created ||
        sys_time(&b) != 0 || st.mtime > b.sec)
        return 1308;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8220;
    /* 目錄的修改時間:裡面的東西增減時更新 */
    unsigned long long file_time = st.mtime;
    if (sys_stat("/", &st) != 0 || sys_unlink("/time-test") != 0)
        return 1309;
    if (sys_stat("/", &st) != 0 || st.mtime < file_time)
        return 1310;

    gmtime_(b.sec, 0, &dt);
    printf("  [user] the clock reads %04d-%02d-%02d %02d:%02d:%02d UTC; dates convert correctly\n",
           dt.year, dt.mon, dt.day, dt.hour, dt.min, dt.sec);
    return 25;
}

/* ---- UMODE_SIGHANDLER ---- */

static volatile int sig_seen[32];                /* 每個訊號的處理函式被呼叫了幾次 */
static volatile int sig_depth, sig_max_depth;

static void on_signal(int sig)
{
    char msg[40];

    /* 處理函式裡照常可以用 stack、呼叫函式、做系統呼叫 */
    sig_depth++;
    if (sig_depth > sig_max_depth)
        sig_max_depth = sig_depth;
    snprintf(msg, sizeof(msg), "x%dx", sig);
    if (msg[0] == 'x' && sig > 0 && sig < 32)
        sig_seen[sig]++;
    sys_getpid();
    sig_depth--;
}

static void on_signal_exit(int sig)
{
    sys_exit(70 + sig);                          /* 處理函式也可以選擇不返回 */
}

/* 一段很吃暫存器的運算:如果處理函式執行前後暫存器沒有被完整還原,結果就會錯 */
static unsigned long churn(unsigned long seed, int rounds)
{
    unsigned long a = seed, b = 3, c = 5, d = 7, e = 11, f = 13, g = 17, h = 19;

    for (int i = 0; i < rounds; i++) {
        a = a * 31 + b;
        b = b * 17 + c;
        c ^= d + (a >> 3);
        d = d * 5 + e;
        e += f ^ (b << 1);
        f = f * 7 + g;
        g ^= h + c;
        h = h * 3 + a;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

static int mode_sighandler(void)
{
    long me = sys_getpid(), pid;
    int st = 0, fds[2];
    char buf[8];

    /* 1. 登記與基本的執行:送給自己,系統呼叫返回之前處理函式就執行過了 */
    if (sys_signal(SIGUSR1, on_signal) != 0)             /* 原本是預設(0) */
        return 1201;
    if (sys_signal(SIGUSR1, on_signal) != (long)on_signal)
        return 1202;                                     /* 回傳原本的處理方式 */
    if (sys_kill(me, SIGUSR1) != 0 || sig_seen[SIGUSR1] != 1)
        return 1203;
    if (sys_signal(SIGKILL, on_signal) != -E_INVAL || sys_signal(19, SIG_IGN) != -E_INVAL ||
        sys_signal(99, on_signal) != -E_INVAL)
        return 1204;                                     /* 不能攔截的、不存在的 */
    if (sys_signal(SIGUSR2, (sighandler_t)KERNEL_ADDR) != -E_FAULT)
        return 1205;                                     /* 處理函式必須是自己可以執行的位址 */

    /* 2. 運算途中被打斷許多次:結果必須和沒有被打斷時完全相同(暫存器有完整還原) */
    unsigned long want = churn(12345, 60000);
    pid = sys_fork();
    if (pid == 0) {
        for (int i = 0; i < 20; i++) {                   /* 子行程不斷送訊號給我們 */
            if (sys_kill(me, SIGUSR1) != 0)
                sys_exit(2);
            sys_sleep(1);
        }
        sys_exit(0);
    }
    unsigned long got = 0;
    for (int i = 0; i < 30; i++)                         /* 算很多次,讓訊號有機會落在各種位置 */
        got ^= churn(12345, 60000) ^ (unsigned long)i;
    unsigned long expect = 0;
    for (int i = 0; i < 30; i++)
        expect ^= want ^ (unsigned long)i;
    /* 等子行程時也會被訊號打斷(-E_INTR):再等一次 */
    while (sys_waitpid(pid, &st, 0) == -E_INTR)
        ;
    if (pid < 0 || got != expect || sig_seen[SIGUSR1] < 5 || sig_max_depth < 1)
        return 1210;

    /* 3. 正在睡的時候收到訊號:提早醒來,處理函式執行 */
    int before = sig_seen[SIGUSR1];
    pid = sys_fork();
    if (pid == 0) {
        sys_sleep(6);
        if (sys_kill(me, SIGUSR1) != 0)
            sys_exit(2);
        sys_exit(0);
    }
    if (pid < 0)
        return 1223 + (pid == -E_NOMEM ? 0 : 1);         /* fork 失敗:1223 = 記憶體 / 預留不足,1224 = 其他 */
    struct sysinfo t0, t1;
    if (sys_sysinfo(&t0) != 0)
        return 1221;
    sys_sleep(5000);
    if (sys_sysinfo(&t1) != 0)
        return 1222;
    if (sig_seen[SIGUSR1] != before + 1 || t1.uptime_ticks - t0.uptime_ticks > 2000)
        return 1220;
    while (sys_waitpid(pid, &st, 0) == -E_INTR)
        ;

    /* 4. 正在等管線的資料時收到訊號:read 回傳 -E_INTR,而不是「結束」(0);之後照常可以讀 */
    if (sys_pipe(fds) != 0)
        return 1230;
    pid = sys_fork();
    if (pid == 0) {
        sys_sleep(6);
        if (sys_kill(me, SIGUSR1) != 0)
            sys_exit(2);
        sys_sleep(6);
        sys_exit(sys_write(fds[1], "ok", 2) == 2 ? 0 : 3);
    }
    if (sys_read(fds[0], buf, sizeof(buf)) != -E_INTR)
        return 1231;
    if (sys_read(fds[0], buf, sizeof(buf)) != 2 || buf[0] != 'o')
        return 1232;
    while (sys_waitpid(pid, &st, 0) == -E_INTR)
        ;
    if (sys_close(fds[0]) != 0 || sys_close(fds[1]) != 0)
        return 1233;

    /* 5. 忽略:SIGTERM 不再終止行程;改回預設之後又會 */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_signal(SIGTERM, SIG_IGN) < 0 || sys_write(1, "", 0) < 0)
            sys_exit(6);
        for (;;)
            sys_sleep(1000);
    }
    sys_sleep(4);
    if (pid <= 0 || sys_kill(pid, SIGTERM) != 0)
        return 1242;
    sys_sleep(4);
    if (sys_waitpid(pid, &st, WNOHANG) != 0)             /* 還活著 */
        return 1240;
    if (sys_kill(pid, SIGKILL) != 0)                     /* SIGKILL 永遠有效 */
        return 1243;
    if (sys_waitpid(pid, &st, 0) != pid || st != 128 + SIGKILL)
        return 1241;

    /* 6. 處理函式可以不返回(直接結束);fork 的子行程沿用父行程的設定 */
    if (sys_signal(SIGINT, on_signal_exit) < 0)
        return 1255;
    pid = sys_fork();
    if (pid == 0) {
        for (;;)
            sys_sleep(1000);
    }
    sys_sleep(5);
    if (pid <= 0 || sys_kill(pid, SIGINT) != 0)
        return 1251;
    if (sys_waitpid(pid, &st, 0) != pid || st != 70 + SIGINT)
        return 1250;
    if (sys_signal(SIGINT, SIG_DFL) < 0)
        return 1256;

    /* 7. exec 之後,處理函式還原成預設(那些位址是舊程式的):被 SIGUSR1 終止 */
    pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { "sleep", "5", 0 };
        long e = sys_exec("/bin/sleep", argv);          /* 成功就不會回來 */
        sys_exit(e < 0 ? 1 : 2);
    }
    sys_sleep(6);
    if (pid <= 0 || sys_kill(pid, SIGUSR1) != 0)
        return 1261;
    if (sys_waitpid(pid, &st, 0) != pid || st != 128 + SIGUSR1)
        return 1260;

    /* 8. 捏造的 sigreturn:給 kernel 一份全是 0xFF 的「暫存器」。
          kernel 不可以因此出事(特權的部分不採用);行程自己跳到不存在的位址而被終止 */
    pid = sys_fork();
    if (pid == 0) {
        static unsigned char fake[1024];
        for (unsigned i = 0; i < sizeof(fake); i++)
            fake[i] = 0xFF;
        syscall3(SYS_sigreturn, (long)fake, 0, 0);
        sys_exit(0);                                     /* 不該到這裡 */
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != -1)
        return 1270;
    pid = sys_fork();
    if (pid == 0) {
        syscall3(SYS_sigreturn, (long)KERNEL_ADDR, 0, 0);    /* 位址根本不是自己的 */
        sys_exit(0);
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != -1)
        return 1271;

    /* 9. stack 壞掉的時候收到訊號:沒有地方放暫存器,行程被終止,kernel 不受影響 */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_signal(SIGUSR2, on_signal) < 0 || sys_write(1, "", 0) < 0)
            sys_exit(7);
#if defined(__i386__)
        __asm__ volatile("mov $16, %esp\n mov $4, %eax\n 1: int $0x80\n jmp 1b");       /* yield 不斷 */
#elif defined(__riscv)
        __asm__ volatile("li sp, 16\n li a7, 4\n 1: ecall\n j 1b");
#elif defined(__aarch64__)
        __asm__ volatile("mov x9, #16\n mov sp, x9\n mov x8, #4\n 1: svc #0\n b 1b");
#endif
    }
    sys_sleep(4);
    if (pid <= 0 || sys_kill(pid, SIGUSR2) != 0)
        return 1281;
    if (sys_waitpid(pid, &st, 0) != pid || st != 128 + SIGUSR2)
        return 1280;

    printf("  [user] signal handlers: %d deliveries, registers intact, interrupted sleeps and reads resume\n",
           sig_seen[SIGUSR1]);
    return 24;
}

/* ---- UMODE_HEAP ---- */

static unsigned self_mem_kb(void)
{
    struct procinfo pi;
    long me = sys_getpid();

    return sys_procinfo(me - 1, &pi) == 0 && pi.pid == me ? pi.mem_kb : 0;
}

static volatile int heap_threads_done, heap_threads_bad;

/* 每條執行緒反覆配置、填入自己的記號、檢查、釋放:別人的資料混進來就看得出來 */
static void heap_thread(long id)
{
    for (int round = 0; round < 150; round++) {
        size_t n = 16 + (size_t)((round * 37 + id * 101) % 900);
        unsigned char *p = malloc(n);
        if (!p) {
            heap_threads_bad = 1;
            break;
        }
        memset(p, (int)(id + 1), n);
        if (round % 7 == 0)
            sys_yield();
        for (size_t i = 0; i < n; i++)
            if (p[i] != (unsigned char)(id + 1))
                heap_threads_bad = 1;
        free(p);
    }
    __sync_fetch_and_add(&heap_threads_done, 1);
    sys_thread_exit();
}

static int mode_heap(void)
{
    char text[80];
    static char *blk[120];
    int st = 0;

    /* ---- sbrk:kernel 這一側 ---- */
    long base = sys_sbrk(0);
    if (base <= 0 || sys_sbrk(0) != base)
        return 1101;
    unsigned kb0 = self_mem_kb();
    if (sys_sbrk(64 * 1024) != base)                     /* 回傳移動之前的結尾 */
        return 1102;
    if (self_mem_kb() != kb0)                            /* 還沒碰:一頁都沒配置 */
        return 1103;
    char *h = (char *)base;
    for (int i = 0; i < 64 * 1024; i += 4096)
        if (h[i] != 0)                                   /* 新的記憶體保證是 0 */
            return 1104;
    if (self_mem_kb() != kb0 + 64)                       /* 碰過的 16 頁現在才配置 */
        return 1105;
    h[100] = 'x';
    if (sys_sbrk(-32 * 1024) != base + 64 * 1024 || self_mem_kb() != kb0 + 32)
        return 1106;                                     /* 縮小:後半立刻歸還 */
    if (sys_sbrk(32 * 1024) != base + 32 * 1024 || h[32 * 1024] != 0 || h[100] != 'x')
        return 1107;                                     /* 再長回來:歸還過的部分又是 0,前半不受影響 */
    if (sys_sbrk(-(1L << 28)) != -E_NOMEM || sys_sbrk((1L << 28) + 4096) != -E_NOMEM)
        return 1108;                                     /* 縮到起點之前;超過上限 */
    /* 結尾之後的位址不屬於行程:碰了會被終止(在子行程裡試) */
    long pid = sys_fork();
    if (pid == 0) {
        volatile char *beyond = (char *)sys_sbrk(0) + 8192;
        *beyond = 1;
        sys_exit(0);
    }
    if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || st != -1)
        return 1109;
    if (sys_sbrk(-64 * 1024) != base + 64 * 1024 || self_mem_kb() != kb0)
        return 1110;

    /* ---- malloc / free ---- */
    for (int i = 0; i < 120; i++) {
        size_t n = 8 + (size_t)(i * 53 % 700);
        blk[i] = malloc(n);
        if (!blk[i] || ((unsigned long)blk[i] & 15))     /* 對齊 16 位元組 */
            return 1120;
        memset(blk[i], i, n);
    }
    for (int i = 0; i < 120; i += 2) {                   /* 釋放一半,留下許多空洞 */
        free(blk[i]);
        blk[i] = 0;
    }
    for (int i = 0; i < 120; i += 2) {                   /* 再配置:會用到那些空洞 */
        blk[i] = malloc(8 + (size_t)(i * 31 % 300));
        if (!blk[i])
            return 1121;
        memset(blk[i], i, 8);
    }
    for (int i = 1; i < 120; i += 2) {                   /* 沒被釋放的那一半,內容必須原封不動 */
        size_t n = 8 + (size_t)(i * 53 % 700);
        for (size_t k = 0; k < n; k++)
            if (blk[i][k] != (char)i)
                return 1122;
    }
    long peak = sys_sbrk(0);
    for (int i = 0; i < 120; i++)
        free(blk[i]);
    /* 全部釋放後,相鄰的空塊應該併成一大塊:再要一塊和整個 heap 差不多大的,不必向 kernel 多要 */
    char *big = malloc((size_t)(peak - base) - 4096);
    if (!big || sys_sbrk(0) != peak)
        return 1123;
    free(big);

    /* realloc 保留內容;calloc 清為 0;大小為 0 也給一個可以 free 的指標 */
    char *r = malloc(10);
    if (!r)
        return 1149;
    strcpy(r, "123456789");
    r = realloc(r, 5000);
    if (!r || strcmp(r, "123456789") != 0)
        return 1124;
    free(r);
    int *z = calloc(100, sizeof(int));
    for (int i = 0; z && i < 100; i++)
        if (z[i] != 0)
            return 1125;
    void *zero = malloc(0);
    if (!z || !zero || malloc((size_t)1 << 31) != 0 || calloc((size_t)1 << 20, (size_t)1 << 20) != 0)
        return 1126;
    free(z);
    free(zero);
    free(0);

    /* 很大的一塊:釋放之後還給 kernel(heap 的結尾退回去,記憶體用量下降) */
    long before = sys_sbrk(0);
    char *mb = malloc(1 << 20);
    if (!mb)
        return 1127;
    for (int i = 0; i < (1 << 20); i += 4096)
        mb[i] = 1;
    unsigned kb_big = self_mem_kb();
    free(mb);
    if (sys_sbrk(0) > before + 128 * 1024 || self_mem_kb() + 512 > kb_big)
        return 1128;

    /* fork:子行程改它自己的 heap,不影響我們的 */
    char *shared = malloc(32);
    if (!shared)
        return 1150;
    strcpy(shared, "parent");
    pid = sys_fork();
    if (pid == 0) {
        strcpy(shared, "child");
        char *more = malloc(100000);                     /* 子行程的 malloc 照常運作 */
        sys_exit(more && strcmp(shared, "child") == 0 ? 0 : 1);
    }
    if (pid < 0 || sys_wait(pid) != 0 || strcmp(shared, "parent") != 0)
        return 1130;
    free(shared);

    /* 同一塊釋放兩次:被偵測到,程式以 134 結束(在子行程裡試) */
    pid = sys_fork();
    if (pid == 0) {
        char *twice = malloc(40);
        sys_close(2);                                    /* 錯誤訊息不必印出來 */
        free(twice);
        free(twice);
        sys_exit(0);
    }
    if (pid < 0 || sys_wait(pid) != 134)
        return 1131;

    /* 三條執行緒同時 malloc / free */
    for (long i = 0; i < 3; i++)
        if (sys_thread_create(heap_thread, i) < 0)
            return 1132;
    for (int i = 0; i < 2000 && heap_threads_done < 3; i++)
        sys_sleep(1);
    if (heap_threads_done != 3 || heap_threads_bad)
        return 1133;

    /* ---- 格式化輸出與字串函式 ---- */
    int len = snprintf(text, sizeof(text), "%d|%5d|%-5d|%05d|%x|%X|%s|%c|%%|%lu", -42, 42, 42, 42,
                       0xbeef, 0xBEEF, "str", 'c', 4000000000ul);
    if (strcmp(text, "-42|   42|42   |00042|beef|BEEF|str|c|%|4000000000") != 0 || len != (int)strlen(text))
        return 1140;
    snprintf(text, sizeof(text), "%lld %llu %llx %-6s| %6s|%*d", -1234567890123ll,
             18446744073709551615ull, 0x123456789abcdefull, "ab", "cd", 4, 7);
    if (strcmp(text, "-1234567890123 18446744073709551615 123456789abcdef ab    |     cd|   7") != 0)
        return 1141;
    if (snprintf(text, 5, "%s", "truncated") != 9 || strcmp(text, "trun") != 0)   /* 回傳完整的長度 */
        return 1142;
    /* 寬度的極端值(Codex 的靜態檢查發現原本會溢位):現在一律限制在 1024,結果是確定的。
       格式字串與寬度經由 volatile 變數傳入:否則編譯器會依「標準的 snprintf」的行為
       預先算出結果並發出警告,而我們要測的正是自己的實作在執行時怎麼處理 */
    {
        const char *volatile star = "%*d", *volatile huge = "%2147483648d",
                   *volatile huger = "%99999999999999999999d";
        volatile int int_min = -2147483647 - 1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
        if (snprintf(text, sizeof(text), star, int_min, 1) != 1024 || text[0] != '1' || text[1] != ' ')
            return 1146;                                 /* 負的寬度:靠左,補在後面 */
        if (snprintf(text, sizeof(text), huge, 1) != 1024 || text[0] != ' ' ||
            snprintf(text, sizeof(text), huger, 7) != 1024)
            return 1147;
#pragma GCC diagnostic pop
    }
    if (snprintf(text, sizeof(text), "%*d|%-*d|", 3, 5, 3, 5) != 8 || strcmp(text, "  5|5  |") != 0)
        return 1148;                                     /* 一般的寬度不受影響 */
    if (strlen("abc") != 3 || strcmp("abc", "abd") >= 0 || strncmp("abcx", "abcy", 3) != 0)
        return 1143;
    strcpy(text, "0123456789");
    memmove(text + 2, text, 5);                          /* 重疊的搬移 */
    if (strcmp(text, "0101234789") != 0 || atoi("  -77x") != -77 || memcmp("ab", "ac", 2) >= 0)
        return 1144;
    const char *path = "/a/b/c";
    if (strrchr(path, '/') != path + 4 || strchr(path, 'b') != path + 3 || strchr(path, 'z') != 0)
        return 1145;

    printf("  [user] heap: %d KB in use after the tests; malloc, printf and the string functions work\n",
           (int)self_mem_kb());
    return 23;
}

/* ---- UMODE_FILEOPS ---- */

static int path_join(char *out, const char *dir, const char *name)
{
    unsigned n = 0;

    for (; *dir; dir++)
        out[n++] = *dir;
    for (; *name; name++)
        out[n++] = *name;
    out[n] = 0;
    return 0;
}

/* 在目錄 base(還不存在)底下測試 lseek、rename、硬連結、工作目錄;結束時全部刪除 */
static int fileops_on(const char *base)
{
    char a[80], b[80], d[80], e[80], buf[16], cwd[80];
    struct stat st;
    long fd;

    path_join(a, base, "/a");
    path_join(b, base, "/b");
    path_join(d, base, "/dir");
    path_join(e, base, "/dir/sub");
    sys_unlink(a); sys_unlink(b); sys_unlink(e); sys_unlink(d); sys_unlink(base);   /* 上次留下的 */
    if (sys_mkdir(base) != 0)
        return 1001;

    /* ---- lseek ---- */
    fd = sys_open(a, O_CREAT | O_RDWR);
    if (fd < 0 || sys_write(fd, "0123456789", 10) != 10)
        return 1002;
    if (sys_lseek(fd, 3, SEEK_SET) != 3 || sys_read(fd, buf, 2) != 2 || buf[0] != '3' || buf[1] != '4')
        return 1003;
    if (sys_lseek(fd, 2, SEEK_CUR) != 7 || sys_read(fd, buf, 1) != 1 || buf[0] != '7')
        return 1004;
    if (sys_lseek(fd, -1, SEEK_END) != 9 || sys_read(fd, buf, 4) != 1 || buf[0] != '9')
        return 1005;
    if (sys_lseek(fd, -1, SEEK_SET) != -E_INVAL || sys_lseek(fd, 0, 7) != -E_INVAL)
        return 1006;
    /* 移到結尾之後再寫:中間沒寫過的部分讀起來是 0 */
    if (sys_lseek(fd, 5000, SEEK_SET) != 5000 || sys_write(fd, "Z", 1) != 1)
        return 1007;
    if (sys_stat(a, &st) != 0 || st.size != 5001)
        return 1008;
    if (sys_lseek(fd, 4999, SEEK_SET) != 4999 || sys_read(fd, buf, 4) != 2 || buf[0] != 0 || buf[1] != 'Z')
        return 1009;
    if (sys_lseek(fd, 2, SEEK_SET) != 2 || sys_write(fd, "xy", 2) != 2 ||
        sys_lseek(fd, 0, SEEK_SET) != 0 || sys_read(fd, buf, 6) != 6 || buf[2] != 'x' || buf[4] != '4')
        return 1010;                                     /* 寫在中間:只蓋掉那兩個位元組 */
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8718;
    if (sys_lseek(fd, 0, SEEK_SET) != -E_BADF || sys_lseek(1, 0, SEEK_SET) != -E_SPIPE)
        return 1011;                                     /* 已關閉;主控台沒有位置 */

    /* ---- rename ---- */
    if (sys_rename(a, b) != 0 || sys_stat(a, &st) != -E_NOENT || sys_stat(b, &st) != 0 ||
        st.size != 5001 || st.nlink != 1)
        return 1020;
    if (sys_rename(a, b) != -E_NOENT)                    /* 來源不存在 */
        return 1021;
    fd = sys_open(a, O_CREAT | O_WRONLY);
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8729;
    if (sys_rename(a, b) != -E_EXIST || sys_stat(a, &st) != 0)   /* 目的已存在:不蓋掉,來源還在 */
        return 1022;
    if (sys_rename(b, b) != 0)                           /* 改成自己:沒有事 */
        return 1023;
    /* 目錄:搬進另一個目錄,".." 跟著改;不可以搬進自己底下 */
    if (sys_mkdir(d) != 0 || sys_mkdir(e) != 0)
        return 1024;
    {
        char into[80], moved[80], up[80];
        path_join(into, e, "/a");                        /* base/dir/sub/a */
        if (sys_rename(a, into) != 0 || sys_stat(into, &st) != 0 || sys_stat(a, &st) != -E_NOENT)
            return 1025;
        path_join(moved, d, "/sub/deeper");
        if (sys_rename(d, moved) != -E_INVAL)            /* base/dir -> base/dir/sub/deeper */
            return 1026;
        path_join(moved, base, "/sub2");
        if (sys_rename(e, moved) != 0)                   /* base/dir/sub -> base/sub2 */
            return 1027;
        path_join(up, moved, "/../b");                   /* base/sub2/../b:".." 現在是 base */
        if (sys_stat(up, &st) != 0 || st.size != 5001)
            return 1028;
        path_join(into, moved, "/a");
        if (sys_unlink(into) != 0 || sys_unlink(moved) != 0 || sys_unlink(d) != 0)
            return 1029;
    }

    /* ---- 硬連結:同一個檔案的兩個名稱 ---- */
    if (sys_link(b, a) != 0 || sys_stat(a, &st) != 0 || st.nlink != 2 || st.size != 5001)
        return 1030;
    if (sys_link(b, a) != -E_EXIST || sys_link(base, d) != -E_INVAL)     /* 已存在;目錄不行 */
        return 1031;
    fd = sys_open(a, O_WRONLY);
    if (fd < 0 || sys_write(fd, "LINK", 4) != 4)
        return 1032;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8764;
    fd = sys_open(b, O_RDONLY);                          /* 從另一個名稱讀到同樣的內容 */
    if (fd < 0 || sys_read(fd, buf, 4) != 4 || buf[0] != 'L' || buf[3] != 'K')
        return 1033;
    sys_close(fd);
    if (sys_unlink(b) != 0 || sys_stat(a, &st) != 0 || st.nlink != 1 || st.size != 5001)
        return 1034;                                     /* 刪掉一個名稱,檔案還在 */

    /* ---- 目前的工作目錄 ---- */
    if (sys_getcwd(cwd, sizeof(cwd)) != 1 || !streq(cwd, "/"))
        return 1040;
    if (sys_chdir(a) != -E_NOTDIR || sys_chdir("/no/such") != -E_NOENT)
        return 1041;
    if (sys_chdir(base) != 0 || sys_getcwd(cwd, sizeof(cwd)) <= 0 || !streq(cwd, base))
        return 1042;
    if (sys_stat("a", &st) != 0 || st.size != 5001 || sys_stat("./a", &st) != 0)   /* 相對路徑 */
        return 1043;
    if (sys_getcwd(cwd, 2) != -E_INVAL || sys_stat("", &st) != -E_NOENT)
        return 1044;
    if (sys_mkdir("rel") != 0 || sys_chdir("rel") != 0 || sys_chdir("..") != 0 ||
        sys_getcwd(cwd, sizeof(cwd)) <= 0 || !streq(cwd, base))
        return 1045;
    /* 子行程沿用工作目錄:fork 出來的用相對路徑建立檔案,我們用完整路徑看得到 */
    long pid = sys_fork();
    if (pid == 0) {
        long f2 = sys_open("rel/child", O_CREAT | O_WRONLY);
        sys_exit(f2 >= 0 ? 0 : 1);
    }
    path_join(cwd, base, "/rel/child");
    if (pid < 0 || sys_wait(pid) != 0 || sys_stat(cwd, &st) != 0)
        return 1046;
    if (sys_chdir("/../../etc/../") != 0 || sys_getcwd(cwd, sizeof(cwd)) != 1)     /* 整理成 "/" */
        return 1047;
    path_join(cwd, base, "/rel/child");
    sys_unlink(cwd);
    path_join(cwd, base, "/rel");
    if (sys_unlink(cwd) != 0 || sys_unlink(a) != 0 || sys_unlink(base) != 0)
        return 1049;
    return 0;
}

/* ---- UMODE_SYSINFO ---- */

/* top 需要的三樣東西:整個系統的狀態、每個行程的記憶體用量、主控台的 raw 模式 */
static int mode_sysinfo(void)
{
    struct sysinfo a, b;
    struct procinfo pi;
    unsigned long long ticks_a = 0, ticks_b = 0;
    long me = sys_getpid();

    if (sys_sysinfo(&a) != 0)
        return 901;
    if (a.hz != 1000 || a.ncpu < 1 || a.ncpu > SYSINFO_MAX_CPUS || a.nprocs < 1 || a.nthreads < 1)
        return 902;
    if (a.mem_total_kb == 0 || a.mem_free_kb == 0 || a.mem_free_kb >= a.mem_total_kb)
        return 903;
    if (sys_sysinfo((struct sysinfo *)KERNEL_ADDR) != -E_FAULT)
        return 904;

    /* 空轉 20ms:時間往前走,而且至少有一顆 CPU 記到忙碌的 tick。
       沒有在使用的 CPU 欄位必須是 0(不可以帶出 kernel 的資料) */
    for (unsigned long long t0 = a.uptime_ticks; ; ) {
        if (sys_sysinfo(&b) != 0)
            return 905;
        if (b.uptime_ticks - t0 >= 20)
            break;
    }
    for (unsigned i = 0; i < SYSINFO_MAX_CPUS; i++) {
        if (b.cpu[i].busy < a.cpu[i].busy || b.cpu[i].idle < a.cpu[i].idle)
            return 906;                                  /* 累計值只會增加 */
        if (i >= b.ncpu && (b.cpu[i].busy || b.cpu[i].idle))
            return 907;
        ticks_a += a.cpu[i].busy;
        ticks_b += b.cpu[i].busy;
    }
    if (ticks_b <= ticks_a || b.ctx_switches < a.ctx_switches)
        return 908;

    /* 自己的記憶體用量:至少有程式碼、資料與 stack 的幾頁;多用一些 stack 之後會增加 */
    if (sys_procinfo(me - 1, &pi) != 0 || pi.pid != me || pi.mem_kb < 16)
        return 910;
    unsigned before = pi.mem_kb;
    if (recurse_limited(0, 200) != 200)                  /* 約 60KB 的 stack */
        return 911;
    if (sys_procinfo(me - 1, &pi) != 0 || pi.mem_kb < before + 32)
        return 912;

    /* 主控台的輸入模式:不認得的位元被拒絕;設定與還原都成功 */
    if (sys_tcsetmode(64) != -E_INVAL)
        return 920;
    if (sys_tcsetmode(CON_RAW | CON_NOECHO) != 0 || sys_tcsetmode(0) != 0)
        return 921;

    puts_("  [user] system information, per-process memory and the console input mode work\n");
    return 21;
}

/* ---- UMODE_DISK ---- */

/* 磁碟上的檔案系統,從使用者程式這一側看:和記憶體裡的檔案用法完全相同 */
static int mode_disk(void)
{
    static char buf[512];
    struct fsinfo f0, f1, fr;
    struct stat st;
    char *argv[] = { "echo", "  [disk] this program was loaded from the disk", 0 };
    long in, out, n, total = 0;

    if (sys_statfs("/disk", &f0) != 0 || !streq(f0.type, "wfs") || f0.block_size != 4096)
        return 801;
    if (sys_statfs("/", &fr) != 0 || !streq(fr.type, "ramfs"))
        return 802;
    if (sys_statfs("/disk", (struct fsinfo *)KERNEL_ADDR) != -E_FAULT ||
        sys_statfs("/nope", &fr) != -E_NOENT)
        return 803;

    /* 把 /bin/args 複製到磁碟上(結束碼 = 參數個數,看得出確實執行了磁碟上的這一份) */
    sys_unlink("/disk/echo-test");                       /* 上次測試被打斷時留下的 */
    if (sys_statfs("/disk", &f0) != 0)
        return 804;
    in = sys_open("/bin/args", O_RDONLY);
    out = sys_open("/disk/echo-test", O_CREAT | O_WRONLY | O_TRUNC);
    if (in < 0 || out < 0)
        return 805;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        if (sys_write(out, buf, (unsigned long)n) != n)
            return 806;
        total += n;
    }
    sys_close(in);
    if (sys_close(out) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 8895;
    if (sys_stat("/disk/echo-test", &st) != 0 || st.size != (unsigned long long)total)
        return 807;
    /* 新建立的檔案沒有執行權限:這時還不能執行;加上之後才可以 */
    if (sys_spawn("/disk/echo-test", argv) != -E_ACCES || sys_chmod("/disk/echo-test", 0755) != 0)
        return 814;
    if (sys_statfs("/disk", &f1) != 0 ||
        f1.free_blocks != f0.free_blocks - (unsigned long long)((total + 4095) / 4096) ||
        f1.free_inodes != f0.free_inodes - 1)
        return 808;

    /* 從磁碟執行它 */
    long pid = sys_spawn("/disk/echo-test", argv);
    if (pid <= 0 || sys_wait(pid) != 2)                  /* args 的結束碼是它收到的參數個數 */
        return 809;
    /* 正在執行的程式檔不可修改的規則,在磁碟上也一樣(這裡它已經結束,所以可以刪除) */
    if (sys_sync() != 0)
        return 810;
    if (sys_unlink("/disk/echo-test") != 0 || sys_stat("/disk/echo-test", &st) != -E_NOENT)
        return 811;
    if (sys_statfs("/disk", &f1) != 0 || f1.free_blocks != f0.free_blocks ||
        f1.free_inodes != f0.free_inodes)
        return 812;
    if (sys_unlink("/disk") != -E_BUSY)                  /* 掛載點不可以刪除 */
        return 813;

    puts_("  [user] copied a program to the disk, ran it from there, and removed it\n");
    return 20;
}

/* ---- UMODE_STOP ---- */

/* 行程 pid 目前的狀態與用掉的 CPU 時間;找不到回傳 -1 */
static int proc_state(long pid, unsigned *ticks)
{
    struct procinfo pi;

    if (sys_procinfo(pid - 1, &pi) != 0 || pi.pid != pid)
        return -1;
    if (ticks)
        *ticks = pi.cpu_ticks;
    return pi.state;
}

/* 暫停與繼續:工作控制(Ctrl-Z、fg、bg)在 kernel 這一側的基礎 */
static int mode_stop(void)
{
    unsigned t0 = 0, t1 = 0, t2 = 0;
    int st = 0, fds[2];
    long a, b, g = 0;

    /* 1. 一個不停空轉的子行程:暫停之後不再用掉任何 CPU 時間,繼續之後又開始用 */
    a = sys_fork();
    if (a == 0) {
        for (;;)
            ;
    }
    if (a < 0)
        return 701;
    sys_sleep(3);
    if (sys_waitpid(a, &st, WNOHANG | WUNTRACED) != 0)   /* 還在執行:沒有事件 */
        return 702;
    if (sys_kill(a, SIGSTOP) != 0)
        return 703;
    if (sys_waitpid(a, &st, WUNTRACED) != a || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP)
        return 704;
    if (sys_waitpid(a, &st, WNOHANG | WUNTRACED) != 0)   /* 同一次暫停只回報一次 */
        return 705;
    sys_sleep(5);                                        /* 它最晚在下一個 tick 真的停下 */
    if (proc_state(a, &t0) != PROC_STOPPED)
        return 706;
    sys_sleep(12);
    if (proc_state(a, &t1) != PROC_STOPPED || t1 != t0)  /* 暫停期間完全沒有執行 */
        return 707;
    if (sys_kill(a, SIGSTOP) != 0 ||                     /* 已經暫停:再暫停一次沒有作用 */
        sys_waitpid(a, &st, WNOHANG | WUNTRACED) != 0)
        return 708;
    if (sys_kill(a, SIGCONT) != 0)
        return 709;
    sys_sleep(12);
    if (proc_state(a, &t2) == PROC_STOPPED || t2 <= t1)  /* 繼續之後又在執行了 */
        return 710;

    /* 2. 暫停中的行程可以直接終止,不必先讓它繼續 */
    if (sys_kill(a, SIGTSTP) != 0)
        return 711;
    if (sys_waitpid(a, &st, WUNTRACED) != a || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTSTP)
        return 712;
    if (sys_kill(a, SIGKILL) != 0 || sys_waitpid(a, &st, WUNTRACED) != a || st != 128 + SIGKILL)
        return 713;

    /* 3. 沒有 WUNTRACED 的 waitpid 不會因為暫停而返回:它等的是「結束」。
          子行程先把自己暫停;我們 5ms 後讓它繼續,它才以 5 結束 */
    b = sys_fork();
    if (b == 0) {
        sys_kill(sys_getpid(), SIGSTOP);                 /* 返回使用者模式之前就停在這裡 */
        sys_exit(5);
    }
    if (b < 0)
        return 720;
    sys_sleep(5);
    if (proc_state(b, 0) != PROC_STOPPED || sys_waitpid(b, &st, WNOHANG) != 0)
        return 721;
    if (sys_kill(b, SIGCONT) != 0)               /* 叫不動的話,下面的等待會永遠卡住 */
        return 723;
    if (sys_waitpid(b, &st, 0) != b || st != 5)
        return 722;

    /* 4. 結束碼落在「暫停回報」用的那一段數值:只留下最低 8 個位元,不會被誤認成暫停 */
    b = sys_fork();
    if (b == 0)
        sys_exit((int)(WSTOPPED_BASE | 20));
    if (b < 0 || sys_waitpid(b, &st, WUNTRACED) != b || WIFSTOPPED(st) || st != 20)
        return 730;

    /* 5. 暫停中的行程失去父行程:沒有人能讓它繼續了,kernel 會終止它。
          中間的子行程建立一個孫行程、暫停它、把它的編號告訴我們,然後自己結束 */
    if (sys_pipe(fds) != 0)
        return 740;
    b = sys_fork();
    if (b == 0) {
        long gc = sys_fork();
        if (gc == 0) {
            for (;;)
                sys_sleep(1000);
        }
        if (gc < 0 || sys_kill(gc, SIGSTOP) != 0)
            sys_exit(9);
        sys_waitpid(gc, &st, WUNTRACED);
        sys_write(fds[1], &gc, sizeof(gc));
        sys_exit(0);
    }
    sys_close(fds[1]);
    if (b < 0 || sys_read(fds[0], &g, sizeof(g)) != (long)sizeof(g) || g <= 0)
        return 741;
    sys_close(fds[0]);
    if (sys_waitpid(b, &st, 0) != b || st != 0)
        return 742;
    for (int i = 0; i < 200 && proc_state(g, 0) != -1; i++)
        sys_sleep(1);
    if (proc_state(g, 0) != -1)                          /* 孫行程應該已經消失 */
        return 743;

    /* 6. 不合法的訊號編號 */
    if (sys_kill(sys_getpid(), 99) != -E_INVAL)
        return 750;

    puts_("  [user] a stopped process uses no CPU time, continues on SIGCONT, and can be killed\n");
    return 19;
}

/* ---- UMODE_JOBS ---- */

/* 背景執行與 ps 需要的三樣東西:不等待的 waitpid、列出行程、/dev/null */
static int mode_jobs(void)
{
    struct procinfo pi;
    char buf[8];
    int st = -1;
    long me = sys_getpid();

    /* 1. 不等待的 waitpid:子行程還在執行時立刻回傳 0,不會卡住 */
    if (sys_waitpid(-1, &st, WNOHANG) != -E_CHILD)       /* 沒有子行程:和一般的 waitpid 相同 */
        return 601;
    long a = fork_sleeper();
    if (a <= 0)
        return 602;
    if (sys_waitpid(-1, &st, WNOHANG) != 0 || st != -1)  /* 還沒結束:回傳 0,*status 不動 */
        return 603;
    if (sys_waitpid(a, &st, 8) != -E_INVAL)              /* 不認得的選項 */
        return 604;

    /* 2. 列出行程:應該找得到自己與那個子行程,而且編號是遞增的 */
    int saw_me = 0, saw_child = 0, count = 0;
    long after = 0;
    while (sys_procinfo(after, &pi) == 0) {
        if (pi.pid <= after)
            return 610;
        after = pi.pid;
        count++;
        if (pi.pid == me) {
            /* 自己:正在執行、一條執行緒、名稱是 init */
            if (pi.state != PROC_RUNNING || pi.nthreads != 1 || !streq(pi.name, "init"))
                return 611;
            saw_me = 1;
        }
        if (pi.pid == a) {
            if (pi.ppid != me || pi.state == PROC_ZOMBIE)
                return 612;
            saw_child = 1;
        }
    }
    if (!saw_me || !saw_child)
        return 613;
    if (sys_procinfo(after, &pi) != 1)                   /* 最後一個之後:沒有更多了 */
        return 614;
    if (sys_procinfo(0, (struct procinfo *)KERNEL_ADDR) != -E_FAULT)
        return 615;

    /* 3. 子行程結束後、還沒被 wait 之前,列表中它是 zombie;wait 之後就不在列表中了 */
    sys_kill(a, SIGKILL);
    for (int i = 0; i < 200; i++) {                      /* 終止不是立即的:最多等 200ms */
        if (sys_procinfo(a - 1, &pi) == 0 && pi.pid == a && pi.state == PROC_ZOMBIE)
            break;
        sys_sleep(1);
    }
    if (pi.pid != a || pi.state != PROC_ZOMBIE)
        return 620;
    if (sys_waitpid(-1, &st, WNOHANG) != a || st != 128 + SIGKILL)   /* 已經結束:WNOHANG 也收得到 */
        return 621;
    if (sys_procinfo(a - 1, &pi) == 0 && pi.pid == a)
        return 622;

    /* 4. /dev/null:讀取立刻得到「結束」,寫入的內容被丟掉 */
    long fd = sys_open("/dev/null", O_RDWR);
    if (fd < 0)
        return 630;
    if (sys_read(fd, buf, sizeof(buf)) != 0 || sys_write(fd, "gone", 4) != 4)
        return 631;
    if (sys_read(fd, buf, sizeof(buf)) != 0)
        return 632;
    sys_close(fd);

    struct line l = { .len = 0 };
    line_str(&l, "  [user] listed ");
    line_num(&l, count);
    line_str(&l, " processes; waitpid without waiting and /dev/null work as expected\n");
    line_out(&l);
    return 18;
}

/* ---- UMODE_PIPE ---- */

static int mode_pipe(void)
{
    char buf[200];
    int fds[2];
    long pid, n;

    /* 1. fork 之後,子行程寫、父行程讀。兩邊各自關掉用不到的那一端:
          父行程若不關掉自己的寫端,就永遠讀不到「結束」(還有人開著寫端) */
    if (sys_pipe(fds) != 0)
        return 401;
    pid = sys_fork();
    if (pid == 0) {
        sys_close(fds[0]);
        sys_write(fds[1], "hello ", 6);
        sys_write(fds[1], "through a pipe", 14);
        sys_exit(21);                            /* 結束時 kernel 會關掉它的寫端 */
    }
    sys_close(fds[1]);
    long total = 0;
    while ((n = sys_read(fds[0], buf + total, sizeof(buf) - 1 - (unsigned long)total)) > 0)
        total += n;
    buf[total] = 0;
    sys_close(fds[0]);
    if (sys_wait(pid) != 21 || !streq(buf, "hello through a pipe"))
        return 402;

    /* 2. 大量資料:子行程寫 40000 位元組(管線只有 4KB,它會被擋下來好幾次),
          父行程邊讀邊核對內容 */
    if (sys_pipe(fds) != 0)
        return 410;
    pid = sys_fork();
    if (pid == 0) {
        char out[250];
        sys_close(fds[0]);
        for (long done = 0; done < 40000; done += (long)sizeof(out)) {
            for (unsigned i = 0; i < sizeof(out); i++)
                out[i] = (char)((done + i) * 3 + 1);
            if (sys_write(fds[1], out, sizeof(out)) != (long)sizeof(out))
                sys_exit(53);
        }
        sys_exit(22);
    }
    sys_close(fds[1]);
    total = 0;
    while ((n = sys_read(fds[0], buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            if (buf[i] != (char)((total + i) * 3 + 1))
                return 411;
        total += n;
    }
    sys_close(fds[0]);
    if (total != 40000 || sys_wait(pid) != 22)
        return 412;

    /* 3. 把另一個程式的輸出接進管線:先把自己的標準輸出換成寫端再建立它
          (新行程繼承 0、1、2 號),然後換回來,從讀端讀出它印的東西 */
    if (sys_pipe(fds) != 0)
        return 420;
    if (sys_dup2(1, 10) != 10)                   /* 記住原本的標準輸出 */
        return 421;
    if (sys_dup2(fds[1], 1) != 1)
        return 422;
    sys_close(fds[1]);
    char *argv[] = { "args", "captured", "by", "a", "pipe", 0 };
    pid = sys_spawn("/bin/args", argv);
    /* 換回來;同時也關掉了我們手上的寫端 — 沒換成功的話,下面的讀取永遠等不到結尾 */
    if (sys_dup2(10, 1) != 1)
        return 423;
    sys_close(10);
    total = 0;
    while ((n = sys_read(fds[0], buf + total, sizeof(buf) - 1 - (unsigned long)total)) > 0)
        total += n;
    buf[total] = 0;
    sys_close(fds[0]);
    if (sys_wait(pid) != 5 || !streq(buf, "captured by a pipe\n"))
        return 421;

    /* 4. 沒有人讀的管線:寫入要得到錯誤 */
    if (sys_pipe(fds) != 0)
        return 430;
    sys_close(fds[0]);
    if (sys_write(fds[1], "x", 1) != -E_PIPE)
        return 431;
    sys_close(fds[1]);

    /* 5. 位址檢查:把 kernel 的位址交給 pipe() */
    if (sys_pipe((int *)KERNEL_ADDR) != -E_FAULT)
        return 440;

    puts_("  [user] pipes work as expected\n");
    return 16;
}

/* ---- UMODE_FS ---- */

/* 每一項檢查失敗就回傳一個不同的數字,從結束碼可以看出是哪一步出錯 */
static int mode_fs(void)
{
    /* 超過一頁:跨頁的讀寫也要正確。放在 stack 上而不是全域:全域變數會讓「每一個」
       行程載入時都多配置這些頁,即使它根本不跑這個測試 */
    char big[4500], back[4500];
    char buf[160];
    struct line l = { .len = 0 };
    struct stat st;
    struct dirent d;
    long fd, n;

    /* 1. 讀取開機檔案包裡的檔案 */
    fd = sys_open("/etc/motd", O_RDONLY);
    if (fd < 0)
        return 101;
    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 102;
    buf[n] = 0;
    for (long i = 0; i < n; i++)                 /* 只印第一行 */
        if (buf[i] == '\n')
            buf[i] = 0;
    line_str(&l, "  [user] /etc/motd says: ");
    line_str(&l, buf);
    line_str(&l, "\n");
    line_out(&l);
    if (sys_write(fd, "x", 1) != -E_BADF)        /* 唯讀開啟的檔案不能寫 */
        return 103;
    sys_close(fd);
    if (sys_read(fd, buf, 1) != -E_BADF)         /* 關閉之後描述符就失效了 */
        return 104;

    /* 2. 建立目錄與檔案,寫入、讀回、比對 */
    if (sys_mkdir("/tmp") != 0 && sys_mkdir("/tmp") != -E_EXIST)
        return 110;
    for (unsigned i = 0; i < sizeof(big); i++)
        big[i] = (char)(i * 7 + 3);
    fd = sys_open("/tmp/data", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0)
        return 111;
    if (sys_write(fd, big, sizeof(big)) != (long)sizeof(big))
        return 112;
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 9266;
    if (sys_stat("/tmp/data", &st) != 0 || st.type != I_FILE || st.size != sizeof(big))
        return 113;
    fd = sys_open("/tmp/data", O_RDONLY);
    if (sys_read(fd, back, sizeof(back)) != (long)sizeof(back))
        return 114;
    for (unsigned i = 0; i < sizeof(big); i++)
        if (back[i] != big[i])
            return 115;
    if (sys_read(fd, back, 16) != 0)             /* 已經到結尾:回傳 0 */
        return 116;
    sys_close(fd);

    /* 3. 附加:每次寫入都接在最後 */
    fd = sys_open("/tmp/log", O_CREAT | O_WRONLY | O_APPEND);
    sys_write(fd, "one ", 4);
    sys_write(fd, "two", 3);
    if (sys_close(fd) != 0)                 /* 寫過資料的檔案:關閉時的寫回失敗要算測試失敗 */
        return 9283;
    fd = sys_open("/tmp/log", O_RDONLY);
    n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    buf[n > 0 ? n : 0] = 0;
    if (!streq(buf, "one two"))
        return 120;

    /* 4. 列出目錄 */
    fd = sys_open("/tmp", O_RDONLY);
    if (fd < 0)
        return 130;
    int seen = 0;
    line_str(&l, "  [user] /tmp contains:");
    while (sys_readdir(fd, &d) == 0) {
        line_str(&l, " ");
        line_str(&l, d.name);
        seen += streq(d.name, "data") + streq(d.name, "log");
    }
    line_str(&l, "\n");
    line_out(&l);
    sys_close(fd);
    if (seen != 2)
        return 131;

    /* 5. 錯誤情況:每一種都應該被拒絕,而不是悄悄成功 */
    if (sys_open("/no/such/file", O_RDONLY) != -E_NOENT)
        return 140;
    if (sys_unlink("/tmp") != -E_NOTEMPTY)       /* 目錄不是空的 */
        return 141;
    if (sys_open((const char *)KERNEL_ADDR, O_RDONLY) != -E_FAULT)   /* 路徑指向 kernel 的記憶體 */
        return 142;
    fd = sys_open("/etc/motd", O_RDONLY);
    if (sys_read(fd, (void *)KERNEL_ADDR, 16) != -E_FAULT)          /* 請 kernel 寫入 kernel 的記憶體 */
        return 143;
    if (sys_read(fd, (void *)mode_fs, 16) != -E_FAULT)              /* 請 kernel 寫入自己的程式碼(唯讀頁) */
        return 144;
    sys_close(fd);
    if (sys_close(99) != -E_BADF || sys_close(-1) != -E_BADF)
        return 145;

    /* 6. 刪除 */
    if (sys_unlink("/tmp/data") != 0 || sys_unlink("/tmp/log") != 0 || sys_unlink("/tmp") != 0)
        return 150;
    if (sys_stat("/tmp", &st) != -E_NOENT)
        return 151;

    puts_("  [user] file system calls work as expected\n");
    return 9;
}


int main(int argc, char **argv);

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
        *(volatile char *)(unsigned long)main = 0;       /* 程式碼頁是唯讀的 */
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
        long r = sys_write(1, (const char *)KERNEL_ADDR, 16);
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

    case UMODE_FS:
        return mode_fs();

    case UMODE_PROC:
        return mode_proc();

    case UMODE_EXEC: {
        /* 成功的話不會返回:這個行程接下來執行的是 /bin/args,
           結束碼也由它決定(args 回傳它收到的參數個數) */
        char *argv[] = { "args", "  [user] this line is printed by /bin/args after exec,", "same pid", 0 };
        long er = sys_exec("/bin/args", argv);
        return er < 0 ? 1 : 2;                           /* 走到這裡表示 exec 失敗(不論它回傳什麼) */
    }

    case UMODE_EXEC_THREADS: {
        sys_thread_create(idle_worker, 0);
        char *argv[] = { "echo", 0 };
        long r = sys_exec("/bin/echo", argv);
        if (sys_fork() != -E_BUSY)                       /* fork 也一樣只允許單一執行緒 */
            return 2;
        return r == -E_BUSY ? 13 : 1;
    }

    case UMODE_FORK:
        return mode_fork();

    case UMODE_PIPE:
        return mode_pipe();

    case UMODE_SIGNAL:
        return mode_signal();

    case UMODE_JOBS:
        return mode_jobs();

    case UMODE_STOP:
        return mode_stop();

    case UMODE_DISK:
        return mode_disk();

    case UMODE_SYSINFO:
        return mode_sysinfo();

    case UMODE_HEAP:
        return mode_heap();

    case UMODE_TIME:
        return mode_time();

    case UMODE_MEMQUOTA:
        return mode_memquota();

    case UMODE_NET:
        return mode_net();

    case UMODE_RT:
        return mode_rt();

    case UMODE_TMR:
        return mode_tmr();

    case UMODE_MEMRACE:
        return mode_memrace();

    case UMODE_THREADCHURN:
        return mode_threadchurn();

    case UMODE_SIGRACE:
        return mode_sigrace();
    case UMODE_FAILSWEEP:
        return mode_failsweep();
    case UMODE_THREADMIX:
        return mode_threadmix();
    case UMODE_FSQUOTA:
        return mode_fsquota();

    case UMODE_PERM: {
        /* 同一組測試在記憶體檔案系統與磁碟上各做一次 */
        struct fsinfo fi;
        int rc = perm_on("/pm");
        if (rc == 0 && sys_statfs("/disk", &fi) == 0 && streq(fi.type, "wfs"))
            rc = perm_on("/disk/pm");
        if (rc == 0)
            rc = perm_process();
        if (rc)
            return rc;
        puts_("  [user] permissions: other users are refused, privileges only ever shrink\n");
        return 26;
    }

    case UMODE_SIGHANDLER:
        return mode_sighandler();

    case UMODE_FILEOPS: {
        /* 同一組測試在記憶體檔案系統與磁碟上各做一次(沒有磁碟時只做前者) */
        struct fsinfo fi;
        int rc = fileops_on("/fo");
        if (rc == 0 && sys_statfs("/disk", &fi) == 0 && streq(fi.type, "wfs")) {
            rc = fileops_on("/disk/fo");
            if (rc == 0 && sys_rename("/fo-x", "/disk/fo-x") != -E_NOENT)
                rc = 1090;
            /* 跨檔案系統不能改名 */
            long fd = sys_open("/fo-x", O_CREAT | O_WRONLY);
            if (fd < 0 || sys_close(fd) != 0)
                rc = rc ? rc : 1092;
            if (rc == 0 && (sys_rename("/fo-x", "/disk/fo-x") != -E_XDEV ||
                            sys_link("/fo-x", "/disk/fo-x") != -E_XDEV))
                rc = 1091;
            sys_unlink("/fo-x");
            if (rc == 0)
                rc = fsync_on("/disk/fsync-x");
        }
        if (rc == 0)
            rc = fsync_on("/fsync-x");
        if (rc)
            return rc;
        puts_("  [user] lseek, rename, hard links and the working directory work as expected\n");
        return 22;
    }

    case UMODE_STACK_GROW: {
        /* 遞迴 600 層,每層約 300 位元組,總共約 180KB,是一開始配置的 16KB 的十倍以上 */
        long r = recurse_limited(0, 600);
        puts_("  [user] used about 180 KB of stack without being killed\n");
        return r == 600 ? 15 : 1;
    }

    case UMODE_FUTEX:
        return mode_futex();

    case UMODE_BAD_SYSCALL:
        return syscall3(9999, 1, 2, 3) < 0 ? 9 : 1;      /* 應回傳錯誤而不是出事 */
    }
    return 3;
}

/* 第一個參數(argv[1])是要執行的測試編號;沒有給就是 0 */
int main(int argc, char **argv)
{
    return umain(argc > 1 ? uatoi(argv[1]) : 0);
}
