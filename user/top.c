/*
 * top.c - 即時顯示系統的狀態(類似 Unix 的 top / htop)
 *
 * 用法:top           全螢幕,每秒更新一次;按 q 離開
 *       top -b        只印一次(純文字,不用控制序列),可以接在管線或 script 裡
 *
 * 畫面:
 *   第一列        運作時間、CPU 數、行程與執行緒數、每秒的執行緒切換次數
 *   CPU0 [|||  ]  每顆 CPU 最近一秒的使用率
 *   Mem / Disk    記憶體與磁碟(/disk)的用量
 *   行程列表      PID、父行程、執行緒數、狀態、CPU%、記憶體、累計 CPU 時間、名稱
 *                 CPU% 以「一顆 CPU 全速 = 100」計,多執行緒的行程可以超過 100
 * 按鍵:
 *   q             離開                      上 / 下(或 k / j)  選擇行程
 *   x 再按 y      終止選到的行程(SIGTERM)   s                   暫停 / 繼續選到的行程
 *                 (終止要再按 y 確認:選到的可能是 shell,誤按一下就等於關機)
 *   c  m  p       依 CPU% / 記憶體 / PID 排序
 *
 * 資料全部來自兩個系統呼叫:sys_sysinfo(整個系統)與 sys_procinfo(逐一列出行程)。
 * 「使用率」都是兩次取樣之間的差:kernel 只提供累計的 tick 數,
 * 這一秒用了多少 = 這次的數字 - 上一次的數字。
 *
 * 兩條執行緒:
 *   主執行緒      等按鍵(把主控台設成 raw 模式:按鍵立刻送到,不必按 Enter,也不回顯)
 *   更新執行緒    每秒取樣一次並重畫
 * 這樣不需要「等按鍵,但最多等一秒」這種系統呼叫;兩條執行緒以一把鎖輪流使用畫面與資料。
 *
 * 畫面用終端機通用的控制序列(ANSI escape code)繪製:游標回到左上角之後整個重畫,
 * 每一列畫完清到列尾。整個畫面先組在一塊緩衝區裡,一次寫出,所以不會閃爍。
 */
#include "ulib.h"

#define MAX_ROWS  64                             /* 最多顯示幾個行程 */
#define LIST_ROWS 14                             /* 行程列表一次顯示幾列(畫面以 80x24 計) */
#define COLS      80

struct row {
    struct procinfo pi;
    unsigned dticks;                             /* 這一輪用掉的 CPU tick */
};

static struct row rows[MAX_ROWS];
static int nrows;
static struct { int pid; unsigned ticks; } prev[MAX_ROWS];   /* 上一輪每個行程的累計 tick */
static int nprev;
static struct sysinfo si, si_prev;
static unsigned interval;                        /* 兩次取樣相隔幾個 tick(依時鐘) */
static unsigned cpu_interval;                    /* 同一段時間內,一顆 CPU 實際記了幾個 tick */
static int have_disk;
static struct fsinfo disk;

static umutex_t lock;                            /* 保護以上的資料與畫面 */
static int sort_key = 'c';
static int sel_pid;                              /* 目前選到的行程 */
static int batch;
static int out_failed;                           /* 畫面寫不出去(見 draw) */
static char message[60];                         /* 最下面一列的訊息(上一個動作的結果) */
static int confirm_pid;                          /* 不為 0:正在等使用者按 y 確認終止這個行程 */

/* ---- 畫面緩衝區 ---- */
static char fb[8192];
static unsigned fb_len;
static unsigned line_cols;                       /* 目前這一列已經畫了幾欄(不含控制序列) */

static void put_raw(const char *s)               /* 控制序列:不佔欄位 */
{
    while (*s && fb_len < sizeof(fb))
        fb[fb_len++] = *s++;
}

static void esc(const char *s)
{
    if (!batch)
        put_raw(s);
}

static void put(const char *s)
{
    for (; *s && line_cols < COLS - 1 && fb_len < sizeof(fb); s++, line_cols++)
        fb[fb_len++] = *s;
}

static void put_num(unsigned long v, int width)
{
    char tmp[24];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = n; i < width; i++)
        put(" ");
    while (n > 0) {
        char one[2] = { tmp[--n], 0 };
        put(one);
    }
}

static void put_pad(const char *s, int width)
{
    put(s);
    for (int n = (int)ustrlen(s); n < width; n++)
        put(" ");
}

static void newline(void)
{
    esc("\x1b[K");                               /* 清掉這一列後面殘留的舊內容 */
    put_raw(batch ? "\n" : "\r\n");
    line_cols = 0;
}

/* 容量:小於 10MB 用 KB,否則用 MB。
   (本檔的除法都先轉成 unsigned long:i386 上 64 位元的除法需要編譯器的輔助函式庫,這裡沒有) */
static void put_size(unsigned long kb)
{
    if (kb < 10240) {
        put_num(kb, 0);
        put("K");
    } else {
        put_num(kb / 1024, 0);
        put("M");
    }
}

/* 使用率的長條:[||||||    ]  42% */
static void put_bar(unsigned long used, unsigned long total, int width)
{
    unsigned pct = total ? (unsigned)(used * 100 / total) : 0;
    int fill;

    if (pct > 100)
        pct = 100;
    fill = (int)(pct * (unsigned)width / 100);
    put("[");
    esc(pct >= 80 ? "\x1b[31m" : pct >= 40 ? "\x1b[33m" : "\x1b[32m");   /* 紅 / 黃 / 綠 */
    for (int i = 0; i < width; i++)
        put(i < fill ? "|" : " ");
    esc("\x1b[0m");
    put("]");
    put_num(pct, 4);
    put("%");
}

/* ---- 取樣 ---- */

static void sample(void)
{
    struct procinfo pi;
    long after = 0;

    si_prev = si;
    if (sys_sysinfo(&si) != 0) {
        si = si_prev;                            /* 取不到:沿用上一次的,不要拿半套的資料去算使用率 */
        return;
    }
    interval = (unsigned)(si.uptime_ticks - si_prev.uptime_ticks);
    if (interval == 0)
        interval = 1;
    /* 行程的 CPU% 要和「CPU 實際記帳的 tick 數」相比,而不是時鐘:計時器中斷被延後時
       時鐘會補上遺失的 tick,但記帳是每次中斷記一次,兩者可能差一些 */
    unsigned long total = 0;
    for (unsigned i = 0; i < si.ncpu && i < SYSINFO_MAX_CPUS; i++)
        total += (unsigned long)(si.cpu[i].busy - si_prev.cpu[i].busy) +
                 (unsigned long)(si.cpu[i].idle - si_prev.cpu[i].idle);
    cpu_interval = si.ncpu ? (unsigned)(total / si.ncpu) : 0;
    if (cpu_interval == 0)
        cpu_interval = 1;

    nrows = 0;
    while (nrows < MAX_ROWS && sys_procinfo(after, &pi) == 0) {
        unsigned before = pi.cpu_ticks;          /* 第一次看到的行程:這一輪算 0 */

        after = pi.pid;
        for (int i = 0; i < nprev; i++)
            if (prev[i].pid == pi.pid)
                before = prev[i].ticks;
        rows[nrows].pi = pi;
        rows[nrows].dticks = pi.cpu_ticks >= before ? pi.cpu_ticks - before : 0;
        nrows++;
    }
    for (int i = 0; i < nrows; i++) {
        prev[i].pid = rows[i].pi.pid;
        prev[i].ticks = rows[i].pi.cpu_ticks;
    }
    nprev = nrows;
    have_disk = sys_statfs("/disk", &disk) == 0 && streq(disk.type, "wfs");
}

/* a 是否應該排在 b 前面 */
static int before(const struct row *a, const struct row *b)
{
    if (sort_key == 'c' && a->dticks != b->dticks)
        return a->dticks > b->dticks;
    if (sort_key == 'm' && a->pi.mem_kb != b->pi.mem_kb)
        return a->pi.mem_kb > b->pi.mem_kb;
    return a->pi.pid < b->pi.pid;
}

static void sort_rows(void)
{
    for (int i = 1; i < nrows; i++) {            /* 插入排序:行程不多,而且每次幾乎已經排好 */
        struct row r = rows[i];
        int j = i;
        for (; j > 0 && before(&r, &rows[j - 1]); j--)
            rows[j] = rows[j - 1];
        rows[j] = r;
    }
}

static int sel_index(void)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].pi.pid == sel_pid)
            return i;
    return -1;
}

/* ---- 繪製 ---- */

static const char *state_name(int st)
{
    return st == PROC_RUNNING ? "run" : st == PROC_SLEEPING ? "sleep" :
           st == PROC_STOPPED ? "stop" : "zombie";
}

static void draw(void)
{
    unsigned long secs = (unsigned long)si.uptime_ticks / (si.hz ? si.hz : 1);

    sort_rows();
    if (sel_index() < 0)
        sel_pid = nrows ? rows[0].pi.pid : 0;    /* 選到的行程不見了:改選第一個 */

    fb_len = 0;
    line_cols = 0;
    esc("\x1b[H");                               /* 游標回到左上角 */

    esc("\x1b[1m");
    put("WElyOS top");
    esc("\x1b[0m");
    put("  up ");
    put_num(secs / 3600, 0);
    put(secs / 60 % 60 < 10 ? ":0" : ":");
    put_num(secs / 60 % 60, 0);
    put(secs % 60 < 10 ? ":0" : ":");
    put_num(secs % 60, 0);
    put("  ");
    put_num(si.ncpu, 0);
    put(" cpus  ");
    put_num(si.nprocs, 0);
    put(" processes  ");
    put_num(si.nthreads, 0);
    put(" threads  ");
    put_num((unsigned long)(si.ctx_switches - si_prev.ctx_switches) * si.hz / interval, 0);
    put(" switches/s");
    newline();

    for (unsigned i = 0; i < si.ncpu && i < SYSINFO_MAX_CPUS; i++) {
        unsigned long busy = (unsigned long)(si.cpu[i].busy - si_prev.cpu[i].busy);
        unsigned long idle = (unsigned long)(si.cpu[i].idle - si_prev.cpu[i].idle);

        put("CPU");
        put_num(i, 0);
        put(" ");
        put_bar(busy, busy + idle, 24);
        if (i % 2 == 0 && i + 1 < si.ncpu)
            put("    ");                         /* 一列放兩顆 */
        else
            newline();
    }

    put("Mem  ");
    put_bar((unsigned long)(si.mem_total_kb - si.mem_free_kb), (unsigned long)si.mem_total_kb, 24);
    put(" ");
    put_size((unsigned long)(si.mem_total_kb - si.mem_free_kb));
    put("/");
    put_size((unsigned long)si.mem_total_kb);
    newline();
    if (have_disk) {
        unsigned long kb = disk.block_size / 1024;
        put("Disk ");
        put_bar((unsigned long)(disk.blocks - disk.free_blocks), (unsigned long)disk.blocks, 24);
        put(" ");
        put_size((unsigned long)(disk.blocks - disk.free_blocks) * kb);
        put("/");
        put_size((unsigned long)disk.blocks * kb);
        put("  /disk");
        newline();
    }
    newline();

    esc("\x1b[7m");                              /* 欄位名稱反白 */
    put("    PID  PPID THR STATE   CPU%  MEM(KB)  TIME(ms)  NAME");
    while (line_cols < COLS - 1)
        put(" ");
    esc("\x1b[0m");
    newline();

    /* 行程太多時只顯示一部分:讓選到的那一列留在視窗裡 */
    int sel = sel_index(), first = 0;
    int visible = batch ? nrows : LIST_ROWS;
    if (sel >= visible)
        first = sel - visible + 1;
    for (int i = first; i < nrows && i < first + visible; i++) {
        const struct procinfo *pi = &rows[i].pi;
        int selected = !batch && pi->pid == sel_pid;

        if (selected)
            esc("\x1b[1m");
        put(selected ? "> " : "  ");
        put_num((unsigned long)pi->pid, 5);
        put_num((unsigned long)pi->ppid, 6);
        put_num((unsigned long)pi->nthreads, 4);
        put(" ");
        put_pad(state_name(pi->state), 6);
        put_num(rows[i].dticks * 100ul / cpu_interval, 6);
        put_num(pi->mem_kb, 9);
        put_num(pi->cpu_ticks, 10);
        put("  ");
        put(pi->name);
        if (selected)
            esc("\x1b[0m");
        newline();
    }
    if (!batch) {
        for (int i = nrows - first; i < LIST_ROWS; i++)      /* 清掉上一次比較長的列表留下的列 */
            newline();
        if (message[0])
            esc("\x1b[1m");
        put(message[0] ? message : "q quit  up/down select  x kill  s stop/continue  "
                                   "sort: c cpu  m memory  p pid");
        esc("\x1b[0m");
        esc("\x1b[K");
    }
    /* 畫面寫不出去(輸出的另一端已經關閉…):記下來,主迴圈會結束,而不是一直畫給沒有人看 */
    if (sys_write(1, fb, fb_len) != (long)fb_len) {
        out_failed = 1;
        if (!batch) {                            /* 互動模式:還原主控台後結束 */
            sys_tcsetmode(0);
            sys_exit(1);
        }
    }
}

/* ---- 更新執行緒 ---- */

static void updater(long arg)
{
    (void)arg;
    for (;;) {
        sys_sleep(1000);
        umutex_lock(&lock);
        /* 每次都重新要求 raw 模式:被 Ctrl-Z 暫停再用 fg 回來之後,shell 已經把模式改回去了 */
        sys_tcsetmode(CON_RAW | CON_NOECHO);
        sample();
        if (!confirm_pid)
            message[0] = 0;                      /* 等確認的時候,問句要留在畫面上 */
        draw();
        umutex_unlock(&lock);
    }
}

static void set_message(const char *what, long pid, long rc)
{
    struct line l = { .len = 0 };

    line_str(&l, rc == 0 ? what : "cannot signal process ");
    line_num(&l, pid);
    l.buf[l.len < sizeof(message) - 1 ? l.len : sizeof(message) - 1] = 0;
    for (unsigned i = 0; i < sizeof(message); i++) {
        message[i] = l.buf[i];
        if (!l.buf[i])
            break;
    }
}

/* 被 Ctrl-C(SIGINT)或 kill(SIGTERM)終止時:先把畫面與主控台還原再結束,
   游標才不會留在畫面中間、顏色也不會留在最後使用的那一種 */
static void on_terminate(int sig)
{
    sys_write(1, "\x1b[0m\x1b[24;1H\r\n", 14);
    sys_tcsetmode(0);
    sys_exit(128 + sig);
}

int main(int argc, char **argv)
{
    char keys[16];
    int state = 0;                               /* 方向鍵是三個字元:ESC [ A;記住讀到第幾個 */
    long n;

    batch = argc > 1 && streq(argv[1], "-b");
    if (sys_sysinfo(&si) != 0) {
        puts_("top: cannot read the system information\n");
        return 1;
    }
    sample();                                    /* 第一次取樣只是建立基準 */
    if (batch) {
        sys_sleep(100);                          /* 兩次取樣之間的間隔:夠算出使用率就好 */
        sort_key = 'p';
        sample();
        draw();
        return out_failed ? 1 : 0;
    }

    if (sys_tcsetmode(CON_RAW | CON_NOECHO) != 0) {
        puts_("top: not in the foreground of the console (try: top -b)\n");
        return 1;
    }
    sys_signal(SIGINT, on_terminate);
    sys_signal(SIGTERM, on_terminate);
    sys_write(1, "\x1b[2J", 4);                  /* 清除整個畫面 */
    sys_sleep(200);
    sample();
    draw();
    if (sys_thread_create(updater, 0) < 0) {
        puts_("\ntop: cannot start the update thread\n");
        return 1;
    }

    while ((n = sys_read(0, keys, sizeof(keys))) > 0 || n == -E_INTR) {
        if (n < 0)
            continue;                            /* 讀取被訊號打斷:再讀一次 */
        umutex_lock(&lock);
        for (long i = 0; i < n; i++) {
            int c = keys[i], move = 0, idx;

            if (confirm_pid) {                   /* 上一個按鍵是 x:只有 y 才真的終止 */
                if (c == 'y')
                    set_message("sent SIGTERM to process ", confirm_pid,
                                sys_kill(confirm_pid, SIGTERM));
                else
                    set_message("cancelled; did not kill process ", confirm_pid, 0);
                confirm_pid = 0;
                continue;
            }
            if (state == 1) {                    /* ESC 之後 */
                state = c == '[' ? 2 : 0;
                continue;
            }
            if (state == 2) {                    /* ESC [ 之後:A = 上,B = 下 */
                state = 0;
                move = c == 'A' ? -1 : c == 'B' ? 1 : 0;
            } else if (c == 0x1B) {
                state = 1;
                continue;
            } else if (c == 'k') {
                move = -1;
            } else if (c == 'j') {
                move = 1;
            } else if (c == 'q') {
                /* 離開:游標移到畫面最下面、換行,把主控台還原成一般模式。
                   拿著鎖結束:更新執行緒不會在這之後又畫一次 */
                sys_write(1, "\x1b[0m\x1b[24;1H\r\n", 14);
                sys_tcsetmode(0);
                sys_exit(0);
            } else if (c == 'c' || c == 'm' || c == 'p') {
                sort_key = c;
            } else if (c == 'x' && sel_pid) {
                confirm_pid = sel_pid;
                set_message("press y to kill (any other key cancels) process ", sel_pid, 0);
            } else if (c == 's' && (idx = sel_index()) >= 0) {
                int stopped = rows[idx].pi.state == PROC_STOPPED;
                long rc = sys_kill(sel_pid, stopped ? SIGCONT : SIGSTOP);
                set_message(stopped ? "continued process " : "stopped process ", sel_pid, rc);
                if (rc == 0)
                    rows[idx].pi.state = stopped ? PROC_SLEEPING : PROC_STOPPED;
            }
            if (move && (idx = sel_index()) >= 0 && idx + move >= 0 && idx + move < nrows)
                sel_pid = rows[idx + move].pi.pid;
        }
        draw();                                  /* 按鍵之後立刻重畫(用上一次取樣的資料) */
        umutex_unlock(&lock);
    }
    sys_tcsetmode(0);
    return 0;
}
