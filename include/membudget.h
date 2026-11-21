#ifndef WELYOS_MEMBUDGET_H
#define WELYOS_MEMBUDGET_H
#include <stddef.h>
#include "pmm.h"   /* PAGE_SIZE */

/*
 * membudget.h - 記憶體配額(I-240 C3.2;設計見 docs/design/C3.2-memory-accounting.md)
 *
 * 一個 domain = 一個行程(或一個長壽命的使用者,例如 ramfs)的配額帳戶。
 * 所有類別合計一個 limit;這裡只做邏輯上的計數,不推論實體用量。
 * 實體的保證在 pmm 的預留(pmm_reserve):mb_commit_bundle 把「配額 + 預留」在同一把鎖裡一次做完。
 *
 * 規則:
 *   - 只有正增量檢查 limit;limit 可以被調到 used 以下(之後不再接受正增量),已記的不撤銷
 *   - 加 0 成功且什麼都不改;任何失敗都不改變狀態
 *   - 單類歸還超過已記的量是帳務錯誤:夾到 0、計數,並回傳 -1 讓呼叫者記入黑盒子
 * 鎖:mb_lk(LK_MB),鎖內只做加減比較,唯一的外部呼叫是 pmm_reserve / pmm_unreserve(純記帳)
 */
enum mb_class { MB_ANON, MB_STACK, MB_IMAGE, MB_PGTABLE, MB_KOBJ, MB_RAMFS, MB_NCLASS };

struct mb_domain;                                       /* 不透明 */

struct mb_bundle {
    unsigned long quota[MB_NCLASS];                     /* 各類別要增加 / 歸還的配額 */
    unsigned long reserve;                              /* 要預留 / 放棄的實體頁 */
};

struct mb_stats {
    unsigned long used[MB_NCLASS];
    unsigned long total, limit;
    unsigned refs;
};

/*
 * exec 交易憑證(設計 R4、第 (c) 批定案 3):由呼叫者在 stack 上提供,mb_exec_begin 不配置記憶體。
 * 交易進行中,帶著這個憑證的記帳(只有記到新位址空間帳本的那些)上限暫時放寬為 limit + pending,
 * pending = 舊位址空間帳本的配額總和(它在 exec 成功後會整個退回)。其他記帳 — 包括同一時間
 * 建立的檔案、管線、kernel 物件 — 不帶憑證,照一般上限檢查,不能借用放寬。
 *   mb_exec_begin       開始(limit + pending 溢位則 -E_NOMEM,什麼都不改)
 *   mb_exec_check_final 公布前的最終檢查:舊空間退完之後 used 仍不超過 limit(old_now = 舊空間帳本的即時總和)
 *   mb_exec_end         成功、失敗都呼叫一次;之後再帶這個憑證記帳就是一般上限
 */
struct mb_exec_txn {
    struct mb_domain *d;
    unsigned long pending;
    int active;
    const void *bound;       /* 綁定的新位址空間(只比較身分,不解參考);一張憑證只能綁一個(Codex 223) */
};
/* 把憑證綁到新位址空間 owner:憑證必須有效、屬於 d、而且還沒綁過。0,否則 -1(什麼都不改) */
int  mb_exec_bind(struct mb_exec_txn *t, struct mb_domain *d, const void *owner);
/* 新空間建立失敗時解除綁定(只有 owner 本人能解);之後憑證可以再綁一次 */
void mb_exec_unbind(struct mb_exec_txn *t, const void *owner);
int  mb_exec_begin(struct mb_domain *d, unsigned long pending, struct mb_exec_txn *t);
int  mb_exec_check_final(struct mb_exec_txn *t, unsigned long old_now);
void mb_exec_end(struct mb_exec_txn *t);

struct mb_domain *mb_domain_new(unsigned long limit);  /* refcount 1;配置失敗回傳 NULL */
int  mb_domain_get(struct mb_domain *d);                /* 0;refcount 已到上限 -E_NOMEM */
int  mb_domain_put(struct mb_domain *d);                /* 0;最後一份時釋放。那時 used 不為 0 → -1(已計數) */

/* 單一類別的配額(不預留實體) */
int  mb_quota_add(struct mb_domain *d, enum mb_class c, unsigned long n);
int  mb_quota_add_txn(struct mb_domain *d, enum mb_class c, unsigned long n, const struct mb_exec_txn *txn);
int  mb_quota_sub(struct mb_domain *d, enum mb_class c, unsigned long n);
/* 略過 limit 的記帳(只檢查不溢位):給有特權的使用者寫共用的檔案系統;照樣記帳,不改共用的 limit */
int  mb_quota_force_add(struct mb_domain *d, enum mb_class c, unsigned long n);

/* 多類別配額 + 實體預留,一次完成;失敗回傳 -E_NOMEM 且不改任何狀態 */
int  mb_commit_bundle(struct mb_domain *d, const struct mb_bundle *b);
int  mb_commit_bundle_txn(struct mb_domain *d, const struct mb_bundle *b, const struct mb_exec_txn *txn);
/* 整批歸還:任一類超量則整批拒絕(-1、計數),配額與預留都不變。
   呼叫者必須持有並驗證自己這筆 bundle 的未兌現票;PMM 只檢查全系統總數。
   全系統退票超量會先停止,配額尚未修改。正常歸還回傳 0。 */
int  mb_release_bundle(struct mb_domain *d, const struct mb_bundle *b);

unsigned long mb_limit(struct mb_domain *d);
void mb_set_limit(struct mb_domain *d, unsigned long limit);
void mb_stats(struct mb_domain *d, struct mb_stats *out);
unsigned long mb_accounting_errors(void);

/* kernel 物件(檔案、管線…)由建立者的 domain 持有:取得一份參考並記 KOBJ 配額(失敗 -E_NOMEM、什麼都不改);
   最後一個參考釋放時以 mb_release_object 退回並放掉參考(帳務不符回傳 -1) */
int  mb_charge_object(struct mb_domain *d, unsigned long pages);
int  mb_release_object(struct mb_domain *d, unsigned long pages);
static inline unsigned long mb_pages_for_bytes(unsigned long bytes) { return (bytes + PAGE_SIZE - 1) / PAGE_SIZE; }              /* 超量歸還、帶著用量釋放的次數 */
#endif
