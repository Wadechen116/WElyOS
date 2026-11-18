/*
 * periodic.c - 以週期性的即時工作執行,並回報有沒有準時
 * 用法:periodic [週期毫秒 [預算毫秒 [次數 [每次工作毫秒]]]]      預設 20 5 50 2
 * 例如:periodic 20 5 100 2     每 20 毫秒做 2 毫秒的事,共 100 次
 *       periodic 20 5 20 12     每次做 12 毫秒,超過 5 毫秒的預算:看得到 overruns
 *       periodic 10 9           要求 90% 的 CPU:被拒絕
 *
 * 「工作」只是空轉指定的時間(以時鐘計)。重點在最後的統計:
 *   releases  準時開始的週期數        misses    錯過期限的次數
 *   overruns  超出預算的次數          max used  單一週期用掉最多的 CPU 時間
 * 同時開著幾個吃滿 CPU 的程式再執行一次,數字應該不變 — 這就是即時工作的意義。
 */
#include "libc.h"

static unsigned long now_us(void)
{
    struct timeval tv;

    sys_time(&tv);
    return (unsigned long)tv.sec * 1000000ul + tv.usec;      /* 只用來算差值,溢位無妨 */
}

int main(int argc, char **argv)
{
    unsigned period = argc > 1 ? (unsigned)atoi(argv[1]) : 20;
    unsigned budget = argc > 2 ? (unsigned)atoi(argv[2]) : 5;
    int count = argc > 3 ? atoi(argv[3]) : 50;
    unsigned work = argc > 4 ? (unsigned)atoi(argv[4]) : 2;
    struct rt_stats st;

    long rc = sys_rt_set(period, budget);
    if (rc != 0) {
        fprintf(2, "periodic: %s\n",
                rc == -E_PERM ? "permission denied (needs the admin capability)" :
                rc == -E_BUSY ? "refused: real-time tasks would use more than 70% of a CPU" :
                rc == -E_NOSPC ? "refused: too many real-time tasks" : "invalid period or budget");
        return 1;
    }
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < count; i++) {
        if (sys_rt_wait(&st) != 0)
            break;                                           /* 被訊號打斷:結束 */
        for (unsigned long t0 = now_us(); now_us() - t0 < work * 1000ul; )
            ;
    }
    sys_rt_wait(&st);                                        /* 最後一次也算完 */
    sys_rt_set(0, 0);
    printf("period %u ms, budget %u ms: %u releases, %u deadline misses, %u budget overruns, "
           "max used %u ms (real-time load %u.%u%%)\n", st.period_ms, st.budget_ms, st.releases,
           st.misses, st.overruns, st.max_used_ms, st.total_util / 10, st.total_util % 10);
    return st.misses ? 3 : 0;
}
