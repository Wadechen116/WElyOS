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
