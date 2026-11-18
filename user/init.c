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
#include "ulib.h"

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
    UMODE_FS,            /* 檔案:建立、寫入、讀回、列出目錄、刪除,以及各種錯誤情況 */
    UMODE_PROC,          /* 建立其他行程並等待、參數傳遞、exec 失敗時原程式不受影響 */
    UMODE_EXEC,          /* 以 exec 把自己換成另一個程式 */
    UMODE_EXEC_THREADS,  /* 還有其他執行緒時呼叫 exec / fork:應被拒絕 */
    UMODE_FORK,          /* fork:複製行程,父子各自修改資料互不影響(寫入時複製) */
    UMODE_STACK_GROW,    /* 用掉遠超過 16KB 的 stack:應該自動成長而不是被終止 */
    UMODE_PIPE,          /* 管線:父子行程之間傳資料、把別的程式的輸出接進管線 */
    UMODE_SIGNAL,        /* 父子關係與訊號:終止子行程、等任何一個子行程、行程群組、孤兒行程 */
    UMODE_JOBS,          /* 不等待的 waitpid、列出行程、/dev/null(背景執行與 ps 的基礎) */
    UMODE_STOP,          /* 暫停與繼續(SIGSTOP、SIGCONT、waitpid 的 WUNTRACED) */
    UMODE_DISK,          /* 磁碟:把程式複製到 /disk 再執行、statfs、sync(有磁碟時才執行) */
    UMODE_SYSINFO,       /* top 用到的東西:系統狀態、行程的記憶體用量、主控台的輸入模式 */
};

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
    char *echo_argv[] = { "echo", "  [user] hello from a spawned /bin/echo:", "one", "two", 0 };
    char *cat_argv[] = { "cat", "/etc/motd", 0 };
    char *ls_argv[] = { "ls", "/bin", 0 };
    long pid, code;

    /* 1. 建立行程並等待。echo 的結束碼是它收到的參數個數 */
    pid = sys_spawn("/bin/echo", echo_argv);
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
    char *redir_argv[] = { "echo", "captured", "output", 0 };
    pid = sys_spawn("/bin/echo", redir_argv);
    code = sys_wait(pid);
    sys_close(1);
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
    if (sys_spawn("/etc/motd", echo_argv) != -E_NOEXEC)              /* 不是程式 */
        return 221;
    if (sys_exec("/bin/nope", echo_argv) != -E_NOENT)                /* exec 失敗:原程式不受影響 */
        return 222;
    if (sys_exec("/etc/motd", echo_argv) != -E_NOEXEC)
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
        char *argv[] = { "echo", "  [user] fork + exec:", "this is /bin/echo", 0 };
        sys_exec("/bin/echo", argv);
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
    sys_kill(a, SIGKILL);
    if (sys_waitpid(a, (int *)KERNEL_ADDR, 0) != -E_FAULT)     /* 要求把結束碼寫到 kernel 的位址 */
        return 543;

    puts_("  [user] process relationships and signals work as expected\n");
    return 17;
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

    /* 把 /bin/echo 複製到磁碟上 */
    sys_unlink("/disk/echo-test");                       /* 上次測試被打斷時留下的 */
    if (sys_statfs("/disk", &f0) != 0)
        return 804;
    in = sys_open("/bin/echo", O_RDONLY);
    out = sys_open("/disk/echo-test", O_CREAT | O_WRONLY | O_TRUNC);
    if (in < 0 || out < 0)
        return 805;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        if (sys_write(out, buf, (unsigned long)n) != n)
            return 806;
        total += n;
    }
    sys_close(in);
    sys_close(out);
    if (sys_stat("/disk/echo-test", &st) != 0 || st.size != (unsigned long long)total)
        return 807;
    if (sys_statfs("/disk", &f1) != 0 ||
        f1.free_blocks != f0.free_blocks - (unsigned long long)((total + 4095) / 4096) ||
        f1.free_inodes != f0.free_inodes - 1)
        return 808;

    /* 從磁碟執行它 */
    long pid = sys_spawn("/disk/echo-test", argv);
    if (pid <= 0 || sys_wait(pid) != 2)                  /* echo 的結束碼是它收到的參數個數 */
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
    sys_kill(b, SIGCONT);
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
        sys_kill(gc, SIGSTOP);
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
    sys_dup2(1, 10);                             /* 記住原本的標準輸出 */
    sys_dup2(fds[1], 1);
    sys_close(fds[1]);
    char *argv[] = { "echo", "captured", "by", "a", "pipe", 0 };
    pid = sys_spawn("/bin/echo", argv);
    sys_dup2(10, 1);                             /* 換回來;同時也關掉了我們手上的寫端 */
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
    sys_close(fd);
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
    sys_close(fd);
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
        /* 成功的話不會返回:這個行程接下來執行的是 /bin/echo,
           結束碼也由它決定(echo 回傳它收到的參數個數) */
        char *argv[] = { "echo", "  [user] this line is printed by /bin/echo after exec,", "same pid", 0 };
        sys_exec("/bin/echo", argv);
        return 1;                                        /* 走到這裡表示 exec 失敗 */
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
