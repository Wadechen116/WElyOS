#ifndef WELYOS_TIMER_H
#define WELYOS_TIMER_H
#include <stdint.h>

void timer_init(unsigned hz);   /* 啟動週期性計時器中斷(每秒 hz 次) */
void timer_tick(void);          /* 由架構層的中斷處理程式在每次計時器中斷時呼叫 */
uint64_t timer_ticks(void);     /* 開機至今的 tick 數 */
unsigned timer_hz(void);        /* 每秒的 tick 數 */

/* 實際的日期時間。開機時向硬體的即時時鐘問一次「現在幾點」,之後靠 tick 往前推算。
   回傳 1970-01-01 00:00:00 UTC 至今的秒數(Unix 時間);*usec 得到秒以下的微秒數(可為 NULL)。
   硬體沒有時鐘時,從 0(1970 年)開始算 */
void     timer_clock_init(void);
uint64_t timer_epoch(uint32_t *usec);
#endif
