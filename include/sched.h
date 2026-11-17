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
   需先完成 pmm_init() 與 timer_init()(時間片要依 tick 頻率換算);
   之後才能呼叫本檔其他函式 */
void sched_init(void);

/* 建立一條執行緒,從 fn(arg) 開始執行;fn 返回即結束。
   回傳執行緒編號(tid,大於 0 且不重複使用);失敗回傳 -1。
   不回傳指標的原因:執行緒結束後結構就被釋放,留著指標會變成懸空指標 */
int thread_create(const char *name, void (*fn)(void *), void *arg);

/* 等待編號為 tid 的執行緒結束。若它已經結束(或不存在)則立即返回。
   等待期間本執行緒不佔 CPU,由對方結束時直接喚醒 */
void thread_join(int tid);

void thread_yield(void);                 /* 主動讓出 CPU,排到隊伍最後 */
void thread_sleep(unsigned ms);          /* 休眠至少 ms 毫秒(精度為一個 tick) */
void thread_exit(void) __attribute__((noreturn));   /* 結束目前的執行緒 */

struct thread *thread_current(void);     /* 目前的執行緒(只用來比較是否為同一條) */
const char *thread_name(void);           /* 目前執行緒的名稱 */
int thread_id(void);                     /* 目前執行緒的編號(main 為 0) */

unsigned sched_thread_count(void);       /* 存活的執行緒數(含 main,不含 idle) */
unsigned long sched_switch_count(void);  /* 累計的 context switch 次數 */

/*
 * 等待佇列(wait queue):「在等某件事發生」的執行緒排隊的地方。
 * 這是所有阻塞式操作(semaphore、mutex、join,以及之後的 I/O)的共同基礎:
 *   等待的一方:把自己掛進佇列並讓出 CPU,狀態變成 BLOCKED,不再被排程
 *   事件發生時:另一方從佇列取出等待者,放回執行佇列
 * 和 thread_sleep 輪詢相比,沒事不會醒來,事情發生時立刻被叫醒。
 */
struct wait_queue {
    struct thread *head, *tail;          /* 先進先出:等最久的先被喚醒 */
};
#define WAIT_QUEUE_INIT { 0, 0 }

/*
 * 讓目前的執行緒睡在 wq 上,直到被 wq_wake_* 喚醒才返回。
 * 呼叫前「必須已關閉中斷」,而且「檢查條件」與「呼叫 wq_sleep」之間不可開中斷,
 * 否則條件可能在兩者之間改變、喚醒先發生,之後就再也沒人叫醒(lost wakeup)。
 * 返回時中斷仍是關閉的。
 */
void wq_sleep(struct wait_queue *wq);

/* 喚醒 wq 上等最久的一條 / 全部。回傳喚醒的數量。可在中斷處理程式中呼叫 */
int wq_wake_one(struct wait_queue *wq);
int wq_wake_all(struct wait_queue *wq);

/* ---- 以下由中斷處理路徑呼叫 ---- */

/* 每個計時器 tick 呼叫一次(中斷關閉中):喚醒到期的休眠者、扣時間片 */
void sched_tick(void);

/* 架構層在中斷處理的最後(已通知中斷控制器之後)呼叫:
   若時間片用完或有人被喚醒,就在這裡切換到別的執行緒(搶佔) */
void sched_irq_exit(void);

/* 目前執行緒的 stack 範圍 [lo, hi),backtrace 用來驗證 frame pointer */
void sched_stack_bounds(uintptr_t *lo, uintptr_t *hi);

/*
 * 目前執行緒 stack 的最低可用位址。RISC-V 與 AArch64 的例外入口(組合語言)
 * 會讀這個變數,在存暫存器之前先檢查 stack pointer 是否已經超出範圍。
 * 排程器在每次切換時更新。0 表示尚未啟用檢查。
 */
extern uintptr_t kstack_limit;

/*
 * 例外處理程式在致命錯誤時呼叫:依出錯當下的 stack pointer 與出錯位址,
 * 判斷是否為 kernel stack 溢位,是的話印出說明並回傳 1。
 */
int sched_report_overflow(uintptr_t sp, uintptr_t fault_addr);

/* 釋放快取中閒置的執行緒 stack(記憶體不足時,或測試要核對記憶體用量時呼叫) */
void sched_trim(void);
#endif
