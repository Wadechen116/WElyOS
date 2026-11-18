/*
 * console.c - 主控台的輸入處理(line discipline)與 /dev/console
 *
 * 輸出很單純:寫入就是印出來。輸入則有一層處理,稱為 line discipline(行規程):
 * 使用者打字時會打錯、會按退格鍵,程式通常不想處理這些,只想拿到「一整行」。
 * 所以由 kernel 負責收集字元、處理編輯、把打的字顯示出來(回顯,echo),
 * 等使用者按下 Enter 才把整行交給正在讀取的程式。Unix 稱這種模式為 canonical mode。
 *
 *   硬體 --(字元)--> console_input() --(編輯中的一行)--> 按 Enter --> 已完成的資料 --> read()
 *
 * 特殊按鍵:
 *   Enter              這一行完成,交給讀取者
 *   Backspace / DEL    刪掉前一個字元
 *   Ctrl-C             放棄目前這一行;有前景的命令在執行的話,終止它
 *   Ctrl-Z             暫停前景的命令(之後可以用 shell 的 fg / bg 讓它繼續)
 *   Ctrl-D             行首:表示「輸入結束」(讀取者得到 0,和檔案結尾相同);
 *                      行中:把目前已經打的部分直接交出去
 *
 * 字元怎麼從硬體進來:由「中斷」驅動。序列埠收到字元時自己發出中斷,
 * 架構層的中斷處理程式呼叫 console_rx(),把硬體緩衝區裡的字元全部取出來處理。
 * (早期的版本是輪詢:計時器每 1ms 來問一次。那樣每秒白問 1000 次,
 * 而且硬體的緩衝區只有 16 個字元,1ms 內進來超過這個數量 — 例如貼上一段文字 —
 * 多的就被丟掉。中斷驅動則是一有字元就取走,沒有輸入時完全不花時間。)
 * 各架構怎麼把序列埠的中斷接到 CPU,見 arch/<名稱>/ 裡的 arch_console_irq_init。
 *
 * 上鎖:輸入的狀態與等待佇列都由排程器的鎖保護,因為讀取者要把
 * 「檢查有沒有資料」與「睡下去」做成不可分割的一步(見 sched.h)。
 */
#include <stddef.h>
#include "arch.h"
#include "console.h"
#include "kprintf.h"
#include "proc.h"
#include "sched.h"

#define LINE_MAX   256u                      /* 編輯中的一行的最大長度 */
#define COOKED_MAX 4096u                     /* 已完成、還沒被讀走的資料的最大量 */

static char line[LINE_MAX];                  /* 正在編輯的這一行 */
static unsigned line_len;

static char cooked[COOKED_MAX];              /* 已完成的資料(環狀緩衝區) */
static unsigned cooked_head, cooked_count;
static unsigned eof_pending;                 /* 有幾次「輸入結束」等著被讀取者看到 */

static struct wait_queue readers = WAIT_QUEUE_INIT;
static int fg_pgid;                          /* 前景行程群組;0 = 沒有 */
static int owner_pgid;                       /* 管理前景的那個行程(shell)的群組;0 = 沒有人管理 */

static unsigned con_mode;                    /* CON_RAW、CON_NOECHO;0 = 一般的整行模式 */

/* 換輸入模式:編輯到一半的那一行作廢;離開 raw 模式時,還沒被讀走的按鍵也丟掉
   (那是打給上一個程式的,不該變成下一個命令的開頭) */
static void mode_change(unsigned mode)
{
    if ((con_mode & CON_RAW) && !(mode & CON_RAW))
        cooked_count = 0;
    if (mode != con_mode)
        line_len = 0;
    con_mode = mode;
}

void console_set_fg(int pgid, int owner)
{
    fg_pgid = pgid;
    owner_pgid = owner;
    mode_change(0);
}

void console_wake_readers(void)
{
    wq_wake_all(&readers);
}

/*
 * 目前這條執行緒可不可以讀鍵盤?
 *
 * 鍵盤只有一個,同時有好幾個行程在讀的話,打的每一行會被隨機的一個拿走。
 * 規則和 Unix 的終端機相同:只有「前景」的那一組可以讀 —
 *   有前景群組(shell 正在等一個命令):只有那一組
 *   沒有前景群組(shell 在提示符號下):只有 shell 自己那一組
 * 其他行程(背景工作)一讀就會被暫停(SIGTTIN),等使用者用 fg 把它帶到前景才繼續。
 * 沒有任何 shell 在管理時(owner 為 0,或那個 shell 已經結束)不限制。
 */
static int may_read(void)
{
    int pg = proc_self_pgid();

    if (pg == 0)
        return 1;                            /* kernel 自己的執行緒 */
    if (fg_pgid)
        return pg == fg_pgid;
    if (owner_pgid && !proc_group_alive_locked(owner_pgid))
        owner_pgid = 0;
    return owner_pgid == 0 || pg == owner_pgid;
}

static int may_read(void);

int console_set_mode(unsigned mode)
{
    if (!may_read())
        return -E_INVAL;
    mode_change(mode);
    return 0;
}

/* 把編輯中的這一行移到「已完成」。放不下的部分丟掉(讀取者太久沒來讀) */
static void commit_line(void)
{
    for (unsigned i = 0; i < line_len && cooked_count < COOKED_MAX; i++)
        cooked[(cooked_head + cooked_count++) % COOKED_MAX] = line[i];
    line_len = 0;
    wq_wake_all(&readers);
}

/* 處理一個輸入的字元。呼叫時必須持有排程器的鎖 */
static void console_input(int c)
{
    if (c == '\r')
        c = '\n';                            /* 終端機的 Enter 送出的是 \r */

    if ((con_mode & CON_RAW) && !((c == 0x03 || c == 0x1A) && fg_pgid > 0)) {
        /* raw 模式(全螢幕的程式,例如 top):每個按鍵原樣、立刻交給讀取者,不做行編輯。
           Ctrl-C 與 Ctrl-Z 仍然照常處理(往下走),所以卡住的程式一樣可以被終止 */
        if (cooked_count < COOKED_MAX)
            cooked[(cooked_head + cooked_count++) % COOKED_MAX] = (char)c;
        wq_wake_all(&readers);
        if (!(con_mode & CON_NOECHO) && (c >= 0x20 || c == '\n'))
            kprintf("%c", c);
        return;
    }

    if (c == '\n') {
        if (line_len < LINE_MAX)
            line[line_len++] = '\n';
        kprintf("\n");
        commit_line();
    } else if (c == 0x7F || c == '\b') {
        if (line_len > 0) {
            line_len--;
            kprintf("\b \b");                /* 游標退一格、蓋掉那個字、再退回來 */
        }
    } else if (c == 0x03) {                  /* Ctrl-C */
        /* 放棄正在打的這一行;有前景的程式正在執行的話,終止它們(SIGINT = 2)。
           shell 在執行命令期間把那個命令設為前景,等輸入時設回「沒有」,
           所以在提示符號下按 Ctrl-C 不會把 shell 自己結束掉 */
        line_len = 0;
        kprintf("^C\n");
        if (fg_pgid > 0) {
            proc_signal_group_locked(fg_pgid, 2);
        } else {
            /* 提示符號下:交出一個空行,讓 shell 重新印出提示符號 */
            line[line_len++] = '\n';
            commit_line();
        }
    } else if (c == 0x1A) {                  /* Ctrl-Z */
        /* 暫停前景的命令(SIGTSTP = 20)。shell 正在等它,會得到通知並回到提示符號。
           沒有前景命令時不做任何事 */
        if (fg_pgid > 0) {
            line_len = 0;
            kprintf("^Z\n");
            proc_signal_group_locked(fg_pgid, 20);
        }
    } else if (c == 0x04) {                  /* Ctrl-D */
        if (line_len == 0)
            eof_pending++;
        commit_line();                       /* 行中:交出已打的部分;行首:只叫醒讀取者 */
    } else if (c >= 0x20 && line_len < LINE_MAX - 1) {
        line[line_len++] = (char)c;          /* 留一格給最後的換行 */
        kprintf("%c", c);
    }
    /* 其他控制字元,以及超過長度的部分:忽略 */
}

void console_rx(void)
{
    int c = arch_console_getc();

    if (c < 0)
        return;                              /* 沒有資料(例如中斷送達前已經被取走) */
    unsigned long flags = sched_lock();
    do {
        console_input(c);
    } while ((c = arch_console_getc()) >= 0);
    sched_preempt_point();                   /* 在中斷裡不會切換;切換留到中斷返回時 */
    sched_unlock(flags);
}

void console_init(void)
{
    arch_console_irq_init();
    console_rx();                            /* 啟用中斷之前就已經收到的字元,現在取出來 */
}

/* 讀取:等到有完成的一行,回傳其中最多 n 個位元組(不跨行)。
   回傳 0 表示輸入結束。被終止的行程會提早返回 */
static long console_read(void *buf, size_t n)
{
    char *out = buf;
    size_t got = 0;
    unsigned long flags = sched_lock();

    for (;;) {
        if (thread_interrupted())
            break;
        if (!proc_self_stopped() && !may_read())
            proc_signal_group_locked(proc_self_pgid(), 21);  /* SIGTTIN:背景的行程不可以讀鍵盤 */
        /* 被暫停的行程不拿輸入:睡到它繼續為止(SIGCONT 會來叫醒,再檢查一次) */
        if (!proc_self_stopped() && (cooked_count > 0 || eof_pending > 0))
            break;
        wq_sleep(&readers);
    }

    if (thread_interrupted() && (proc_self_stopped() || !may_read())) {
        /* 被終止或被訊號打斷而離開等待:不是輪到它讀,什麼都不拿 */
    } else if (cooked_count == 0 && eof_pending == 0 && thread_interrupted() && !thread_killed()) {
        sched_unlock(flags);
        return -E_INTR;                      /* 被訊號打斷而且沒有輸入:不是「結束」 */
    } else if (cooked_count == 0) {
        if (eof_pending)
            eof_pending--;                   /* 一次 Ctrl-D 只讓一次讀取看到「結束」 */
    } else {
        while (got < n && cooked_count > 0) {
            char c = cooked[cooked_head];
            cooked_head = (cooked_head + 1) % COOKED_MAX;
            cooked_count--;
            out[got++] = c;
            if (c == '\n')
                break;                       /* 一次最多交出一行 */
        }
    }
    sched_unlock(flags);
    return (long)got;
}

/* 寫入 = 輸出到主控台。這樣使用者程式的輸出就只是「寫入 1 號檔案描述符」,
   之後要把輸出導向檔案,只要換掉描述符指向的東西,程式不必修改 */
static long console_write(const void *buf, size_t n)
{
    const char *p = buf;
    char chunk[128];
    size_t done = 0;

    while (done < n) {
        size_t k = n - done;
        if (k > sizeof(chunk) - 1)
            k = sizeof(chunk) - 1;
        for (size_t i = 0; i < k; i++)
            chunk[i] = p[done + i];
        chunk[k] = '\0';
        kprintf("%s", chunk);
        done += k;
    }
    return (long)n;
}

static const struct dev_ops console_dev = { console_read, console_write };

const struct dev_ops *console_dev_ops(void)
{
    return &console_dev;
}
