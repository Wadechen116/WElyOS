/*
 * switch.S (RISC-V 64) - context switch
 *
 * void arch_switch(void **old_sp, void *new_sp);     a0 = old_sp, a1 = new_sp
 *
 * 只需要存「callee-saved」暫存器:ra(返回位址)與 s0~s11。
 * 其餘(a0~a7、t0~t6)依呼叫慣例在函式呼叫後本來就不保證保留。
 * x86 的返回位址由 call 推上 stack;RISC-V 則放在 ra 暫存器,所以要自己存。
 *
 * 13 個暫存器 = 104 位元組,補到 112 以維持 16 位元組對齊。
 * arch_stack_init 要仿造同樣的版面:slot 0 = ra,slot 1~12 = s0~s11。
 */
#define CTX_SIZE 112

    .section .text
    .global arch_switch
arch_switch:
    addi    sp, sp, -CTX_SIZE
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      sp, 0(a0)             /* 記下舊執行緒的 stack pointer */
    mv      sp, a1                /* ---- 從這一行起,已經在新執行緒的 stack 上 ---- */
    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, CTX_SIZE
    ret                           /* 跳到 ra:新執行緒上次切出去的地方 */

/*
 * 新執行緒的起點。arch_stack_init 把初始的 ra 設成這裡、s1 設成真正的進入函式。
 * 多這一小段的目的是把 ra 清成 0 再跳過去:進入函式會把 ra 存進自己的 frame,
 * backtrace 讀到 0 就知道這是呼叫鏈的終點,不會多印一層無意義的位址。
 */
    .global arch_thread_start
arch_thread_start:
    li      ra, 0
    jr      s1
