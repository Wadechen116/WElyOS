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
    /*
     * 第一步:分辨是從使用者模式還是 kernel 進來的,並取得可用的 kernel stack。
     *
     * 約定:sscratch 在「使用者模式執行期間」存放該執行緒的 kernel stack 頂端,
     *       在「kernel 執行期間」為 0。
     * 把 sp 與 sscratch 交換後看 sp:
     *   非 0 -> 從使用者模式來。sp 現在是 kernel stack,使用者的 sp 在 sscratch
     *   為 0 -> 從 kernel 來。再交換一次換回原樣,繼續用原本的 stack
     * 不能直接沿用使用者的 sp:那是不可信任的值,可能指向任何地方。
     */
    csrrw   sp, sscratch, sp
    bnez    sp, from_user

    /* ---- 從 kernel 來 ---- */
    csrrw   sp, sscratch, sp          /* 換回來:sp = 原本的 kernel sp,sscratch = 0 */

    /*
     * 檢查 stack 還放不放得下 trap frame。
     * 如果例外的原因正是 stack 溢位,sp 此刻指著保護頁(沒有對映),
     * 直接往下存暫存器會再次觸發例外,又進到這裡,無窮迴圈。
     * 所以先比較 sp 與目前執行緒的 stack 下限(kstack_limit,排程器維護);
     * 不夠就改用專用的例外 stack(exc_stack),讓處理程式能正常執行並回報。
     *
     * 做比較需要一個暫存器,但此刻所有暫存器都還是被打斷程式的值,
     * 不能直接拿來用,所以借 sscratch 暫放 t0,用完再歸 0。
     */
    csrw    sscratch, t0
    la      t0, kstack_limit
    ld      t0, 0(t0)
    addi    t0, t0, FRAME_SIZE        /* sp 至少要比下限高出一個 frame */
    bgeu    sp, t0, 1f

    /* 溢位:改用例外 stack;原本的 sp 記進 frame 供回報 */
    mv      t0, sp
    la      sp, exc_stack_top
    addi    sp, sp, -FRAME_SIZE
    sd      t0, 8(sp)                 /* x2 的 slot = 被打斷時的 sp */
    j       2f

1:  addi    sp, sp, -FRAME_SIZE       /* 正常:在目前 stack 上挖出 trap frame */
    addi    t0, sp, FRAME_SIZE
    sd      t0, 8(sp)                 /* x2 的 slot = 被打斷時的 sp */

2:  csrr    t0, sscratch              /* 取回 t0 原本的值 */
    csrw    sscratch, zero            /* 恢復約定:在 kernel 中 sscratch 為 0 */
    j       save_regs

    /* ---- 從使用者模式來:sp = kernel stack 頂端,sscratch = 使用者的 sp ---- */
from_user:
    addi    sp, sp, -FRAME_SIZE
    sd      t0, 4*8(sp)               /* 先存 t0(x5 的 slot),騰出一個暫存器 */
    csrr    t0, sscratch
    sd      t0, 8(sp)                 /* x2 的 slot = 使用者的 sp */
    csrw    sscratch, zero            /* 現在進入 kernel 了:sscratch 歸 0 */
    ld      t0, 4*8(sp)

save_regs:
    /* 存 x1 與 x3~x31(x2 即 sp,上面已存) */
    .irp    n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, (\n-1)*8(sp)
    .endr

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

    /* sstatus 的 SPP 位元(bit 8)記錄被打斷時的模式:0 = 使用者模式 */
    andi    t0, t0, 0x100
    bnez    t0, 3f

    /* ---- 返回使用者模式 ---- */
    addi    t0, sp, FRAME_SIZE
    csrw    sscratch, t0              /* 依約定存入 kernel stack 頂端,下次進來時使用 */
    .irp    n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, (\n-1)*8(sp)
    .endr
    ld      sp, 8(sp)                 /* 最後才換回使用者的 sp */
    sret

3:  /* ---- 返回 kernel ---- */

    /* 還原 x1、x3~x31(最後一次載入 t0 會蓋掉上面用的暫存值,正確) */
    .irp    n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, (\n-1)*8(sp)
    .endr

    addi    sp, sp, FRAME_SIZE        /* 釋放 trap frame,sp 回到被打斷時的值 */
    sret                              /* 回到 sepc,並依 sstatus.SPIE 還原中斷狀態 */

/*
 * 例外專用 stack(8KB):只在偵測到 kernel stack 溢位時使用。
 * 那種情況必定以 panic 收場,不會返回,所以全系統共用一條即可。
 */
    .section .bss
    .balign 16
exc_stack:
    .skip 8192
    .global exc_stack_top
exc_stack_top:
