#ifndef WELYOS_MEMBUDGET_H
#define WELYOS_MEMBUDGET_H
#include <stddef.h>

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

struct mb_domain *mb_domain_new(unsigned long limit);  /* refcount 1;配置失敗回傳 NULL */
int  mb_domain_get(struct mb_domain *d);                /* 0;refcount 已到上限 -E_NOMEM */
int  mb_domain_put(struct mb_domain *d);                /* 0;最後一份時釋放。那時 used 不為 0 → -1(已計數) */

/* 單一類別的配額(不預留實體) */
int  mb_quota_add(struct mb_domain *d, enum mb_class c, unsigned long n);
int  mb_quota_sub(struct mb_domain *d, enum mb_class c, unsigned long n);

/* 多類別配額 + 實體預留,一次完成;失敗回傳 -E_NOMEM 且不改任何狀態 */
int  mb_commit_bundle(struct mb_domain *d, const struct mb_bundle *b);
/* 整批歸還:任一類超量則整批拒絕(-1、計數),配額與預留都不變。
   呼叫者必須持有並驗證自己這筆 bundle 的未兌現票;PMM 只檢查全系統總數。
   全系統退票超量會先停止,配額尚未修改。正常歸還回傳 0。 */
int  mb_release_bundle(struct mb_domain *d, const struct mb_bundle *b);

unsigned long mb_limit(struct mb_domain *d);
void mb_set_limit(struct mb_domain *d, unsigned long limit);
void mb_stats(struct mb_domain *d, struct mb_stats *out);
unsigned long mb_accounting_errors(void);              /* 超量歸還、帶著用量釋放的次數 */
#endif
