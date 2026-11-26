#ifndef WELYOS_SPINLOCK_H
#define WELYOS_SPINLOCK_H

#include <stdint.h>

/*
 * 自旋鎖(spinlock):多核心之下保護共用資料的最基本工具。
 *
 * 為什麼需要:單一 CPU 時,「關中斷」就能保證一小段程式不被打斷,
 * 因為唯一會打斷它的就是中斷。多顆 CPU 時,關中斷只管得到「自己這顆」,
 * 別顆 CPU 仍然同時在執行,可能正在修改同一份資料。
 *
 * 做法:鎖是一個整數,0 = 沒人持有,1 = 有人持有。上鎖時用一條「原子」指令
 * 把它換成 1 並取回舊值:舊值是 0 代表搶到了;是 1 代表別人持有,
 * 就原地反覆重試(自旋)直到對方釋放。
 *
 * 規則(違反時會直接 panic,而不是悄悄卡死):
 *   1. 持有 spinlock 期間中斷必須關閉。否則中斷處理程式若也要拿同一把鎖,
 *      就會永遠等待被它打斷的那段程式,也就是自己。
 *      所以平常都用 spin_lock_irqsave / spin_unlock_irqrestore 這一組。
 *   2. 同一顆 CPU 不可重複取得同一把鎖(同樣會永遠等自己)。
 *   3. 持有期間要短,而且不可以睡覺(不可切換到別的執行緒)。
 *      會等很久的情況應該用 mutex(見 sync.h):等不到就讓出 CPU。
 *   4. 需要同時持有多把鎖時,一律依固定的順序取得(見 docs/DEVLOG.md 第 16 步),
 *      否則兩顆 CPU 以相反順序各拿到一把,就互相等待(死結)。
 *      第 50 步起由執行期強制:見下面的層級表。
 *
 * ---- 鎖的層級(lock hierarchy)----
 * 每一把 spinlock 都有一個層級。持有某些鎖時,只能再拿「層級比它們都高」的鎖;
 * 違反時立刻 panic,印出兩把鎖的名稱與層級。所以任何「兩條路徑以相反順序拿兩把鎖」
 * 的可能,第一次走到其中一條路徑時就被抓到,不必等到兩條路徑真的同時發生、卡死。
 * (航太軟體的做法:鎖的順序是設計的一部分,集中寫在一個地方,由程式檢查,不靠記憶。)
 * spin_trylock 不檢查順序(拿不到就回來,不會等待,不會形成死結),但一樣記入持有中。
 *
 * 層級的依據是實際量到的巢狀關係(第 50 步:所有自我測試在三個架構上跑過,
 * 記錄每一組「持有 A 時拿 B」),整理成由外到內:
 */
enum lock_rank {
    LK_TEST_OUTER = 10,          /* 自我測試的外層鎖(test-a;crash=* 的測試鎖) */
    LK_TEST_INNER = 11,          /* 自我測試的內層鎖(test-b) */
    LK_PSTATE     = 20,          /* 行程的狀態(proc.c 的 pstate_lk) */
    LK_INODE      = 30,          /* inode 的參考計數(vfs.c);pstate 之內唯一會拿的鎖 */
    LK_TIME       = 35,          /* 系統時間的推進權(timer.c 的 time_lk;只用 trylock 取得,之內會拿 sched_lk) */
    LK_SCHED      = 40,          /* 排程器(sched.c 的 sched_lk) */
    LK_RUNQ       = 45,          /* 執行佇列(runq-overflow / runq;只在排程器的鎖之內使用) */
    LK_STACKS     = 50,          /* 執行緒 stack 的快取 */
    LK_VM         = 60,          /* 頁表(vm.c 的 vm_lk) */
    LK_SMP_CALL   = 70,          /* 跨 CPU 呼叫(TLB shootdown 在 vm_lk 之內發出) */
    LK_SMP_TEST   = 71,
    LK_MB         = 75,          /* 記憶體配額(membudget.c);vm_lk 之內取得,之內只呼叫 pmm 的純記帳函式 */
    LK_HEAP       = 80,          /* kmalloc */
    LK_PMM        = 90,          /* 實體頁配置器 */
    LK_TSTAT      = 100,         /* 計時器的統計 */
    LK_CONSOLE    = 110,         /* 主控台:任何地方都可能印訊息,所以在最內層 */
};

struct spinlock {
    volatile unsigned locked;    /* 0 = 空閒,1 = 已被持有 */
    int cpu;                     /* 持有者的 CPU 編號;-1 = 無 */
    const char *name;            /* 診斷訊息用 */
    unsigned rank;               /* 層級(enum lock_rank) */
    uint8_t probe_id;            /* 鎖持有量測(I-349)的編號;0 = 不量測 */
};
#define SPINLOCK_INIT(nm, rk) { 0, -1, nm, rk, 0 }
/* 要量測持有時間的鎖:id 由 lockhold 後端定義(見 kernel/lockhold.c) */
#define SPINLOCK_INIT_PROBED(nm, rk, id) { 0, -1, nm, rk, id }
#define LOCK_DEPTH_MAX 8         /* 一顆 CPU 同時最多持有幾把 spinlock */

/* 量測模式(只在找出巢狀關係時暫時打開):違反層級時不 panic,而是記下並由
   spin_order_report() 印出每一組不同的違反。平常是 0 */
#define LOCK_ORDER_LEARN 0
void spin_order_report(void);

/* 關中斷並上鎖;回傳值交給 spin_unlock_irqrestore,用來恢復「原本的」中斷狀態 */
unsigned long spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, unsigned long flags);

/* 不處理中斷狀態的版本:呼叫時中斷必須已經關閉 */
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* 試著上鎖,不等待:拿到回傳 1,被別人持有回傳 0。呼叫時中斷必須已經關閉 */
int spin_trylock(struct spinlock *l);

/* 這把鎖是否正由「目前這顆 CPU」持有(斷言用;呼叫時中斷須已關閉) */
int spin_held(struct spinlock *l);

/* panic 開始時呼叫:之後所有 spinlock 操作都直接放行。
   panic 要印訊息,而出事的當下可能正持有主控台或其他的鎖,不放行就印不出來 */
void spin_panic_mode(void);
#endif
