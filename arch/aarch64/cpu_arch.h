#ifndef WELYOS_CPU_ARCH_H
#define WELYOS_CPU_ARCH_H
/*
 * AArch64:系統暫存器 TPIDR_EL1 固定存放這顆 CPU 的 struct cpu 位址。
 * 它只有 EL1(kernel)能存取,使用者程式碰不到,所以進出使用者模式都不必處理。
 */
struct cpu;

static inline struct cpu *arch_this_cpu(void)
{
    struct cpu *c;
    __asm__("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

/* 自旋等待時的提示:告訴 CPU 目前只是在空等 */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile("yield");
}
#endif
