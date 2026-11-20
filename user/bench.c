/*
 * bench.c - 效能基準(docs/ISSUES.md 的 N-004):量測系統呼叫、行程切換、fork、exec、檔案讀寫與開機各要多久
 *
 * 用法:bench [輪數]      預設 5 輪
 *
 * 時間來源是 SYS_clock 的高精度時鐘(i386 TSC、RISC-V time CSR、AArch64 cntpct_el0)。
 * 每個項目重複 N 次算一輪,一輪的總時間除以 N 得到「每次幾奈秒」;跑 R 輪後印出最小、中位數、最大值。
 * 最小值最接近「這件事本身的成本」,中位數代表一般情況,最大值顯示受干擾(計時器中斷、模擬器的主機排程)的程度。
 *
 * 輸出格式(一行一項,regress / bench.sh 解析用):
 *   bench: <項目> n=<每輪次數> rounds=<R> min=<ns> med=<ns> max=<ns> ns/op
 *
 * 注意:這些是 QEMU 上的數字,反映的是「模擬出來的機器」,不代表實體硬體的速度。
 * 它的用途是同一台主機上前後比較(發現變慢)與找出明顯偏慢的地方。
 *
 * 結束碼:0 = 全部量完;1 = 某一項的系統呼叫失敗(結果不可信,不印數字)。
 */
#include <stdint.h>
#include "usys.h"
#include "ulib.h"
#include "libc.h"

#define ROUNDS_MAX 15

static uint64_t hz;

/* 64 位元除法:i386 沒有 libgcc 的 __udivdi3,用位移-相減自己做 */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

/* 讀時鐘;失敗回傳 -1(那一輪的數字不能用)。Codex 的 probe_bench_clock.py 驗證這些錯誤路徑 */
static int now(uint64_t *out)
{
    struct clockinfo ci;
    if (sys_clock(&ci) != 0)
        return -1;
    *out = ci.count;
    return 0;
}

/* 從 t0 到現在經過的計數。時鐘讀不到、或比 t0 還小(倒退:例如換到時鐘不同步的 CPU)都回傳 -1,
   不可以讓無號數相減繞回去變成一個巨大的「結果」 */
static int since(uint64_t t0, uint64_t *out)
{
    uint64_t t1;
    if (now(&t1) != 0 || t1 < t0)
        return -1;
    *out = t1 - t0;
    return 0;
}

/* 計數 -> 奈秒。分成整秒與餘數兩段算,不會溢位(餘數 < hz,hz 在幾 GHz 以內) */
static uint64_t to_ns(uint64_t counts)
{
    uint64_t rem, sec = udiv64(counts, hz, &rem);
    return sec * 1000000000ULL + udiv64(rem * 1000000000ULL, hz, NULL);
}

static void sort_u64(uint64_t *v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

static int failed;
static int close_err;                    /* 有一次 close 失敗:這一輪的結果不可信 */

/* 關閉並記下失敗。量測中的描述符都是管線與檔案;close 失敗表示有東西不對(資料沒寫回、描述符錯了) */
static void xclose(long fd)
{
    if (sys_close(fd) != 0)
        close_err = 1;
}

/* 一輪:執行 fn(n),回傳每次的奈秒;fn 回傳非 0 表示失敗 */
static void run(const char *name, int (*fn)(int n), int n, int rounds)
{
    uint64_t ns[ROUNDS_MAX];

    for (int r = 0; r < rounds; r++) {
        uint64_t t0, d;
        if (now(&t0) != 0) {
            printf("bench: %s FAILED (cannot read the clock)\n", name);
            failed = 1;
            return;
        }
        close_err = 0;
        if (fn(n) != 0 || close_err) {
            printf("bench: %s FAILED (a system call returned an error)\n", name);
            failed = 1;
            return;
        }
        if (since(t0, &d) != 0) {
            printf("bench: %s FAILED (clock unreadable or went backwards)\n", name);
            failed = 1;
            return;
        }
        ns[r] = udiv64(to_ns(d), (uint64_t)n, NULL);
    }
    sort_u64(ns, rounds);
    printf("bench: %s n=%d rounds=%d min=%llu med=%llu max=%llu ns/op\n", name, n, rounds,
           (unsigned long long)ns[0], (unsigned long long)ns[rounds / 2],
           (unsigned long long)ns[rounds - 1]);
}

/* ---- 各項目 ---- */

static int b_getpid(int n)
{
    for (int i = 0; i < n; i++)
        if (sys_getpid() <= 0)
            return 1;
    return 0;
}

/* 管線來回:子行程讀一個位元組就寫回一個。單 CPU 時每一次來回包含兩次行程切換 */
static int b_pipe_rtt(int n)
{
    int to[2], from[2];
    if (sys_pipe(to) != 0)
        return 1;
    if (sys_pipe(from) != 0) {
        xclose(to[0]);
        xclose(to[1]);
        return 1;
    }
    long pid = sys_fork();
    if (pid < 0) {
        xclose(to[0]);                           /* 不留下開著的管線,免得影響後面的項目 */
        xclose(to[1]);
        xclose(from[0]);
        xclose(from[1]);
        return 1;
    }
    if (pid == 0) {
        char c;
        xclose(to[1]);
        xclose(from[0]);
        while (sys_read(to[0], &c, 1) == 1)
            if (sys_write(from[1], &c, 1) != 1)
                sys_exit(2);
        sys_exit(close_err ? 3 : 0);
    }
    xclose(to[0]);
    xclose(from[1]);
    int bad = 0;
    char c = 'x';
    for (int i = 0; i < n && !bad; i++)
        if (sys_write(to[1], &c, 1) != 1 || sys_read(from[0], &c, 1) != 1)
            bad = 1;
    xclose(to[1]);                            /* 子行程讀到 EOF 就結束 */
    xclose(from[0]);
    if (sys_wait(pid) != 0)
        bad = 1;
    return bad;
}

static int b_fork(int n)
{
    for (int i = 0; i < n; i++) {
        long pid = sys_fork();
        if (pid < 0)
            return 1;
        if (pid == 0)
            sys_exit(7);
        if (sys_wait(pid) != 7)
            return 1;
    }
    return 0;
}

/* 產生新程式:spawn 自己(bench --child,一進 main 就以 7 結束,不輸出任何東西 —
   量到的是載入 ELF、建立與拆掉位址空間,不含主控台輸出)並等它結束(Codex 的審查意見) */
static int b_spawn(int n)
{
    char *argv[] = { "bench", "--child", NULL };
    for (int i = 0; i < n; i++) {
        long pid = sys_spawn("/bin/bench", argv);
        if (pid < 0 || sys_wait(pid) != 7)
            return 1;
    }
    return 0;
}

#define IO_CHUNK 4096
#define IO_TOTAL (256 * 1024)
static char iobuf[IO_CHUNK];
static const char *io_path;
static int io_sync;

/* 寫入 256 KB(每次 4 KB)再讀回來核對;n 是重複次數。每次的時間除以 64 個區塊 = 每 4 KB 幾奈秒 */
static int b_file(int n)
{
    for (int k = 0; k < n; k++) {
        long fd = sys_open(io_path, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0)
            return 1;
        for (int i = 0; i < IO_TOTAL / IO_CHUNK; i++) {
            iobuf[0] = (char)i;
            if (sys_write(fd, iobuf, IO_CHUNK) != IO_CHUNK) {
                xclose(fd);
                return 1;
            }
        }
        if (io_sync && sys_fsync(fd) != 0) {
            xclose(fd);
            return 1;
        }
        xclose(fd);                              /* 寫入端的 close 會把資料交給檔案系統:失敗記在 close_err */
        fd = sys_open(io_path, O_RDONLY);
        if (fd < 0)
            return 1;
        for (int i = 0; i < IO_TOTAL / IO_CHUNK; i++)
            if (sys_read(fd, iobuf, IO_CHUNK) != IO_CHUNK || iobuf[0] != (char)i) {
                xclose(fd);
                return 1;
            }
        xclose(fd);                           /* 唯讀:close 只放掉描述符 */
    }
    return 0;
}

static void file_bench(const char *name, const char *path, int sync, int rounds)
{
    uint64_t ns[ROUNDS_MAX];

    io_path = path;
    io_sync = sync;
    for (int r = 0; r < rounds; r++) {
        uint64_t t0, d;
        close_err = 0;
        if (now(&t0) != 0 || b_file(1) != 0 || close_err || since(t0, &d) != 0) {
            printf("bench: %s FAILED (a system call or the clock failed)\n", name);
            failed = 1;
            (void)sys_unlink(path);
            return;
        }
        ns[r] = udiv64(to_ns(d), 2 * (IO_TOTAL / IO_CHUNK), NULL);   /* 寫 + 讀,每 4 KB */
    }
    sys_unlink(path);
    sort_u64(ns, rounds);
    printf("bench: %s n=%d rounds=%d min=%llu med=%llu max=%llu ns/op\n", name,
           2 * (IO_TOTAL / IO_CHUNK), rounds, (unsigned long long)ns[0],
           (unsigned long long)ns[rounds / 2], (unsigned long long)ns[rounds - 1]);
}

int main(int argc, char **argv)
{
    if (argc > 1 && streq(argv[1], "--child"))
        return 7;                                /* spawn 項目的子行程:什麼都不做 */
    int rounds = argc > 1 ? (int)uatoi(argv[1]) : 5;
    if (rounds < 1 || rounds > ROUNDS_MAX) {
        printf("bench: rounds must be 1..%d\n", ROUNDS_MAX);
        return 2;
    }
    struct clockinfo ci;
    if (sys_clock(&ci) != 0) {
        printf("bench: no high-resolution clock on this machine\n");
        return 2;
    }
    hz = ci.hz;
    struct sysinfo si;
    if (sys_sysinfo(&si) != 0)
        return 1;
    printf("bench: clock %llu Hz (resolution %llu ns), %u cpu(s)\n", (unsigned long long)hz,
           (unsigned long long)to_ns(1) + (to_ns(1) == 0), si.ncpu);
    /* 開機:從模擬器重置到 kmain、kmain 到開機完成(單位微秒) */
    if (ci.kmain && ci.ready >= ci.kmain)
        printf("bench: boot reset_to_kmain=%llu us kmain_to_ready=%llu us\n",
               (unsigned long long)udiv64(to_ns(ci.kmain), 1000, NULL),
               (unsigned long long)udiv64(to_ns(ci.ready - ci.kmain), 1000, NULL));
    else
        printf("bench: boot marks missing\n"), failed = 1;

    /* 時鐘自我檢查:用它量 sleep(100 ms)。
       下界是硬性的:睡眠承諾「至少」100 ms(kernel 以真實時間為起點並多算一個 tick,見 timer.h),
       量到不足 100 ms 表示時鐘換算錯了,或睡眠提早醒(第 51 步的 N-021 就是這樣被發現的)— 兩者都是缺陷。
       上界不是:主機停頓可以讓醒來晚很多,那不代表換算錯。超過 150 ms 時重量一次,連三次都超過才算失敗
       (換算錯是固定的倍數,每次都會超過;主機干擾不會每次都剛好發生) */
    uint64_t slept = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t c0, d;
        if (now(&c0) != 0 || sys_sleep(100) != 0 || since(c0, &d) != 0) {
            printf("bench: clock_check FAILED: sleep or the clock failed\n");
            return 1;
        }
        slept = udiv64(to_ns(d), 1000, NULL);
        printf("bench: clock_check sleep(100 ms) measured %llu us\n", (unsigned long long)slept);
        if (slept < 100000) {
            printf("bench: clock_check FAILED: woke before 100 ms (clock conversion wrong, or sleep woke early)\n");
            return 1;
        }
        if (slept <= 150000)
            break;
        printf("bench: clock_check: more than 150 ms (host scheduling delay?), measuring again\n");
    }
    if (slept > 150000) {
        printf("bench: clock_check FAILED: three sleeps of 100 ms all took more than 150 ms\n");
        return 1;
    }

    run("syscall_getpid", b_getpid, 2000, rounds);
    run("pipe_roundtrip", b_pipe_rtt, 500, rounds);
    run("fork_exit_wait", b_fork, 50, rounds);
    run("spawn_exit_wait", b_spawn, 20, rounds);
    sys_mkdir("/tmp");                           /* 已存在也沒關係 */
    file_bench("file_ram_4k", "/tmp/bench.dat", 0, rounds);
    struct stat st;
    if (sys_stat("/disk", &st) == 0)
        file_bench("file_disk_4k_fsync", "/disk/bench.dat", 1, rounds);
    else
        printf("bench: no disk mounted, disk file test skipped\n");
    return failed;
}
