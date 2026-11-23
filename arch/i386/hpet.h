#ifndef WELYOS_I386_HPET_H
#define WELYOS_I386_HPET_H
#include <stdint.h>

/*
 * HPET(High Precision Event Timer)主計數器:i386 上所有 CPU 共用的備援時鐘(I-341)。
 * 只用主計數器,不用它的比較器 / 中斷(PIT 與 LAPIC 計時器照舊負責「叫醒」)。
 */

/* 探測、對映、啟用並以 PIT 交叉驗證速率。成功回傳每秒計數(> 0);沒有或不合格回傳 0。
   force32 = 測試用:把 64 位元計數器當成 32 位元用,並把起始值設在離繞回 wrap_after 個計數處,
   讓 32 位元延伸的繞回處理在開機後幾秒內真的被執行 */
uint64_t hpet_init(int force32, uint64_t wrap_after_ms);
/* 讀主計數器(64 位元;32 位元硬體時以軟體延伸)。hpet_init 成功後才可呼叫 */
uint64_t hpet_read(void);
/* 診斷:是否 32 位元模式、已延伸幾次繞回 */
int      hpet_is32(void);
uint32_t hpet_wraps(void);

#endif
