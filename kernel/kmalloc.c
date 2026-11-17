/*
 * kmalloc.c - kernel heap:first-fit 空閒串列配置器
 *
 * 記憶體被切成一個個「區塊」,每個區塊開頭有一個標頭(struct block),
 * 標頭之後才是交給呼叫者的空間:
 *
 *     +--------+----------------------+--------+-------------+
 *     | 標頭   | 使用者資料           | 標頭   | 使用者資料  | ...
 *     +--------+----------------------+--------+-------------+
 *              ^ kmalloc 回傳這裡
 *
 * 空閒的區塊用單向串列串起來,並且「依位址由小到大排序」。排序的目的
 * 是讓釋放時能立刻知道前後鄰居是不是也空閒,若相鄰就合併成一塊大的
 * (coalescing),否則用久了會碎成一堆小塊,明明總量夠卻配不出大塊。
 *
 * 配置:從頭找第一個夠大的空閒區塊(first-fit),太大就切成兩塊。
 * 不夠:向 pmm 要幾頁,當成一個新的空閒區塊加進串列。
 *
 * 空閒串列以一把 spinlock 保護,所以可被多條執行緒、多顆 CPU 以及中斷處理程式使用。
 * 限制:取得的頁不會還給 pmm。
 */
#include <stdint.h>
#include "kmalloc.h"
#include "spinlock.h"
#include "arch.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"

/* 保護空閒串列。持有期間可能再取得 pmm 的鎖(heap 擴充時),順序固定為 heap -> pmm */
static struct spinlock heap_lk = SPINLOCK_INIT("heap");

#define KM_ALIGN       16u            /* 回傳位址與區塊大小的對齊單位 */
#define KM_GROW_PAGES  4u             /* 每次至少向 pmm 要幾頁(16KB) */
#define KM_MAGIC_USED  0x55534544u    /* "USED":使用中的區塊 */
#define KM_MAGIC_FREE  0x46524545u    /* "FREE":空閒的區塊 */

/*
 * 區塊標頭。_Alignas(16) 讓整個結構對齊到 16 位元組,sizeof 也因此是
 * 16 的倍數(i386 為 16、64 位元為 32),標頭之後的使用者資料自然對齊。
 */
struct block {
    _Alignas(KM_ALIGN) size_t size;   /* 整個區塊的大小,含標頭 */
    uint32_t magic;                   /* KM_MAGIC_USED 或 KM_MAGIC_FREE */
    struct block *next;               /* 下一個空閒區塊(僅空閒時有意義) */
};
#define HDR sizeof(struct block)

static struct block *free_list;       /* 空閒串列,依位址遞增排序 */
static size_t heap_bytes;             /* 已向 pmm 取得的總量 */

/* 把區塊放回空閒串列(維持位址順序),並與相鄰的空閒區塊合併 */
static void insert_free(struct block *b)
{
    struct block *prev = NULL, *next = free_list;

    while (next && next < b) {         /* 找到插入點:prev < b < next */
        prev = next;
        next = next->next;
    }

    b->magic = KM_MAGIC_FREE;
    b->next = next;
    if (prev)
        prev->next = b;
    else
        free_list = b;

    /* 與後一塊相鄰:把後一塊併進來 */
    if (next && (char *)b + b->size == (char *)next) {
        b->size += next->size;
        b->next = next->next;
    }
    /* 與前一塊相鄰:把自己併進前一塊 */
    if (prev && (char *)prev + prev->size == (char *)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
}

/* 向 pmm 取得至少 need 位元組,加入空閒串列。成功回傳 1 */
static int grow(size_t need)
{
    size_t pages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < KM_GROW_PAGES)
        pages = KM_GROW_PAGES;

    struct block *b = pmm_alloc_pages(pages);
    if (!b)
        return 0;
    b->size = pages * PAGE_SIZE;
    heap_bytes += b->size;
    insert_free(b);
    return 1;
}

/* 實際的配置邏輯;呼叫時中斷已關閉 */
static void *kmalloc_locked(size_t size)
{
    if (size == 0 || size > SIZE_MAX - HDR - KM_ALIGN)   /* 後者防止加法溢位 */
        return NULL;

    /* 實際需要的區塊大小:使用者大小向上對齊,再加標頭 */
    size_t need = ((size + KM_ALIGN - 1) & ~(size_t)(KM_ALIGN - 1)) + HDR;

    for (int attempt = 0; attempt < 2; attempt++) {
        struct block *prev = NULL;
        for (struct block *b = free_list; b; prev = b, b = b->next) {
            if (b->size < need)
                continue;

            struct block *rest = b->next;
            /* 剩餘空間還放得下一個標頭加最小資料,就切出來繼續當空閒區塊;
               否則整塊都給出去(多出來的幾個位元組算內部浪費) */
            if (b->size - need >= HDR + KM_ALIGN) {
                struct block *tail = (struct block *)((char *)b + need);
                tail->size = b->size - need;
                tail->magic = KM_MAGIC_FREE;
                tail->next = b->next;
                rest = tail;
                b->size = need;
            }
            if (prev)
                prev->next = rest;
            else
                free_list = rest;

            b->magic = KM_MAGIC_USED;
            b->next = NULL;
            return (char *)b + HDR;
        }
        /* 第一輪找不到:擴充 heap 後再找一次 */
        if (attempt == 0 && !grow(need))
            break;
    }
    return NULL;
}

void *kmalloc(size_t size)
{
    unsigned long flags = spin_lock_irqsave(&heap_lk);
    void *p = kmalloc_locked(size);
    spin_unlock_irqrestore(&heap_lk, flags);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)((char *)ptr - HDR);
    /* 標頭的 magic 不是 USED:指標不是 kmalloc 給的、已經釋放過,
       或標頭被前一塊的越界寫入破壞。三種都是嚴重錯誤,立刻停下來 */
    if (b->magic != KM_MAGIC_USED)
        panic("kfree: invalid pointer or double free: %p", ptr);

    unsigned long flags = spin_lock_irqsave(&heap_lk);
    insert_free(b);
    spin_unlock_irqrestore(&heap_lk, flags);
}

size_t kmalloc_size(void *ptr)
{
    struct block *b = (struct block *)((char *)ptr - HDR);

    if (b->magic != KM_MAGIC_USED)
        panic("kmalloc_size: invalid pointer %p", ptr);
    return b->size;
}

void kheap_stats(struct kheap_stats *st)
{
    st->heap_bytes = heap_bytes;
    st->free_bytes = 0;
    st->free_blocks = 0;

    unsigned long flags = spin_lock_irqsave(&heap_lk);
    for (struct block *b = free_list; b; b = b->next) {
        st->free_bytes += b->size;
        st->free_blocks++;
    }
    spin_unlock_irqrestore(&heap_lk, flags);
}
