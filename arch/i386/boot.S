/*
 * boot.S - WElyOS 最早執行的程式(i386, AT&T 語法)
 *
 * 開機流程:
 *   1. QEMU(-kernel)在 ELF 檔內找到 multiboot header(見下方 .multiboot 區段),
 *      把 kernel 載入 linker.ld 指定的 1MB 位址。
 *   2. QEMU 跳到 _start。此時 CPU 狀態(由 multiboot 規範保證):
 *        - 32 位元保護模式,分頁關閉,中斷關閉
 *        - eax = 0x2BADB002(multiboot 魔術數)
 *        - ebx = multiboot info 結構的實體位址
 *        - esp 無效,尚未有 stack  <-- 所以第一件事必須自己設定 stack
 *   3. 本檔設好 stack 後呼叫 C 函式 arch_start(magic, info)(arch/i386/start.c),
 *      它再整理開機資訊並進入與架構無關的 kmain。
 */

/* ---- multiboot v1 header 的三個欄位 ---- */
.set MB_MAGIC,    0x1BADB002              /* 魔術數:bootloader 靠它辨識 kernel */
.set MB_FLAGS,    0                       /* 0 = 不要求額外功能(記憶體對齊、圖形模式等) */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)  /* 三欄位相加必須為 0(32 位元溢位) */

/*
 * header 必須位於檔案前 8KB 內且 4 位元組對齊。
 * linker.ld 會把 .multiboot 放在 .text 最前面以滿足此條件。
 */
.section .multiboot
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

/*
 * kernel 的初始 stack:16KB,放在 .bss(不佔檔案空間,載入後為 0)。
 * x86 的 stack 往低位址成長,所以 esp 要指向「最高位址」stack_top。
 * 16 位元組對齊是 System V i386 ABI 對呼叫時 stack 的要求。
 */
.section .bss
.align 16
.global stack_bottom
stack_bottom:
.skip 16384
.global stack_top
stack_top:

.section .text
.global _start                /* linker.ld 的 ENTRY(_start) 指向此處 */
_start:
    mov $stack_top, %esp      /* 設定 stack,C 程式才能運作 */
    xor %ebp, %ebp            /* frame pointer 歸零:backtrace 以 0 判斷鏈的終點 */

    /*
     * 依 cdecl 呼叫慣例,參數由右到左推入 stack:
     *   arch_start(uint32_t magic, uint32_t mbi_addr)
     * 所以先推第 2 個參數 ebx,再推第 1 個參數 eax。
     * (eax/ebx 保存著 bootloader 傳入的值,到此為止尚未被改動)
     */
    push %ebx                 /* 參數 2:multiboot info 位址 */
    push %eax                 /* 參數 1:multiboot 魔術數 */
    call arch_start

    /*
     * arch_start 正常不會返回;萬一返回,就關中斷並停住 CPU。
     * hlt 在中斷關閉時會永久停止,但 NMI 等仍可能喚醒,所以放進迴圈。
     */
1:  cli
    hlt
    jmp 1b                    /* 1b = 往回找最近的標籤 "1:" */
