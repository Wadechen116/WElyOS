/*
 * failinj.h - 配置失敗注入(fault injection,只給 kernel 自我測試用;I-335)
 *
 * 讓「目前這條執行緒」的第 N 次配置(kmalloc / 一般的 pmm 配置)失敗,用來執行平常跑不到的
 * 錯誤路徑,核對回退與錯誤碼。只對啟用它的那條執行緒、而且不在中斷處理中生效,
 * 其他 CPU 上的工作不受影響。沒有任何系統呼叫可以啟用它(使用者程式碰不到)。
 * 應急與預留的配置(pmm_alloc_pages_reserve、pmm_alloc_reserved)依設計不會失敗,不注入。
 */
#ifndef WELYOS_FAILINJ_H
#define WELYOS_FAILINJ_H

enum fi_site { FI_KMALLOC, FI_PMM, FI_NSITES };

void failinj_arm(enum fi_site site, unsigned nth);   /* 目前執行緒的第 nth 次(從 1 起)失敗 */
void failinj_disarm(void);
int  failinj_fired(void);
int  failinj_enabled(void);                          /* 1 = 以 selftest 開機(測試專用的掛鉤只在此時生效) */
void failinj_enable(void);                           /* 以 selftest 開機時呼叫;之後系統呼叫才有效 */
long failinj_syscall(long op, long site, long n);    /* SYS_failinj(呼叫者已確認有 CAP_ADMIN) */                            /* 這次啟用之後是否真的注入過 */

/* 配置函式呼叫:回傳 1 = 這次要假裝失敗。沒有啟用時只多讀一個旗標 */
extern int failinj_armed;
int failinj_check_slow(enum fi_site site);
static inline int failinj_check(enum fi_site site)
{
    return __atomic_load_n(&failinj_armed, __ATOMIC_RELAXED) ? failinj_check_slow(site) : 0;
}

/* 延遲注入(I-334):把返回使用者模式時兩次讀取之間的空檔撐大,讓訊號競爭可以穩定重現 */
int  failinj_sig_window(const void *proc);         /* 1 = 這次忙等了(測試對象的那一次返回) */
void failinj_note_lost_wake(void);
void failinj_note_sleep_pending(void);           /* 睡下去時帶著可交付訊號(I-334 不變量)*/

#endif
