/*
 * commit_release_probe.c - 記憶體承諾的「歸還」永遠成功(第 50 步,D6)
 *
 * 靜態檢查的基準紀錄(sys_munmap、commit_release_all、sbrk 縮小)都依賴同一個前提:
 * commit_change_locked 在 delta <= 0 時一定回傳 0。這裡用摘取的真函式窮舉驗證,
 * 而不是只靠再讀一次程式碼:
 *   - 各種 commit / limit / commit_total / commit_cap 的組合,搭配 0、-1、-commit、
 *     -(commit+1)、-大數、LONG_MIN 等 delta
 *   - 每次都必須回傳 0;commit 減少 min(-delta, commit);commit_total 減少同樣的量;不會變成負數
 *   - 對照:delta > 0 超過配額時必須回傳 -E_NOMEM 且什麼都不變(確認測的是真的函式)
 * 以 UBSan 編譯:有號數溢位等未定義行為會直接失敗。
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#define E_NOMEM 12
struct proc { struct { unsigned long commit, limit; } mem; };
static unsigned long free_count = 1000, total_pages = 4096;
static unsigned long pmm_free_count(void) { return free_count; }
static unsigned long pmm_total_pages(void) { return total_pages; }
#include "function.c"

int main(void)
{
    static const unsigned long vals[] = { 0, 1, 2, 7, 100, 4095, 65536, ULONG_MAX / 2, ULONG_MAX - 1, ULONG_MAX };
    static const long deltas[] = { 0, -1, -2, -100, -65536, LONG_MIN + 1, LONG_MIN };
    unsigned long cases = 0, bad = 0;
    for (unsigned ic = 0; ic < sizeof(vals) / sizeof(vals[0]); ic++)
    for (unsigned il = 0; il < sizeof(vals) / sizeof(vals[0]); il++)
    for (unsigned ie = 0; ie < 3; ie++)
    for (unsigned id = 0; id < sizeof(deltas) / sizeof(deltas[0]) + 2; id++) {
        struct proc p = { { vals[ic], vals[il] } };
        unsigned long extra = ie == 0 ? 0 : ie == 1 ? 5 : 1000;   /* 其他行程的承諾 */
        commit_total = p.mem.commit + extra;
        if (commit_total < p.mem.commit) continue;                 /* 不變量:總和 >= 單一行程(不溢位) */
        commit_cap = 0;
        long delta = id < sizeof(deltas) / sizeof(deltas[0]) ? deltas[id]
                   : id == sizeof(deltas) / sizeof(deltas[0]) ? -(long)(p.mem.commit > LONG_MAX ? LONG_MAX : p.mem.commit)
                   : -(long)(p.mem.commit >= (unsigned long)LONG_MAX ? LONG_MAX : p.mem.commit + 1);
        unsigned long c0 = p.mem.commit, t0 = commit_total;
        unsigned long want = delta == LONG_MIN ? (unsigned long)LONG_MAX + 1 : (unsigned long)-delta;
        if (want > c0) want = c0;
        int rc = commit_change_locked(&p, delta);
        cases++;
        if (rc != 0 || p.mem.commit != c0 - want || commit_total != t0 - want) {
            bad++;
            if (bad < 5) printf("FAIL commit=%lu limit=%lu delta=%ld -> rc=%d commit=%lu total=%lu\n",
                                c0, p.mem.limit, delta, rc, p.mem.commit, commit_total);
        }
    }
    /* 對照:增加超過配額 */
    struct proc q = { { 10, 10 } };
    commit_total = 10; commit_cap = 0;
    int rc = commit_change_locked(&q, 1);
    int control = rc == -E_NOMEM && q.mem.commit == 10 && commit_total == 10;
    printf("release cases: %lu, failures: %lu; over-limit control: %s\n", cases, bad, control ? "ok" : "FAILED");
    return bad || !control;
}
