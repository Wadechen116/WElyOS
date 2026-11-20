/*
 * pmm.c - 實體記憶體管理員:以位元圖(bitmap)管理頁面
 *
 * 每一頁用 1 個位元表示:1 = 使用中(或不可用),0 = 空閒。
 * 128MB 記憶體 = 32768 頁,位元圖只需 4KB。
 *
 * 選位元圖而非鏈結串列(free list)的理由:
 *   - 能配置「連續的多頁」(heap 擴充、之後的 kernel stack 都需要)
 *   - 能檢查重複釋放(該位元已是 0)
 *   - 管理資料不放在被管理的頁面裡,頁面內容被寫壞也不影響管理
 * 代價是配置要掃描位元圖,為 O(頁數);以「最低空閒頁提示」減少掃描。
 *
 * 配置與釋放以一把 spinlock 保護(見 spinlock.h):多顆 CPU 可能同時配置,
 * 也可能在中斷處理程式中配置,所以上鎖的同時也關中斷。
 */
#include <stdint.h>
#include "pmm.h"
#include "spinlock.h"
#include "arch.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "uapi/errno.h"

/* 保護位元圖與相關計數。多顆 CPU 可能同時配置,只關中斷已經不夠 */
static struct spinlock pmm_lk = SPINLOCK_INIT("pmm", LK_PMM);

/* linker script 定義的符號:kernel 映像的範圍。只取位址,不讀內容 */
extern char __kernel_start[], __kernel_end[];

static uint8_t  *bitmap;      /* 位元圖本身(放在 kernel 映像之後) */
static uint16_t *refs;        /* 每頁的參考計數(緊接在位元圖之後);0 = 空閒 */
static uintptr_t mem_start;   /* 第 0 頁的實體位址 */
static size_t    npages;      /* 管理的總頁數 */
static size_t    nfree;       /* 目前空閒頁數 */
static size_t    hint;        /* 比它小的頁都不是空閒的:掃描的起點 */
/*
 * kernel 的保留底線(I-240、第 52 步 C3.1)。啟用之後,一般的配置(pmm_alloc /
 * pmm_alloc_pages,以及經由它們的 kmalloc、頁表、stack…)不能讓空閒頁數低於它:
 * 檢查與配置在同一把鎖裡完成,多顆 CPU 同時配置也不會一起跌破。
 * 只有明確命名的 pmm_alloc_pages_reserve 可以動用這一塊。
 * 預設保護而不是「使用者的配置另外走一個會檢查的函式」:後者只要漏掉一條
 * 間接的路徑(kmalloc 擴充、頁表、ramfs 的區塊表…)底線就形同虛設。
 */
static size_t    floor_pages; /* 0 = 尚未啟用 */
static int       floor_on;
static unsigned long floor_refusals;   /* 因為底線(或預留)而拒絕的配置次數(診斷用) */
/*
 * 預留(I-240 C3.2 第 (a) 批):已經承諾、但還沒真正配置的頁數。
 * 不變量 N1:nreserved <= nfree,永遠成立 — 持有預留的一方兌現一頁一定拿得到。
 * 不變量 N2:nfree >= floor + nreserved,在沒有應急配置欠帳時成立。
 *   一般配置與新的預留:配置 / 預留之後 N2 仍要成立;
 *   應急配置(pmm_alloc_pages_reserve):只要求 N1 — 可以吃進底線,不能吃進別人的預留;
 *   兌現預留:nfree 與 nreserved 同時減一,兩條都不受影響。
 * 檢查一律寫成「先算 avail = nfree - nreserved(由 N1 保證不下溢),再和 floor、n 比較」,
 * 不做 floor + nreserved 的加法。
 */
static size_t    nreserved;

#define ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))
#define ALIGN_DOWN(x) ((x) & ~(uint64_t)(PAGE_SIZE - 1))

static int  bit_test(size_t i)  { return bitmap[i / 8] & (1u << (i % 8)); }
static void bit_set(size_t i)   { bitmap[i / 8] |= (uint8_t)(1u << (i % 8)); }
static void bit_clear(size_t i) { bitmap[i / 8] &= (uint8_t)~(1u << (i % 8)); }

/* 兩個範圍 [a, a+alen) 與 [b, b+blen) 是否重疊 */
enum { ALLOC_NORMAL, ALLOC_EMERGENCY, ALLOC_RESERVED };
static void *alloc_pages_locked(size_t n, int mode);

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

void pmm_init(const struct boot_info *bi)
{
    if (bi->mem_size == 0)
        panic("pmm: memory size unknown");

    /* 只管理完整的頁:起點向上、終點向下對齊 */
    uint64_t start = ALIGN_UP(bi->mem_base);
    uint64_t end   = ALIGN_DOWN(bi->mem_base + bi->mem_size);
    if (end > UINTPTR_MAX)                 /* 32 位元架構無法定址 4GB 以上 */
        end = ALIGN_DOWN((uint64_t)UINTPTR_MAX);

    mem_start = (uintptr_t)start;
    npages    = (size_t)((end - start) >> PAGE_SHIFT);
    size_t bitmap_bytes = (npages + 7) / 8;
    /* 位元圖之後緊接著每頁 2 位元組的參考計數。以下的 bm_bytes 是兩者合計的大小 */
    size_t refs_off = (bitmap_bytes + 7) & ~(size_t)7;
    size_t bm_bytes = refs_off + npages * sizeof(uint16_t);

    /*
     * 決定位元圖放哪裡:從 kernel 映像結尾開始找,避開 bootloader
     * 保留的範圍。每次碰到重疊就跳到該範圍之後,再從頭檢查一次。
     */
    uint64_t bm = ALIGN_UP((uint64_t)(uintptr_t)__kernel_end);
    for (unsigned i = 0; i < bi->n_reserved; i++) {
        const struct mem_region *r = &bi->reserved[i];
        if (overlaps(bm, bm_bytes, r->base, r->size)) {
            bm = ALIGN_UP(r->base + r->size);
            i = (unsigned)-1;              /* 迴圈遞增後回到 0,重新檢查 */
        }
    }
    if (bm < start || bm + bm_bytes > end)
        panic("pmm: no room for bitmap");
    bitmap = (uint8_t *)(uintptr_t)bm;
    refs = (uint16_t *)(uintptr_t)(bm + refs_off);

    /* 先全部標成使用中,再把確定可用的頁逐一放出來。
       這樣即使漏判,結果是「少用幾頁」而不是「把別人的資料配置出去」 */
    memset(bitmap, 0xFF, bitmap_bytes);
    memset(refs, 0, npages * sizeof(uint16_t));
    nfree = 0;
    hint  = npages;
    for (size_t i = 0; i < npages; i++) {
        uint64_t p = start + ((uint64_t)i << PAGE_SHIFT);

        /* kernel 映像以及它之前的部分(RISC-V 的 OpenSBI、AArch64 開頭的
           保留區都在 kernel 之前)一律不使用 */
        if (p < (uint64_t)(uintptr_t)__kernel_end)
            continue;
        if (overlaps(p, PAGE_SIZE, bm, bm_bytes))      /* 位元圖自己 */
            continue;

        int reserved = 0;
        for (unsigned r = 0; r < bi->n_reserved && !reserved; r++)
            reserved = overlaps(p, PAGE_SIZE, bi->reserved[r].base,
                                bi->reserved[r].size);
        if (reserved)
            continue;

        bit_clear(i);
        nfree++;
        if (i < hint)
            hint = i;
    }

    kprintf("pmm: kernel %p-%p, bitmap %p (%zu bytes)\n",
            (void *)__kernel_start, (void *)__kernel_end,
            (void *)bitmap, bm_bytes);
    for (unsigned i = 0; i < bi->n_reserved; i++)
        kprintf("pmm: reserved 0x%llx-0x%llx\n",
                (unsigned long long)bi->reserved[i].base,
                (unsigned long long)(bi->reserved[i].base + bi->reserved[i].size));
    kprintf("pmm: %zu pages total, %zu free (%zu KB)\n",
            npages, nfree, nfree * (PAGE_SIZE / 1024));
}

static void *alloc_pages_ref(size_t n, int mode)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    void *p = alloc_pages_locked(n, mode);
    if (p) {
        size_t first = ((uintptr_t)p - mem_start) >> PAGE_SHIFT;
        for (size_t i = first; i < first + n; i++)
            refs[i] = 1;                       /* 剛配置:一個擁有者 */
    }
    spin_unlock_irqrestore(&pmm_lk, flags);
    return p;
}

void *pmm_alloc_pages(size_t n)
{
    return alloc_pages_ref(n, ALLOC_NORMAL);
}

void *pmm_alloc_pages_reserve(size_t n)
{
    return alloc_pages_ref(n, ALLOC_EMERGENCY);
}

/* 一般配置與新的預留共用的判斷:拿走 n 頁之後,未預留的空閒頁仍不低於底線(N2)。持有 pmm_lk */
static int fits_normal_locked(size_t n)
{
    size_t avail = nfree - nreserved;          /* N1 保證不下溢 */
    if (floor_on)
        return floor_pages <= avail && n <= avail - floor_pages;
    return n <= avail;
}

int pmm_reserve(size_t n)
{
    if (n == 0)
        return 0;                              /* 什麼都不預留:成功,不改任何狀態 */
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    int rc = -E_NOMEM;
    if (fits_normal_locked(n)) {               /* n <= avail - floor <= nfree - nreserved:加總不溢位 */
        nreserved += n;
        rc = 0;
    } else {
        floor_refusals++;
    }
    spin_unlock_irqrestore(&pmm_lk, flags);
    return rc;
}

void pmm_unreserve(size_t n)
{
    if (n == 0)
        return;
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    if (n > nreserved)                         /* 帳務錯誤:先判斷,不改任何狀態就停下 */
        panic("pmm: unreserve of %zu pages but only %zu are reserved", n, nreserved);
    nreserved -= n;
    spin_unlock_irqrestore(&pmm_lk, flags);
}

void *pmm_alloc_reserved(void)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    if (nreserved == 0)
        panic("pmm: allocation from an empty reservation");
    /* N1:nfree >= nreserved >= 1,一定有一頁空閒;單頁不受碎片影響 */
    void *p = alloc_pages_locked(1, ALLOC_RESERVED);
    if (!p)
        panic("pmm: reserved page missing (free %zu, reserved %zu)", nfree, nreserved);
    nreserved--;
    refs[((uintptr_t)p - mem_start) >> PAGE_SHIFT] = 1;
    spin_unlock_irqrestore(&pmm_lk, flags);
    return p;
}

size_t pmm_reserved_count(void)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    size_t value = nreserved;
    spin_unlock_irqrestore(&pmm_lk, flags);
    return value;
}

void pmm_snapshot(struct pmm_snapshot *out)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    out->free = nfree;
    out->reserved = nreserved;
    out->floor = floor_pages;
    out->refusals = floor_refusals;
    spin_unlock_irqrestore(&pmm_lk, flags);
}

int pmm_reserve_enable(size_t pages)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    int rc = -1;
    /* 只能啟用一次;不能設成比「未預留的空閒頁」還多,否則一啟用 N2 就不成立 */
    if (!floor_on && pages > 0 && pages <= nfree - nreserved) {
        floor_pages = pages;
        floor_on = 1;
        rc = 0;
    }
    spin_unlock_irqrestore(&pmm_lk, flags);
    return rc;
}

size_t pmm_reserve_count(void)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    size_t value = floor_pages;
    spin_unlock_irqrestore(&pmm_lk, flags);
    return value;
}
unsigned long pmm_reserve_refusals(void)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    unsigned long value = floor_refusals;
    spin_unlock_irqrestore(&pmm_lk, flags);
    return value;
}

/* 實際的配置邏輯;呼叫時已持有 pmm_lk */
static void *alloc_pages_locked(size_t n, int mode)
{
    if (n == 0 || n > nfree)
        return NULL;
    if (mode == ALLOC_NORMAL && !fits_normal_locked(n)) {
        floor_refusals++;
        return NULL;
    }
    if (mode == ALLOC_EMERGENCY && n > nfree - nreserved) {   /* 應急:可以吃底線,不能吃預留(N1) */
        floor_refusals++;
        return NULL;
    }
    /* ALLOC_RESERVED:呼叫者已確認 nreserved > 0,兌現的這一頁本來就算在預留裡 */

    /* first-fit:從 hint 往後找第一段連續 n 個 0 */
    size_t run = 0;                        /* 目前連續空閒的頁數 */
    size_t first_free = npages;            /* 這次掃描遇到的第一個空閒頁 */
    for (size_t i = hint; i < npages; i++) {
        /* 加速:位於位元組邊界且整個位元組都是 1(8 頁全在使用中),一次跳過 */
        if (run == 0 && (i & 7) == 0 && bitmap[i / 8] == 0xFF) {
            i += 7;
            continue;
        }
        if (bit_test(i)) {
            run = 0;
            continue;
        }
        if (first_free == npages)
            first_free = i;
        if (++run < n)
            continue;

        size_t first = i + 1 - n;
        for (size_t j = first; j <= i; j++)
            bit_set(j);
        nfree -= n;
        /*
         * 更新 hint,維持「比 hint 小的頁都不是空閒的」:
         *   - 配置到的就是掃描中遇到的第一個空閒頁:它之前全是使用中,
         *     所以下次可以直接從這段之後開始找
         *   - 否則(前面有不夠長的空洞被跳過):hint 移到那個空洞
         * 重點是 hint 一定要跨過「已使用」的區段往前推進。若只在
         * first == hint 時才更新,hint 一旦停在使用中的頁上就永遠不動,
         * 之後每次配置都重掃同一段,配置 n 頁變成 O(n^2)(見 DEVLOG 第 12 步)。
         */
        hint = (first_free == first) ? i + 1 : first_free;
        return (void *)(mem_start + ((uintptr_t)first << PAGE_SHIFT));
    }
    return NULL;                           /* 總數夠但沒有夠長的連續區段 */
}

void *pmm_alloc(void)
{
    return pmm_alloc_pages(1);
}

void pmm_free_pages(void *page, size_t n)
{
    uintptr_t addr = (uintptr_t)page;
    unsigned long flags = spin_lock_irqsave(&pmm_lk);

    if (addr % PAGE_SIZE != 0 || addr < mem_start)
        panic("pmm: free of invalid address %p", page);
    size_t first = (addr - mem_start) >> PAGE_SHIFT;
    if (first >= npages || n > npages - first)
        panic("pmm: free out of range %p (%zu pages)", page, n);

    for (size_t i = first; i < first + n; i++) {
        if (!bit_test(i) || refs[i] == 0)
            panic("pmm: double free of page %p",
                  (void *)(mem_start + ((uintptr_t)i << PAGE_SHIFT)));
        if (--refs[i] > 0)
            continue;                          /* 還有別的擁有者:這一頁繼續存在 */
        bit_clear(i);
        nfree++;
        if (i < hint)
            hint = i;
    }
    spin_unlock_irqrestore(&pmm_lk, flags);
}

/* 位址轉成頁號;不是「配置中的頁」就 panic */
static size_t page_index(void *page, const char *who)
{
    uintptr_t addr = (uintptr_t)page;

    if (addr % PAGE_SIZE != 0 || addr < mem_start ||
        ((addr - mem_start) >> PAGE_SHIFT) >= npages)
        panic("pmm: %s of invalid address %p", who, page);
    return (addr - mem_start) >> PAGE_SHIFT;
}

void pmm_ref(void *page)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    size_t i = page_index(page, "ref");

    if (refs[i] == 0)
        panic("pmm: ref of a free page %p", page);
    if (refs[i] == UINT16_MAX)
        panic("pmm: reference count overflow on page %p", page);
    refs[i]++;
    spin_unlock_irqrestore(&pmm_lk, flags);
}

unsigned pmm_refcount(void *page)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    unsigned n = refs[page_index(page, "refcount")];
    spin_unlock_irqrestore(&pmm_lk, flags);
    return n;
}

void pmm_free(void *page)
{
    pmm_free_pages(page, 1);
}

size_t pmm_total_pages(void) { return npages; }
uintptr_t pmm_mem_start(void) { return mem_start; }
size_t pmm_free_count(void)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    size_t value = nfree;
    spin_unlock_irqrestore(&pmm_lk, flags);
    return value;
}
