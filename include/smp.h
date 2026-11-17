#ifndef WELYOS_SMP_H
#define WELYOS_SMP_H
#include "boot_info.h"

struct cpu;

/*
 * 多核心(Symmetric Multiprocessing,SMP)。
 *
 * 開機時只有一顆 CPU 在執行(BSP,Bootstrap Processor);其他的(AP,Application
 * Processor)停在韌體裡,要由 kernel 逐一叫醒。分兩個階段:
 *
 *   smp_init()         叫醒所有 AP。它們完成自己的初始化後「待命」,還不執行任何執行緒
 *                      (run_test 非 0 時先一起跑一輪並行測試)
 *   smp_sched_start()  讓待命的 AP 啟動自己的計時器、進入排程器。從這一刻起
 *                      執行緒會真正同時在多顆 CPU 上執行
 *
 * 分開的原因:有些測試檢查的是「單一 CPU 上的排程行為」(例如誰先執行),
 * 要在其他 CPU 加入之前跑。
 * 兩者都必須在排程器啟動、開中斷之後,由開機的 CPU 呼叫。
 */
void smp_init(const struct boot_info *bi, int run_test);
void smp_sched_start(void);

/* 已啟動的 CPU 數(含開機的那一顆;不論是否已參與排程) */
int smp_started(void);

/* AP 啟動後由架構層呼叫(此時已在自己的 stack 上、分頁已開、this_cpu() 可用)。不會返回 */
void smp_ap_main(void) __attribute__((noreturn));

/*
 * ---- 處理器間中斷(Inter-Processor Interrupt,IPI) ----
 *
 * 一顆 CPU 要求另一顆 CPU 做事的方法:在對方的 struct cpu 上設定「要做什麼」的位元,
 * 再對它發出中斷。硬體只提供「叫一下對方」,不能附帶內容,所以內容放在共用的記憶體裡。
 */
#define IPI_RESCHED 1u       /* 你該重新排程了(need_resched 已由發送者設定) */
#define IPI_CALL    2u       /* 執行跨 CPU 呼叫(見 smp_call_others) */
#define IPI_STOP    4u       /* 系統 panic 了:立刻停下來 */

void smp_send(struct cpu *c, unsigned what);

/* 架構層收到 IPI 時呼叫(中斷關閉中) */
void smp_ipi_handler(void);

/* 不靠中斷、直接檢查有沒有別顆 CPU 的要求並處理。給「關著中斷等待」的地方使用
   (例如 spinlock 的等待迴圈)。呼叫時中斷必須已關閉 */
void smp_poll(void);

/*
 * 跨 CPU 呼叫:讓「其他所有參與排程的 CPU」各執行一次 fn(arg),全部做完才返回。
 * 用於必須讓每顆 CPU 都知道的變更,例如頁表改了之後要各自清除 TLB。
 *
 * fn 會在對方的中斷處理程式(或 spinlock 的等待迴圈)中執行,所以:
 *   - 必須很短,而且「不可以取得任何鎖」
 *   - 只能動到那顆 CPU 自己的狀態
 * 呼叫者可以持有鎖(等這把鎖的 CPU 會在等待迴圈中回應),但不可以持有排程器的鎖。
 */
void smp_call_others(void (*fn)(void *), void *arg);

/* panic 時呼叫:要求其他所有 CPU 停止(不等它們回應) */
void smp_stop_others(void);
#endif
