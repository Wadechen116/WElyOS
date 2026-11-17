#ifndef WELYOS_CPU_ARCH_H
#define WELYOS_CPU_ARCH_H
/*
 * RISC-V:kernel 執行期間,tp(thread pointer,x4)固定指向這顆 CPU 的 struct cpu。
 * 編譯器不會自行使用 tp(它保留給執行緒區域儲存,而 kernel 沒有使用)。
 * 使用者程式可以任意更改 tp,所以例外入口會把它換回來(見 trap.S)。
 */
struct cpu;

static inline struct cpu *arch_this_cpu(void)
{
    struct cpu *c;
    __asm__("mv %0, tp" : "=r"(c));
    return c;
}

/* 自旋等待時的提示。基本指令集沒有專用指令,用 nop 即可 */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile("nop");
}
#endif
