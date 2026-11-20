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
 * 另外支援優先權繼承:高優先權的執行緒在等鎖時,持有者暫時繼承它的優先權。
 *
 * ---- mutex 的層級(第 50 步)----
 * 和 spinlock 一樣(見 spinlock.h):每個 mutex 有名稱與層級,持有某些 mutex 時只能再拿
 * 層級更高的,違反時立刻 panic。mutex 一定在所有 spinlock 之外(持有 spinlock 時不能睡,
 * schedule() 會檢查),所以兩張表各自獨立。沒有初始化(沒有名稱)的 mutex 一上鎖就 panic,
 * 確保每一個都被分類。
 * 同一類可以巢狀的只有 inode:目錄由上而下鎖(上層目錄 -> 其中的項目),樹狀的順序本身
 * 不會形成循環;唯一會同時鎖兩個不相干目錄的 rename 先拿全系統一把 rename_mx 序列化。
 * 依據是實際量到的巢狀關係(三個架構的所有自我測試、磁碟與網路測試),由外到內:
 */
enum mutex_rank {
    MX_TEST       = 10,          /* 自我測試(單獨使用) */
    MX_TEST_PI_B  = 11,          /* 優先權繼承的連鎖測試:B -> A -> C */
    MX_TEST_PI_A  = 12,
    MX_TEST_PI_C  = 13,
    MX_PROC_MEM   = 20,          /* 行程的記憶體對映(proc_mem.c 的 mem.mx) */
    MX_RENAME     = 30,          /* rename 的全系統序列化 */
    MX_FILE       = 40,          /* 開啟中的檔案(位置) */
    MX_INODE      = 50,          /* inode 的內容(可同類巢狀,見上) */
    MX_WFS        = 60,          /* 磁碟檔案系統 */
    MX_NET        = 65,          /* 網路堆疊 */
    MX_BLACKBOX   = 70,          /* 黑盒子紀錄 */
    MX_DEVICE     = 80,          /* 裝置驅動程式(磁碟、網路卡):最內層 */
};

struct mutex {
    struct thread *owner;                /* NULL = 沒有人持有 */
    int inherit;                         /* 1 = 啟用優先權繼承(預設)。0 僅供示範反轉現象 */
    struct wait_queue wq;
    struct mutex *next_held;             /* 串在擁有者「持有的 mutex」串列中 */
    const char *name;                    /* 診斷訊息用;NULL = 沒有初始化 */
    unsigned rank;                       /* 層級(enum mutex_rank) */
};
#define MUTEX_INIT(nm, rk) { 0, 1, WAIT_QUEUE_INIT, 0, nm, rk }

void mutex_init(struct mutex *m, const char *name, unsigned rank);
void mutex_order_report(void);           /* 量測模式(LOCK_ORDER_LEARN)時印出巢狀關係 */
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
#endif
