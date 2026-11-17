#ifndef WELYOS_SCHED_H
#define WELYOS_SCHED_H
#include <stdint.h>

/*
 * 核心執行緒(kernel thread)與排程器
 *
 * 執行緒 = 一條獨立的執行流程,有自己的 stack 與暫存器狀態,
 * 但與其他執行緒共用同一份 kernel 記憶體(全域變數、heap)。
 * 排程器決定「現在輪到誰用 CPU」,採 round-robin(輪流,每人一個時間片)。
 */
struct thread;

/* 初始化排程器:把目前正在跑的開機流程登記為第一條執行緒(main)。
   需先完成 pmm_init();之後才能呼叫本檔其他函式 */
void sched_init(void);

/* 建立一條執行緒,從 fn(arg) 開始執行;fn 返回即結束。失敗回傳 NULL */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

void thread_yield(void);                 /* 主動讓出 CPU,排到隊伍最後 */
void thread_sleep(unsigned ms);          /* 休眠至少 ms 毫秒(精度為一個 tick) */
void thread_exit(void) __attribute__((noreturn));   /* 結束目前的執行緒 */

const char *thread_name(void);           /* 目前執行緒的名稱 */
int thread_id(void);                     /* 目前執行緒的編號(main 為 0) */

unsigned sched_thread_count(void);       /* 存活的執行緒數(含 main,不含 idle) */
unsigned long sched_switch_count(void);  /* 累計的 context switch 次數 */

/* ---- 以下由中斷處理路徑呼叫 ---- */

/* 每個計時器 tick 呼叫一次(中斷關閉中):喚醒到期的休眠者、扣時間片 */
void sched_tick(void);

/* 架構層在中斷處理的最後(已通知中斷控制器之後)呼叫:
   若時間片用完或有人被喚醒,就在這裡切換到別的執行緒(搶佔) */
void sched_irq_exit(void);

/* 目前執行緒的 stack 範圍 [lo, hi),backtrace 用來驗證 frame pointer */
void sched_stack_bounds(uintptr_t *lo, uintptr_t *hi);
#endif
