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
 *   Ctrl-C             放棄目前這一行
 *   Ctrl-D             行首:表示「輸入結束」(讀取者得到 0,和檔案結尾相同);
 *                      行中:把目前已經打的部分直接交出去
 *
 * 字元怎麼從硬體進來:目前用「輪詢」— 計時器每個 tick(1ms)去問一次硬體有沒有新的
 * 字元(console_poll)。比起讓序列埠自己發中斷,輪詢簡單得多(不必為三個架構各自
 * 設定中斷控制器),代價是最多 1ms 的延遲,以及沒有輸入時每個 tick 多一次讀取。
 * 對人打字來說感覺不出來。之後改成中斷驅動時,只要改「誰呼叫 console_input」。
 *
 * 上鎖:輸入的狀態與等待佇列都由排程器的鎖保護,因為讀取者要把
 * 「檢查有沒有資料」與「睡下去」做成不可分割的一步(見 sched.h)。
 */
#include <stddef.h>
#include "arch.h"
#include "console.h"
#include "kprintf.h"
#include "sched.h"

#define LINE_MAX   256u                      /* 編輯中的一行的最大長度 */
#define COOKED_MAX 1024u                     /* 已完成、還沒被讀走的資料的最大量 */

static char line[LINE_MAX];                  /* 正在編輯的這一行 */
static unsigned line_len;

static char cooked[COOKED_MAX];              /* 已完成的資料(環狀緩衝區) */
static unsigned cooked_head, cooked_count;
static unsigned eof_pending;                 /* 有幾次「輸入結束」等著被讀取者看到 */

static struct wait_queue readers = WAIT_QUEUE_INIT;

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
        line_len = 0;
        kprintf("^C\n");
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

void console_poll(void)
{
    int c = arch_console_getc();

    if (c < 0)
        return;                              /* 絕大多數時候沒有輸入:只花一次讀取 */
    unsigned long flags = sched_lock();
    do {
        console_input(c);
    } while ((c = arch_console_getc()) >= 0);
    sched_preempt_point();                   /* 在中斷裡不會切換;切換留到中斷返回時 */
    sched_unlock(flags);
}

void console_wake_readers(void)
{
    wq_wake_all(&readers);
}

/* 讀取:等到有完成的一行,回傳其中最多 n 個位元組(不跨行)。
   回傳 0 表示輸入結束。被終止的行程會提早返回 */
static long console_read(void *buf, size_t n)
{
    char *out = buf;
    size_t got = 0;
    unsigned long flags = sched_lock();

    while (cooked_count == 0 && eof_pending == 0 && !thread_killed())
        wq_sleep(&readers);

    if (cooked_count == 0) {
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
