/*
 * proc_mem.c - 行程的記憶體:page fault、和使用者空間之間的複製、記憶體配額、mmap / sbrk
 *
 * 行程管理內部共用的定義(struct proc 等)在 proc_internal.h;對外的介面在 include/proc.h。
 */
#define PROC_OWNER_MEM              /* 這個檔案是這一組狀態的擁有者(見 proc_internal.h) */
#include "proc_internal.h"

/* ------------------------------------------------------------------ */
/* 系統呼叫                                                            */
/*                                                                    */
/* 這是 kernel 的對外邊界:參數全部來自不可信任的使用者程式,            */
/* 每一個都要檢查。尤其是指標,使用者可以傳任何數值,包括 kernel 內部的  */
/* 位址;kernel 若不檢查就照著讀寫,等於替使用者突破了保護。            */
/* ------------------------------------------------------------------ */

/*
 * 處理一個「也許可以解決」的 page fault。回傳 0 表示已經解決,重試那次存取就會成功。
 * 能解決的有兩種:
 *
 * 1. 寫入時複製:寫入一個 fork 之後和別的行程共用的頁(見 vm_space_fault)。
 *
 * 2. stack 自動成長:每條執行緒的 stack 佔一格 1MB 的位址範圍,但一開始只配置
 *    最上面的 16KB。往下用到還沒配置的部分時,當場配置一頁(內容為 0)。
 *    大多數程式用不到 16KB,不必為它們預先配置;用得多的程式也不會因此被終止。
 *    每格最下面的一頁永遠不配置(保護頁):stack 真的用完整格時會在那裡出錯,
 *    而不是悄悄長進下一條執行緒的 stack。
 *
 * 其他情況(存取沒有對映的位址、寫入唯讀的頁)回傳 -1,呼叫者會終止行程。
 */
/*
 * ---- 對映的變更要序列化 ----
 *
 * 「判斷這個位址屬於某個區域 → 配置一頁 → 建立對映」必須和「把區域拿掉 → 解除對映」互斥。
 * 否則:fault 判斷完、放鎖去配置的空檔裡,另一條執行緒 munmap 或縮小 heap,完成之後 fault 照樣建立
 * 對映 — 已經歸還的區域又活過來,而且記帳(commit)已經扣掉了(Codex 以確定的交錯證實過)。
 * 縮小 heap 的同時又有人把它長回來,也會讓剛建立的頁被縮小那一方的解除迴圈拿掉。
 *
 * 所以每個行程有一把 mem.mx(可以睡眠的 mutex):proc_fault 的配置路徑、sys_munmap、sys_sbrk
 * 都整段持有它。用 mutex 而不是 spinlock:解除一大片對映可能很久,不能關著中斷做(會傷害即時工作)。
 * 鎖的順序:mem.mx → pstate_lk → vm_lk(vm_space_* 內部)→ 頁面配置器。持有 mem.mx 時不呼叫
 * copy_from_user / copy_to_user(它們可能需要 fault,會再拿 mem.mx)。
 */
static int proc_fault_locked(struct proc *p, uintptr_t addr, int write);

static int proc_fault(struct proc *p, uintptr_t addr, int write)
{
    if (write && vm_space_fault(p->space, addr, 1) == 0)
        return 0;                                /* 寫入時複製:只動自己的頁表項目,vm_lk 就夠了 */
    mutex_lock(&p->mem.mx);
    int rc = proc_fault_locked(p, addr, write);
    mutex_unlock(&p->mem.mx);
    return rc;
}

static int proc_fault_locked(struct proc *p, uintptr_t addr, int write)
{
    (void)write;

    /* heap:落在 [USER_HEAP_BASE, brk) 而還沒配置的頁,當場配置(內容為 0)。
       sbrk 只是移動結尾,不配置任何東西;程式要了很大一塊卻只用一點點時,只花用到的部分 */
    if (addr >= USER_HEAP_BASE && addr < USER_HEAP_BASE + USER_HEAP_MAX) {
        unsigned long fl = spin_lock_irqsave(&pstate_lk);
        uintptr_t end = p->mem.brk;
        spin_unlock_irqrestore(&pstate_lk, fl);
        if (addr >= end)
            return -1;                           /* 超出目前的結尾:真正的錯誤 */
        void *hp = pmm_alloc();
        if (!hp)
            return -1;                           /* 記憶體用完:這個行程會被終止 */
        memset(hp, 0, PAGE_SIZE);
        int hrc = vm_space_map_new(p->space, addr & ~(uintptr_t)(PAGE_SIZE - 1), (uintptr_t)hp,
                                   VM_READ | VM_WRITE);
        if (hrc != 0)
            pmm_free(hp);
        return hrc < 0 ? -1 : 0;
    }

    /* 匿名對映:落在某一塊 mmap 的範圍內而還沒配置的頁,同樣當場配置 */
    if (addr >= USER_MMAP_BASE && addr < USER_MMAP_BASE + USER_MMAP_MAX) {
        int inside = 0;
        unsigned long fl = spin_lock_irqsave(&pstate_lk);
        for (unsigned i = 0; i < PROC_MAX_MAPS && !inside; i++)
            inside = p->mem.maps[i].pages && addr >= p->mem.maps[i].start &&
                     addr - p->mem.maps[i].start < p->mem.maps[i].pages * PAGE_SIZE;
        spin_unlock_irqrestore(&pstate_lk, fl);
        if (!inside)
            return -1;
        void *mp = pmm_alloc();
        if (!mp)
            return -1;
        memset(mp, 0, PAGE_SIZE);
        int mrc = vm_space_map_new(p->space, addr & ~(uintptr_t)(PAGE_SIZE - 1), (uintptr_t)mp,
                                   VM_READ | VM_WRITE);
        if (mrc != 0)
            pmm_free(mp);
        return mrc < 0 ? -1 : 0;
    }

    uintptr_t lowest = USER_STACK_TOP - (uintptr_t)PROC_MAX_THREADS * USER_STACK_SLOT;
    if (addr < lowest || addr >= USER_STACK_TOP)
        return -1;
    unsigned slot = (unsigned)((USER_STACK_TOP - 1 - addr) / USER_STACK_SLOT);
    uintptr_t slot_base = USER_STACK_TOP - (uintptr_t)(slot + 1) * USER_STACK_SLOT;
    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    if (slot >= p->nstacks || page == slot_base)     /* 沒有這條 stack,或碰到保護頁 */
        return -1;

    void *mem = pmm_alloc();
    if (!mem)
        return -1;
    memset(mem, 0, PAGE_SIZE);
    int rc = vm_space_map_new(p->space, page, (uintptr_t)mem, VM_READ | VM_WRITE);
    if (rc != 0)
        pmm_free(mem);       /* 1 = 同一行程的另一條執行緒搶先配置了同一頁:用它的 */
    return rc < 0 ? -1 : 0;
}

int proc_page_fault(uintptr_t addr, int write)
{
    struct proc *p = thread_data();

    return p ? proc_fault(p, addr, write) : -1;
}

/*
 * 把使用者位址 [uaddr, uaddr+len) 的內容複製到 kernel 緩衝區 dst。
 * 逐頁透過行程的頁表查出實體位址,並確認該頁是使用者可存取的;
 * 任何一頁不合格就回傳 -1。不直接解參考使用者的指標。
 */
/*
 * 和使用者空間交換資料。每一頁的「查表 + 複製」在 VM 的鎖之內一次完成(vm_space_copy),
 * 所以不會讀寫到一頁「查到之後才被 munmap、釋放、給了別人」的記憶體。
 * 查不到(還沒配置的頁、寫入時複製的唯讀頁)就先處理 fault,再試一次;第二次仍然不行
 * (例如同一時間被另一條執行緒 munmap 了)就回傳 -1(呼叫者回報 E_FAULT)。
 */
static int copy_user(struct proc *p, uintptr_t uaddr, uint8_t *kbuf, size_t len, int to_user)
{
    while (len > 0) {
        size_t n = PAGE_SIZE - (uaddr & (PAGE_SIZE - 1));    /* 這一頁剩下的長度 */
        if (n > len)
            n = len;
        if (vm_space_copy(p->space, uaddr, kbuf, n, to_user) != 0 &&
            (proc_fault(p, uaddr, to_user) != 0 || vm_space_copy(p->space, uaddr, kbuf, n, to_user) != 0))
            return -1;
        kbuf += n;
        uaddr += n;
        len -= n;
    }
    return 0;
}

int copy_from_user(struct proc *p, void *dst, uintptr_t uaddr, size_t len)
{
    return copy_user(p, uaddr, dst, len, 0);
}

int copy_to_user(struct proc *p, uintptr_t uaddr, const void *src, size_t len)
{
    return copy_user(p, uaddr, (uint8_t *)(uintptr_t)src, len, 1);
}

/* 從使用者位址複製一個以 0 結尾的字串(路徑)到 kernel。太長或位址不合法回傳負的錯誤碼 */
int copy_path_from_user(struct proc *p, char *dst, uintptr_t uaddr)
{
    for (size_t i = 0; i <= FS_PATH_MAX; i++) {
        if (copy_from_user(p, &dst[i], uaddr + i, 1) != 0)
            return -E_FAULT;
        if (dst[i] == '\0')
            return 0;
    }
    return -E_NAMETOOLONG;
}

/*
 * ---- 記憶體配額:答應了就一定給得出來 ----
 *
 * heap 與匿名對映的頁是「用到才配置」的。如果 kernel 對每個要求都說好,等到程式真的去用、
 * 實體記憶體卻不夠時,只剩下一個選擇:終止某個行程(第 30 步的做法,稱為過度承諾)。
 * 被終止的程式沒有做錯任何事,也沒有機會收拾;對要求可靠的系統,這是不能接受的行為。
 *
 * 這裡改成記帳:kernel 記著「已經答應出去的頁數」(commit),包括還沒真的配置的。
 * 每個要求在當下就決定成不成功:
 *   - 行程自己的配額(p->mem.limit):一個行程失控,最多用掉它自己的額度
 *   - 整個系統的上限(commit_cap):答應出去的總量不超過實際有的記憶體,
 *     而且扣掉一塊保留給 kernel 自己(頁表、kernel heap、檔案快取…)
 * 超過就回傳 -E_NOMEM:程式在「要」的時候得到明確的答案,可以改做別的事、
 * 釋放其他東西、或把工作有秩序地結束。之後去使用那些已經答應的頁時,不會再失敗。
 *
 * 配額只能調低(memlimit);調高需要 CAP_ADMIN。子行程沿用父行程的配額。
 * fork 會讓子行程得到同樣多的承諾(寫入時複製:最壞情況下每一頁都要另外一份),
 * 所以 fork 也可能因為配額而失敗。
 */
static unsigned long commit_total;           /* 所有行程的 commit 總和;受 pstate_lk 保護 */
static unsigned long commit_cap;             /* 上限;0 = 還沒算(第一次使用時依當時的空閒記憶體決定) */

/* 把 p 的承諾增減 delta 頁。增加時檢查兩個上限,不行就回傳 -E_NOMEM(什麼都不變)。
   呼叫時必須持有 pstate_lk */
static int commit_change_locked(struct proc *p, long delta)
{
    if (!commit_cap) {
        /* 保留給 kernel 的部分:至少 2MB,或全部記憶體的 1/16(取大的) */
        /* 再加上 pmm 的保留底線:承諾出去的頁必須配置得到,而一般配置拿不到底線以下的頁 */
        unsigned long freep = pmm_free_count(), reserve = pmm_total_pages() / 16;
        if (reserve < 512)
            reserve = 512;
        reserve += pmm_reserve_count();
        commit_cap = freep > reserve ? freep - reserve : 1;
    }
    if (delta > 0) {
        unsigned long d = (unsigned long)delta;
        if (d > p->mem.limit || p->mem.commit > p->mem.limit - d ||
            d > commit_cap || commit_total > commit_cap - d)
            return -E_NOMEM;
        p->mem.commit += d;
        commit_total += d;
    } else {
        /* 以無號數取絕對值:delta == LONG_MIN 時 -delta 是未定義行為,0UL - (unsigned long)delta 不是。
           這一支(歸還)永遠回傳 0 — sys_munmap、sbrk 縮小、commit_release_all 依賴這一點
           (第 50 步以主機端測試 probe_commit_release.py 窮舉驗證) */
        unsigned long d = 0UL - (unsigned long)delta;
        if (d > p->mem.commit)
            d = p->mem.commit;
        p->mem.commit -= d;
        commit_total -= d;
    }
    return 0;
}

/* 行程結束或換成另一個程式:它的承諾全部歸還 */
void commit_release_all(struct proc *p)
{
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    commit_change_locked(p, -(long)p->mem.commit);
    spin_unlock_irqrestore(&pstate_lk, fl);
}

static unsigned long pages_for(uintptr_t bytes)
{
    return (unsigned long)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* memlimit(kb):kb 為 0 = 查詢;否則把配額設成 kb。調低永遠允許,調高需要 CAP_ADMIN。回傳設定後的配額(KB) */
long sys_memlimit(struct proc *p, long kb)
{
    if (kb < 0)
        return -E_INVAL;
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    long rc = 0;
    if (kb > 0) {
        unsigned long pages = (unsigned long)kb / (PAGE_SIZE / 1024);
        if (pages > p->mem.limit && !(p->cred.caps & CAP_ADMIN))
            rc = -E_PERM;
        else
            p->mem.limit = pages;
    }
    if (rc == 0)
        rc = (long)(p->mem.limit * (PAGE_SIZE / 1024));
    spin_unlock_irqrestore(&pstate_lk, fl);
    return rc;
}

/*
 * mmap(len):要一塊 len 位元組(進位到整頁)、內容為 0 的記憶體,回傳它的位址。
 * 和 heap 的差別:每一塊各自獨立,可以個別歸還(munmap),不必是「最後要的那一塊」。
 * 頁同樣是用到才配置;承諾在這裡就記帳。只支援「匿名」的對映(不對應任何檔案)。
 * 失敗回傳 -E_INVAL(長度為 0 或太大)、-E_NOMEM(超過配額、塊數已滿、位址範圍用完)。
 */
long sys_mmap(struct proc *p, uintptr_t len)
{
    if (len == 0 || len > USER_MMAP_MAX)
        return -E_INVAL;
    unsigned long pages = pages_for(len);
    long rc = -E_NOMEM;

    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    uintptr_t start = p->mem.mmap_next ? p->mem.mmap_next : USER_MMAP_BASE;
    for (unsigned i = 0; i < PROC_MAX_MAPS; i++) {
        if (p->mem.maps[i].pages)
            continue;
        if (start + pages * PAGE_SIZE <= USER_MMAP_BASE + USER_MMAP_MAX &&
            commit_change_locked(p, (long)pages) == 0) {
            p->mem.maps[i].start = start;
            p->mem.maps[i].pages = pages;
            p->mem.mmap_next = start + pages * PAGE_SIZE + PAGE_SIZE;   /* 兩塊之間留一頁不對映:越界會立刻被發現 */
            rc = (long)start;
        }
        break;
    }
    spin_unlock_irqrestore(&pstate_lk, fl);
    return rc;
}

/* munmap(addr):歸還 mmap 得到的一整塊(addr 必須是 mmap 回傳的位址) */
long sys_munmap(struct proc *p, uintptr_t addr)
{
    unsigned long pages = 0;

    mutex_lock(&p->mem.mx);                       /* 拿掉區域到解除完對映,整段不讓 fault 插進來 */

    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    for (unsigned i = 0; i < PROC_MAX_MAPS; i++) {
        if (p->mem.maps[i].pages && p->mem.maps[i].start == addr) {
            pages = p->mem.maps[i].pages;
            p->mem.maps[i].pages = 0;            /* 先從表格拿掉:之後碰這一塊就是錯誤,不會再被配置 */
            commit_change_locked(p, -(long)pages);
            break;
        }
    }
    spin_unlock_irqrestore(&pstate_lk, fl);
    for (unsigned long i = 0; i < pages; i++)
        vm_space_unmap(p->space, addr + i * PAGE_SIZE);
    mutex_unlock(&p->mem.mx);
    return pages ? 0 : -E_INVAL;
}

/*
 * sbrk(incr):把 heap 的結尾移動 incr 個位元組(可以是負的),回傳「移動之前」的結尾。
 * 使用者端的 malloc 用它向 kernel 要記憶體。
 *   變大:只是把結尾往後移。新的範圍在第一次被碰到時才配置(見 proc_fault),內容保證是 0
 *   變小:結尾之後整頁落在範圍外的,立刻歸還給系統
 * 失敗(超出配額或系統的上限、或縮到起點之前)回傳 -E_NOMEM,結尾不變。
 */
long sys_sbrk(struct proc *p, long incr)
{
    mutex_lock(&p->mem.mx);                      /* 移動結尾到解除完對映,整段不讓 fault 或另一次 sbrk 插進來 */
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    uintptr_t old = p->mem.brk ? p->mem.brk : USER_HEAP_BASE;
    uintptr_t now = old + (uintptr_t)incr;
    int ok = incr >= 0 ? (uintptr_t)incr <= USER_HEAP_MAX && now <= USER_HEAP_BASE + USER_HEAP_MAX
                       : (uintptr_t)-incr <= old - USER_HEAP_BASE;
    /* 記帳:heap 佔用的頁數改變多少,就增減多少承諾。超過配額的話整個要求失敗 */
    if (ok && commit_change_locked(p, (long)pages_for(now - USER_HEAP_BASE) -
                                      (long)pages_for(old - USER_HEAP_BASE)) != 0)
        ok = 0;
    if (ok)
        p->mem.brk = now;
    spin_unlock_irqrestore(&pstate_lk, fl);
    if (!ok) {
        mutex_unlock(&p->mem.mx);
        return -E_NOMEM;
    }

    /* 縮小:釋放不再屬於 heap 的整頁(第一個「完全在新結尾之後」的頁起) */
    uintptr_t first = (now + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    for (uintptr_t va = first; va < old; va += PAGE_SIZE)
        vm_space_unmap(p->space, va);
    mutex_unlock(&p->mem.mx);
    return (long)old;
}

/* ------------------------------------------------------------------ */
/* 給行程核心(proc.c)與訊號檢查點使用的介面                            */
/* 記憶體的記帳(p->mem)只有這個檔案可以存取                            */
/* ------------------------------------------------------------------ */

/* 新行程:配額沿用建立者的;由 kernel 建立的用預設值 */
void pmem_init(struct proc *p, const struct proc *parent)
{
    mutex_init(&p->mem.mx, "proc-mem", MX_PROC_MEM);
    p->mem.limit = parent ? parent->mem.limit : PROC_DEFAULT_LIMIT_PAGES;
}

/* exec:新的程式從空的 heap 開始(舊的頁隨舊位址空間釋放),匿名對映也都不在了 */
void pmem_exec(struct proc *p)
{
    p->mem.brk = 0;
    commit_release_all(p);
    memset(p->mem.maps, 0, sizeof(p->mem.maps));
    p->mem.mmap_next = 0;
}

/* fork:子行程得到和父行程一樣多的承諾(最壞情況下,共用的每一頁都要各自一份)。
   先記帳,系統給不出來就回傳非 0(不 fork);heap 與對映的紀錄照抄(內容隨位址空間複製) */
int pmem_fork(struct proc *child, const struct proc *parent)
{
    mutex_init(&child->mem.mx, "proc-mem", MX_PROC_MEM);
    child->mem.limit = parent->mem.limit;
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    int rc = commit_change_locked(child, (long)parent->mem.commit);
    memcpy(child->mem.maps, parent->mem.maps, sizeof(child->mem.maps));
    child->mem.mmap_next = parent->mem.mmap_next;
    child->mem.brk = parent->mem.brk;
    spin_unlock_irqrestore(&pstate_lk, fl);
    return rc;
}

/*
 * ---- 故障注入(SYS_seu):翻轉行程記憶體裡的一個位元 ----
 * 系統呼叫只把「位址與位元」記在目標行程上;目標在下一次返回使用者模式之前自己動手
 * (pmem_apply_injection,由 user_return_check 呼叫)。別的行程不直接碰它的位址空間,
 * 所以不必處理「對方同時在結束、在 fork」這些情況。一次只保留一筆。
 */
long sys_seu(struct proc *p, long pid, uintptr_t addr, long bit)
{
    long rc = -E_SRCH;

    if (!(p->cred.caps & CAP_ADMIN))             /* 只給有特權的行程 */
        return -E_PERM;
    if (bit < 0 || bit > 7)
        return -E_INVAL;
    unsigned long flags = sched_lock();
    struct proc *q = proc_find((int)pid);
    if (q && !q->zombie && q->mem.seu_pending) {
        rc = -E_BUSY;                            /* 上一筆還沒被對方處理:不覆蓋它 */
    } else if (q && !q->zombie) {
        q->mem.seu_addr = addr;
        q->mem.seu_mask = (uint8_t)(1u << bit);
        q->mem.seu_pending = 1;
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

void pmem_apply_injection(struct proc *p)
{
    if (!p->mem.seu_pending)                     /* 不上鎖先看一眼:這條路徑每次返回使用者模式都會經過 */
        return;
    /* 「有沒有、位址、哪個位元」三樣要在鎖裡一次取出:只把旗標清掉、之後才讀位址的話,
       中間若有第二筆注入進來,這一次用到的就是第二筆的位址,而第二筆的旗標還立著,
       等於同一筆做了兩次、第一筆沒做 */
    unsigned long flags = sched_lock();
    int pending = p->mem.seu_pending;
    uintptr_t addr = p->mem.seu_addr;
    uint8_t mask = p->mem.seu_mask, byte;
    p->mem.seu_pending = 0;
    sched_unlock(flags);
    /* 位址不合法就什麼都不做 */
    if (pending && copy_from_user(p, &byte, addr, 1) == 0) {
        byte ^= mask;
        if (copy_to_user(p, addr, &byte, 1) != 0)
            return;
    }
}
