#ifndef WELYOS_SELFTEST_H
#define WELYOS_SELFTEST_H
#include <stdint.h>

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
/* (kernel/proc.c)讓 pid 建立執行緒連續失敗 rounds 次,回報前後的 stack 格位與頁數;回傳成功次數 */
int proc_thread_failure_probe(int pid, unsigned rounds, uint32_t *slots0, uint32_t *slots1,
                              unsigned long *pages0, unsigned long *pages1, unsigned long *reached);

/* ---- kernel/test/mem_test.c ---- */
void vm_demo(void);                      /* 分頁:權限、別名對映 */
void mem_selftest(void);                 /* 頁面配置器與 kmalloc */
void mem_fulltest(void);                 /* memtest:把所有可用的記憶體寫過讀回 */
void spin_selftest(void);                /* spinlock 與每顆 CPU 的記帳 */
void pmm_floor_test(void);
void pmm_reservation_test(void);
void membudget_test(void);               /* I-240 C3.2 (b1):配額計數(不下溢、bundle 全有或全無) */         /* I-240 C3.2 (a):預留不被一般 / 應急配置吃掉,兌現永遠成功 */               /* I-240 C3.1:一般配置不能跌破 kernel 保留底線 */
void memhog_test(const char *mode);      /* I-240 C3.1:使用者把記憶體用到被拒絕,底線守住、全部歸還 */
void pte_encoding_test(void);            /* I-240 C3.2 (b1):PTE 軟體位元(SHARED / COW / COW+TICKET)編碼往返 */
void vm_ledger_test(void);               /* I-240 C3.2 (b1):位址空間帳本與 legacy 建表的配額 */
void anon_recommit_test(void);           /* I-336:反覆承諾未碰觸的範圍,頁表票不累積 */
void anon_ticket_test(void);             /* I-240 C3.2 (b1):匿名記憶體承諾後第一次觸及一定成功(I2) */
void fork_ticket_test(void);             /* I-240 C3.2 (b1):fork 的兩筆帳,飽和後父子寫入與觸及都成功 */
void thread_kobj_test(void);             /* I-240 C3.2 (b2):執行緒的 KOBJ 記帳、先準備後啟動 */
void pipe_kobj_test(void);               /* I-240 C3.2 (b2):檔案與管線由建立者的 domain 持有 */
void user_stack_rollback_test(void);     /* Codex 191:建立執行緒失敗時 stack 與格子退回 */
void ramfs_eof_test(void);               /* Codex 209:寫入失敗時 ramfs 檔案長度不變 */
void stack_ticket_test(void);            /* I-240 C3.2 (c1):承諾的 stack 在飽和後整格可觸及 */
void stack_release_exhausted_test(void); /* I-240 T5 / Codex 166:記憶體耗盡時歸還 stack 格不配置、票精確退回 */
void stack_concurrent_fail_test(void);   /* I-240 T4:一格承諾失敗只放自己的格與票 */
void stack_pt_boundary_test(void);       /* I-240 T1:預付 pt 票讓飽和時的第一次觸及建得出中間表 */
void stack_guard_test(void);             /* I-240 T2:保護頁觸及不消耗票、帳本不變 */
void anon_commit_map_test(void);         /* I-340:VM 層拒絕已承諾範圍外的匿名 fault、不耗票 */
void kstack_pool_test(void);             /* I-339:kernel stack 區塊長大後,第二輪與 churn 完全不改變記憶體 / 頁表 / 大分頁 */
void image_ticket_test(void);            /* I-240 C3.2 (c2):映像私有頁記 IMAGE 配額,fork 後寫入一定成功 */
void exec_txn_test(void);                /* I-240 C3.2 (c3):exec 交易的放寬上限 */
void r1_lifecycle_test(void);            /* I-240 C3.2 R1:部分歸還、交錯 COW、子 destroy、再 fork 的票帳 */
void pmm_balance_test(void);             /* I-240 C3.2:實體平衡 free + allocated == usable,與預留分開驗 */
void image_class_test(void);             /* I-240 C3.2 6.2:映像頁只記 IMAGE,fork 照抄、destroy 退回 */
void exec_txn_end_test(void);            /* I-240 C3.2 6.2:mb_exec_end 之後憑證不再放寬 */
void exec_txn_lower_limit_test(void);    /* I-240 C3.2 6.2:交易中途調低 limit,最終檢查拒絕 */
void pte_tear_test(void);                /* I-090:頁表項目 64 位元寫入不撕裂(需要 2 顆 CPU) */
void crash_test(const char *cmd);        /* crash=<種類>:故意觸發錯誤(排程器啟動之前可以做的那些) */

/* ---- kernel/test/sched_test.c ---- */
void sched_demo(void);                   /* 讓出、搶佔、休眠 */
void sync_demo(void);                    /* mutex、號誌、條件變數 */
void prio_demo(void);                    /* 優先權、搶佔延遲、優先權繼承、防飢餓 */
void smp_sched_demo(void);               /* 多顆 CPU 同時排程 */
void rt_demo(void);                      /* 週期性的即時工作 */
void sleep_wall_test(void);
void clksync_test(void);                 /* I-337:跨 CPU 計數器同步驗證(含注入偏移的負對照) */
void cpu0_stall_wake_test(void);         /* 第 0 顆 CPU 關中斷 300 ms 時休眠者照樣醒(I-337) */              /* 睡眠以真實時間計不可提早醒(高精度時鐘量) */
void sched_stress(void);                 /* stress:大量執行緒 */
void sched_crash_test(const char *cmd);  /* crash=<種類>:需要排程器的那些 */
void spin_irqs_off(void *arg);           /* crash=wdog 用:關著中斷空轉的執行緒 */
void spin_forever(void *arg);            /* crash=smp 用:永遠忙碌的執行緒 */

/* ---- kernel/test/io_test.c ---- */
void nic_selftest(void);                 /* 網路卡驅動程式(不經過協定堆疊) */
void disk_demo(void);                    /* 磁碟上的檔案系統 */
void wfs_mount_failinj_test(void);       /* 掛載 / 格式化路徑的配置失敗掃描(記憶體磁碟,disk_init 之前) */
void disk_stage_test(const char *cmd);   /* disktest=N:跨開機的磁碟測試 */
void disk_user_stage_test(const char *cmd);  /* disktest=12~15:使用者程式在寫回失敗時(網路啟動之後) */
void fs_demo(void);                      /* VFS 與記憶體檔案系統 */
void pipe_demo(void);                    /* 管線 */

/* ---- kernel/test/user_test.c ---- */
int  spawn_init(long mode);              /* 以測試模式 mode(uapi/selftest.h)啟動 /bin/init;回傳 pid */
void user_demo(void);                    /* 以各種模式啟動 /bin/init,核對結束碼 */
#endif
