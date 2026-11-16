/*
 * trap.S (RISC-V 64) - S-mode 的例外/中斷入口
 *
 * 什麼是 trap:CPU 遇到例外(非法指令、存取錯誤、ebreak...)或中斷時,
 * 硬體自動:
 *   1. 把當時的 pc 存進 sepc,原因存進 scause,附加資訊(如出錯位址)存進 stval
 *   2. 關閉中斷(sstatus.SIE 清 0),記住原本的狀態於 sstatus.SPIE
 *   3. 跳到 stvec 指向的位址(本檔的 trap_entry)
 * 硬體「不會」替我們存任何一般暫存器,所以入口必須先把它們全部存起來,
 * 否則被打斷的程式會被破壞。
 *
 * 堆疊上存放的格式必須與 trap.c 的 struct trap_frame 完全一致:
 *   slot 0..30 : x1..x31(slot = 暫存器編號 - 1;x0 恆為 0 不用存)
 *   slot 31..34: sepc、sstatus、scause、stval
 *   共 35 個 slot = 280 位元組,補到 288 以維持 16 位元組對齊(ABI 要求)
 */
#define FRAME_SIZE 288

    .section .text
    .align 4                          /* stvec 要求位址 4 位元組對齊(直接模式) */
    .global trap_entry
trap_entry:
    addi    sp, sp, -FRAME_SIZE       /* 在目前 stack 上挖出 trap frame */

    /* 存 x1 與 x3~x31。x2 是 sp,稍後處理;t0(x5)先存下才能拿來當暫存 */
    .irp    n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, (\n-1)*8(sp)
    .endr

    /* 被打斷時的 sp = 目前 sp + FRAME_SIZE,存進 x2 的 slot */
    addi    t0, sp, FRAME_SIZE
    sd      t0, 8(sp)

    /* 存 CSR */
    csrr    t0, sepc
    sd      t0, 31*8(sp)
    csrr    t0, sstatus
    sd      t0, 32*8(sp)
    csrr    t0, scause
    sd      t0, 33*8(sp)
    csrr    t0, stval
    sd      t0, 34*8(sp)

    mv      a0, sp                    /* 參數:指向 trap frame 的指標 */
    call    trap_handler              /* C 語言處理函式;可修改 frame(例如改 sepc) */

    /* 還原 CSR:handler 可能調整了 sepc(例如跳過 ebreak) */
    ld      t0, 31*8(sp)
    csrw    sepc, t0
    ld      t0, 32*8(sp)
    csrw    sstatus, t0

    /* 還原 x1、x3~x31(最後一次載入 t0 會蓋掉上面用的暫存值,正確) */
    .irp    n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, (\n-1)*8(sp)
    .endr

    addi    sp, sp, FRAME_SIZE        /* 釋放 trap frame,sp 回到被打斷時的值 */
    sret                              /* 回到 sepc,並依 sstatus.SPIE 還原中斷狀態 */
