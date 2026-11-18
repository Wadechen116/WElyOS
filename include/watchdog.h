#ifndef WELYOS_WATCHDOG_H
#define WELYOS_WATCHDOG_H
/* 看門狗(說明見 kernel/watchdog.c) */
void watchdog_tick(void);                    /* 每顆 CPU 的每個計時器中斷呼叫一次 */
void watchdog_set_timeout(unsigned ms);      /* 多久沒有心跳算卡死(預設 5000) */
#endif
