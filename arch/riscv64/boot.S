/*
 * boot.S (RISC-V 64) - QEMU virt 機器的啟動程式
 *
 * 開機流程:
 *   1. QEMU 內建的 OpenSBI(M-mode 韌體)先執行,完成基本初始化。
 *   2. OpenSBI 把 kernel 載入 0x80200000(linker.ld 指定),切到 S-mode 後跳到 _start。
 *   3. 進入時的暫存器(SBI 規範):
 *        a0 = hartid(目前 CPU 核心編號)
 *        a1 = 裝置樹(DTB)的實體位址
 *      MMU 關閉、中斷關閉,sp 未定義。
 *   4. 本檔設 stack、清 BSS、呼叫 C 函式 arch_start(hartid, dtb)。
 */
    .section .text.boot
    .global _start
_start:
    /*
     * 只讓「第一顆到達這裡的 hart」往下執行。哪一顆 hart 負責開機由 OpenSBI 決定,
     * 不一定是 0 號;其他 hart 正常情況下停在韌體裡,等 kernel 之後用 SBI 叫醒
     * (見 smp.c 與下面的 ap_entry)。保險起見,萬一韌體讓多顆同時進來,
     * 用一個原子交換搶旗標:搶到的繼續,其餘休眠。
     */
    la      t0, boot_lottery
    li      t1, 1
    amoswap.w t1, t1, (t0)
    bnez    t1, park

    la      sp, stack_top             /* 設定 stack(RISC-V stack 同樣往低位址成長) */
    la      tp, cpus                  /* tp = 第 0 顆 CPU 的 struct cpu(kernel 中固定如此,見 cpu_arch.h) */
    li      s0, 0                     /* frame pointer 歸零:backtrace 以 0 判斷鏈的終點 */

    /*
     * 清除 .bss(未初始化全域變數必須為 0)。ELF 載入器不保證替我們清,
     * 所以自己清。以 8 位元組為單位(linker.ld 保證 __bss_start/end 對齊 8)。
     * 使用 t0/t1 暫存,不動 a0/a1,它們還要傳給 arch_start。
     */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    call    arch_start                /* a0=hartid, a1=dtb 原樣傳入(C 呼叫慣例) */

park:                                 /* arch_start 不返回;其他核心也停在這裡 */
    wfi                               /* 等待中斷;中斷關閉時等同閒置 */
    j       park

/*
 * 其他 hart 的起點(由 SBI 的 hart_start 指定):a0 = hartid,a1 = 它的 struct cpu。
 * 和 _start 的差別:BSS 已經清過,stack 用的是 kernel 替它配置的那一條。
 */
    .global ap_entry
ap_entry:
    mv      tp, a1                    /* tp = 自己的 struct cpu */
    ld      sp, 8(tp)                 /* struct cpu 的 kstack_top */
    li      s0, 0
    csrw    sscratch, zero            /* 約定:在 kernel 中 sscratch 為 0(見 trap.S) */
    call    arch_ap_start             /* 不會返回 */
1:  wfi
    j       1b

    .section .data
    .balign 4
boot_lottery:                         /* 0 = 還沒有 hart 開機;放在 .data,不受清 BSS 影響 */
    .word   0

    /* 16KB 初始 stack,放在 .bss;RISC-V ABI 要求 16 位元組對齊 */
    .section .bss
    .balign 4096
    .global stack_guard               /* 保護頁:排程器啟動時移除對映,stack 用過頭會立刻出錯 */
stack_guard:
    .skip 4096
    .global stack_bottom
stack_bottom:
    .skip 16384
    .global stack_top
stack_top:
