/*
 * smp.c (RISC-V 64) - 啟動其他 hart
 *
 * RISC-V 把一顆 CPU 核心稱為 hart(hardware thread)。開機時 OpenSBI 只讓其中一顆
 * 進入 kernel(哪一顆不一定,不保證是 0 號),其餘停在韌體裡。
 * 要叫醒它們,用 SBI 的 HSM(Hart State Management)擴充:
 *
 *     sbi_hart_start(hartid, 起始位址, 參數)
 *
 * 那顆 hart 會以 S-mode 從「起始位址」開始執行,進入時:
 *     a0 = 它自己的 hartid,a1 = 我們給的參數,分頁關閉、中斷關閉
 * 我們把參數設成它的 struct cpu 位址,起始位址設成 boot.S 的 ap_entry。
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "cpu.h"
#include "smp.h"
#include "trap.h"
#include "vm.h"

#define SBI_EXT_HSM        0x48534DUL        /* "HSM" */
#define SBI_HSM_HART_START 0UL
#define SBI_EXT_IPI        0x735049UL        /* "sPI":處理器間中斷擴充 */
#define SBI_IPI_SEND       0UL

void ap_entry(void);                         /* boot.S */

/* SBI 呼叫慣例:a7 = 擴充 ID,a6 = 功能 ID,a0~a2 = 參數;返回時 a0 = 錯誤碼(0 = 成功) */
static long sbi_call3(unsigned long ext, unsigned long fid,
                      unsigned long x0, unsigned long x1, unsigned long x2)
{
    register unsigned long a0 __asm__("a0") = x0;
    register unsigned long a1 __asm__("a1") = x1;
    register unsigned long a2 __asm__("a2") = x2;
    register unsigned long a6 __asm__("a6") = fid;
    register unsigned long a7 __asm__("a7") = ext;

    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
    return (long)a0;
}

int arch_smp_discover(const struct boot_info *bi, uint64_t *ids, int max)
{
    int n = 0;

    /* 裝置樹列出所有 hart;扣掉自己(開機時記在 cpus[0].arch_id) */
    for (unsigned i = 0; i < bi->ncpus && n < max; i++)
        if (bi->cpu_ids[i] != cpus[0].arch_id)
            ids[n++] = bi->cpu_ids[i];
    return n;
}

int arch_smp_start(struct cpu *c)
{
    return sbi_call3(SBI_EXT_HSM, SBI_HSM_HART_START, (unsigned long)c->arch_id,
                     (unsigned long)(uintptr_t)ap_entry, (unsigned long)(uintptr_t)c) == 0 ? 0 : -1;
}

/* AP 的第一段 C 程式:由 ap_entry 呼叫,此時 tp 與 sp 已設定,分頁還沒開 */
void arch_ap_start(void)
{
    trap_init();                             /* 每顆 hart 有自己的 stvec */
    vm_enable_secondary();                   /* 每顆 hart 有自己的 satp */
    smp_ap_main();
}

void arch_smp_park(void)
{
    arch_irq_disable();
    for (;;)
        __asm__ volatile("wfi");
}

void arch_smp_done(void)
{
}

/* 接收 IPI 需要的設定(sie 的 SSIE)已在 trap_init 完成 */
void arch_smp_cpu_init(void)
{
}

/*
 * 發出處理器間中斷:sbi_send_ipi(hart_mask, hart_mask_base)。
 * 目標以位元遮罩表示:bit i 代表 hartid = base + i。這裡一次只送一顆,
 * 所以 base 設成目標的 hartid、遮罩為 1。目標 hart 會收到 S-mode 軟體中斷。
 */
void arch_ipi_send(struct cpu *c)
{
    sbi_call3(SBI_EXT_IPI, SBI_IPI_SEND, 1UL, (unsigned long)c->arch_id, 0);
}
