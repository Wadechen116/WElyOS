/*
 * smp.c (AArch64) - 啟動其他 CPU
 *
 * QEMU virt 機器開機時只有一顆 CPU 在執行,其餘處於「關機」狀態。
 * 叫醒它們用 PSCI(Power State Coordination Interface)的 CPU_ON:
 *
 *     CPU_ON(目標 CPU 的 MPIDR, 起始位址, 參數)
 *
 * 那顆 CPU 會從「起始位址」開始執行(EL1、分頁關閉、中斷遮蔽),x0 = 我們給的參數。
 * 我們把參數設成它的 struct cpu 位址,起始位址設成 boot.S 的 ap_entry。
 * PSCI 的呼叫方式(conduit)在 QEMU virt 上是 hvc 指令(與 arch_exit 的關機相同)。
 *
 * MPIDR(Multiprocessor Affinity Register)是每顆 CPU 硬體上的編號,
 * 裝置樹裡 cpu 節點的 reg 就是它的親和值欄位。
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "cpu.h"
#include "smp.h"
#include "trap.h"
#include "vm.h"

#define PSCI_CPU_ON 0xC4000003UL             /* 64 位元呼叫慣例的 CPU_ON */
#define MPIDR_AFF_MASK 0xFF00FFFFFFUL        /* MPIDR 中的親和值欄位(Aff3、Aff2~Aff0) */

void ap_entry(void);                         /* boot.S */

int arch_smp_discover(const struct boot_info *bi, uint64_t *ids, int max)
{
    uint64_t self;
    int n = 0;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
    self &= MPIDR_AFF_MASK;
    cpus[0].arch_id = self;

    for (unsigned i = 0; i < bi->ncpus && n < max; i++)
        if (bi->cpu_ids[i] != self)
            ids[n++] = bi->cpu_ids[i];
    return n;
}

int arch_smp_start(struct cpu *c)
{
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON;
    register uint64_t x1 __asm__("x1") = c->arch_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)(uintptr_t)ap_entry;
    register uint64_t x3 __asm__("x3") = (uint64_t)(uintptr_t)c;

    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return x0 == 0 ? 0 : -1;                 /* 0 = SUCCESS */
}

/* AP 的第一段 C 程式:由 ap_entry 呼叫,此時 TPIDR_EL1 與 sp 已設定,分頁還沒開 */
void arch_ap_start(void)
{
    trap_init();                             /* 每顆 CPU 有自己的 VBAR_EL1 */
    vm_enable_secondary();                   /* 每顆 CPU 有自己的 TTBR0、TCR、SCTLR */
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
