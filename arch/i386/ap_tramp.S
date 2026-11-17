/*
 * ap_tramp.S (i386) - 其他 CPU(AP)的啟動跳板
 *
 * x86 的 AP 被叫醒時處於「真實模式」(real mode):16 位元、沒有分頁、只能定址 1MB,
 * 和 1981 年的 IBM PC 一樣。它從 SIPI(Startup IPI)指定的那一頁(本專案用 0x8000)
 * 開始執行。kernel 本身在 1MB 以上、是 32 位元程式,AP 在真實模式下根本到不了,
 * 所以需要一小段「跳板」:
 *
 *   1. kernel 把這段程式複製到 0x8000(smp.c)
 *   2. AP 從 0x8000 開始:載入臨時的 GDT -> 切到 32 位元保護模式
 *   3. 設定和開機的 CPU 相同的 CR4、EFER、CR3、CR0(開啟分頁)
 *   4. 換到 kernel 替它準備的 stack,跳進 C 程式 arch_ap_start()
 *
 * 這段程式被組譯時位在 kernel 映像裡,執行時卻在 0x8000,所以凡是引用「跳板內部」
 * 的位址都要自己換算:TRAMP + (標籤 - ap_tramp_start)。
 * 引用 kernel 的變數(ap_boot_*)則直接用它們的位址:進入保護模式後、開啟分頁之前,
 * 位址就是實體位址,而 kernel 正是載入在它連結的位址上。
 */
#define TRAMP 0x8000
#define T(sym) (TRAMP + (sym - ap_tramp_start))

    .section .rodata
    .global ap_tramp_start, ap_tramp_end
    .balign 16
    .code16
ap_tramp_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    lgdtl   T(tramp_gdt_ptr)          /* 臨時的 GDT:平坦的 4GB 程式碼與資料區段 */
    movl    %cr0, %eax
    orl     $1, %eax                  /* CR0.PE:進入保護模式 */
    movl    %eax, %cr0
    ljmpl   $0x08, $T(tramp32)        /* 遠跳躍載入 cs,之後才真的以 32 位元執行 */

    .code32
tramp32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* 以下的值由開機的 CPU 在送出 SIPI 之前填好,內容和它自己使用的相同 */
    movl    ap_boot_cr4, %eax         /* PAE、全域頁、SMEP、SMAP */
    movl    %eax, %cr4
    movl    ap_boot_efer, %eax        /* EFER(含 NXE);0 = 這顆 CPU 沒有 NX,不必設 */
    testl   %eax, %eax
    jz      1f
    movl    $0xC0000080, %ecx
    xorl    %edx, %edx
    wrmsr
1:  movl    ap_boot_cr3, %eax         /* kernel 的頁表 */
    movl    %eax, %cr3
    movl    ap_boot_cr0, %eax         /* 含 PG:這一行之後分頁開啟。
                                         下一條指令仍在 0x8000 這一頁,所以它必須在頁表中
                                         對映成可執行(smp.c 負責) */
    movl    %eax, %cr0

    movl    ap_boot_stack, %esp
    xorl    %ebp, %ebp                /* frame pointer 歸零:backtrace 的終點 */
    movl    $arch_ap_start, %eax
    call    *%eax                     /* 絕對位址:離開跳板,進入 kernel。不會返回 */
2:  hlt
    jmp     2b

    .balign 8
tramp_gdt:
    .quad   0                         /* 空描述符 */
    .quad   0x00CF9A000000FFFF        /* 0x08:ring 0 程式碼,基底 0、長度 4GB */
    .quad   0x00CF92000000FFFF        /* 0x10:ring 0 資料 */
tramp_gdt_ptr:
    .word   23                        /* 長度減 1 */
    .long   T(tramp_gdt)
ap_tramp_end:
