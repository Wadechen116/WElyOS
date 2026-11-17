/*
 * boot.S (AArch64) - QEMU virt 機器的啟動程式
 *
 * 開機流程:
 *   1. QEMU 直接把 kernel 載入 RAM(0x40080000,linker.ld 指定)並跳到 _start。
 *      注意:必須以原始二進位(welyos.bin)載入,QEMU 才會設定 x0;ELF 不會。
 *   2. 進入時:通常為 EL1,MMU 與快取關閉,中斷遮蔽;
 *        x0 = 裝置樹(DTB)的實體位址
 *      每個核心都可能進入,sp 未定義。
 *   3. 本檔讓非 0 號核心休眠,設 stack、清 BSS,呼叫 C 函式 arch_start(dtb)。
 *
 * 注意:MMU 關閉時記憶體被視為 Device 類型,未對齊存取會觸發例外,
 * 所以 C 程式以 -mstrict-align 編譯,且不使用浮點/SIMD(-mgeneral-regs-only)。
 */
    .section .text.boot
    .global _start
_start:
    /* mpidr_el1 的低 8 位元為核心編號;非 0 號核心停在 park */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbnz    x1, park

    /* 設定 stack。adrp+add 以位置無關的方式取得符號位址 */
    adrp    x1, stack_top
    add     x1, x1, :lo12:stack_top
    mov     sp, x1                    /* AArch64 要求 sp 16 位元組對齊 */
    mov     x29, xzr                  /* frame pointer 歸零:backtrace 以 0 判斷鏈的終點 */

    /* TPIDR_EL1 = 第 0 顆 CPU 的 struct cpu(kernel 中固定如此,見 cpu_arch.h) */
    adrp    x1, cpus
    add     x1, x1, :lo12:cpus
    msr     tpidr_el1, x1

    /* 清除 .bss,以 8 位元組為單位;x0(dtb)保持不動 */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    bl      arch_start                /* x0 = dtb 原樣傳入(C 呼叫慣例) */

park:                                 /* arch_start 不返回;其他核心也停在這裡 */
    wfe
    b       park

/*
 * 其他 CPU 的起點(由 PSCI 的 CPU_ON 指定):x0 = 它的 struct cpu。
 * 和 _start 的差別:BSS 已經清過,stack 用的是 kernel 替它配置的那一條。
 */
    .section .text
    .global ap_entry
ap_entry:
    msr     tpidr_el1, x0             /* TPIDR_EL1 = 自己的 struct cpu */
    ldr     x1, [x0, #8]              /* struct cpu 的 kstack_top */
    mov     sp, x1
    mov     x29, xzr
    bl      arch_ap_start             /* 不會返回 */
1:  wfe
    b       1b

    /* 16KB 初始 stack,放在 .bss */
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
