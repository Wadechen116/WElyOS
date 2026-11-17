#ifndef WELYOS_KMALLOC_H
#define WELYOS_KMALLOC_H
#include <stddef.h>

/*
 * kernel heap:任意大小的動態記憶體配置(用法同 malloc / free)。
 * 底層向實體記憶體管理員(pmm)以頁為單位取得記憶體,再切成小塊。
 * 需先呼叫 pmm_init()。
 */

/* 配置 size 位元組,回傳的位址對齊到 16 位元組;失敗或 size 為 0 回傳 NULL */
void *kmalloc(size_t size);

/* 同 kmalloc,但內容清為 0 */
void *kzalloc(size_t size);

/* 釋放 kmalloc/kzalloc 取得的記憶體;NULL 會被忽略。
   傳入不合法的指標或重複釋放會 panic */
void kfree(void *ptr);

/* heap 使用狀況 */
struct kheap_stats {
    size_t heap_bytes;    /* 已向 pmm 取得的總位元組數 */
    size_t free_bytes;    /* 其中目前空閒的位元組數(含區塊標頭) */
    size_t free_blocks;   /* 空閒區塊數;越多代表越破碎 */
};
void kheap_stats(struct kheap_stats *st);
#endif
