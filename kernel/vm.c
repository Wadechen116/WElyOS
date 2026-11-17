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

#define ENTRIES   (1u << MMU_BITS)           /* 每個表格的項目數 */
#define IDX_MASK  (ENTRIES - 1)

/* linker script 定義:kernel 映像各區段的邊界(都對齊到頁) */
extern char __kernel_start[], __text_end[], __rodata_end[];

static pte_t *root;                          /* kernel 頁表的根表格 */

struct vm_space {
    pte_t *root;                             /* 這個位址空間的根表格 */
    unsigned asid;                           /* 位址空間編號(見 mmu.h);不支援的架構恆為 0 */
};

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
        t[i] = arch_pte_leaf(pa + i * step, flags, level - 1);

    *e = arch_pte_table((uintptr_t)t, 0, level);
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
static pte_t *walk(pte_t *t, uintptr_t va, int target, int create, int split,
                   int user, int *out_level)
{
    int level;

    if (MMU_VA_BITS < sizeof(uintptr_t) * 8 && (va >> MMU_VA_BITS) != 0)
        return NULL;                         /* 超出頁表能表示的位址範圍 */

    for (level = MMU_LEVELS - 1; level > target; level--) {
        pte_t *e = &t[va_index(va, level)];
        if (!arch_pte_valid(*e)) {
            if (!create)
                return NULL;
            pte_t *next = table_alloc();
            if (!next)
                return NULL;
            *e = arch_pte_table((uintptr_t)next, user, level);
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
static int flags_ok(unsigned flags)
{
    return !((flags & VM_WRITE) && (flags & VM_EXEC));
}

int vm_map(uintptr_t va, uintptr_t pa, unsigned flags)
{
    if (((va | pa) & (PAGE_SIZE - 1)) || !flags_ok(flags))
        return -1;

    unsigned long irq = arch_irq_save();
    pte_t *e = walk(root, va, 0, 1, 1, 0, NULL);
    int rc = -1;
    if (e) {
        *e = arch_pte_leaf(pa, flags & ~VM_USER, 0);     /* kernel 頁表不放使用者頁 */
        /* CPU 會把查過的轉換結果快取在 TLB;改了 PTE 要讓那一筆失效,
           否則 CPU 可能繼續用舊的對映 */
        if (enabled)
            arch_tlb_flush(va);
        rc = 0;
    }
    arch_irq_restore(irq);
    return rc;
}

/* 以一個大分頁對映 [va, va + LARGE_SIZE)。只在 vm_init 建立初始對映時使用 */
static int map_large(uintptr_t va, uintptr_t pa, unsigned flags)
{
    pte_t *e = walk(root, va, MMU_LARGE_LEVEL, 1, 0, 0, NULL);
    if (!e)
        return -1;
    *e = arch_pte_leaf(pa, flags, MMU_LARGE_LEVEL);
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

    unsigned long irq = arch_irq_save();
    pte_t *e = walk(root, va, 0, 0, 1, 0, NULL);   /* 落在大分頁裡就先拆開,只移除這一頁 */
    int rc = -1;
    if (e && arch_pte_valid(*e)) {
        *e = 0;
        if (enabled)
            arch_tlb_flush(va);
        rc = 0;
    }
    arch_irq_restore(irq);
    return rc;
}

int vm_query(uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    unsigned long irq = arch_irq_save();
    int level = 0;
    pte_t *e = walk(root, va, 0, 0, 0, 0, &level);
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
    arch_irq_restore(irq);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 位址空間                                                            */
/* ------------------------------------------------------------------ */

static int user_range(uintptr_t va)
{
    return va >= USER_BASE && va < USER_TOP;
}

struct vm_space *vm_space_create(void)
{
    struct vm_space *vs = kmalloc(sizeof(*vs));
    if (!vs)
        return NULL;

    unsigned long irq = arch_irq_save();
    vs->root = table_alloc();
    if (vs->root) {
        /* 複製 kernel 頁表最上層的所有項目。下層表格因此是共用的:之後 kernel
           在既有範圍內的修改(例如拆開大分頁、移除保護頁)所有位址空間都看得到 */
        memcpy(vs->root, root, PAGE_SIZE);
        vs->asid = asid_alloc();
    }
    arch_irq_restore(irq);

    if (!vs->root) {
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
        else
            pmm_free((void *)arch_pte_addr(t[i]));       /* 行程擁有的實體頁 */
    }
    pmm_free(t);
    table_pages--;
}

void vm_space_destroy(struct vm_space *vs)
{
    unsigned long irq = arch_irq_save();
    int top = MMU_LEVELS - 1;

    /* 只處理最上層中屬於使用者範圍的項目;其餘是和 kernel 共用的,不能動 */
    for (unsigned i = va_index(USER_BASE, top); i <= va_index(USER_TOP - 1, top); i++) {
        if (arch_pte_valid(vs->root[i]))
            free_table((pte_t *)arch_pte_addr(vs->root[i]), top - 1);
    }
    pmm_free(vs->root);
    table_pages--;
    asid_free(vs->asid);
    arch_irq_restore(irq);
    kfree(vs);
}

int vm_space_map(struct vm_space *vs, uintptr_t va, uintptr_t pa, unsigned flags)
{
    flags |= VM_USER;
    if (((va | pa) & (PAGE_SIZE - 1)) || !flags_ok(flags) || !user_range(va))
        return -1;

    unsigned long irq = arch_irq_save();
    pte_t *e = walk(vs->root, va, 0, 1, 0, 1, NULL);
    int rc = -1;
    if (e) {
        *e = arch_pte_leaf(pa, flags, 0);
        if (enabled)
            arch_tlb_flush(va);
        rc = 0;
    }
    arch_irq_restore(irq);
    return rc;
}

int vm_space_query(struct vm_space *vs, uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    if (!user_range(va))
        return -1;

    unsigned long irq = arch_irq_save();
    int level = 0;
    pte_t *e = walk(vs->root, va, 0, 0, 0, 0, &level);
    int rc = -1;
    if (e && arch_pte_valid(*e) && level == 0 && (arch_pte_flags(*e) & VM_USER)) {
        if (pa)
            *pa = arch_pte_addr(*e) | (va & (PAGE_SIZE - 1));
        if (flags)
            *flags = arch_pte_flags(*e);
        rc = 0;
    }
    arch_irq_restore(irq);
    return rc;
}

void vm_space_switch(struct vm_space *vs)
{
    if (!vs) {
        arch_mmu_switch(root, 0, 0);         /* kernel 頁表:編號 0,裡面沒有使用者的頁 */
        return;
    }
    /* 有自己編號的位址空間不必清空 TLB;共用編號的(或不支援 ASID 的架構)才要 */
    arch_mmu_switch(vs->root, vs->asid, vs->asid == MMU_ASID_MAX);
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
