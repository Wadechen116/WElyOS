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

/*
 * 每一頁有一個參考計數:有幾個「擁有者」。配置時為 1。
 * 一頁可以被多個位址空間共用(fork 之後的寫入時複製):每多一個使用者就 pmm_ref 一次,
 * 每個使用者不再需要時各自 pmm_free 一次,計數降到 0 才真的還給系統。
 * 所以對只有一個擁有者的頁(絕大多數情況),pmm_free 的行為和以前完全一樣。
 */
void pmm_ref(void *page);              /* 多一個擁有者 */
unsigned pmm_refcount(void *page);     /* 目前有幾個擁有者 */

/* 放棄先前配置的 1 頁 / 連續 n 頁(每頁的參考計數減一,降到 0 才釋放)。
   位址不合法,或對已經沒有擁有者的頁再放棄一次(重複釋放),會 panic */
void pmm_free(void *page);
void pmm_free_pages(void *page, size_t n);

size_t pmm_total_pages(void);   /* 管理的總頁數 */
uintptr_t pmm_mem_start(void);  /* 第 0 頁的實體位址(管理範圍的起點) */
size_t pmm_free_count(void);    /* 目前可用的頁數 */

/*
 * kernel 的保留底線。pmm_reserve_enable(n) 之後,一般配置(上面的 pmm_alloc /
 * pmm_alloc_pages)一律不能讓空閒頁數低於 n,不足就回傳 NULL、什麼都不變。
 * 只能啟用一次,n 不可為 0 或超過目前的空閒頁數(回傳 -1)。
 * pmm_alloc_pages_reserve 可以動用底線以下的頁:只給經過審查的 kernel 復原路徑,
 * 不可用在使用者能觸發的配置上。
 */
int    pmm_reserve_enable(size_t pages);
size_t pmm_reserve_count(void);              /* 底線頁數;0 = 尚未啟用 */
unsigned long pmm_reserve_refusals(void);    /* 因為底線被拒絕的配置次數 */
void  *pmm_alloc_pages_reserve(size_t n);   /* 應急:可以動用底線,但不能動用預留 */

/*
 * 預留(I-240 C3.2 第 (a) 批):先答應、之後才真正配置的頁。
 *   pmm_reserve(n)      預留 n 頁:之後未預留的空閒頁仍不低於底線才成功(0),否則 -E_NOMEM、不改狀態;
 *                       n == 0 成功且什麼都不做
 *   pmm_unreserve(n)    放棄 n 頁尚未兌現的預留;超過目前的預留量是帳務錯誤 → panic(不先改狀態)
 *   pmm_alloc_reserved  兌現一頁預留:一定成功(預留量為 0 時呼叫是帳務錯誤 → panic)
 * 一般配置拿不到預留的頁;應急配置也拿不到。所以持有預留的一方兌現時永遠有頁可拿。
 * 誰持有多少預留由上層記帳,pmm 只知道總數。
 */
int    pmm_reserve(size_t n);
void   pmm_unreserve(size_t n);
void  *pmm_alloc_reserved(void);
size_t pmm_reserved_count(void);

/* 在同一把鎖裡讀出的一組數字(測試核對不變量用,不同時間分別讀取的值不能互相比較) */
struct pmm_snapshot {
    size_t free, reserved, floor;
    unsigned long refusals;
};
void   pmm_snapshot(struct pmm_snapshot *out);
#endif
