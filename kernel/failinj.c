/*
 * failinj.c - 配置失敗注入(見 include/failinj.h)
 *
 * 狀態只有一組:同一時間只有一條測試執行緒使用它。目標執行緒、地點、剩餘次數都在
 * 啟用前寫好,最後才以 release 設定 failinj_armed;檢查端先 acquire 讀旗標再讀其他欄位。
 */
#include "failinj.h"
#include "sched.h"
#include "cpu.h"
#include "panic.h"

int failinj_armed;
static struct thread *fi_thread;
static enum fi_site fi_site;
static unsigned fi_left;
static int fi_fired;

void failinj_arm(enum fi_site site, unsigned nth)
{
    if (nth == 0 || site >= FI_NSITES)
        panic("failinj: bad arm (%d, %u)", (int)site, nth);
    fi_thread = thread_current();
    fi_site = site;
    fi_left = nth;
    fi_fired = 0;
    __atomic_store_n(&failinj_armed, 1, __ATOMIC_RELEASE);
}

void failinj_disarm(void)
{
    __atomic_store_n(&failinj_armed, 0, __ATOMIC_RELEASE);
}

int failinj_fired(void)
{
    return fi_fired;
}

int failinj_check_slow(enum fi_site site)
{
    if (!__atomic_load_n(&failinj_armed, __ATOMIC_ACQUIRE))
        return 0;
    /* 只有啟用的那條執行緒、不在中斷處理中(中斷時 thread_current 是被打斷的那條)才計數 */
    if (thread_current() != fi_thread || this_cpu()->in_irq || site != fi_site)
        return 0;
    if (--fi_left != 0)
        return 0;
    fi_fired = 1;
    __atomic_store_n(&failinj_armed, 0, __ATOMIC_RELEASE);   /* 只失敗一次,之後照常 */
    return 1;
}
