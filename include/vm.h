#ifndef WELYOS_VM_H
#define WELYOS_VM_H
#include <stddef.h>
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
#define VM_COW     0x40u       /* 寫入時複製(copy-on-write):這一頁目前和別的位址空間共用,
                                  對映成唯讀;有人寫入時才複製出自己的一份(見 vm_space_fault)。
                                  同樣記在軟體使用的位元裡 */
#define VM_TICKET  0x80u       /* 只和 VM_COW 一起出現:這筆 COW 映射持有一張實體預留的票
                                  (I-240 C3.2;寫入時用自己的票複製,或在只剩自己時退票)。
                                  設計見 docs/design/C3.2-memory-accounting.md 的 BR3 */

/*
 * 軟體位元的編碼。三個架構都只保證兩個軟體位元(RISC-V 的 RSW 只有 bit 8、9),
 * 而 VM_SHARED(屬於檔案的頁)與 VM_COW 不會同時成立,所以兩個位元表示四種狀態:
 *   0 一般   1 SHARED   2 COW(沒有票)   3 COW + TICKET
 * 架構層只負責把這個 0~3 的值放進自己的兩個位元、再取出來;規則集中在這裡。
 */
static inline int vm_sw_flags_ok(unsigned flags)
{
    unsigned sw = flags & (VM_SHARED | VM_COW | VM_TICKET);
    return sw == 0 || sw == VM_SHARED || sw == VM_COW || sw == (VM_COW | VM_TICKET);
}
static inline unsigned vm_sw_encode(unsigned flags)      /* 呼叫前必須 vm_sw_flags_ok */
{
    if (flags & VM_SHARED)
        return 1;
    if (flags & VM_COW)
        return (flags & VM_TICKET) ? 3 : 2;
    return 0;
}
static inline unsigned vm_sw_decode(unsigned code)
{
    static const unsigned map[4] = { 0, VM_SHARED, VM_COW, VM_COW | VM_TICKET };
    return map[code & 3];
}

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
struct vm_space;
int vm_space_unmap(struct vm_space *vs, uintptr_t va);   /* 移除一個使用者頁的對映並放棄那一頁 */
unsigned long vm_space_pages(struct vm_space *vs);   /* 這個位址空間目前對映了幾頁(含和別人共用的) */

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

struct mb_domain;
/* dom 不為 NULL 時,這個空間的配額與預留記在 dom 上(持有一份參考,根表記 PGTABLE 1 頁);
   NULL = 不記帳。失敗回傳 NULL */
struct vm_space *vm_space_create(struct mb_domain *dom);

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

/*
 * ---- 寫入時複製(copy-on-write)與 fork ----
 *
 * vm_space_fork:建立 parent 的複本。不真的複製任何一頁的內容:兩邊的頁表指向
 *     同一批實體頁,原本可寫的頁在「兩邊」都改成唯讀並標記 VM_COW。
 *     之後誰先寫入,誰就在那一刻複製出自己的一份(由 vm_space_fault 處理)。
 *     從來沒被寫入的頁永遠共用,省下複製的時間與記憶體。
 *     與檔案共用的程式碼頁(VM_SHARED)照樣共用。失敗回傳 NULL。
 *
 * vm_space_fault:使用者程式存取 va 時發生了 page fault,write 表示是不是寫入。
 *     如果原因是「寫入一個 VM_COW 的頁」,就在這裡解決(複製,或確認只剩自己在用後
 *     直接改回可寫),回傳 0,呼叫者讓使用者程式重新執行那條指令即可。
 *     回傳 -1 表示不是這種情況(真正的存取違規),或記憶體不足。
 *
 * vm_space_map_new:同 vm_space_map,但 va 已經有對映時不覆蓋,回傳 1。
 *     用於「用到才配置」的頁:兩條執行緒可能同時為同一頁發生 page fault。
 */
struct vm_range { uintptr_t start, len; };
/* dom 不為 NULL(且 parent 也有 domain)時,子空間記在 dom 上;anon 是父行程已承諾的匿名記憶體範圍,
   依 R1 替父子雙方預留 COW 票(設計 BR4:兩筆帳分開,失敗只退子的帳) */
struct vm_space *vm_space_fork(struct vm_space *parent, struct mb_domain *dom,
                               const struct vm_range *anon, unsigned nanon);
int vm_space_fault(struct vm_space *vs, uintptr_t va, int write);
int vm_space_map_new(struct vm_space *vs, uintptr_t va, uintptr_t pa, unsigned flags);
/* 在 VM 的鎖之內查表並複製 n 個位元組(不跨頁);to_user = 1 是寫入使用者頁(必須可寫)。
   0 = 完成;-1 = 沒有對映或權限不符(呼叫者處理 fault 後重試)。說明見 vm.c */
int vm_space_copy(struct vm_space *vs, uintptr_t va, void *kbuf, size_t n, int to_user);
void vm_space_switch_flush(struct vm_space *vs);   /* 重新載入 vs 並清掉它在這顆 CPU 上的 TLB 紀錄 */

/* 統計(測試用):寫入時複製真的複製了幾頁、有幾次因為只剩自己在用而免於複製 */
unsigned long vm_cow_copies(void);
unsigned long vm_cow_reuses(void);

/* 其他 CPU 啟動時呼叫:在「目前這顆 CPU」上載入 kernel 頁表並開啟分頁。
   頁表本身全系統共用,但開啟分頁、功能位元這些設定是每顆 CPU 各自的 */
void vm_enable_secondary(void);

/* kernel 頁表根表格的實體位址(i386 的啟動程式要在開啟分頁前把它載入 CR3) */
uintptr_t vm_kernel_root(void);
/*
 * 匿名記憶體(heap、mmap)的承諾、兌現、歸還(I-240 C3.2 (b1);設計見 docs/design/C3.2-memory-accounting.md)。
 * 只用於以 domain 建立的位址空間。
 *   vm_set_anon_windows  開機時由行程記憶體模組登記匿名記憶體的位址區域(由低到高、不重疊)
 *   vm_space_commit_anon 承諾 [va, va+len):配額(ANON + PGTABLE)與實體預留一次完成;0 或 -E_NOMEM / -E_INVAL
 *   vm_space_fault_anon  在已承諾的 va 配置一頁(清 0):用自己的票,一定成功;1 = 已經有對映
 *   vm_space_release_anon 歸還 [va, va+len):逐頁依 R1 釋放頁、退票,配額退回(頁表票留到 destroy)
 */
void vm_set_anon_windows(const uintptr_t base[], const uintptr_t len[], unsigned n);
int  vm_anon_window(unsigned i, uintptr_t *base, uintptr_t *len);
unsigned long vm_anon_pt_max(void);
int  vm_space_commit_anon(struct vm_space *vs, uintptr_t va, uintptr_t len);
int  vm_space_fault_anon(struct vm_space *vs, uintptr_t va);
void vm_space_release_anon(struct vm_space *vs, uintptr_t va, uintptr_t len);
/* 同上,但指定配額類別(MB_ANON 或 MB_STACK;第 (c1) 批起 stack 也走承諾 / 兌現 / 歸還) */
enum mb_class;
int  vm_space_commit(struct vm_space *vs, enum mb_class cls, uintptr_t va, uintptr_t len);
void vm_space_release(struct vm_space *vs, enum mb_class cls, uintptr_t va, uintptr_t len);
struct vm_ledger { unsigned long data_tickets, pt_tickets, anon_tables, anon_quota, pt_quota; };
void vm_space_ledger(struct vm_space *vs, struct vm_ledger *out);   /* 同一把鎖內讀出(測試用) */
#endif
