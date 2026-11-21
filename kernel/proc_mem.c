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
        /* sbrk 時已經承諾(預留)過:用自己的票配置,一定成功(I-240 C3.2 的 I2) */
        return vm_space_fault_anon(p->space, addr) < 0 ? -1 : 0;
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
        return vm_space_fault_anon(p->space, addr) < 0 ? -1 : 0;   /* mmap 時已承諾:一定成功 */
    }

    uintptr_t lowest = USER_STACK_TOP - (uintptr_t)PROC_MAX_THREADS * USER_STACK_SLOT;
    if (addr < lowest || addr >= USER_STACK_TOP)
        return -1;
    unsigned slot = (unsigned)((USER_STACK_TOP - 1 - addr) / USER_STACK_SLOT);
    uintptr_t slot_base = USER_STACK_TOP - (uintptr_t)(slot + 1) * USER_STACK_SLOT;
    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    if (slot >= PROC_MAX_THREADS || !(__atomic_load_n(&p->stack_slots, __ATOMIC_ACQUIRE) & (1u << slot)) ||
        page == slot_base)                       /* 沒有這條 stack,或碰到保護頁 */
        return -1;

    /* 已承諾的格(stack_ready)才兌現,用自己的票 — 一定成功(I-240 C3.2 (c1)) */
    if (!(__atomic_load_n(&p->stack_ready, __ATOMIC_ACQUIRE) & (1u << slot)))
        return -1;
    return vm_space_fault_anon(p->space, page) < 0 ? -1 : 0;
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
 * 這裡改成承諾:每個要求在當下就決定成不成功(第 54 步起,I-240 C3.2):
 *   - 行程自己的配額:行程的 domain(membudget)合計 heap、mmap 與頁表,一個行程失控最多用掉它自己的額度
 *   - 整個系統:承諾的頁在 pmm 裡預留,一般配置(包括 kernel 自己的)拿不到;
 *     預留不到(空閒 − 預留 − 底線不夠)就拒絕。不再有第一次使用時才估算的固定上限
 *   - 承諾記在位址空間的帳本上(vm_space_commit_anon),第一次觸及時用自己的票配置,一定成功
 * 超過就回傳 -E_NOMEM:程式在「要」的時候得到明確的答案,可以改做別的事、
 * 釋放其他東西、或把工作有秩序地結束。之後去使用那些已經答應的頁時,不會再失敗。
 *
 * 配額只能調低(memlimit,可以低於已用量:已承諾的不撤銷,之後的增加被拒絕);調高需要 CAP_ADMIN。
 * 子行程沿用父行程的配額。fork 依 R1 替父子雙方預留寫入時複製的票,所以 fork 也可能因為配額或預留而失敗。
 */
static unsigned long pages_for(uintptr_t bytes)
{
    return (unsigned long)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* memlimit(kb):kb 為 0 = 查詢;否則把配額設成 kb。調低永遠允許,調高需要 CAP_ADMIN。回傳設定後的配額(KB) */
long sys_memlimit(struct proc *p, long kb)
{
    if (kb < 0)
        return -E_INVAL;
    struct mb_domain *d = p->mem.dom;
    if (kb > 0) {
        unsigned long pages = (unsigned long)kb / (PAGE_SIZE / 1024);
        if (pages > mb_limit(d) && !(p->cred.caps & CAP_ADMIN))
            return -E_PERM;
        mb_set_limit(d, pages);
    }
    return (long)(mb_limit(d) * (PAGE_SIZE / 1024));
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
            vm_space_commit_anon(p->space, start, pages * PAGE_SIZE) == 0) {
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
            break;
        }
    }
    spin_unlock_irqrestore(&pstate_lk, fl);
    if (pages)                                   /* 頁、票、配額一起歸還(分段放鎖,見 vm.c) */
        vm_space_release_anon(p->space, addr, pages * PAGE_SIZE);
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
    /* 承諾只依「進位到整頁」的頁數差增減(同一頁內移動結尾不改帳)。超過配額或預留不到就整個失敗 */
    unsigned long op = ok ? pages_for(old - USER_HEAP_BASE) : 0, np = ok ? pages_for(now - USER_HEAP_BASE) : 0;
    if (ok && np > op &&
        vm_space_commit_anon(p->space, USER_HEAP_BASE + op * PAGE_SIZE, (np - op) * PAGE_SIZE) != 0)
        ok = 0;
    if (ok)
        p->mem.brk = now;
    spin_unlock_irqrestore(&pstate_lk, fl);
    if (!ok) {
        mutex_unlock(&p->mem.mx);
        return -E_NOMEM;
    }

    /* 縮小:不再屬於 heap 的整頁連同票與配額歸還 */
    if (np < op)
        vm_space_release_anon(p->space, USER_HEAP_BASE + np * PAGE_SIZE, (op - np) * PAGE_SIZE);
    mutex_unlock(&p->mem.mx);
    return (long)old;
}

/* ------------------------------------------------------------------ */
/* 給行程核心(proc.c)與訊號檢查點使用的介面                            */
/* 記憶體的記帳(p->mem)只有這個檔案可以存取                            */
/* ------------------------------------------------------------------ */

/* 新行程的配額帳戶:沿用建立者的配額;由 kernel 建立的用預設值。失敗回傳 NULL */
struct mb_domain *pmem_domain_new(const struct proc *parent)
{
    return mb_domain_new(parent ? mb_limit(parent->mem.dom) : PROC_DEFAULT_LIMIT_PAGES);
}

/* 行程結構本身的 KOBJ 配額:以 kmalloc 實際配置的大小向上取整成頁(保守的邏輯配額,設計 B6) */
static unsigned long proc_kobj_pages(const struct proc *p)
{
    return (kmalloc_size((void *)p) + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* 新行程:接手 dom 的一份參考(由 pmem_domain_new 取得),記上行程結構的 KOBJ。
   配額不夠回傳 -E_NOMEM(dom 仍由 p 持有,呼叫者以 pmem_release 放掉) */
int pmem_init(struct proc *p, struct mb_domain *dom)
{
    mutex_init(&p->mem.mx, "proc-mem", MX_PROC_MEM);
    p->mem.dom = dom;
    if (mb_quota_add(dom, MB_KOBJ, proc_kobj_pages(p)) != 0)
        return -E_NOMEM;
    p->mem.kobj = proc_kobj_pages(p);
    return 0;
}

struct mb_domain *pmem_domain(const struct proc *p)
{
    return p->mem.dom;
}

/* 行程結構釋放前:放掉 domain 的參考。位址空間此時已經釋放(它記的帳已經退回),
   domain 不應再有用量 — 有的話是帳務錯誤,記入黑盒子 */
void pmem_release(struct proc *p)
{
    if (p->mem.dom && p->mem.kobj && mb_quota_sub(p->mem.dom, MB_KOBJ, p->mem.kobj) != 0)
        blackbox_log(BB_FAULT, "memory domain of pid %d: kernel-object quota mismatch", p->pid);
    p->mem.kobj = 0;
    if (p->mem.dom && mb_domain_put(p->mem.dom) != 0)
        blackbox_log(BB_FAULT, "memory domain of pid %d released with charges left", p->pid);
    p->mem.dom = NULL;
}

/* exec:新的程式從空的 heap 開始(舊的頁、票與配額隨舊位址空間一起退回),匿名對映也都不在了 */
void pmem_exec(struct proc *p)
{
    p->mem.brk = 0;
    memset(p->mem.maps, 0, sizeof(p->mem.maps));
    p->mem.mmap_next = 0;
}

/* fork 的第一部分:子行程的 domain(沿用父的配額)與行程結構的 KOBJ。失敗回傳 -E_NOMEM,不留東西 */
int pmem_fork_domain(struct proc *child, const struct proc *parent)
{
    mutex_init(&child->mem.mx, "proc-mem", MX_PROC_MEM);
    child->mem.dom = pmem_domain_new(parent);
    if (!child->mem.dom)
        return -E_NOMEM;
    if (mb_quota_add(child->mem.dom, MB_KOBJ, proc_kobj_pages(child)) != 0) {
        pmem_release(child);
        return -E_NOMEM;
    }
    child->mem.kobj = proc_kobj_pages(child);
    return 0;
}

/* fork 的最後一個可能失敗的步驟:子行程的位址空間。父行程已承諾的匿名記憶體範圍同步交給 VM,
   由它依 R1 替父子雙方預留寫入時複製的票(兩趟:先建子、後標父)。呼叫者必須把其他所有可能失敗的
   配置都放在這一步之前 — 這一步成功之後父的頁已經改成 COW,不能再因別的原因回退(Codex 198)。
   失敗回傳 NULL;父不變,子的 domain 仍由 child 持有(呼叫者以 pmem_release 放掉) */
struct vm_space *pmem_fork_space(struct proc *child, struct proc *parent)
{
    struct vm_range r[1 + PROC_MAX_MAPS + PROC_MAX_THREADS];
    unsigned n = 0;

    mutex_lock(&parent->mem.mx);                 /* 範圍與頁表在複製期間不變 */
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    memcpy(child->mem.maps, parent->mem.maps, sizeof(child->mem.maps));
    child->mem.mmap_next = parent->mem.mmap_next;
    child->mem.brk = parent->mem.brk;
    if (parent->mem.brk > USER_HEAP_BASE) {
        r[n].start = USER_HEAP_BASE;
        r[n++].len = pages_for(parent->mem.brk - USER_HEAP_BASE) * PAGE_SIZE;
    }
    for (unsigned i = 0; i < PROC_MAX_MAPS; i++)
        if (parent->mem.maps[i].pages) {
            r[n].start = parent->mem.maps[i].start;
            r[n++].len = parent->mem.maps[i].pages * PAGE_SIZE;
        }
    /* 已承諾的 stack 格也帶走(I-240 C3.2 (c1)):保護頁不在範圍內 */
    uint32_t ready = __atomic_load_n(&parent->stack_ready, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < PROC_MAX_THREADS; i++)
        if (ready & (1u << i)) {
            r[n].start = USER_STACK_TOP - (uintptr_t)(i + 1) * USER_STACK_SLOT + PAGE_SIZE;
            r[n++].len = USER_STACK_SLOT - PAGE_SIZE;
        }
    child->stack_ready = ready;
    spin_unlock_irqrestore(&pstate_lk, fl);
    struct vm_space *vs = vm_space_fork(parent->space, child->mem.dom, r, n);
    mutex_unlock(&parent->mem.mx);
    return vs;
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

/* 開機時把匿名記憶體(heap 與 mmap)的位址區域登記給 VM:VM 依此算出頁表票的上限(I-240 C3.2 BR5) */
void proc_mem_boot(void)
{
    /* 三個承諾窗由低到高:heap、mmap、stack 區(mmap 窗的結尾就是 stack 區的起點,相鄰共用的表 VM 會扣掉) */
    static const uintptr_t base[3] = { USER_HEAP_BASE, USER_MMAP_BASE,
                                       USER_STACK_TOP - (uintptr_t)PROC_MAX_THREADS * USER_STACK_SLOT };
    static const uintptr_t len[3] = { USER_HEAP_MAX, USER_MMAP_MAX, (uintptr_t)PROC_MAX_THREADS * USER_STACK_SLOT };
    vm_set_anon_windows(base, len, 3);
}

/* 讓行程核心在「配置 / 回退使用者 stack 的格子與頁」時持有 mem.mx,與 stack 成長的 fault 序列化
   (否則 fault 可能在格子已佔、頁還沒對映的空檔裡自己裝上一頁,之後被覆寫而漏掉 — 審查代理人的發現) */
void pmem_lock(struct proc *p)   { mutex_lock(&p->mem.mx); }
void pmem_unlock(struct proc *p) { mutex_unlock(&p->mem.mx); }
