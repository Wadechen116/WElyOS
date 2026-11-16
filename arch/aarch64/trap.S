/*
 * trap.S (AArch64) - EL1 的例外向量表與入口
 *
 * 例外發生時,硬體自動:
 *   1. 把返回位址存進 ELR_EL1,當時的處理器狀態(PSTATE)存進 SPSR_EL1
 *   2. 原因存進 ESR_EL1(高 6 位元 EC 為例外類別),出錯位址存進 FAR_EL1
 *   3. 遮蔽中斷,跳到「VBAR_EL1 + 偏移」。偏移由兩件事決定:
 *        - 例外來自哪裡(4 組):目前 EL 用 SP_EL0 / 目前 EL 用 SP_ELx /
 *          較低 EL 的 AArch64 / 較低 EL 的 AArch32
 *        - 例外種類(每組 4 種):同步(sync)、IRQ、FIQ、SError
 *      共 16 個入口,每個入口固定 0x80 位元組(32 條指令),整張表須 2KB 對齊。
 * 硬體不會存一般暫存器,入口必須自己存。
 *
 * trap frame(須與 trap.h 的 struct trap_frame 一致),共 36 slot = 288 位元組:
 *   slot 0..30 : x0..x30
 *   slot 31    : 被打斷時的 sp
 *   slot 32..35: elr、spsr、esr、far
 */
#define FRAME_SIZE 288

/* 每個向量入口空間有限(0x80),只做最少的事:挖 frame、存 x0/x1、帶著種類編號跳走 */
.macro VENTRY kind
    .align  7                         /* 每個入口對齊到 0x80 */
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\kind                /* 0~15:第幾個向量,供 C 判斷來源與種類 */
    b       trap_common
.endm

    .section .text
    .align  11                        /* 向量表須 2048 位元組對齊 */
    .global trap_vectors
trap_vectors:
    /* 目前 EL,使用 SP_EL0 */
    VENTRY  0                    /* sync */
    VENTRY  1                    /* irq */
    VENTRY  2                    /* fiq */
    VENTRY  3                    /* serror */
    /* 目前 EL,使用 SP_ELx(kernel 平常的狀態) */
    VENTRY  4                    /* sync */
    VENTRY  5                    /* irq */
    VENTRY  6                    /* fiq */
    VENTRY  7                    /* serror */
    /* 較低 EL,AArch64 */
    VENTRY  8                    /* sync */
    VENTRY  9                    /* irq */
    VENTRY  10                    /* fiq */
    VENTRY  11                    /* serror */
    /* 較低 EL,AArch32 */
    VENTRY  12                    /* sync */
    VENTRY  13                    /* irq */
    VENTRY  14                    /* fiq */
    VENTRY  15                    /* serror */

trap_common:
    /* 存 x2~x29(x0、x1 已在入口存好) */
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]

    add     x2, sp, #FRAME_SIZE       /* 被打斷時的 sp */
    stp     x30, x2, [sp, #240]

    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3, [sp, #256]
    mrs     x2, esr_el1
    mrs     x3, far_el1
    stp     x2, x3, [sp, #272]

    mov     x1, x0                    /* 參數 2:向量種類編號 */
    mov     x0, sp                    /* 參數 1:trap frame 指標 */
    bl      trap_handler              /* 可修改 frame(例如調整 elr 跳過指令) */

    /* 還原返回位址與處理器狀態(handler 可能改了 elr) */
    ldp     x2, x3, [sp, #256]
    msr     elr_el1, x2
    msr     spsr_el1, x3

    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret                              /* 回到 ELR_EL1,並以 SPSR_EL1 還原狀態 */
