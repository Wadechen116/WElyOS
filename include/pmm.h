#ifndef WELYOS_PMM_H
#define WELYOS_PMM_H
#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"

/*
 * 實體記憶體管理員(Physical Memory Manager)
 *
 * 把主記憶體切成固定大小的「頁」(page),以頁為單位配置與釋放。
 * 目前尚未開啟分頁(MMU),實體位址可以直接當指標使用。
 */
#define PAGE_SIZE  4096u
#define PAGE_SHIFT 12

/* 依開機資訊建立頁面配置表。必須在其他 pmm_* 與 kmalloc 之前呼叫 */
void pmm_init(const struct boot_info *bi);

/* 配置 1 頁 / 連續 n 頁,回傳起始位址(對齊到頁);不足時回傳 NULL。
   內容不保證為 0 */
void *pmm_alloc(void);
void *pmm_alloc_pages(size_t n);

/* 釋放先前配置的 1 頁 / 連續 n 頁。位址不合法或重複釋放會 panic */
void pmm_free(void *page);
void pmm_free_pages(void *page, size_t n);

size_t pmm_total_pages(void);   /* 管理的總頁數 */
size_t pmm_free_count(void);    /* 目前可用的頁數 */
#endif
