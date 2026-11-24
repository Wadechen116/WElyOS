#ifndef WELYOS_WATCHDOG_H
#define WELYOS_WATCHDOG_H
/* 看門狗(說明見 kernel/watchdog.c) */
void watchdog_tick(void);                    /* 每顆 CPU 的每個計時器中斷呼叫一次 */
void watchdog_set_timeout(unsigned ms);      /* 多久沒有心跳算卡死(預設 5000) */
/* 階段看門狗(I-350,只給自我測試用):這個階段必須在 secs 秒內結束,以高精度時鐘量(不靠系統 tick,
   降級時 tick 會落後)。逾時就印出排程器狀態(sched_dump_state)後 panic。name = NULL 清除。
   沒有高精度時鐘時不作用。progress(可為 NULL)= 這個階段的進度計數,逾時時一起印出,
   用來分辨「很慢但在前進」與「完全停住」 */
void watchdog_stage(const char *name, unsigned secs, const volatile unsigned *progress);
#endif
