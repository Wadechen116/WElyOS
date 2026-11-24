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
#include "sched.h"
#include "timer.h"
#include "watchdog.h"

static unsigned timeout_ticks = 5000;        /* 預設 5 秒(1 tick = 1ms) */
static unsigned long seen_beat[MAX_CPUS];    /* 上次看到對方的心跳;只由負責看它的那顆 CPU 讀寫 */
static unsigned long seen_at[MAX_CPUS];      /* 那是在「我自己」的第幾個 tick */

void watchdog_set_timeout(unsigned ms)
{
    timeout_ticks = ms;
}

/*
 * 階段看門狗(I-350):心跳看門狗只抓「某顆 CPU 關著中斷卡死」;中斷照常進來、卻有執行緒永遠等不到
 * (喚醒遺失、排程死結)時,所有 CPU 都有心跳,它看不出來 — 第 72 步 aarch64 降級模式的親和性測試
 * 就這樣靜靜卡到 90 秒逾時,沒有留下任何現場。自我測試的一個階段可以設期限;每顆 CPU 的每次
 * 計時器中斷比較一次(一次讀取 + 比較),逾時的第一顆 CPU 印出排程器狀態再 panic。
 * 用高精度時鐘量:降級時系統 tick 會落後(I-346),不能拿它量「真的過了多久」。
 */
static const char *stage_name;
static const volatile unsigned *stage_progress;
static uint64_t stage_start, stage_limit;    /* 時鐘計數;只在 stage_armed == 0 時改 */
static volatile int stage_armed, stage_fired;   /* 32 位元旗標:i386 沒有 64 位元的 atomic */
static volatile unsigned stage_gen;          /* 每次設定加一:讀者比較前後,換過就不算(像 seqlock) */

void watchdog_stage(const char *name, unsigned secs, const volatile unsigned *progress)
{
    uint64_t hz = timer_clock_hz();
    __atomic_store_n(&stage_armed, 0, __ATOMIC_SEQ_CST);     /* 先停,再改內容(讀者只在 armed 時讀) */
    __atomic_add_fetch(&stage_gen, 1, __ATOMIC_SEQ_CST);
    if (!name || hz == 0)
        return;
    stage_name = name;
    stage_progress = progress;
    stage_start = timer_clock_count();
    stage_limit = hz * secs;
    __atomic_store_n(&stage_armed, 1, __ATOMIC_SEQ_CST);     /* 內容寫好之後才啟用 */
}

static void stage_check(void)
{
    unsigned g = __atomic_load_n(&stage_gen, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&stage_armed, __ATOMIC_SEQ_CST))
        return;
    uint64_t start = stage_start, lim = stage_limit;
    if (__atomic_load_n(&stage_gen, __ATOMIC_SEQ_CST) != g)
        return;                              /* 讀的期間被重設:這次不算,下一次中斷再看 */
    if (timer_clock_count() - start < lim)
        return;
    if (__atomic_exchange_n(&stage_fired, 1, __ATOMIC_SEQ_CST))
        return;                              /* 別顆 CPU 已經在報了 */
    sched_dump_state();
    panic("watchdog: self-test stage \"%s\" did not finish in time (progress %d; threads above)", stage_name,
          stage_progress ? (int)*stage_progress : -1);
}

void watchdog_tick(void)
{
    struct cpu *me = this_cpu();
    unsigned long now = ++me->heartbeat;

    stage_check();

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
