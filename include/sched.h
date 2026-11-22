#ifndef WELYOS_SCHED_H
#define WELYOS_SCHED_H
#include <stddef.h>
#include "uapi/proc.h"        /* struct rt_stats */
#include <stdint.h>

/*
 * 核心執行緒(kernel thread)與排程器
 *
 * 執行緒 = 一條獨立的執行流程,有自己的 stack 與暫存器狀態,
 * 但與其他執行緒共用同一份 kernel 記憶體(全域變數、heap)。
 * 排程器決定「現在輪到誰用 CPU」:優先權高的先執行,相同優先權之間輪流(每人一個時間片)。
 */
struct thread;

/*
 * 優先權:0(最低)~ 7(最高),數字越大越優先。排程規則:
 *   - 永遠執行「可執行的執行緒中優先權最高」的那一條
 *   - 相同優先權之間輪流(round-robin),每次一個時間片
 *   - 較高優先權的執行緒一變成可執行(被喚醒、被建立),立刻搶佔目前的執行緒
 * 這是「固定優先權搶佔式」排程,另外有兩個機制會暫時調整「有效優先權」:
 *   - 防飢餓(老化):可執行卻等了超過 STARVE_MS 都輪不到的執行緒,
 *     暫時提升到最高優先權執行一個時間片,之後回到原本的優先權。
 *     所以即使有高優先權的執行緒一直不睡,較低的仍能緩慢前進,不會完全停住。
 *   - 優先權繼承:見 sync.h 的 mutex。
 */
#define PRIO_MIN     0
#define PRIO_LOW     2
#define PRIO_NORMAL  4      /* thread_create 的預設值 */
#define PRIO_HIGH    6
#define PRIO_MAX     7
#define PRIO_RT      8      /* 8 ~ 15:只給週期性的即時工作使用(見 thread_set_periodic);不能直接指定。 */
#define PRIO_RT_MAX  15     /*          週期越短的排在越高的層級(rate-monotonic) */
#define PRIO_LEVELS  16

/* 初始化排程器:把目前正在跑的開機流程登記為第一條執行緒(main)。
   需先完成 pmm_init() 與 timer_init()(時間片要依 tick 頻率換算);
   之後才能呼叫本檔其他函式 */
void sched_init(void);

/* 建立一條執行緒,從 fn(arg) 開始執行;fn 返回即結束。
   回傳執行緒編號(tid,大於 0 且不重複使用);失敗回傳 -1。
   不回傳指標的原因:執行緒結束後結構就被釋放,留著指標會變成懸空指標 */
int thread_create(const char *name, void (*fn)(void *), void *arg);

/* 同 thread_create,但指定優先權(PRIO_MIN ~ PRIO_MAX) */
int thread_create_prio(const char *name, void (*fn)(void *), void *arg, int prio);
/*
 * 先準備、後啟動(I-240 C3.2 (b2)):thread_prepare 配置 kernel stack 與結構,dom 不為 NULL 時
 * 記 KOBJ 配額並持有 dom 的參考(到 thread_free 才退回);不排入執行、還沒有編號。
 * 之後只能呼叫一次 thread_start(排入佇列、回傳編號,不會失敗)或 thread_abandon(全部退回)。
 * 用途:把「可能失敗的配置」放在公布結果(例如 fork 公布子行程編號)之前。
 */
struct mb_domain;
struct thread *thread_prepare(const char *name, void (*fn)(void *), void *arg, int prio,
                              struct mb_domain *dom);
int  thread_start(struct thread *t);
void thread_abandon(struct thread *t);
struct mb_domain *thread_domain(void);   /* 目前執行緒記帳的 domain;kernel 執行緒為 NULL */
/* 目前執行緒的擁有者標記(結構從 pool 取得時清為 0 = 沒有);只讀寫自己的 */
void thread_set_tag(unsigned long tag);
unsigned long thread_tag(void);

/* 設定 / 取得目前執行緒的優先權。調低後若有更優先的執行緒在等,會立刻讓出 CPU */
void thread_set_priority(int prio);
int  thread_priority(void);

/* 目前的執行緒累計使用了多少 CPU 時間(以 tick 計:每個計時器 tick 記在當時正在執行的
   那條執行緒頭上)。這是排程器自己的記帳,和時間片用的是同一個依據 */
unsigned long thread_cpu_ticks(void);

/* 等待編號為 tid 的執行緒結束。若它已經結束(或不存在)則立即返回。
   等待期間本執行緒不佔 CPU,由對方結束時直接喚醒 */
void thread_join(int tid);

/*
 * ---- 週期性的即時工作(見 sched.c「期限排程」) ----
 *
 * thread_set_periodic:宣告目前的執行緒是一個週期性工作 — 每 period_ms 毫秒要做一次事,
 * 每次最多使用 budget_ms 毫秒的 CPU 時間,而且必須在下一個週期開始之前做完(期限 = 週期)。
 * 被接受之後,它在自己的預算之內優先於所有一般的執行緒。
 *   回傳 0 = 接受;-E_INVAL = 參數不合理;-E_BUSY = 接受的話,即時工作合計會佔用太多 CPU
 *   (系統不承諾做不到的事);-E_NOSPC = 即時工作的數量已滿。period_ms 為 0 = 取消。
 *
 * thread_wait_period:這一次的工作做完了,睡到下一個週期開始。
 *   回傳 0;被終止或有訊號時回傳 -E_INTR。st 不是 NULL 時填入累計的統計。
 *
 * 沒做到的事情不會被隱藏,而是被計數:
 *   misses   錯過期限 — 新的週期開始時,上一次的工作還沒做完
 *   overruns 超出預算 — 那一次剩下的時間被降回一般的優先權,不讓它拖累別人
 */
int thread_set_periodic(unsigned period_ms, unsigned budget_ms);
int thread_wait_period(struct rt_stats *st);

size_t sched_stack_cache_pages(void);    /* 閒置 stack 快取佔住的實體頁數 */
void thread_yield(void);                 /* 主動讓出 CPU,排到隊伍最後 */
void thread_sleep(unsigned ms);          /* 休眠至少 ms 毫秒(精度為一個 tick) */
void thread_exit(void) __attribute__((noreturn));   /* 結束目前的執行緒 */

struct thread *thread_current(void);     /* 目前的執行緒(只用來比較是否為同一條) */
const char *thread_name(void);           /* 目前執行緒的名稱 */
int thread_id(void);                     /* 目前執行緒的編號(main 為 0) */

unsigned sched_thread_count(void);       /* 存活的執行緒數(含 main,不含 idle) */
unsigned long sched_switch_count(void);  /* 累計的 context switch 次數 */
unsigned long sched_starve_boosts(void); /* 累計的防飢餓提升次數 */
void sched_trace_dump(int cpu, unsigned last); /* 印出第 cpu 顆 CPU 最近 last 次切換(測試失敗時用) */
int sched_trace_dump_all(unsigned last);       /* 印出所有 CPU 的紀錄;回傳有紀錄的 CPU 數 */

/*
 * 行程支援(kernel/proc.c 使用):
 *   space:這條執行緒使用的位址空間。排程器切換到它時會一併換頁表。
 *          NULL 表示純 kernel 執行緒,沿用當下載入的頁表(kernel 部分各處相同)。
 *   data :附掛的私有資料(行程用來記錄這條執行緒屬於哪個行程)。
 *   kill :把所有 data 相同的執行緒標記為「應終止」(呼叫時必須持有排程器的鎖)。被標記的執行緒不會被強行中斷,
 *          而是在下一次要從 kernel 返回使用者模式時自行結束(見 user_return_check)。
 */
struct vm_space;
void  thread_set_space(struct vm_space *vs);     /* 設定目前執行緒的位址空間並立即切換 */
void  thread_set_data(void *data);
void *thread_data(void);
int   thread_killed(void);
void  sched_kill_group(void *data);
/* 訊號處理函式用:sched_interrupt_group 讓行程的每條執行緒放下正在等的事情(睡眠、等輸入、
   等管線、等子行程),thread_interrupted() 在那些等待的迴圈裡用來判斷該不該放棄
   (被終止時也是真),thread_clear_interrupt() 在訊號處理完之後清掉標記 */
void  sched_interrupt_group(void *data);
int   thread_interrupted(void);
void  thread_clear_interrupt(void);
void  sched_group_usage(void *data, unsigned long *ticks, unsigned *runnable);

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
 * 排程器的鎖。等待佇列以及建立在它上面的所有東西(semaphore、mutex、futex、
 * 行程的結束狀態)都由這一把鎖保護,用法固定是:
 *
 *     unsigned long flags = sched_lock();
 *     while (條件不成立)
 *         wq_sleep(&佇列);          // 睡著期間鎖會被釋放,醒來時又是持有的
 *     ...
 *     sched_unlock(flags);
 *
 * sched_lock 同時關閉中斷;sched_unlock 恢復原本的中斷狀態。
 * 持有期間不可再呼叫 sched_lock(同一顆 CPU 重複上鎖會 panic)。
 */
unsigned long sched_lock(void);
void sched_unlock(unsigned long flags);

/*
 * 讓目前的執行緒睡在 wq 上,直到被 wq_wake_* 喚醒才返回。
 * 呼叫前「必須持有排程器的鎖」,而且「檢查條件」與「呼叫 wq_sleep」之間不可解鎖,
 * 否則條件可能在兩者之間改變、喚醒先發生,之後就再也沒人叫醒(lost wakeup)。
 * 返回時仍持有鎖。
 */
void wq_sleep(struct wait_queue *wq);

/* 喚醒 wq 上優先權最高的一條(相同則等最久的優先)/ 全部。回傳喚醒的數量。
   呼叫前必須持有排程器的鎖;可在中斷處理程式中呼叫。被喚醒者若比目前的執行緒優先,只會「記下需要重新排程」,
   實際的切換發生在呼叫 sched_preempt_point() 或中斷返回時 */
int wq_wake_one(struct wait_queue *wq);
int wq_wake_all(struct wait_queue *wq);

/*
 * 搶佔點:若有更優先的執行緒在等(剛被喚醒或建立),立刻切換過去。
 * 在喚醒別人之後、解鎖之前呼叫;呼叫時必須持有排程器的鎖。
 * 在中斷處理程式中呼叫則無作用(切換留到中斷返回時由 sched_irq_exit 處理)。
 */
void sched_preempt_point(void);

/*
 * 優先權繼承用(kernel/sync.c 的 mutex 使用;呼叫時必須持有排程器的鎖):
 *   thread_effective_prio(t) :t 目前的有效優先權
 *   sched_set_inherit(t, p)  :設定 t「繼承來的優先權」,-1 表示沒有。
 *                              有效優先權 = max(自己的, 繼承來的)
 *   thread_blocked_on(t) / thread_set_blocked_on(m)
 *                            :t 正在等哪一把 mutex(沒有則為 NULL),用來沿著
 *                              「A 等 B、B 等 C」的鏈往下傳遞優先權
 *   thread_held_list(t)      :t 持有的 mutex 串列的頭(由 sync.c 維護)
 *   wq_max_prio(wq)          :等待佇列上最高的有效優先權;沒有人在等回傳 -1
 */
int   thread_effective_prio(struct thread *t);
void  sched_set_inherit(struct thread *t, int prio);
void *thread_blocked_on(struct thread *t);
void  thread_set_blocked_on(void *m);
void **thread_held_list(struct thread *t);
int   wq_max_prio(struct wait_queue *wq);

/* ---- 多核心 ---- */
void sched_init_ap(void);                /* AP 加入排程時在自己身上呼叫(見 sched.c) */
void sched_set_cpu_count(int n);         /* 設定參與排程的 CPU 數(所有 AP 都登記之後) */
int  sched_cpu_id(void);                 /* 目前執行在哪一顆 CPU 上(只是當下的值) */
void sched_space_dropped(struct vm_space *vs);   /* 位址空間即將釋放:這顆 CPU 若還載入著就換掉 */
void sched_space_flush(struct vm_space *vs);     /* 位址空間的對映大量更改:這顆 CPU 若載入著就清空 TLB */

/* ---- 以下由中斷處理路徑呼叫 ---- */

/* 架構層在中斷處理程式的開頭呼叫:標記「現在在中斷中」 */
void sched_irq_enter(void);

/* 每顆 CPU 的每個計時器 tick 呼叫一次(中斷關閉中):喚醒到期的休眠者、扣時間片 */
void sched_tick(int charge);             /* 計時器中斷時呼叫;charge = 0:補記的 tick(不記 CPU 時間,見 timer.h) */

/* 架構層在中斷處理的最後(已通知中斷控制器之後)呼叫:
   若時間片用完或有人被喚醒,就在這裡切換到別的執行緒(搶佔) */
void sched_irq_exit(void);

/* 目前執行緒的 stack 範圍 [lo, hi),backtrace 用來驗證 frame pointer */
void sched_stack_bounds(uintptr_t *lo, uintptr_t *hi);

/*
 * 例外處理程式在致命錯誤時呼叫:依出錯當下的 stack pointer 與出錯位址,
 * 判斷是否為 kernel stack 溢位,是的話印出說明並回傳 1。
 */
int sched_report_overflow(uintptr_t sp, uintptr_t fault_addr);

/* struct thread 的池子累計向 heap 取得的位元組數。執行緒結束後結構留在池子裡
   重複使用、不還給 heap(原因見 sched.c),測試核對 heap 用量時要把這部分算進去 */
size_t sched_pool_bytes(void);
unsigned sched_unfreed_threads(void);   /* 存活 + 已結束未回收(測試用:確認結束的執行緒都已回收) */

/* 釋放快取中閒置的執行緒 stack(記憶體不足時,或測試要核對記憶體用量時呼叫) */
void sched_trim(void);
#endif
