#ifndef WELYOS_TIMER_H
#define WELYOS_TIMER_H
#include <stdint.h>

void timer_init(unsigned hz);   /* 啟動週期性計時器中斷(每秒 hz 次) */
void timer_tick(void);          /* 由架構層的中斷處理程式在每次計時器中斷時呼叫 */
uint64_t timer_ticks(void);     /* 開機至今的 tick 數 */
unsigned timer_hz(void);        /* 每秒的 tick 數 */
#endif
