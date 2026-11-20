/*
 * membudget.c - 記憶體配額的帳戶(I-240 C3.2 第 (b1) 批的第一部分:純計數)
 *
 * 只擁有 domain 的計數;不知道頁表,也不知道 pmm 的內部(只呼叫 pmm_reserve / pmm_unreserve)。
 * 介面與規則見 include/membudget.h,設計與審查見 docs/design/C3.2-memory-accounting.md。
 *
 * 溢位:所有比較都寫成「先確認不會下溢 / 溢位,再相減 / 相加」:
 *   正增量 n:used <= limit 且 n <= limit - used 才成立(limit 被調低到 used 以下時一律拒絕)
 *   bundle 的總量:每加一項先確認 x <= ULONG_MAX - sum
 */
#include <stdint.h>
#include "membudget.h"
#include "kmalloc.h"
#include "pmm.h"
#include "spinlock.h"
#include "uapi/errno.h"

#define MB_REFS_MAX 0xFFFFFFF0u

struct mb_domain {
    unsigned long used[MB_NCLASS];
    unsigned long total;                     /* = Σ used */
    unsigned long limit;
    unsigned refs;
};

static struct spinlock mb_lk = SPINLOCK_INIT("membudget", LK_MB);
static unsigned long errors;                 /* 帳務錯誤次數;受 mb_lk 保護 */

struct mb_domain *mb_domain_new(unsigned long limit)
{
    struct mb_domain *d = kzalloc(sizeof(*d));
    if (d) {
        d->limit = limit;
        d->refs = 1;
    }
    return d;
}

int mb_domain_get(struct mb_domain *d)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    int rc = d->refs < MB_REFS_MAX ? (d->refs++, 0) : -E_NOMEM;
    spin_unlock_irqrestore(&mb_lk, f);
    return rc;
}

int mb_domain_put(struct mb_domain *d)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    int last = --d->refs == 0, rc = 0;
    if (last && d->total != 0) {
        errors++;                            /* 還帶著用量就釋放:某處漏了歸還 */
        rc = -1;
    }
    spin_unlock_irqrestore(&mb_lk, f);
    if (last)
        kfree(d);                            /* 鎖外釋放:mb_lk 之內不呼叫 heap */
    return rc;
}

/* 正增量 n 是否放得下(持有 mb_lk) */
static int fits_locked(const struct mb_domain *d, unsigned long n)
{
    return n == 0 || (d->total <= d->limit && n <= d->limit - d->total);
}

int mb_quota_add(struct mb_domain *d, enum mb_class c, unsigned long n)
{
    if ((unsigned)c >= MB_NCLASS)
        return -E_INVAL;
    unsigned long f = spin_lock_irqsave(&mb_lk);
    int rc = -E_NOMEM;
    if (fits_locked(d, n)) {                 /* total + n <= limit:各類別與總和都不會溢位 */
        d->used[c] += n;
        d->total += n;
        rc = 0;
    }
    spin_unlock_irqrestore(&mb_lk, f);
    return rc;
}

/* 歸還 n(持有 mb_lk);超量時夾到 0 並計數 */
static int sub_locked(struct mb_domain *d, enum mb_class c, unsigned long n)
{
    int rc = 0;
    if (n > d->used[c]) {
        errors++;
        n = d->used[c];
        rc = -1;
    }
    d->used[c] -= n;
    d->total -= n;                           /* total = Σ used >= used[c] >= n */
    return rc;
}

int mb_quota_sub(struct mb_domain *d, enum mb_class c, unsigned long n)
{
    if ((unsigned)c >= MB_NCLASS)
        return -E_INVAL;
    unsigned long f = spin_lock_irqsave(&mb_lk);
    int rc = sub_locked(d, c, n);
    spin_unlock_irqrestore(&mb_lk, f);
    return rc;
}

/* bundle 各類別的總量;溢位回傳 -1 */
static int bundle_sum(const struct mb_bundle *b, unsigned long *sum)
{
    unsigned long s = 0;
    for (unsigned i = 0; i < MB_NCLASS; i++) {
        if (b->quota[i] > (unsigned long)-1 - s)
            return -1;
        s += b->quota[i];
    }
    *sum = s;
    return 0;
}

int mb_commit_bundle(struct mb_domain *d, const struct mb_bundle *b)
{
    unsigned long n;
    if (bundle_sum(b, &n) != 0)
        return -E_NOMEM;
    unsigned long f = spin_lock_irqsave(&mb_lk);
    int rc = -E_NOMEM;
    /* 完成點:配額放得下 → pmm 預留成功 → 套用配額。預留失敗時配額還沒動 */
    if (fits_locked(d, n) && pmm_reserve(b->reserve) == 0) {
        for (unsigned i = 0; i < MB_NCLASS; i++)
            d->used[i] += b->quota[i];
        d->total += n;
        rc = 0;
    }
    spin_unlock_irqrestore(&mb_lk, f);
    return rc;
}

int mb_release_bundle(struct mb_domain *d, const struct mb_bundle *b)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    /* 整批先檢查:不能先退前幾類,才在後面的類別發現超量。 */
    for (unsigned i = 0; i < MB_NCLASS; i++) {
        if (b->quota[i] > d->used[i]) {
            errors++;
            spin_unlock_irqrestore(&mb_lk, f);
            return -1;
        }
    }
    /* 先交由 PMM 檢查與退票;若帳務錯誤而停止,配額仍未改變。
       呼叫者必須持有並驗證這筆 bundle 自己的未兌現票,不可借用其他 owner 的票。 */
    pmm_unreserve(b->reserve);
    for (unsigned i = 0; i < MB_NCLASS; i++) {
        d->used[i] -= b->quota[i];
        d->total -= b->quota[i];
    }
    spin_unlock_irqrestore(&mb_lk, f);
    return 0;
}

unsigned long mb_limit(struct mb_domain *d)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    unsigned long v = d->limit;
    spin_unlock_irqrestore(&mb_lk, f);
    return v;
}

void mb_set_limit(struct mb_domain *d, unsigned long limit)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    d->limit = limit;                        /* 可以低於 used:已記的不撤銷,之後的正增量被拒絕 */
    spin_unlock_irqrestore(&mb_lk, f);
}

void mb_stats(struct mb_domain *d, struct mb_stats *out)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    for (unsigned i = 0; i < MB_NCLASS; i++)
        out->used[i] = d->used[i];
    out->total = d->total;
    out->limit = d->limit;
    out->refs = d->refs;
    spin_unlock_irqrestore(&mb_lk, f);
}

unsigned long mb_accounting_errors(void)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    unsigned long v = errors;
    spin_unlock_irqrestore(&mb_lk, f);
    return v;
}
