/*
 * memhog.c - 把記憶體用到被拒絕為止(I-240 的壓力測試,第 52 步 C3.1)
 * 用法:memhog file   在 / (記憶體檔案系統)寫檔案,直到寫不進去
 *       memhog fork   不斷 fork(子行程停在管線上等待),直到 fork 失敗
 *       memhog threads 不斷 fork 工作行程,每個建執行緒到失敗(-E_BUSY = 16 格用完,
 *                     -E_NOMEM = 承諾不到),直到 fork 或建執行緒得到 -E_NOMEM(C3.2 T11)
 *       memhog stack  同 threads;記憶體被拒絕之後才讓每條執行緒把自己那格 stack 遞迴觸到
 *                     896KB(預付 255 頁之內):fault 必須兌現預留頁,不可終止;
 *                     最後子行程越過格子(碰保護頁):必須被終止(定義好的行為)
 *       memhog maps   (有 CAP_ADMIN 時把配額調到 1GB)mmap 到 -E_NOMEM,每頁都寫過
 *       memhog exec   配額 2MB,反覆 exec 自己 16 次(每次先觸 128KB 對映);
 *                     最後配額調到 1MB(低於用量)再 exec:必須 -E_NOMEM 且原程式繼續
 * 用到被拒絕之後保持 500ms(kernel 的測試在這段期間量空閒頁數),再全部歸還。
 * 新模式成功時印出「memhog <模式>: ok」。
 * 被拒絕時是「明確的錯誤」才回傳 0;一直沒被拒絕(不應該發生)回傳 2,其他錯誤回傳 3。
 */
#include "ulib.h"

#define MAX_FILES 64
#define MAX_KIDS  4096
#define CHUNK     65536

static char buf[CHUNK];
static long kids[MAX_KIDS];

static int hog_files(void)
{
    char name[16] = "/hog00";
    int nfiles = 0, refused = 0, bad = 0;
    unsigned long kb = 0;

    for (; nfiles < MAX_FILES && !refused; nfiles++) {
        name[4] = (char)('0' + nfiles / 10);
        name[5] = (char)('0' + nfiles % 10);
        long fd = sys_open(name, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            refused = 1;                         /* 連建立檔案都不行:同樣是被拒絕 */
            break;
        }
        for (int i = 0; i < 15 * 16; i++) {     /* 每個檔案最多 15MB(單檔上限 16MB) */
            long n = sys_write(fd, buf, CHUNK);
            if (n > 0)
                kb += (unsigned long)n / 1024;
            if (n != CHUNK) {
                if (n < 0 && n != -E_NOMEM && n != -E_NOSPC)
                    bad = 1;                     /* 非預期的錯誤:關檔之後以 3 結束 */
                refused = 1;
                break;
            }
        }
        if ((sys_close(fd) != 0 && !refused) || bad)
            return 3;
    }
    struct line l = { .len = 0 };
    line_str(&l, "memhog: wrote ");
    line_num(&l, (long)kb);
    line_str(&l, " KB in ");
    line_num(&l, nfiles);
    line_str(&l, " files before being refused\n");
    line_out(&l);
    sys_sleep(500);
    for (int i = 0; i < nfiles; i++) {
        name[4] = (char)('0' + i / 10);
        name[5] = (char)('0' + i % 10);
        if (sys_unlink(name) != 0)
            bad = 1;
    }
    return bad ? 3 : (refused ? 0 : 2);
}

static int hog_fork(void)
{
    int fds[2];
    if (sys_pipe(fds) != 0)
        return 3;
    int n = 0, refused = 0;
    long err = 0;
    while (n < MAX_KIDS) {
        long pid = sys_fork();
        if (pid == 0) {
            char c;
            if (sys_close(fds[1]) != 0)
                sys_exit(4);
            /* 父行程關閉寫入端時回傳 0:正常結束;其他結果都算錯 */
            sys_exit(sys_read(fds[0], &c, 1) == 0 ? 0 : 5);
        }
        if (pid < 0) {
            err = pid;
            refused = 1;
            break;
        }
        kids[n++] = pid;
    }
    struct line l = { .len = 0 };
    line_str(&l, "memhog: ");
    line_num(&l, n);
    line_str(&l, " children before fork was refused (error ");
    line_num(&l, -err);
    line_str(&l, ")\n");
    line_out(&l);
    sys_sleep(500);
    int bad = sys_close(fds[1]) != 0;
    if (sys_close(fds[0]) != 0)
        bad = 1;
    for (int i = 0; i < n; i++) {
        int st = -1;
        if (sys_waitpid(kids[i], &st, 0) != kids[i] || st != 0)
            bad = 1;
    }
    if (bad)
        return 3;
    return refused && err == -E_NOMEM ? 0 : (refused ? 3 : 2);
}

/* ---- threads / stack(I-240 C3.2 T11) ---- */

#define MAX_THR     16                           /* 每個行程最多 16 格 stack(含主執行緒) */
#define FRAME       (32 * 1024)
#define DEPTH       28                           /* 28 x 32KB = 896KB:在 255 頁(1020KB)的承諾之內留餘裕 */

static volatile int thr_go, thr_done, thr_bad;
static volatile int thr_deep;                    /* stack 模式:收到 go 之後遞迴觸 stack */

/* 每層 32KB 的 frame,每頁寫入再讀回;回傳值用到下一層的結果,避免尾呼叫被最佳化掉 */
static long deep(int depth, long tag)
{
    volatile char f[FRAME];
    for (unsigned long i = 0; i < sizeof(f); i += 4096)
        f[i] = (char)(tag + depth + i / 4096);
    long r = depth > 1 ? deep(depth - 1, tag) : 0;
    for (unsigned long i = 0; i < sizeof(f); i += 4096)
        if (f[i] != (char)(tag + depth + i / 4096))
            thr_bad = 1;
    return r + f[0];
}

/* 一直遞迴到碰到這格的保護頁為止。停止條件看一個永遠是 0 的 volatile 變數:
   編譯器看不出它不會變,就不會報「無限遞迴」的警告,行為仍是一路往下直到保護頁 */
static volatile int forever_stop;

static long forever(long n)
{
    volatile char f[FRAME];
    f[0] = (char)n;
    if (forever_stop)
        return f[0];
    return forever(n + 1) + f[0];
}

static void forever_fn(long n)
{
    (void)forever(n);
    sys_thread_exit();
}

static void thr_fn(long idx)
{
    while (!__atomic_load_n(&thr_go, __ATOMIC_SEQ_CST))
        sys_sleep(5);
    if (thr_deep)
        (void)deep(DEPTH, idx);
    __atomic_fetch_add(&thr_done, 1, __ATOMIC_SEQ_CST);
    for (;;)
        sys_sleep(1000);                         /* 停著保持承諾,行程結束時一起收回 */
}

/* 工作行程:建執行緒到失敗,回報 {原因, 條數};等 go 管線關閉後(stack 模式)觸 stack,
   回報 'D';等 rel 管線關閉後結束。結束碼 0 = 一切如預期 */
static int hog_worker(int rep, int go, int rel)
{
    int n = 0;
    long err = 0;
    while (n < MAX_THR) {
        long r = sys_thread_create(thr_fn, n);
        if (r < 0) {
            err = r;
            break;
        }
        n++;
    }
    char msg[2];
    msg[0] = err == -E_BUSY ? 'B' : err == -E_NOMEM ? 'N' : err == 0 ? 'F' : 'X';
    msg[1] = (char)n;
    if (sys_write(rep, msg, 2) != 2)
        return 4;
    char c;
    if (sys_read(go, &c, 1) != 0)
        return 5;
    __atomic_store_n(&thr_go, 1, __ATOMIC_SEQ_CST);
    if (thr_deep)
        (void)deep(DEPTH - 2, 99);               /* 主執行緒那格也觸(它已經用掉一些) */
    for (int w = 0; __atomic_load_n(&thr_done, __ATOMIC_SEQ_CST) < n; w++) {
        if (w > 20000)
            return 6;
        sys_sleep(1);
    }
    msg[0] = thr_bad ? 'X' : 'D';
    if (sys_write(rep, msg, 2) != 2)
        return 7;
    if (sys_read(rel, &c, 1) != 0)
        return 8;
    return thr_bad ? 9 : 0;
}

static int overflow_child(void)
{
    if (sys_thread_create(forever_fn, 0) < 0)
        return 2;
    sys_sleep(5000);                             /* 正常情況下活不到這裡 */
    return 3;
}

static int hog_threads(int touch)
{
    int rep[2], go[2], rel[2];
    if (sys_pipe(rep) != 0 || sys_pipe(go) != 0 || sys_pipe(rel) != 0)
        return 3;
    thr_deep = touch;
    int n = 0, threads = 0, busy = 0, nomem_thr = 0, bad = 0;
    long ferr = 0;
    while (n < MAX_KIDS && !nomem_thr) {
        long pid = sys_fork();
        if (pid == 0) {
            /* 子行程只留自己用得到的那一端;關不掉代表 fd 狀態不對,以專屬結束碼回報 */
            if (sys_close(rep[0]) != 0 || sys_close(go[1]) != 0 || sys_close(rel[1]) != 0)
                sys_exit(9);
            sys_exit(hog_worker(rep[1], go[0], rel[0]));
        }
        if (pid < 0) {
            ferr = pid;
            break;
        }
        kids[n++] = pid;
        char msg[2];
        if (sys_read(rep[0], msg, 2) != 2) {
            bad = 1;
            break;
        }
        threads += msg[1];
        if (msg[0] == 'B')
            busy++;
        else if (msg[0] == 'N')
            nomem_thr = 1;
        else
            bad = 1;                             /* 'F':第 17 條沒得到 -E_BUSY;'X':非預期的錯誤碼 */
    }
    int refused = ferr == -E_NOMEM || nomem_thr;
    if (ferr != 0 && ferr != -E_NOMEM)
        bad = 1;
    /* 記憶體已經被拒絕:這時才讓所有執行緒觸 stack — 只能用預付的預留頁 */
    if (sys_close(go[1]) != 0)
        bad = 1;
    int done = 0;
    for (int i = 0; i < n; i++) {
        char msg[2];
        if (sys_read(rep[0], msg, 2) != 2 || msg[0] != 'D')
            bad = 1;
        else
            done++;
    }
    struct line l = { .len = 0 };
    line_str(&l, touch ? "memhog stack: " : "memhog threads: ");
    line_num(&l, n);
    line_str(&l, " processes, ");
    line_num(&l, threads);
    line_str(&l, " threads (");
    line_num(&l, busy);
    line_str(&l, " stopped by E_BUSY), E_NOMEM at ");
    line_str(&l, nomem_thr ? "thread_create" : ferr == -E_NOMEM ? "fork" : "(none)");
    line_str(&l, touch ? ", stacks touched in " : ", done in ");
    line_num(&l, done);
    line_str(&l, "\n");
    line_out(&l);
    sys_sleep(500);
    if (sys_close(rel[1]) != 0 || sys_close(rel[0]) != 0 || sys_close(go[0]) != 0)
        bad = 1;
    for (int i = 0; i < n; i++) {
        int st = -1;
        if (sys_waitpid(kids[i], &st, 0) != kids[i] || st != 0)
            bad = 1;                             /* 在 fault 被終止也在這裡被抓到 */
    }
    if (sys_close(rep[0]) != 0 || sys_close(rep[1]) != 0)
        bad = 1;
    if (touch && !bad) {
        /* 越過格子:碰保護頁,行程必須被終止(負值或 128 以上),而不是無聲擴張 */
        long pid = sys_fork();
        if (pid == 0)
            sys_exit(overflow_child());
        int st = 0;
        if (pid < 0 || sys_waitpid(pid, &st, 0) != pid || !(st < 0 || st >= 128))
            bad = 1;
        else
            puts_("memhog stack: overflow past the slot was killed at the guard page\n");
    }
    if (bad)
        return 3;
    if (!refused)
        return 2;
    puts_(touch ? "memhog stack: ok\n" : "memhog threads: ok\n");
    return 0;
}

/* ---- maps ---- */

#define MAX_MAPS 15                              /* 同時最多 16 塊;留一塊給最後的探測 */

static int hog_maps(void)
{
    static char *maps[MAX_MAPS];
    static unsigned long sizes[MAX_MAPS];
    long lim = sys_memlimit(1 << 20);            /* 有 CAP_ADMIN:調到 1GB,讓系統的承諾先擋下 */
    unsigned long sz = 16ul << 20, kb = 0;
    int n = 0, refused = 0, bad = 0;

    while (n < MAX_MAPS) {
        char *p = sys_mmap(sz);
        if ((long)p == -E_NOMEM) {
            if (sz == 4096) {
                refused = 1;
                break;
            }
            sz /= 2;
            continue;
        }
        if ((long)p < 0) {
            bad = 1;
            break;
        }
        for (unsigned long i = 0; i < sz; i += 4096)   /* 每一頁都要真的拿得到(兌現預留頁) */
            p[i] = (char)(n + i / 4096);
        maps[n] = p;
        sizes[n++] = sz;
        kb += sz / 1024;
    }
    if (!refused && !bad && n == MAX_MAPS)
        refused = (long)sys_mmap(1ul << 20) == -E_NOMEM;   /* 15 塊都沒擋下:最後 1MB 探測 */
    struct line l = { .len = 0 };
    line_str(&l, "memhog maps: mapped and touched ");
    line_num(&l, (long)kb);
    line_str(&l, " KB in ");
    line_num(&l, n);
    line_str(&l, lim == (1 << 20) ? " maps (limit 1GB)" : " maps (default limit)");
    line_str(&l, refused ? ", then mmap failed with E_NOMEM\n" : ", never refused\n");
    line_out(&l);
    sys_sleep(500);
    for (int i = 0; i < n; i++) {
        for (unsigned long j = 0; j < sizes[i]; j += 4096)
            if (maps[i][j] != (char)(i + j / 4096))
                bad = 1;
        if (sys_munmap(maps[i]) != 0)
            bad = 1;
    }
    if (bad)
        return 3;
    if (!refused)
        return 2;
    puts_("memhog maps: ok\n");
    return 0;
}

/* ---- exec ---- */

#define EXEC_STEPS 16

static int hog_exec(int argc, char **argv)
{
    long step = argc >= 3 ? uatoi(argv[2]) : 0;
    char num[24];
    char *nargv[] = { "memhog", "exec", num, 0 };
    struct line l = { .len = 0 };

    if (step == 0 && sys_memlimit(2048) != 2048)
        return 3;
    if (sys_memlimit(0) != 2048)                 /* 配額跨 exec 保留 */
        return 3;
    if (step < EXEC_STEPS) {
        char *m = sys_mmap(128 * 1024);          /* 接近上限:舊空間多 128KB(exec 時歸還) */
        if ((long)m < 0)
            return 3;
        for (int i = 0; i < 128 * 1024; i += 4096)
            m[i] = 1;
        line_num(&l, step + 1);
        for (unsigned i = 0; i < l.len; i++)
            num[i] = l.buf[i];
        num[l.len] = 0;
        l.len = 0;
        long rc = sys_exec("/bin/memhog", nargv);   /* 成功就不會回來 */
        line_str(&l, "memhog exec: step ");
        line_num(&l, step);
        line_str(&l, " exec failed with error ");
        line_num(&l, -rc);
        line_str(&l, "\n");
        line_out(&l);
        return 3;
    }
    /* 調到行程本身的用量(約 1.1MB)以下:調低允許,但 exec 必須當場被拒絕、原程式照常繼續 */
    if (sys_memlimit(1024) != 1024)
        return 3;
    num[0] = '9';
    num[1] = '9';
    num[2] = 0;
    long rc = sys_exec("/bin/memhog", nargv);
    line_str(&l, "memhog exec: ");
    line_num(&l, EXEC_STEPS);
    line_str(&l, " execs at a 2MB limit, then exec at 1MB returned error ");
    line_num(&l, -rc);
    line_str(&l, "\n");
    line_out(&l);
    sys_sleep(500);
    if (rc != -E_NOMEM || sys_sbrk(4096) != -E_NOMEM)
        return 3;
    puts_("memhog exec: ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && streq(argv[1], "file"))
        return hog_files();
    if (argc == 2 && streq(argv[1], "fork"))
        return hog_fork();
    if (argc == 2 && streq(argv[1], "threads"))
        return hog_threads(0);
    if (argc == 2 && streq(argv[1], "stack"))
        return hog_threads(1);
    if (argc == 2 && streq(argv[1], "maps"))
        return hog_maps();
    if (argc >= 2 && argc <= 3 && streq(argv[1], "exec"))
        return hog_exec(argc, argv);
    puts_("usage: memhog file|fork|threads|stack|maps|exec\n");
    return 1;
}
