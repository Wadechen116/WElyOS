#ifndef WELYOS_TIMER_H
#define WELYOS_TIMER_H
#include <stdint.h>

void timer_init(unsigned hz);   /* 啟動週期性計時器中斷(每秒 hz 次) */
void timer_tick(void);          /* 由架構層的中斷處理程式在每次計時器中斷時呼叫 */
/* 中斷晚到、一次補記好幾個 tick 時,第一個以外的用這個:時間照樣往前(喚醒休眠者、檢查期限),
   但不算成任何執行緒用掉的 CPU 時間 — 那段時間這顆 CPU 是被停住的,沒有人在執行 */
void timer_tick_stolen(void);
uint64_t timer_ticks(void);     /* 開機至今的 tick 數(任何一顆 CPU 都可能推進它,見 timer.c) */
uint32_t timer_advances(int cpu);  /* 第 cpu 顆 CPU 推進過幾次系統時間(量測與測試用) */
/*
 * 共用計數器路徑(I-337)。kernel/clksync.c 驗證各 CPU 的計數器一致之後才讓 AP 推進時間;
 * 驗證失敗時呼叫 timer_shared_disable() 退回「只有第 0 顆推進」(降級,不可逆)。
 *   timer_shared_enabled()  目前是否走共用路徑(有共用計數器且未停用)
 *   timer_tick_counts()     一個 tick 相當於幾個計數(timer_init 算好、之後唯讀;0 = 沒有共用計數器)。
 *                           各架構一律無條件進位:計數 × 每秒 tick 數 >= 計數器頻率,系統時間只會比真實時間慢、
 *                           不會快(「至少睡 n 毫秒」才成立;clock_view_test 檢查)。
 *                           i386 沒有 64 位元除法,需要「每 tick 計數」的地方用它,不要自己除
 */
void     timer_shared_disable(void);
int      timer_shared_enabled(void);
uint64_t timer_tick_counts(void);
/* I-341:換到備援的共用計數器(i386 TSC → HPET)。1 = 已換,呼叫端必須接著驗證;0 = 沒有備援 */
int         timer_clock_fallback(void);
const char *timer_clock_name(void);      /* 目前共用計數器的名稱(tsc / hpet / hpet32 / time / cntpct / none) */
unsigned timer_hz(void);        /* 每秒的 tick 數 */

/*
 * 計時器中斷的「晚到」紀錄(量測,不是推測)。
 * 正常時每次中斷記 1 個 tick;中斷晚到時架構層一次補記好幾個 — 那段時間這顆 CPU 上沒有任何程式
 * 在執行,時間卻跳過去了。實測(2026-11-19):單一 CPU 每次開機約 650 次補記 2~3 個(中斷晚到
 * 1~2 毫秒,常態);8 顆虛擬 CPU 時在 RISC-V / AArch64 上出現 50~69 個 tick 的停頓。
 *
 * 以「依幅度分組的累計次數」記錄:測試在一段期間的前後各取一次,相減就知道這段期間發生過幾次
 * 多大的跳躍 — 不會因為歷史紀錄被蓋過而失真。分組:2、3、4~5、6、7~9、10 以上。
 */
#define TIMER_BURST_BUCKETS 6
struct timer_stats {
    uint32_t burst[TIMER_BURST_BUCKETS];    /* 各組的累計次數 */
    uint32_t max_burst;                     /* 單次補記最多幾個 tick */
    uint32_t resyncs;                       /* 晚太多、放棄補記直接重新對時的次數(算在「10 以上」) */
    /* I-346:降級(沒有共用計數器)時不補記,系統時間落後實際時間。累計丟掉的 tick 與連續丟掉最多幾個 */
    uint32_t degraded_dropped;
    uint32_t degraded_max_run;
};
void timer_note_catchup(int n, int resynced);   /* 架構層:這次中斷補記了 n 個 tick */
void timer_get_stats(struct timer_stats *st);
/* 診斷用(panic 之前的狀態傾印):不會無限等待、不會卡在鎖上。bit 0 = *t 讀到一致的系統時間,
   bit 1 = *st 讀到統計(統計的鎖只試一次)。讀不到的就不要印 */
int timer_diag_peek(uint64_t *t, struct timer_stats *st);
/* I-349 校準(只給自我測試):stallinj=steady 時,推進時間的 CPU 在持有時間鎖時每 10 tick 忙等 3 tick。
   enable 在開機時設;window 由測試在 CPU0 卡住期間開關(打開時清統計);stats 讀注入次數、
   執行過的 CPU 遮罩、實際延遲的合計與最大值(計數) */
void timer_test_steady(int enable);
void timer_test_steady_window(int active);
void timer_test_steady_stats(uint32_t *count, uint32_t *cpus, uint64_t *total, uint64_t *max);
/* 累計有幾次「一次補記 >= k 個 tick」。k 會往上取到分組的邊界(2、3、4、6、7、10),
   所以算出來只會少不會多 — 測試用它放寬條件時,只會更嚴格,不會更寬鬆 */
uint32_t timer_bursts_at_least(const struct timer_stats *st, unsigned k);
/* 實際的日期時間。開機時向硬體的即時時鐘問一次「現在幾點」,之後靠 tick 往前推算。
   回傳 1970-01-01 00:00:00 UTC 至今的秒數(Unix 時間);*usec 得到秒以下的微秒數(可為 NULL)。
   硬體沒有時鐘時,從 0(1970 年)開始算 */
void     timer_clock_init(void);
uint64_t timer_epoch(uint32_t *usec);
/* 開機時間的記號(效能量測用):在 kmain 開頭與「開機完成」各記一次高精度時鐘的值,
   SYS_clock 交給使用者程式。只記第一次,之後的呼叫不改變它 */
enum { CLOCK_MARK_KMAIN, CLOCK_MARK_READY };
void     timer_clock_mark(int which);
void     timer_clock_marks(uint64_t *kmain, uint64_t *ready);
uint64_t timer_clock_count(void);    /* 高精度時鐘的現值(架構層的計數器) */
uint64_t timer_clock_hz(void);       /* 它每秒的計數;0 = 沒有可用的時鐘 */
/*
 * 以真實時間為準的 tick(第 51 步;I-337 起任何 CPU 都能推進)。timer_ticks() 在每顆 CPU 的計時器中斷裡
 * 推進,所有 CPU 的中斷都晚到時會暫時落後真實時間;拿它當「等多久」的起點,真正等到的時間就會不足。
 *   timer_ticks_real()     現在「應該」是第幾個 tick:timer_ticks() 加上已經到期、還沒記的 tick
 *                          (由共用的高精度時鐘與全系統下一個 tick 的預定時間算出)。永遠 >= timer_ticks()
 *   timer_deadline_ms(ms)  「至少 ms 毫秒之後」對應的 tick;ms 為 0 時回傳 0(立刻到期)
 *   timer_expired(dl)      timer_ticks() >= dl。因為 timer_ticks() 不會超前真實時間,
 *                          到期時真實經過的時間一定 >= 要求的時間
 * 架構層沒有高精度時鐘時,timer_ticks_real() 等於 timer_ticks()(無從得知落後多少)。
 */
uint64_t timer_ticks_real(void);
/* 「至少等多久」的起點(thread_sleep、timer_deadline_ms 用):timer_ticks_real(),降級時再加 1 —
   補記的 tick 被丟掉(I-346)後,恢復時最多只剩一個「已經過了、照常記上」的晚到中斷,往前多算一個就夠 */
uint64_t timer_wait_base(void);
uint64_t timer_deadline_ms(unsigned ms);
int      timer_expired(uint64_t deadline);
#endif
