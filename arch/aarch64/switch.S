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

/*
 * void arch_user_return(entry, usp, arg, ktop)      x0, x1, x2, x3
 * 第一次進入使用者模式。做法是偽造成「從 EL0 的例外返回」:
 *   ELR_EL1  = 進入點(eret 會跳到這裡)
 *   SPSR_EL1 = 0:模式 EL0t,DAIF 全為 0(開中斷)
 *   SP_EL0   = 使用者的 stack pointer
 * 並把 SP_EL1 重設為 kernel stack 頂端:之後從 EL0 進入例外時就從這裡開始用。
 * 其餘暫存器全部清 0,不把 kernel 內部的數值(可能是位址)洩漏給使用者程式。
 */
/*
 * void arch_user_resume_asm(struct trap_frame *tf, uintptr_t ktop)      x0, x1
 * 依 tf 的內容進入使用者模式。
 *
 * AArch64 從使用者模式進入例外時,kernel stack 從「上次離開時的 SP_EL1」開始用,
 * 所以 trap frame 必須擺在 kernel stack 的最頂端,返回後 SP_EL1 才會回到頂端。
 * 先把 tf 搬到 [ktop - 304, ktop),再接到例外入口的返回路徑。
 * tf 本身也在這條 stack 上、位置比較低,兩塊可能重疊,所以「從尾端往前」搬
 * (目的地在比較高的位址時,這個方向不會蓋掉還沒搬的部分)。
 * 搬的過程會蓋掉呼叫者的 stack 內容,但我們不會再返回,不需要它們了。
 * 呼叫時中斷必須已關閉。
 */
    .global arch_user_resume_asm
arch_user_resume_asm:
    mov     x2, #304                  /* FRAME_SIZE(見 trap.S) */
1:  sub     x2, x2, #8
    ldr     x3, [x0, x2]
    sub     x4, x1, #304
    str     x3, [x4, x2]
    cbnz    x2, 1b
    sub     sp, x1, #304
    b       trap_return

    .global arch_user_return
arch_user_return:
    msr     elr_el1, x0
    msr     spsr_el1, xzr
    msr     sp_el0, x1
    msr     tpidrro_el0, xzr
    mov     sp, x3
    mov     x0, x2                    /* 第一個參數 */
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    mov     x\n, xzr
    .endr
    eret
