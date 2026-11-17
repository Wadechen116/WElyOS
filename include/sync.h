#ifndef WELYOS_SYNC_H
#define WELYOS_SYNC_H
#include "sched.h"

/*
 * 同步機制:讓多條執行緒安全地共用資料、互相等待。
 * 兩者都建立在等待佇列(sched.h)之上:拿不到就睡,對方釋放時直接喚醒。
 * 都「不可」在中斷處理程式中呼叫會阻塞的那一半(sem_wait / mutex_lock)。
 */

/*
 * 號誌(semaphore):一個不會小於 0 的計數器。
 *   sem_wait:計數 > 0 就減 1 並繼續;等於 0 就睡到有人 sem_post
 *   sem_post:計數加 1,並喚醒一條等待者
 * 用途:計算「還有幾個資源」(緩衝區的空位、待處理的項目),或通知「某件事完成了」。
 * 任何執行緒都可以 post,不一定要是先前 wait 的那一條。
 */
struct semaphore {
    int count;
    struct wait_queue wq;
};
#define SEMAPHORE_INIT(n) { (n), WAIT_QUEUE_INIT }

void sem_init(struct semaphore *s, int count);
void sem_wait(struct semaphore *s);
int  sem_trywait(struct semaphore *s);   /* 不等待:成功回傳 1,計數為 0 回傳 0 */
void sem_post(struct semaphore *s);      /* 可在中斷處理程式中呼叫 */

/*
 * 互斥鎖(mutex):一次只允許一條執行緒進入的區段(臨界區段)。
 * 和「初值為 1 的號誌」的差別在於它記得擁有者:
 *   - 只有上鎖的那一條可以解鎖
 *   - 同一條重複上鎖會被偵測出來(否則會自己等自己,永遠卡住)
 * 兩種錯誤都會 panic。
 */
struct mutex {
    struct thread *owner;                /* NULL = 沒有人持有 */
    struct wait_queue wq;
};
#define MUTEX_INIT { 0, WAIT_QUEUE_INIT }

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
#endif
