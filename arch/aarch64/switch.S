/*
 * switch.S (AArch64) - context switch
 *
 * void arch_switch(void **old_sp, void *new_sp);     x0 = old_sp, x1 = new_sp
 *
 * 只需要存「callee-saved」暫存器:x19~x28、x29(frame pointer)、x30(返回位址)。
 * 其餘(x0~x18)依呼叫慣例在函式呼叫後本來就不保證保留。
 * 共 12 個 = 96 位元組(已是 16 的倍數)。
 * arch_stack_init 要仿造同樣的版面:slot 0~9 = x19~x28,slot 10 = x29,slot 11 = x30。
 */
#define CTX_SIZE 96

    .section .text
    .global arch_switch
arch_switch:
    sub     sp, sp, #CTX_SIZE
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x9, sp                /* sp 不能直接當 str 的來源,先搬到一般暫存器 */
    str     x9, [x0]              /* 記下舊執行緒的 stack pointer */
    mov     sp, x1                /* ---- 從這一行起,已經在新執行緒的 stack 上 ---- */
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #CTX_SIZE
    ret                           /* 跳到 x30:新執行緒上次切出去的地方 */

/*
 * 新執行緒的起點。arch_stack_init 把初始的 x30 設成這裡、x19 設成真正的進入函式。
 * 多這一小段的目的是把 x30 清成 0 再跳過去:進入函式會把 x30 存進自己的 frame,
 * backtrace 讀到 0 就知道這是呼叫鏈的終點,不會多印一層無意義的位址。
 */
    .global arch_thread_start
arch_thread_start:
    mov     x30, xzr
    br      x19
