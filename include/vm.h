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
#define VM_USER    0x10u       /* 使用者模式可存取;沒有這個旗標的頁只有 kernel 能碰 */
#define VM_SHARED  0x20u       /* 這一頁不屬於這個位址空間(例如和檔案共用的程式碼頁):
                                  位址空間釋放時只移除對映,不釋放實體頁。
                                  記在頁表項目中保留給軟體使用的位元裡,硬體會忽略它 */

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

/*
 * 位址空間(address space):一份獨立的頁表,每個行程一份。
 *
 * 每個位址空間分成兩部分:
 *   kernel 部分:所有位址空間共用同一份對映(建立時從 kernel 頁表複製最上層的項目,
 *               下層表格是共用的)。所以進入 kernel(系統呼叫、中斷)時不必換頁表。
 *               這些頁沒有 VM_USER,使用者模式碰不到。
 *   使用者部分:[USER_BASE, USER_TOP)(範圍依架構而定,見 mmu.h),每個行程各自獨立。
 *               行程 A 的頁表裡根本沒有行程 B 的頁,這就是行程之間的隔離。
 */
struct vm_space;

struct vm_space *vm_space_create(void);          /* 失敗回傳 NULL */

/* 釋放位址空間:使用者部分的所有實體頁與頁表都還給 pmm。
   呼叫時這個位址空間不可以是 CPU 正在使用的那一份 */
void vm_space_destroy(struct vm_space *vs);

/* 在使用者範圍內對映一頁(flags 會自動加上 VM_USER)。pa 必須是 pmm_alloc 取得的頁,
   之後歸這個位址空間所有,destroy 時一併釋放。va 超出使用者範圍回傳 -1 */
int vm_space_map(struct vm_space *vs, uintptr_t va, uintptr_t pa, unsigned flags);

/* 查詢使用者位址 va 的對映;只回報帶有 VM_USER 的頁,其餘視為不存在。
   kernel 存取使用者傳來的指標之前,一定要先經過這個檢查 */
int vm_space_query(struct vm_space *vs, uintptr_t va, uintptr_t *pa, unsigned *flags);

/* 讓 CPU 改用這個位址空間;傳 NULL 表示回到 kernel 自己的頁表 */
void vm_space_switch(struct vm_space *vs);

/* 其他 CPU 啟動時呼叫:在「目前這顆 CPU」上載入 kernel 頁表並開啟分頁。
   頁表本身全系統共用,但開啟分頁、功能位元這些設定是每顆 CPU 各自的 */
void vm_enable_secondary(void);

/* kernel 頁表根表格的實體位址(i386 的啟動程式要在開啟分頁前把它載入 CR3) */
uintptr_t vm_kernel_root(void);
#endif
