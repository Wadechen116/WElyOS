/*
 * watchdog.c - 看門狗:發現「卡死」的 CPU
 *
 * 一顆 CPU 如果在關著中斷的情況下進入無窮迴圈(例如死結、驅動程式等一個永遠不來的事件),
 * 它就不再處理任何事,而且自己不可能發現 — 能發現的只有別人。
 *
 * 做法:每顆 CPU 在自己的計時器中斷裡把一個計數器(心跳)加一。
 * CPU 排成一圈,每顆負責看「下一顆」:如果對方的心跳超過期限都沒有變,
 * 表示它已經那麼久沒有處理計時器中斷了。這時整個系統的狀態已經不可信
 * (那顆 CPU 可能握著別人需要的鎖),所以記下事件並 panic,而不是試著繼續。
 * 真實的系統接著會重新啟動;黑盒子裡留下的紀錄說明原因。
 *
 * 期限是以「觀察者自己的 tick」計算的:整台機器(模擬器)被暫停時大家一起停,不會誤判。
 * 檢查的成本:每顆 CPU 每個 tick 多一次加法,每 64 個 tick 比較一次。
 *
 * 限制:只有一顆 CPU 時沒有人可以看它(需要獨立的硬體看門狗計時器,這裡沒有)。
 */
#include "cpu.h"
#include "panic.h"
#include "watchdog.h"

static unsigned timeout_ticks = 5000;        /* 預設 5 秒(1 tick = 1ms) */
static unsigned long seen_beat[MAX_CPUS];    /* 上次看到對方的心跳;只由負責看它的那顆 CPU 讀寫 */
static unsigned long seen_at[MAX_CPUS];      /* 那是在「我自己」的第幾個 tick */

void watchdog_set_timeout(unsigned ms)
{
    timeout_ticks = ms;
}

void watchdog_tick(void)
{
    struct cpu *me = this_cpu();
    unsigned long now = ++me->heartbeat;

    if (cpu_count < 2 || (now & 63) != 0)
        return;
    int next = (me->id + 1) % cpu_count;
    struct cpu *c = &cpus[next];
    if (!c->online)
        return;
    unsigned long beat = c->heartbeat;
    if (beat != seen_beat[next] || seen_at[next] == 0) {
        seen_beat[next] = beat;              /* 有在動:記下現在的狀況 */
        seen_at[next] = now;
    } else if (now - seen_at[next] > timeout_ticks) {
        panic("watchdog: cpu %d has not handled a timer interrupt for %u ms "
              "(stuck with interrupts disabled)", next, (unsigned)(now - seen_at[next]));
    }
}
