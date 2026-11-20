#ifndef WELYOS_TIMER_H
#define WELYOS_TIMER_H
#include <stdint.h>

void timer_init(unsigned hz);   /* 啟動週期性計時器中斷(每秒 hz 次) */
void timer_tick(void);          /* 由架構層的中斷處理程式在每次計時器中斷時呼叫 */
/* 中斷晚到、一次補記好幾個 tick 時,第一個以外的用這個:時間照樣往前(喚醒休眠者、檢查期限),
   但不算成任何執行緒用掉的 CPU 時間 — 那段時間這顆 CPU 是被停住的,沒有人在執行 */
void timer_tick_stolen(void);
uint64_t timer_ticks(void);     /* 開機至今的 tick 數 */
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
};
void timer_note_catchup(int n, int resynced);   /* 架構層:這次中斷補記了 n 個 tick */
void timer_get_stats(struct timer_stats *st);
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
 * 以真實時間為準的 tick(第 51 步)。timer_ticks() 只由第 0 顆 CPU 的計時器中斷推進,
 * 那顆 CPU 的中斷晚到時會暫時落後真實時間;拿它當「等多久」的起點,真正等到的時間就會不足。
 *   timer_ticks_real()     現在「應該」是第幾個 tick:timer_ticks() 加上已經到期、還沒記的 tick
 *                          (由高精度時鐘與第 0 顆 CPU 下一個 tick 的預定時間算出)。永遠 >= timer_ticks()
 *   timer_deadline_ms(ms)  「至少 ms 毫秒之後」對應的 tick;ms 為 0 時回傳 0(立刻到期)
 *   timer_expired(dl)      timer_ticks() >= dl。因為 timer_ticks() 不會超前真實時間,
 *                          到期時真實經過的時間一定 >= 要求的時間
 * 架構層沒有高精度時鐘時,timer_ticks_real() 等於 timer_ticks()(無從得知落後多少)。
 */
uint64_t timer_ticks_real(void);
uint64_t timer_deadline_ms(unsigned ms);
int      timer_expired(uint64_t deadline);
#endif
