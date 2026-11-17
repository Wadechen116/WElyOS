#ifndef WELYOS_SPINLOCK_H
#define WELYOS_SPINLOCK_H

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
 */
struct spinlock {
    volatile unsigned locked;    /* 0 = 空閒,1 = 已被持有 */
    int cpu;                     /* 持有者的 CPU 編號;-1 = 無 */
    const char *name;            /* 診斷訊息用 */
};
#define SPINLOCK_INIT(nm) { 0, -1, nm }

/* 關中斷並上鎖;回傳值交給 spin_unlock_irqrestore,用來恢復「原本的」中斷狀態 */
unsigned long spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, unsigned long flags);

/* 不處理中斷狀態的版本:呼叫時中斷必須已經關閉 */
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* 這把鎖是否正由「目前這顆 CPU」持有(斷言用;呼叫時中斷須已關閉) */
int spin_held(struct spinlock *l);

/* panic 開始時呼叫:之後所有 spinlock 操作都直接放行。
   panic 要印訊息,而出事的當下可能正持有主控台或其他的鎖,不放行就印不出來 */
void spin_panic_mode(void);
#endif
