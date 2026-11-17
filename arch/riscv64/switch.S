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

/*
 * void arch_user_return(entry, usp, arg)            a0, a1, a2
 * 第一次進入使用者模式。做法是偽造成「從使用者模式的 trap 返回」:
 *   sepc    = 進入點(sret 會跳到這裡)
 *   sstatus = SPP 清為 0(返回後是 U-mode)、SPIE 設為 1(返回後開中斷)
 *   sscratch= 這顆 CPU 的 struct cpu(約定:在使用者模式期間存放它,見 trap.S)
 * 其餘暫存器全部清 0,不把 kernel 內部的數值(可能是位址)洩漏給使用者程式。
 */
/*
 * void arch_user_resume_asm(struct trap_frame *tf)      a0
 * 依 tf 的內容進入使用者模式:把 sp 指向它,接到例外入口的返回路徑。
 * 那段路徑會還原所有暫存器、換成使用者的 sp 再 sret,和「從一次系統呼叫返回」完全一樣。
 * 呼叫時中斷必須已關閉。
 */
    .global arch_user_resume_asm
arch_user_resume_asm:
    mv      sp, a0
    j       trap_return

    .global arch_user_return
arch_user_return:
    csrw    sepc, a0
    csrr    t0, sstatus
    li      t1, 0x100                 /* SPP */
    not     t1, t1
    and     t0, t0, t1
    ori     t0, t0, 0x20              /* SPIE */
    csrw    sstatus, t0
    csrw    sscratch, tp              /* 要在下面把 tp 清 0 之前做 */
    mv      sp, a1
    mv      a0, a2                    /* 第一個參數 */
    .irp    n, 1,3,4,5,6,7,8,9,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    li      x\n, 0
    .endr
    sret
