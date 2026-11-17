#ifndef WELYOS_VM_H
#define WELYOS_VM_H
#include <stdint.h>
#include "boot_info.h"

/*
 * 虛擬記憶體(分頁)
 *
 * 開啟分頁後,程式使用的位址(虛擬位址)要先經過「頁表」轉換才得到
 * 實際的記憶體位址(實體位址)。轉換以頁(4KB)為單位,每一頁可以各自
 * 設定權限。這帶來三個能力:
 *   - 保護:沒有對映的位址一碰就觸發例外(例如 NULL 指標);
 *           程式碼可以設成唯讀,資料可以設成不可執行
 *   - 重新安排:任何實體頁可以出現在任何虛擬位址
 *   - 隔離(之後):每個行程一份自己的頁表,互相看不到對方的記憶體
 *
 * 目前只有一份 kernel 頁表,採「恆等對映」(identity mapping):
 * 虛擬位址 = 實體位址,所以開啟前後所有指標都繼續有效。
 */
#define VM_READ    0x01u       /* 可讀(三個架構上,有對映就一定可讀) */
#define VM_WRITE   0x02u       /* 可寫 */
#define VM_EXEC    0x04u       /* 可執行(i386 未使用 PAE,無法禁止執行,此旗標無作用) */
#define VM_DEVICE  0x08u       /* 裝置暫存器(MMIO):AArch64 需要不同的記憶體屬性 */

/* 建立 kernel 頁表並開啟分頁。需先完成 pmm_init() */
void vm_init(const struct boot_info *bi);

/*
 * 把虛擬頁 va 對映到實體頁 pa(兩者都必須對齊 4KB),權限為 flags。
 * 已有對映則覆蓋(可用來改權限)。需要的中間層頁表會自動向 pmm 配置。
 * 成功回傳 0;位址超出範圍或記憶體不足回傳 -1。
 */
int vm_map(uintptr_t va, uintptr_t pa, unsigned flags);

/* 對映一段連續範圍 [va, va+size) -> [pa, pa+size);size 會向上取整到頁 */
int vm_map_range(uintptr_t va, uintptr_t pa, uintptr_t size, unsigned flags);

/* 移除 va 的對映,之後存取該頁會觸發 page fault。原本沒有對映回傳 -1 */
int vm_unmap(uintptr_t va);

/* 查詢 va 目前對映到哪個實體位址與權限(可傳 NULL 表示不需要)。
   沒有對映回傳 -1 */
int vm_query(uintptr_t va, uintptr_t *pa, unsigned *flags);

unsigned long vm_table_pages(void);    /* 頁表本身用掉的頁數 */
#endif
