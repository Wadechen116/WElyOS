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

/* 保護位元圖與相關計數。多顆 CPU 可能同時配置,只關中斷已經不夠 */
static struct spinlock pmm_lk = SPINLOCK_INIT("pmm");

/* linker script 定義的符號:kernel 映像的範圍。只取位址,不讀內容 */
extern char __kernel_start[], __kernel_end[];

static uint8_t  *bitmap;      /* 位元圖本身(放在 kernel 映像之後) */
static uintptr_t mem_start;   /* 第 0 頁的實體位址 */
static size_t    npages;      /* 管理的總頁數 */
static size_t    nfree;       /* 目前空閒頁數 */
static size_t    hint;        /* 比它小的頁都不是空閒的:掃描的起點 */

#define ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))
#define ALIGN_DOWN(x) ((x) & ~(uint64_t)(PAGE_SIZE - 1))

static int  bit_test(size_t i)  { return bitmap[i / 8] & (1u << (i % 8)); }
static void bit_set(size_t i)   { bitmap[i / 8] |= (uint8_t)(1u << (i % 8)); }
static void bit_clear(size_t i) { bitmap[i / 8] &= (uint8_t)~(1u << (i % 8)); }

/* 兩個範圍 [a, a+alen) 與 [b, b+blen) 是否重疊 */
static void *alloc_pages_locked(size_t n);

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
    size_t bm_bytes = (npages + 7) / 8;

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

    /* 先全部標成使用中,再把確定可用的頁逐一放出來。
       這樣即使漏判,結果是「少用幾頁」而不是「把別人的資料配置出去」 */
    memset(bitmap, 0xFF, bm_bytes);
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

void *pmm_alloc_pages(size_t n)
{
    unsigned long flags = spin_lock_irqsave(&pmm_lk);
    void *p = alloc_pages_locked(n);
    spin_unlock_irqrestore(&pmm_lk, flags);
    return p;
}

/* 實際的配置邏輯;呼叫時已持有 pmm_lk */
static void *alloc_pages_locked(size_t n)
{
    if (n == 0 || n > nfree)
        return NULL;

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
        if (!bit_test(i))
            panic("pmm: double free of page %p",
                  (void *)(mem_start + ((uintptr_t)i << PAGE_SHIFT)));
        bit_clear(i);
    }
    nfree += n;
    if (first < hint)
        hint = first;
    spin_unlock_irqrestore(&pmm_lk, flags);
}

void pmm_free(void *page)
{
    pmm_free_pages(page, 1);
}

size_t pmm_total_pages(void) { return npages; }
uintptr_t pmm_mem_start(void) { return mem_start; }
size_t pmm_free_count(void)  { return nfree; }
