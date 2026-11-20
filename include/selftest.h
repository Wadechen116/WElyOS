#ifndef WELYOS_SELFTEST_H
#define WELYOS_SELFTEST_H

/*
 * selftest.h - kernel 自我測試的進入點(實作在 kernel/test/)
 *
 * 開機流程(kernel/main.c 的 kmain)依開機參數在適當的時機呼叫它們:
 * 例如記憶體的測試要在排程器啟動之前,多核心的測試要在其他 CPU 加入之後。
 * 測試都以 KASSERT 檢查:不符預期就 panic,所以「開機走完」就代表全部通過。
 */
struct netdev;
struct blkdev;

/* ---- 開機流程提供給測試使用的東西(定義在 kernel/main.c) ---- */
int cmdline_has(const char *cmd, const char *word);      /* 開機參數裡有沒有 word 這個完整的單字 */
unsigned resident_threads(void);         /* 常駐的 kernel 執行緒數(網路、巡檢):核對執行緒數時要扣掉 */
extern struct netdev *boot_nic;          /* 開機時找到的網路卡;沒有則為 NULL */
struct blkdev *arch_blk_probe_cached(void);              /* 開機時找到的磁碟;沒有則為 NULL */

/* ---- kernel/test/mem_test.c ---- */
void vm_demo(void);                      /* 分頁:權限、別名對映 */
void mem_selftest(void);                 /* 頁面配置器與 kmalloc */
void mem_fulltest(void);                 /* memtest:把所有可用的記憶體寫過讀回 */
void spin_selftest(void);                /* spinlock 與每顆 CPU 的記帳 */
void crash_test(const char *cmd);        /* crash=<種類>:故意觸發錯誤(排程器啟動之前可以做的那些) */

/* ---- kernel/test/sched_test.c ---- */
void sched_demo(void);                   /* 讓出、搶佔、休眠 */
void sync_demo(void);                    /* mutex、號誌、條件變數 */
void prio_demo(void);                    /* 優先權、搶佔延遲、優先權繼承、防飢餓 */
void smp_sched_demo(void);               /* 多顆 CPU 同時排程 */
void rt_demo(void);                      /* 週期性的即時工作 */
void sleep_wall_test(void);              /* 睡眠以真實時間計不可提早醒(高精度時鐘量) */
void sched_stress(void);                 /* stress:大量執行緒 */
void sched_crash_test(const char *cmd);  /* crash=<種類>:需要排程器的那些 */
void spin_irqs_off(void *arg);           /* crash=wdog 用:關著中斷空轉的執行緒 */
void spin_forever(void *arg);            /* crash=smp 用:永遠忙碌的執行緒 */

/* ---- kernel/test/io_test.c ---- */
void nic_selftest(void);                 /* 網路卡驅動程式(不經過協定堆疊) */
void disk_demo(void);                    /* 磁碟上的檔案系統 */
void disk_stage_test(const char *cmd);   /* disktest=N:跨開機的磁碟測試 */
void disk_user_stage_test(const char *cmd);  /* disktest=12~15:使用者程式在寫回失敗時(網路啟動之後) */
void fs_demo(void);                      /* VFS 與記憶體檔案系統 */
void pipe_demo(void);                    /* 管線 */

/* ---- kernel/test/user_test.c ---- */
int  spawn_init(long mode);              /* 以測試模式 mode(uapi/selftest.h)啟動 /bin/init;回傳 pid */
void user_demo(void);                    /* 以各種模式啟動 /bin/init,核對結束碼 */
#endif
