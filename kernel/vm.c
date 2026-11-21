/*
 * vm.c - 頁表管理(與架構無關的部分)
 *
 * 頁表是一棵多層的樹。把虛擬位址切成幾段,每一段當作該層表格的索引:
 *
 *   RISC-V Sv39 / AArch64(39 位元位址,3 層,每層 9 位元 = 512 項):
 *     | 第 2 層索引 (9) | 第 1 層索引 (9) | 第 0 層索引 (9) | 頁內偏移 (12) |
 *   i386(32 位元位址,2 層,每層 10 位元 = 1024 項):
 *     |     第 1 層索引 (10)     |     第 0 層索引 (10)     | 頁內偏移 (12) |
 *
 * 從根表格開始,用最高一段索引找到下一層表格的位址,一路走到最後一層,
 * 最後一層的項目(PTE,page table entry)記錄實體頁的位址與權限。
 * 每個表格剛好佔一頁。用多層而不是一張大表的原因:沒用到的位址範圍
 * 不必配置下層表格,39 位元的位址空間若用單層需要 1GB 的表。
 *
 * 三個架構的差異只有「層數、每層位元數、PTE 的位元格式、如何開啟」,
 * 由 arch/<名稱>/mmu.h 與 mmu.c 提供;走訪樹的邏輯在這裡共用。
 *
 * 因為採恆等對映,表格的實體位址可以直接當指標使用,開啟分頁前後皆然。
 */
#include <stddef.h>
#include "arch.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
#include "panic.h"
#include "pmm.h"
#include "vm.h"
#include "cpu.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "membudget.h"
#include "uapi/errno.h"

/* 保護 kernel 頁表、所有位址空間的頁表與 ASID 的分配。
   持有期間可能再取得 pmm 的鎖(配置表格時),順序固定為 vm -> pmm */
static struct spinlock vm_lk = SPINLOCK_INIT("vm", LK_VM);

#define ENTRIES   (1u << MMU_BITS)           /* 每個表格的項目數 */
#define IDX_MASK  (ENTRIES - 1)

/* linker script 定義:kernel 映像各區段的邊界(都對齊到頁) */
extern char __kernel_start[], __text_end[], __rodata_end[];

static pte_t *root;                          /* kernel 頁表的根表格 */

struct vm_space {
    pte_t *root;                             /* 這個位址空間的根表格 */
    unsigned asid;                           /* 位址空間編號(見 mmu.h);不支援的架構恆為 0 */
    /*
     * 這個位址空間曾經在哪些 CPU 上被載入過(bit n = 第 n 顆)。只增不減。
     * 用途:要讓它的 TLB 紀錄失效、或釋放它之前要確認沒有人還載入著時,
     * 只需要通知這些 CPU。其他 CPU 從來沒碰過它,不可能留有它的任何東西。
     * 大多數行程只在一兩顆 CPU 上執行過,所以通常只需要通知很少的 CPU,甚至不必通知。
     */
    volatile unsigned used_cpus;
    unsigned long pages;                     /* 目前對映了幾個使用者的頁(top 顯示行程的記憶體用量) */
    /*
     * 記憶體帳本(I-240 C3.2;設計見 docs/design/C3.2-memory-accounting.md 的 B2 / BR2)。受 vm_lk 保護。
     * dom 為 NULL:不記帳(kernel 或還沒接上配額的建立者)。
     */
    struct mb_domain *dom;                   /* 持有一份參考 */
    unsigned long quota[MB_NCLASS];          /* 這個空間記在 dom 上的配額 */
    unsigned long data_tickets;              /* R1:未兌現 + 標 VM_TICKET 的已承諾映射數 */
    unsigned long pt_tickets;                /* 替還不存在的中間表預留的票 */
    unsigned long anon_tables;               /* E:已存在、且會被匿名記憶體區域用到的中間表數(BR5) */
    struct mb_exec_txn *txn;                 /* exec 交易中的新空間:記帳帶這個憑證(只有這個空間);NULL = 一般上限 */
};

/*
 * 匿名記憶體(heap、mmap)的位址區域,由行程記憶體模組在開機時登記(VM 不知道它們的意義)。
 * anon_pt_max(BR5 的 M):整個區域集合用得到的「不同」中間表總數,由區域與分頁格式算出。
 * 每個空間維持 E + T <= M:承諾時只預留 min(pt_need, M - E - T) 張頁表票。到達上限時,
 * 區域裡每一張可能的表不是已經存在、就是已經有票,所以少預留也安全;反覆 grow / shrink 也不會無限多領。
 */
#define ANON_WIN_MAX 3
static struct { uintptr_t base, len; } anon_win[ANON_WIN_MAX];
static unsigned n_anon_win;
static unsigned long anon_pt_max;

/*
 * 建頁表的用途(BR2)。不同區域之間不能借用承諾:
 *   WALK_KERNEL  kernel 的位址空間:一般配置,不記配額
 *   WALK_LEGACY  還沒接上預留的使用者區域(stack、映像…):一般配置並記 PGTABLE 配額,不碰 pt_tickets
 *   WALK_ANON    已承諾的匿名記憶體:只能兌現 pt_tickets;沒有票是帳務錯誤
 * 已經存在的表,任何用途都可以直接使用,不耗票。
 */
enum walk_use { WALK_KERNEL, WALK_LEGACY, WALK_ANON };

#if MMU_ASID_MAX > 0
static uint32_t asid_used[(MMU_ASID_MAX + 31) / 32];     /* bit n = 1:編號 n 使用中 */
#endif

/* 取得一個沒人用的編號;用完了就回傳共用編號 MMU_ASID_MAX(切換時要清空 TLB) */
static unsigned asid_alloc(void)
{
#if MMU_ASID_MAX > 0
    for (unsigned n = 1; n < MMU_ASID_MAX; n++) {
        if (!(asid_used[n / 32] & (1u << (n % 32)))) {
            asid_used[n / 32] |= 1u << (n % 32);
            return n;
        }
    }
#endif
    return MMU_ASID_MAX;
}

static void asid_free(unsigned asid)
{
#if MMU_ASID_MAX > 0
    if (asid == 0 || asid >= MMU_ASID_MAX)
        return;
    /* 歸還之前先清掉 TLB 裡屬於這個編號的紀錄:下一個拿到同一編號的行程
       絕不能看到上一個行程的對映 */
    arch_asid_flush(asid);
    asid_used[asid / 32] &= ~(1u << (asid % 32));
#else
    (void)asid;
#endif
}
static unsigned long table_pages;            /* 頁表用掉的頁數(含根) */
static int enabled;                          /* 分頁是否已開啟 */
static unsigned long large_pages;            /* 目前以大分頁對映的數量 */

/* 一個大分頁的大小:i386 為 4MB,RISC-V / AArch64 為 2MB */
#define LARGE_SIZE ((uintptr_t)PAGE_SIZE << (MMU_LARGE_LEVEL * MMU_BITS))

/* va 在第 level 層表格中的索引(level 0 為最後一層) */
static unsigned va_index(uintptr_t va, int level)
{
    return (unsigned)(va >> (PAGE_SHIFT + level * MMU_BITS)) & IDX_MASK;
}

static pte_t *table_alloc(void)
{
    pte_t *t = pmm_alloc();
    if (t) {
        memset(t, 0, PAGE_SIZE);             /* 全 0 = 全部無效 */
        table_pages++;
    }
    return t;
}

/*
 * 把第 level 層的一個大分頁項目 *e 拆成一整張下層表格(512 或 1024 個 4KB 頁),
 * 內容與權限完全相同。之後才能單獨修改其中某一頁。
 * base 為這個大分頁涵蓋的起始虛擬位址。
 */
static int split_large(pte_t *e, int level, uintptr_t base)
{
    pte_t *t = table_alloc();
    if (!t)
        return -1;

    uintptr_t pa = arch_pte_addr(*e);
    unsigned flags = arch_pte_flags(*e);
    uintptr_t step = (uintptr_t)PAGE_SIZE << ((level - 1) * MMU_BITS);
    for (unsigned i = 0; i < ENTRIES; i++)
        arch_pte_set(&t[i], arch_pte_leaf(pa + i * step, flags, level - 1));

    arch_pte_set(e, arch_pte_table((uintptr_t)t, 0, level));
    /* 拆開前後的轉換結果相同,但 TLB 裡還留著「一整塊」的舊紀錄;
       先清掉,之後對單一頁的修改才不會被舊紀錄蓋過 */
    if (enabled)
        arch_tlb_flush(base);
    large_pages--;
    return 0;
}

/*
 * 走訪頁表,回傳 va 對應的 PTE 的位置,並由 *out_level 回報它在第幾層
 * (0 = 4KB 頁;大於 0 = 大分頁)。
 *   target:要走到第幾層(0 = 4KB 頁,MMU_LARGE_LEVEL = 大分頁)
 *   create:途中缺少的下層表格是否當場配置
 *   split :途中遇到大分頁時是否拆開繼續往下。為 0 則直接回傳該大分頁項目
 * 失敗(缺表格又不配置、位址超出範圍、記憶體不足)回傳 NULL。
 */
/* 第 level 層的項目所指的那張表(涵蓋 va 附近 PAGE_SIZE << (level * MMU_BITS) 位元組)
   是否會被匿名記憶體區域用到 */
static int table_in_anon(uintptr_t va, int level)
{
    uintptr_t span = (uintptr_t)PAGE_SIZE << (level * MMU_BITS);
    uintptr_t tb = va & ~(span - 1);
    for (unsigned i = 0; i < n_anon_win; i++)
        if (tb < anon_win[i].base + anon_win[i].len && anon_win[i].base < tb + span)
            return 1;
    return 0;
}

/* 新建了一張表之後的 E / T 記帳(持有 vm_lk)。用票建的:T 減一、E 加一,總和不變。
   legacy 建的而且落在匿名區域:E 加一;若因此 E + T 超過 M,代表那張表本來就算在某張票裡,
   退掉一張票(連同它的 PGTABLE 配額 — legacy 建表時已經另外記過一頁),維持 E + T <= M */
static void anon_table_built(struct vm_space *vs, uintptr_t va, int level, enum walk_use use)
{
    if (!vs || !vs->dom || !table_in_anon(va, level))
        return;
    vs->anon_tables++;
    if (use != WALK_ANON && vs->pt_tickets > 0 && vs->anon_tables + vs->pt_tickets > anon_pt_max) {
        vs->pt_tickets--;
        vs->quota[MB_PGTABLE]--;
        pmm_unreserve(1);
        if (mb_quota_sub(vs->dom, MB_PGTABLE, 1) != 0)
            panic("vm: page-table ticket quota mismatch");
    }
}

/* 依用途配置一張新的中間表(持有 vm_lk)。失敗回傳 NULL,狀態不變 */
static pte_t *new_table(struct vm_space *vs, enum walk_use use)
{
    pte_t *t;

    switch (use) {
    case WALK_ANON:
        if (!vs || vs->pt_tickets == 0)
            panic("vm: anonymous-memory table without a reserved ticket");
        t = pmm_alloc_reserved();            /* 一定成功(pmm 的 N1) */
        memset(t, 0, PAGE_SIZE);
        table_pages++;
        vs->pt_tickets--;                    /* 配額在承諾時已經記過 */
        return t;
    case WALK_LEGACY:
        if (vs && vs->dom && mb_quota_add_txn(vs->dom, MB_PGTABLE, 1, vs->txn) != 0)
            return NULL;
        t = table_alloc();
        if (vs && vs->dom) {
            if (!t) {
                if (mb_quota_sub(vs->dom, MB_PGTABLE, 1) != 0)   /* 撤回剛記的 1 頁,不可能超量 */
                    panic("vm: table quota rollback mismatch");
            } else
                vs->quota[MB_PGTABLE]++;
        }
        return t;
    case WALK_KERNEL:
    default:
        return table_alloc();
    }
}

static pte_t *walk(struct vm_space *vs, pte_t *t, uintptr_t va, int target, int create, int split,
                   enum walk_use use, int *out_level)
{
    int level;

    if (MMU_VA_BITS < sizeof(uintptr_t) * 8 && (va >> MMU_VA_BITS) != 0)
        return NULL;                         /* 超出頁表能表示的位址範圍 */

    for (level = MMU_LEVELS - 1; level > target; level--) {
        pte_t *e = &t[va_index(va, level)];
        if (!arch_pte_valid(*e)) {
            if (!create)
                return NULL;
            pte_t *next = new_table(vs, use);
            if (!next)
                return NULL;
            arch_pte_set(e, arch_pte_table((uintptr_t)next, use != WALK_KERNEL, level));
            anon_table_built(vs, va, level, use);
            if (enabled && level == MMU_LEVELS - 1)
                arch_mmu_root_changed(t);    /* i386 PAE:最上層的修改要重新載入才生效 */
        } else if (!arch_pte_is_table(*e, level)) {
            if (!split) {
                if (out_level)
                    *out_level = level;
                return e;                    /* 查詢:回報這個大分頁 */
            }
            uintptr_t span = (uintptr_t)PAGE_SIZE << (level * MMU_BITS);
            if (split_large(e, level, va & ~(span - 1)) != 0)
                return NULL;
        }
        t = (pte_t *)arch_pte_addr(*e);
    }
    if (out_level)
        *out_level = level;
    return &t[va_index(va, level)];
}

/* 權限檢查:拒絕「同時可寫又可執行」的對映(W^X)。
   這種頁讓攻擊者能寫入資料再當成程式碼執行,kernel 沒有任何正當理由需要它 */
/*
 * TLB shootdown:讓 va 這一頁的轉換紀錄在「所有 CPU」上失效。
 * 每顆 CPU 有自己的 TLB,在一顆 CPU 上執行失效指令只清得到它自己的。
 * 其他 CPU 若繼續使用舊的紀錄,就會存取到已經移除(或權限已改)的對映。
 *
 * 做法依架構而定:
 *   AArch64      有會「廣播」的失效指令(tlbi ...is),硬體自己通知所有 CPU
 *   RISC-V、i386 以跨 CPU 呼叫請每顆 CPU 各自執行一次,等全部完成才返回
 *
 * 只在「原本有效的對映被移除或更改」時才需要。新增對映(原本無效)不必:
 * CPU 不會把「無效」這件事存進 TLB,其他 CPU 第一次用到時自然會查到新的內容。
 */
#if !MMU_TLB_BROADCAST
/* 跨 CPU 呼叫用:在被通知的那顆 CPU 上清掉 va 的紀錄 */
static void flush_one_fn(void *arg)
{
    arch_tlb_flush((uintptr_t)arg);
}
#endif

/* kernel 的對映:每一顆 CPU 都可能用過,全部都要通知 */
static void tlb_shootdown(uintptr_t va)
{
#if MMU_TLB_BROADCAST
    arch_tlb_flush_broadcast(va);
#else
    arch_tlb_flush(va);
    smp_call_others(flush_one_fn, (void *)va);
#endif
}

/* 某個位址空間裡的對映:只有載入過它的 CPU 才可能留有舊紀錄。
   如果除了自己以外沒有別人,清自己的就夠了(最常見的情況) */
static void tlb_shootdown_space(struct vm_space *vs, uintptr_t va)
{
    unsigned others = __atomic_load_n(&vs->used_cpus, __ATOMIC_SEQ_CST) & ~(1u << this_cpu()->id);

    if (!others) {
        arch_tlb_flush(va);
        return;
    }
#if MMU_TLB_BROADCAST
    arch_tlb_flush_broadcast(va);
#else
    arch_tlb_flush(va);
    smp_call_mask(others, flush_one_fn, (void *)va);
#endif
}

static int flags_ok(unsigned flags)
{
    return !((flags & VM_WRITE) && (flags & VM_EXEC)) && vm_sw_flags_ok(flags);
}

int vm_map(uintptr_t va, uintptr_t pa, unsigned flags)
{
    if (((va | pa) & (PAGE_SIZE - 1)) || !flags_ok(flags))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    pte_t *e = walk(NULL, root, va, 0, 1, 1, WALK_KERNEL, NULL);
    int rc = -1;
    if (e) {
        int was_valid = arch_pte_valid(*e);
        arch_pte_set(e, arch_pte_leaf(pa, flags & ~VM_USER, 0));     /* kernel 頁表不放使用者頁 */
        /* CPU 會把查過的轉換結果快取在 TLB;改了 PTE 要讓那一筆失效,
           否則 CPU 可能繼續用舊的對映。取代既有的對映時,所有 CPU 都要失效 */
        if (enabled) {
            if (was_valid)
                tlb_shootdown(va);
            else
                arch_tlb_flush(va);
        }
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

/* 以一個大分頁對映 [va, va + LARGE_SIZE)。只在 vm_init 建立初始對映時使用 */
static int map_large(uintptr_t va, uintptr_t pa, unsigned flags)
{
    pte_t *e = walk(NULL, root, va, MMU_LARGE_LEVEL, 1, 0, WALK_KERNEL, NULL);
    if (!e)
        return -1;
    arch_pte_set(e, arch_pte_leaf(pa, flags, MMU_LARGE_LEVEL));
    large_pages++;
    return 0;
}

int vm_map_range(uintptr_t va, uintptr_t pa, uintptr_t size, unsigned flags)
{
    for (uintptr_t off = 0; off < size; off += PAGE_SIZE)
        if (vm_map(va + off, pa + off, flags) != 0)
            return -1;
    return 0;
}

int vm_unmap(uintptr_t va)
{
    va &= ~(uintptr_t)(PAGE_SIZE - 1);

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    pte_t *e = walk(NULL, root, va, 0, 0, 1, WALK_KERNEL, NULL);   /* 落在大分頁裡就先拆開,只移除這一頁 */
    int rc = -1;
    if (e && arch_pte_valid(*e)) {
        arch_pte_set(e, 0);
        if (enabled)
            tlb_shootdown(va);               /* 移除對映:每顆 CPU 的舊紀錄都要清掉 */
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

int vm_query(uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int level = 0;
    pte_t *e = walk(NULL, root, va, 0, 0, 0, WALK_KERNEL, &level);
    int rc = -1;
    if (e && arch_pte_valid(*e)) {
        /* 大分頁的頁內偏移比較寬:第 level 層的一項涵蓋 4KB << (level * 位元數) */
        uintptr_t span = (uintptr_t)PAGE_SIZE << (level * MMU_BITS);
        if (pa)
            *pa = arch_pte_addr(*e) | (va & (span - 1));
        if (flags)
            *flags = arch_pte_flags(*e);
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 位址空間                                                            */
/* ------------------------------------------------------------------ */

static int user_range(uintptr_t va)
{
    return va >= USER_BASE && va < USER_TOP;
}

struct vm_space *vm_space_create(struct mb_domain *dom)
{
    return vm_space_create_txn(dom, NULL);
}

struct vm_space *vm_space_create_txn(struct mb_domain *dom, struct mb_exec_txn *txn)
{
    struct vm_space *vs = kzalloc(sizeof(*vs));     /* 帳本從 0 開始 */
    if (!vs)
        return NULL;
    vs->txn = dom ? txn : NULL;              /* 根表的 1 頁也用交易的放寬上限(否則接近上限的 exec 會提早被拒) */
    /* 根表是一般配置實際取得,記 PGTABLE 配額 1 頁、不用票(R5) */
    if (dom) {
        if (mb_domain_get(dom) != 0) {
            kfree(vs);
            return NULL;
        }
        if (mb_quota_add_txn(dom, MB_PGTABLE, 1, vs->txn) != 0) {
            mb_domain_put(dom);
            kfree(vs);
            return NULL;
        }
        vs->dom = dom;
        vs->quota[MB_PGTABLE] = 1;
    }

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    vs->root = table_alloc();
    if (vs->root) {
        /* 複製 kernel 頁表最上層的所有項目。下層表格因此是共用的:之後 kernel
           在既有範圍內的修改(例如拆開大分頁、移除保護頁)所有位址空間都看得到 */
        memcpy(vs->root, root, PAGE_SIZE);
        vs->asid = asid_alloc();
        vs->used_cpus = 0;                   /* 還沒有任何 CPU 載入過 */
    }
    spin_unlock_irqrestore(&vm_lk, irq);

    if (!vs->root) {
        if (dom && (mb_quota_sub(dom, MB_PGTABLE, 1) != 0 || mb_domain_put(dom) != 0))
            panic("vm: root table quota rollback mismatch");
        kfree(vs);
        return NULL;
    }
    return vs;
}

/* 釋放一張表格底下的所有東西(僅用於使用者範圍)。level 為這張表格所在的層 */
static void free_table(pte_t *t, int level)
{
    for (unsigned i = 0; i < ENTRIES; i++) {
        if (!arch_pte_valid(t[i]))
            continue;
        if (level > 0)
            free_table((pte_t *)arch_pte_addr(t[i]), level - 1);
        else if (!(arch_pte_flags(t[i]) & VM_SHARED))
            pmm_free((void *)arch_pte_addr(t[i]));       /* 行程擁有的實體頁;共用的頁不歸它釋放 */
    }
    pmm_free(t);
    table_pages--;
}

/* 位址空間即將釋放時在每顆 CPU 上執行(跨 CPU 呼叫;中斷關閉、不可上鎖) */
static void space_gone_fn(void *arg)
{
    struct vm_space *vs = arg;

    sched_space_dropped(vs);                 /* 這顆 CPU 若還載入著它的頁表,換回 kernel 的 */
#if MMU_ASID_MAX > 0
    /* 編號即將交給別的行程:每顆 CPU 的 TLB 裡屬於這個編號的紀錄都要清掉 */
    if (vs->asid != 0 && vs->asid < MMU_ASID_MAX)
        arch_asid_flush(vs->asid);
#endif
}

void vm_space_destroy(struct vm_space *vs)
{
    /* 先確定沒有任何 CPU 還在使用這份頁表,才能釋放它。呼叫者保證已經沒有
       執行緒屬於這個位址空間;剩下的只可能是別顆 CPU 上「沿用上一個行程頁表」
       的 kernel 執行緒(見 sched.c) */
    unsigned long f = arch_irq_save();
    space_gone_fn(vs);
    arch_irq_restore(f);
    smp_call_mask(vs->used_cpus, space_gone_fn, vs);   /* 只有載入過它的 CPU 需要知道 */

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int top = MMU_LEVELS - 1;

    /* 只處理最上層中屬於使用者範圍的項目;其餘是和 kernel 共用的,不能動 */
    for (unsigned i = va_index(USER_BASE, top); i <= va_index(USER_TOP - 1, top); i++) {
        if (arch_pte_valid(vs->root[i]))
            free_table((pte_t *)arch_pte_addr(vs->root[i]), top - 1);
    }
    pmm_free(vs->root);
    table_pages--;
    asid_free(vs->asid);
    /* 帳本:所有表與頁都已釋放;剩下沒兌現的票退回 pmm,配額全部退回 domain(vm_lk → mb_lk → pmm_lk) */
    if (vs->dom) {
        struct mb_bundle b = { .reserve = vs->data_tickets + vs->pt_tickets };
        for (unsigned i = 0; i < MB_NCLASS; i++)
            b.quota[i] = vs->quota[i];
        if (mb_release_bundle(vs->dom, &b) != 0)
            panic("vm: address space ledger does not match its domain");
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    if (vs->dom && mb_domain_put(vs->dom) != 0)
        panic("vm: domain released with memory still charged");
    kfree(vs);
}

int vm_space_map(struct vm_space *vs, uintptr_t va, uintptr_t pa, unsigned flags)
{
    flags |= VM_USER;
    if (((va | pa) & (PAGE_SIZE - 1)) || !flags_ok(flags) || !user_range(va))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    pte_t *e = walk(vs, vs->root, va, 0, 1, 0, WALK_LEGACY, NULL);
    int rc = -1;
    if (e) {
        int was_valid = arch_pte_valid(*e);
        arch_pte_set(e, arch_pte_leaf(pa, flags, 0));
        vs->pages += !was_valid;
        if (enabled) {
            if (was_valid)
                tlb_shootdown_space(vs, va); /* 同一行程的其他執行緒可能正在別顆 CPU 上執行 */
            else
                arch_tlb_flush(va);
        }
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

int vm_space_query(struct vm_space *vs, uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    if (!user_range(va))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int level = 0;
    pte_t *e = walk(vs, vs->root, va, 0, 0, 0, WALK_LEGACY, &level);
    int rc = -1;
    if (e && arch_pte_valid(*e) && level == 0 && (arch_pte_flags(*e) & VM_USER)) {
        if (pa)
            *pa = arch_pte_addr(*e) | (va & (PAGE_SIZE - 1));
        if (flags)
            *flags = arch_pte_flags(*e);
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

/*
 * 在 vm_lk 之內完成「查表 + 檢查權限 + 複製」:和使用者空間交換資料時用(copy_from_user / copy_to_user)。
 *
 * 為什麼不能「先查出實體位址、放開鎖、再複製」:放鎖之後,同一個行程的另一條執行緒可以 munmap
 * 那一頁、讓它被釋放並配置給別人,複製就會讀寫到別人的資料(Codex 以確定的交錯證實過)。
 * 移除對映(vm_space_unmap)、寫入時複製(vm_space_fault)、fork 都要拿 vm_lk,所以在鎖內複製,
 * 這些事情就不可能插在「查到」與「用完」之間。
 *
 * 不跨頁;n 最多一頁,複製是記憶體對記憶體(kernel 經由實體位址存取,不會觸發 page fault),時間有界。
 * to_user = 1 時必須是可寫的使用者頁:寫入時複製的頁在這裡是唯讀的,回傳 -1,由呼叫者先處理 fault 再重試。
 * 回傳 0 = 複製完成;-1 = 沒有對映,或權限不符。
 */
int vm_space_copy(struct vm_space *vs, uintptr_t va, void *kbuf, size_t n, int to_user)
{
    if (!user_range(va) || n == 0 || n > PAGE_SIZE - (va & (PAGE_SIZE - 1)))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int level = 0;
    int rc = -1;
    pte_t *e = walk(vs, vs->root, va, 0, 0, 0, WALK_LEGACY, &level);
    if (e && arch_pte_valid(*e) && level == 0) {
        unsigned f = arch_pte_flags(*e);
        if ((f & VM_USER) && (!to_user || (f & VM_WRITE))) {
            uint8_t *page = (uint8_t *)(arch_pte_addr(*e) | (va & (PAGE_SIZE - 1)));
            if (to_user)
                memcpy(page, kbuf, n);
            else
                memcpy(kbuf, page, n);
            rc = 0;
        }
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

int vm_space_map_new(struct vm_space *vs, uintptr_t va, uintptr_t pa, unsigned flags)
{
    flags |= VM_USER;
    if (((va | pa) & (PAGE_SIZE - 1)) || !flags_ok(flags) || !user_range(va))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    pte_t *e = walk(vs, vs->root, va, 0, 1, 0, WALK_LEGACY, NULL);
    int rc = -1;
    if (e && arch_pte_valid(*e)) {
        rc = 1;                              /* 別人已經先對映了 */
    } else if (e) {
        arch_pte_set(e, arch_pte_leaf(pa, flags, 0));
        vs->pages++;
        if (enabled)
            arch_tlb_flush(va);
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

/* 移除一個使用者頁的對映,並放棄那一頁(行程自己的頁會被釋放;和檔案共用的頁不歸它釋放)。
   0 = 已移除,-1 = 那裡本來就沒有對映 */
int vm_space_unmap(struct vm_space *vs, uintptr_t va)
{
    if ((va & (PAGE_SIZE - 1)) || !user_range(va))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int level = 0;
    pte_t *e = walk(vs, vs->root, va, 0, 0, 0, WALK_LEGACY, &level);
    int rc = -1;
    if (e && arch_pte_valid(*e) && level == 0) {
        uintptr_t pa = arch_pte_addr(*e);
        unsigned flags = arch_pte_flags(*e);
        arch_pte_set(e, 0);
        vs->pages--;
        if (enabled)
            tlb_shootdown_space(vs, va);     /* 同一行程的其他執行緒可能正在別顆 CPU 上使用這一頁 */
        if (!(flags & VM_SHARED))
            pmm_free((void *)pa);            /* 參考計數減一;fork 之後還和別人共用的話不會真的釋放 */
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

unsigned long vm_space_pages(struct vm_space *vs)
{
    return vs->pages;                        /* 只是統計用的數字,不上鎖 */
}

/* ---- 寫入時複製 ---- */

static unsigned long cow_copies, cow_reuses;

unsigned long vm_cow_copies(void) { return cow_copies; }
unsigned long vm_cow_reuses(void) { return cow_reuses; }

/* 讓 vs 的轉換紀錄在每一顆 CPU 上失效(跨 CPU 呼叫;中斷關閉、不可上鎖) */
static void space_flush_fn(void *arg)
{
    struct vm_space *vs = arg;

    sched_space_flush(vs);                   /* 這顆 CPU 正載入著它:重新載入並清空 */
#if MMU_ASID_MAX > 0
    /* 沒載入的 CPU 也可能留著帶有這個編號的舊紀錄(它之前在這顆 CPU 上執行過) */
    if (vs->asid != 0 && vs->asid < MMU_ASID_MAX)
        arch_asid_flush(vs->asid);
#endif
}

/* 複製一張表格底下的所有對映到 child(僅用於使用者範圍)。呼叫時持有 vm_lk。
   base 為這張表格第 0 項對應的虛擬位址 */
static unsigned long pt_need(uintptr_t va, uintptr_t len);   /* 定義在檔案後段(匿名記憶體) */

/* fork 時,哪些頁屬於匿名記憶體的已承諾範圍(由行程記憶體模組在 mem.mx 之下同步傳入) */
struct fork_anon {
    const struct vm_range *r;
    unsigned n;
    unsigned long marked;                    /* 這次由「父的獨佔頁」變成 COW + TICKET 的頁數 */
};

static int in_anon(const struct fork_anon *fa, uintptr_t va)
{
    for (unsigned i = 0; fa && i < fa->n; i++)
        if (va >= fa->r[i].start && va - fa->r[i].start < fa->r[i].len)
            return 1;
    return 0;
}

/*
 * fork 分兩趟(設計 BR4:先完成所有可能失敗的步驟,最後才改父行程):
 *   第一趟 mark_parent = 0:替子空間建表、寫入 PTE(可寫的頁在子這邊直接是唯讀 + COW,
 *          匿名記憶體另帶 TICKET),增加實體頁的參考。可能因為配置不出頁表而失敗 —
 *          此時父空間完全沒有被改過。
 *   第二趟 mark_parent = 1:只把父空間裡可寫的頁改成同樣的唯讀 + COW(+ TICKET),
 *          不配置任何東西,不會失敗;fa->marked 數出父這邊新增的有票映射。
 * 兩趟之間一直持有 vm_lk,而且 fork 只允許單一執行緒的行程,父的 PTE 不會被別人改動。
 */
static int fork_table(struct vm_space *child, pte_t *t, int level, uintptr_t base,
                      unsigned first, unsigned last, struct fork_anon *fa, int mark_parent)
{
    uintptr_t span = (uintptr_t)PAGE_SIZE << (level * MMU_BITS);

    for (unsigned i = first; i <= last; i++) {
        if (!arch_pte_valid(t[i]))
            continue;
        uintptr_t va = base + i * span;
        if (level > 0) {
            if (fork_table(child, (pte_t *)arch_pte_addr(t[i]), level - 1, va, 0, ENTRIES - 1, fa,
                           mark_parent) != 0)
                return -1;
            continue;
        }

        uintptr_t pa = arch_pte_addr(t[i]);
        unsigned flags = arch_pte_flags(t[i]);

        /* 這一頁從此多一個擁有者。原本可寫的頁在兩邊都變成「唯讀 + COW」:任何一邊寫入都會
           觸發 page fault。匿名記憶體的頁另外帶 VM_TICKET(R1:每一筆 COW 映射各持一張票);
           票在呼叫者那裡已經預留好(父:這次變成 COW 的頁數;子:全部承諾數) */
        if (!(flags & VM_SHARED) && (flags & VM_WRITE)) {
            /* 不變量 U1(第 (c2) 批):有 domain 的空間裡,每一頁可寫的私有頁都屬於某個承諾範圍
               (heap、mmap、stack 格、映像可寫範圍)。不在範圍內就是帳務漏洞 — 讓 fork 失敗(第一趟,
               父還沒被改過),而不是產生一筆沒有票、之後可能在 page fault 失敗的 COW */
            if (fa && !in_anon(fa, va)) {
                kprintf("vm: fork found a writable private page at %p outside every committed range\n",
                        (void *)va);
                return -1;
            }
            flags = (flags & ~VM_WRITE) | VM_COW;
            if (in_anon(fa, va))
                flags |= VM_TICKET;
            if (mark_parent) {
                arch_pte_set(&t[i], arch_pte_leaf(pa, flags, 0));
                if (flags & VM_TICKET)
                    fa->marked++;
            }
        }
        if (mark_parent)
            continue;
        pte_t *e = walk(child, child->root, va, 0, 1, 0, WALK_LEGACY, NULL);
        if (!e)
            return -1;                       /* 記憶體不足(配置不出頁表) */
        if (!(flags & VM_SHARED))
            pmm_ref((void *)pa);             /* 對映成功才算數:失敗時 child 會被整個釋放 */
        arch_pte_set(e, arch_pte_leaf(pa, flags, 0));
        child->pages++;
    }
    return 0;
}

struct vm_space *vm_space_fork(struct vm_space *parent, struct mb_domain *dom,
                               const struct vm_range *anon, unsigned nanon)
{
    struct vm_space *child = vm_space_create(dom);
    int top = MMU_LEVELS - 1;
    struct fork_anon fa = { anon, nanon, 0 };

    if (!child)
        return NULL;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int rc = 0;
    unsigned long pending = 0;
    if (dom && parent->dom) {
        /* 先數出父的新增票:父的每一頁可寫獨佔、位於承諾範圍內的頁都會變成 COW + TICKET */
        for (unsigned i = 0; i < nanon; i++)
            for (uintptr_t off = 0; off < anon[i].len; off += PAGE_SIZE) {
                int level = 0;
                pte_t *e = walk(parent, parent->root, anon[i].start + off, 0, 0, 0, WALK_LEGACY, &level);
                if (e && arch_pte_valid(*e) && level == 0 && (arch_pte_flags(*e) & VM_WRITE))
                    pending++;
            }
        /* 第一筆帳:子空間。R1:父的承諾範圍內每一頁不是未兌現(A)、就是 COW + TICKET(B)、
           就是可寫獨佔(X);父持有 |A| + |B| 張票(COW 只剩自己時會退票,所以沒有多餘的票),
           子要 |A| + |B| + |X| = 父的票 + pending。配額照抄父的 ANON 與 STACK(保守的邏輯配額);
           中間表以子空間的承諾範圍最壞情況預留(上限 M) */
        unsigned long q = parent->data_tickets + pending, pt = 0;
        for (unsigned i = 0; i < nanon; i++)
            pt += pt_need(anon[i].start, anon[i].len);
        if (pt > anon_pt_max)
            pt = anon_pt_max;
        struct mb_bundle b = { .reserve = q + pt };
        b.quota[MB_ANON] = parent->quota[MB_ANON];
        b.quota[MB_STACK] = parent->quota[MB_STACK];
        b.quota[MB_IMAGE] = parent->quota[MB_IMAGE];
        b.quota[MB_PGTABLE] = pt;
        if (q < pending || q + pt < q || mb_commit_bundle(dom, &b) != 0) {
            rc = -1;
        } else {
            child->data_tickets = q;
            child->pt_tickets = pt;
            child->quota[MB_ANON] = parent->quota[MB_ANON];
            child->quota[MB_STACK] = parent->quota[MB_STACK];
            child->quota[MB_IMAGE] = parent->quota[MB_IMAGE];
            child->quota[MB_PGTABLE] += pt;
            /* 第二筆帳:父的新增票。只動實體預留(那些頁早已算在父的配額裡) */
            if (pmm_reserve(pending) != 0)
                rc = -1;                     /* 子的帳由下面的 destroy 退回 */
        }
    }
    if (rc == 0) {
        struct fork_anon *fap = dom && parent->dom ? &fa : NULL;
        unsigned lo = va_index(USER_BASE, top), hi = va_index(USER_TOP - 1, top);
        rc = fork_table(child, parent->root, top, 0, lo, hi, fap, 0);      /* 第一趟:只建子空間 */
        if (rc == 0)
            fork_table(child, parent->root, top, 0, lo, hi, fap, 1);       /* 第二趟:標記父,不會失敗 */
        if (fap) {
            if (rc == 0) {
                if (fa.marked != pending)
                    panic("vm: fork marked %lu parent pages but reserved %lu", fa.marked, pending);
                parent->data_tickets += fa.marked;      /* 待生效的票轉為正式 */
            } else {
                pmm_unreserve(pending);                 /* 失敗:父的待生效票全部退回,父的 PTE 沒有被改過 */
            }
        }
    }
    spin_unlock_irqrestore(&vm_lk, irq);

    /* parent 有些頁剛從可寫變成唯讀:它在各顆 CPU 上的舊紀錄(還寫著「可寫」)都要清掉,
       否則它之後的寫入不會觸發 page fault,兩邊就會改到同一份資料 */
    unsigned long f = arch_irq_save();
    space_flush_fn(parent);
    arch_irq_restore(f);
    smp_call_mask(parent->used_cpus, space_flush_fn, parent);

    if (rc != 0) {
        vm_space_destroy(child);             /* 只退子空間自己的帳;已經對映的部分各自歸還參考 */
        return NULL;
    }
    return child;
}

int vm_space_fault(struct vm_space *vs, uintptr_t va, int write)
{
    if (!write || !user_range(va))
        return -1;
    va &= ~(uintptr_t)(PAGE_SIZE - 1);

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int level = 0, rc = -1;
    pte_t *e = walk(vs, vs->root, va, 0, 0, 0, WALK_LEGACY, &level);

    if (e && arch_pte_valid(*e) && level == 0) {
        unsigned flags = arch_pte_flags(*e);
        uintptr_t pa = arch_pte_addr(*e);

        if (!(flags & VM_USER)) {
            /* kernel 的頁:不是我們要處理的 */
        } else if (flags & VM_WRITE) {
            /* 已經可寫了:同一行程的另一條執行緒剛剛在別顆 CPU 上處理完同一頁。
               清掉自己這顆的舊紀錄,重試就會成功 */
            arch_tlb_flush(va);
            rc = 0;
        } else if (flags & VM_COW) {
            unsigned nflags = (flags & ~(VM_COW | VM_TICKET)) | VM_WRITE;   /* 兩條分支都清掉 COW 與 TICKET */

            if (pmm_refcount((void *)pa) == 1) {
                /* 只剩自己在用(其他人都已經複製走或結束了):不必複製,改回可寫即可。
                   有票的映射(R1)從此是獨佔的,不再需要那張票:退回 */
                arch_pte_set(e, arch_pte_leaf(pa, nflags, 0));
                if (flags & VM_TICKET) {
                    if (vs->data_tickets == 0)
                        panic("vm: ticketed copy-on-write page without a ticket");
                    vs->data_tickets--;
                    pmm_unreserve(1);
                }
                cow_reuses++;
                rc = 0;
            } else {
                /* 有票的映射用自己的票複製:一定成功(I2);沒有票的照舊一般配置,可能失敗 */
                void *copy;
                if (flags & VM_TICKET) {
                    if (vs->data_tickets == 0)
                        panic("vm: ticketed copy-on-write page without a ticket");
                    vs->data_tickets--;
                    copy = pmm_alloc_reserved();
                } else {
                    copy = pmm_alloc();
                }
                if (copy) {
                    memcpy(copy, (const void *)pa, PAGE_SIZE);
                    arch_pte_set(e, arch_pte_leaf((uintptr_t)copy, nflags, 0));
                    pmm_free((void *)pa);    /* 放棄原本那一頁的參考;別人還在用,不會被釋放 */
                    cow_copies++;
                    rc = 0;
                }
                /* 配置失敗:回傳 -1,這個行程會被終止 */
            }
            if (rc == 0)
                tlb_shootdown_space(vs, va); /* 同一行程的其他執行緒可能在別顆 CPU 上 */
        }
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

void vm_space_switch_flush(struct vm_space *vs)
{
    __atomic_fetch_or(&vs->used_cpus, 1u << this_cpu()->id, __ATOMIC_SEQ_CST);
    arch_mmu_switch(vs->root, vs->asid, 1);
}

void vm_space_switch(struct vm_space *vs)
{
    if (!vs) {
        arch_mmu_switch(root, 0, 0);         /* kernel 頁表:編號 0,裡面沒有使用者的頁 */
        return;
    }
    /* 記下「這顆 CPU 載入過它」。要在真正載入之前記:之後任何人看到這一位是 0,
       就能確定這顆 CPU 沒有、也不曾有過它的任何 TLB 紀錄 */
    __atomic_fetch_or(&vs->used_cpus, 1u << this_cpu()->id, __ATOMIC_SEQ_CST);
    /* 有自己編號的位址空間不必清空 TLB;共用編號的(或不支援 ASID 的架構)才要 */
    arch_mmu_switch(vs->root, vs->asid, vs->asid == MMU_ASID_MAX);
}

void vm_enable_secondary(void)
{
    arch_mmu_enable(root);
}

uintptr_t vm_kernel_root(void)
{
    return (uintptr_t)root;
}

unsigned long vm_table_pages(void)
{
    return table_pages;
}

void vm_init(const struct boot_info *bi)
{
    uintptr_t ram_start = (uintptr_t)__kernel_start;
    uintptr_t ram_end = pmm_mem_start() + (uintptr_t)pmm_total_pages() * PAGE_SIZE;
    unsigned long npages = 0;

    arch_mmu_init();                         /* 偵測 CPU 功能(決定項目格式要不要帶 NX 等位元) */

    root = table_alloc();
    if (!root)
        panic("vm: out of memory for page table");

    /*
     * 1. 主記憶體:從 kernel 映像開頭到記憶體結尾,恆等對映。
     *    權限依內容決定,這就是所謂的 W^X(可寫的不可執行,可執行的不可寫):
     *      程式碼  .text    唯讀 + 可執行:防止程式碼被改寫
     *      常數    .rodata  唯讀
     *      其餘(.data、.bss、heap、stack、空閒頁) 可讀寫、不可執行:
     *              防止把資料當成程式碼執行
     */
    for (uintptr_t p = ram_start; p < ram_end; p += PAGE_SIZE) {
        unsigned flags;

        /*
         * 能用大分頁就用:位址對齊、整塊都在記憶體範圍內、而且整塊權限相同
         * (已經過了 kernel 的程式碼與常數)。好處:
         *   - TLB 一筆紀錄就涵蓋 2MB / 4MB,而不是 4KB,TLB miss 大幅減少
         *   - 少走一層頁表;也省下最後一層表格本身佔用的記憶體
         * 之後若要單獨修改其中一頁,vm_map / vm_unmap 會自動把它拆開。
         */
        if ((p & (LARGE_SIZE - 1)) == 0 && p >= (uintptr_t)__rodata_end &&
            ram_end - p >= LARGE_SIZE) {
            if (map_large(p, p, VM_READ | VM_WRITE) != 0)
                panic("vm: cannot map %p", (void *)p);
            npages += LARGE_SIZE / PAGE_SIZE;
            p += LARGE_SIZE - PAGE_SIZE;     /* 迴圈本身還會再加一頁 */
            continue;
        }

        if (p < (uintptr_t)__text_end)
            flags = VM_READ | VM_EXEC;
        else if (p < (uintptr_t)__rodata_end)
            flags = VM_READ;
        else
            flags = VM_READ | VM_WRITE;
        if (vm_map(p, p, flags) != 0)
            panic("vm: cannot map %p", (void *)p);
        npages++;
    }

    /* 2. bootloader 留下的資料(裝置樹、開機參數):唯讀。
          kernel 只會讀它們,設成唯讀可以擋掉誤寫。
          若落在上面的大分頁裡,vm_map 會把那一塊拆成 4KB 頁再改這幾頁 */
    for (unsigned i = 0; i < bi->n_reserved; i++) {
        uintptr_t base = (uintptr_t)bi->reserved[i].base & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t end = (uintptr_t)(bi->reserved[i].base + bi->reserved[i].size);
        for (uintptr_t p = base; p < end; p += PAGE_SIZE) {
            if (vm_map(p, p, VM_READ) != 0)
                panic("vm: cannot map reserved %p", (void *)p);
        }
    }

    /* 3. 裝置暫存器(序列埠、中斷控制器、VGA…),位置依架構而定 */
    arch_mmu_map_devices();

    /* 位址 0 所在的頁刻意不對映:解參考 NULL 指標會立刻觸發 page fault */

    /* 4. 開啟。這一行之後,CPU 的每次記憶體存取都會經過頁表 */
    arch_mmu_enable(root);
    enabled = 1;

    kprintf("vm: paging enabled (%s), %lu MB mapped, %lu large pages of %lu MB, "
            "%lu page-table pages\n", MMU_NAME, npages / 256, large_pages,
            (unsigned long)(LARGE_SIZE >> 20), table_pages);
    kprintf("vm: text %p-%p r-x, rodata -%p r--, rest rw-\n",
            (void *)__kernel_start, (void *)__text_end, (void *)__rodata_end);
}

/* ---- 匿名記憶體的承諾、兌現、歸還(I-240 C3.2 (b1) 第 3 步;設計 B3 / BR5) ---- */

/* [start, end) 在第 l 層(l = 0 是最底層的表)需要幾張表 */
static unsigned long tables_at(uintptr_t start, uintptr_t end, int l)
{
    unsigned sh = PAGE_SHIFT + (unsigned)(l + 1) * MMU_BITS;
    return (unsigned long)(((end - 1) >> sh) - (start >> sh) + 1);
}

/* 範圍需要的中間表數,假設一張都還不存在(根表不算) */
static unsigned long pt_need(uintptr_t va, uintptr_t len)
{
    unsigned long n = 0;
    for (int l = 0; l < MMU_LEVELS - 1; l++)
        n += tables_at(va, va + len, l);
    return n;
}

void vm_set_anon_windows(const uintptr_t base[], const uintptr_t len[], unsigned n)
{
    KASSERT(n <= ANON_WIN_MAX && n_anon_win == 0);
    for (unsigned i = 0; i < n; i++) {
        KASSERT(len[i] > 0 && (base[i] & (PAGE_SIZE - 1)) == 0 && user_range(base[i]) &&
                user_range(base[i] + len[i] - 1));
        KASSERT(i == 0 || base[i] >= base[i - 1] + len[i - 1]);   /* 由低到高、不重疊 */
        anon_win[i].base = base[i];
        anon_win[i].len = len[i];
    }
    n_anon_win = n;
    /* M:每一層把各區域的表數加起來,再扣掉相鄰兩區共用的同一張表 */
    unsigned long m = 0;
    for (int l = 0; l < MMU_LEVELS - 1; l++) {
        unsigned sh = PAGE_SHIFT + (unsigned)(l + 1) * MMU_BITS;
        for (unsigned i = 0; i < n; i++) {
            m += tables_at(base[i], base[i] + len[i], l);
            if (i > 0 && ((base[i - 1] + len[i - 1] - 1) >> sh) == (base[i] >> sh))
                m--;
        }
    }
    anon_pt_max = m;
}

unsigned long vm_anon_pt_max(void) { return anon_pt_max; }

int vm_anon_window(unsigned i, uintptr_t *base, uintptr_t *len)
{
    if (i >= n_anon_win)
        return -1;
    *base = anon_win[i].base;
    *len = anon_win[i].len;
    return 0;
}

int vm_space_commit_anon(struct vm_space *vs, uintptr_t va, uintptr_t len)
{
    return vm_space_commit(vs, MB_ANON, va, len);
}

int vm_space_commit(struct vm_space *vs, enum mb_class cls, uintptr_t va, uintptr_t len)
{
    if (cls != MB_ANON && cls != MB_STACK)
        return -E_INVAL;
    if (!vs->dom || len == 0 || ((va | len) & (PAGE_SIZE - 1)) || !user_range(va) ||
        va + len < va || !user_range(va + len - 1))
        return -E_INVAL;
    unsigned long n = (unsigned long)(len / PAGE_SIZE);

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    if (vs->anon_tables + vs->pt_tickets > anon_pt_max)
        panic("vm: page-table tickets exceed the anonymous-region bound");
    unsigned long room = anon_pt_max - vs->anon_tables - vs->pt_tickets;
    unsigned long pt = pt_need(va, len);
    if (pt > room)
        pt = room;
    struct mb_bundle b = { .reserve = n + pt };
    b.quota[cls] = n;
    b.quota[MB_PGTABLE] = pt;
    int rc = -E_NOMEM;
    if (n + pt >= n && mb_commit_bundle_txn(vs->dom, &b, vs->txn) == 0) {   /* 完成點:配額與預留一起 */
        vs->data_tickets += n;
        vs->pt_tickets += pt;
        vs->quota[cls] += n;
        vs->quota[MB_PGTABLE] += pt;
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

int vm_space_fault_anon(struct vm_space *vs, uintptr_t va)
{
    va &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (!vs->dom || !user_range(va))
        return -1;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    int rc = 1;
    pte_t *e = walk(vs, vs->root, va, 0, 1, 0, WALK_ANON, NULL);   /* 缺的表用預付的票:不會失敗 */
    if (!arch_pte_valid(*e)) {
        if (vs->data_tickets == 0)
            panic("vm: anonymous fault at %p without a ticket", (void *)va);
        vs->data_tickets--;
        void *page = pmm_alloc_reserved();
        memset(page, 0, PAGE_SIZE);          /* 不把別人用過的資料交給行程 */
        arch_pte_set(e, arch_pte_leaf((uintptr_t)page, VM_READ | VM_WRITE | VM_USER, 0));
        vs->pages++;
        if (enabled)
            arch_tlb_flush(va);
        rc = 0;
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return rc;
}

#define RELEASE_SEGMENT 64u                  /* 每段最多處理幾頁就放一次鎖 */

void vm_space_release_anon(struct vm_space *vs, uintptr_t va, uintptr_t len)
{
    vm_space_release(vs, MB_ANON, va, len);
}

void vm_space_release(struct vm_space *vs, enum mb_class cls, uintptr_t va, uintptr_t len)
{
    KASSERT(vs->dom && ((va | len) & (PAGE_SIZE - 1)) == 0 && (cls == MB_ANON || cls == MB_STACK));
    unsigned long total = (unsigned long)(len / PAGE_SIZE);

    for (unsigned long done = 0; done < total; ) {
        unsigned long seg = total - done < RELEASE_SEGMENT ? total - done : RELEASE_SEGMENT;
        unsigned long tickets = 0;
        unsigned long irq = spin_lock_irqsave(&vm_lk);
        for (unsigned long i = 0; i < seg; i++) {
            uintptr_t a = va + (done + i) * PAGE_SIZE;
            int level = 0;
            pte_t *e = walk(vs, vs->root, a, 0, 0, 0, WALK_LEGACY, &level);
            /* 每一頁都在鎖內重新讀 PTE:段與段之間 COW 快路徑仍可能改過它 */
            if (!e || !arch_pte_valid(*e)) {
                tickets++;                   /* 未兌現:退一張票 */
                continue;
            }
            KASSERT(level == 0);
            unsigned flags = arch_pte_flags(*e);
            uintptr_t pa = arch_pte_addr(*e);
            arch_pte_set(e, 0);
            vs->pages--;
            if (enabled)
                tlb_shootdown_space(vs, a);
            if (flags & VM_TICKET)
                tickets++;                   /* 共用中的 COW:不再需要複製,退票 */
            if (!(flags & VM_SHARED))
                pmm_free((void *)pa);        /* 獨佔的頁釋放;共用的只減參考 */
        }
        if (tickets > vs->data_tickets)
            panic("vm: releasing more tickets than the address space holds");
        struct mb_bundle b = { .reserve = tickets };
        b.quota[cls] = seg;
        if (mb_release_bundle(vs->dom, &b) != 0)
            panic("vm: committed-memory release does not match the domain");
        vs->data_tickets -= tickets;
        vs->quota[cls] -= seg;
        spin_unlock_irqrestore(&vm_lk, irq);
        done += seg;
    }
}

void vm_space_ledger(struct vm_space *vs, struct vm_ledger *out)
{
    unsigned long irq = spin_lock_irqsave(&vm_lk);
    out->data_tickets = vs->data_tickets;
    out->pt_tickets = vs->pt_tickets;
    out->anon_tables = vs->anon_tables;
    out->anon_quota = vs->quota[MB_ANON];
    out->pt_quota = vs->quota[MB_PGTABLE];
    spin_unlock_irqrestore(&vm_lk, irq);
}

/*
 * 帶配額的立即配置(I-240 C3.2 (c2),設計 1.3):在 va 配置一頁清 0 的私有頁並對映,記 quota[cls] += 1。
 * 用於程式映像的私有頁:建立時就配置好,不留未兌現的票。全有或全無:
 *   配額(mb_lk)→ 一般配置(pmm 的 N2 檢查)→ walk(WALK_LEGACY,缺表就建、記 PGTABLE)→ 寫 PTE。
 * 任一步失敗:撤回這一頁的配額、釋放這一頁,回傳 NULL;途中新建的中間表留在空間上(已記 PGTABLE,
 * destroy 時一起退,與 legacy 建表相同)。va 已經有對映也算失敗(不覆寫)
 */
void *vm_space_map_charged(struct vm_space *vs, enum mb_class cls, uintptr_t va, unsigned flags)
{
    flags |= VM_USER;
    if (!vs->dom || (va & (PAGE_SIZE - 1)) || !flags_ok(flags) || !user_range(va) ||
        (flags & (VM_SHARED | VM_COW | VM_TICKET)))
        return NULL;

    unsigned long irq = spin_lock_irqsave(&vm_lk);
    void *page = NULL;
    if (mb_quota_add_txn(vs->dom, cls, 1, vs->txn) == 0) {
        page = pmm_alloc();
        pte_t *e = page ? walk(vs, vs->root, va, 0, 1, 0, WALK_LEGACY, NULL) : NULL;
        if (e && !arch_pte_valid(*e)) {
            memset(page, 0, PAGE_SIZE);
            arch_pte_set(e, arch_pte_leaf((uintptr_t)page, flags, 0));
            vs->pages++;
            vs->quota[cls]++;
            if (enabled)
                arch_tlb_flush(va);
        } else {
            if (page)
                pmm_free(page);
            page = NULL;
            if (mb_quota_sub(vs->dom, cls, 1) != 0)
                panic("vm: charged-map quota rollback mismatch");
        }
    }
    spin_unlock_irqrestore(&vm_lk, irq);
    return page;
}

void vm_space_clear_txn(struct vm_space *vs)
{
    unsigned long irq = spin_lock_irqsave(&vm_lk);
    vs->txn = NULL;                          /* 之後對這個空間的記帳回到一般上限 */
    spin_unlock_irqrestore(&vm_lk, irq);
}

unsigned long vm_space_quota_total(struct vm_space *vs)
{
    unsigned long irq = spin_lock_irqsave(&vm_lk), t = 0;
    for (unsigned i = 0; i < MB_NCLASS; i++)
        t += vs->quota[i];
    spin_unlock_irqrestore(&vm_lk, irq);
    return t;
}
