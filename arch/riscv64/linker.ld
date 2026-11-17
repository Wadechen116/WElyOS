/* linker.ld (RISC-V 64, QEMU virt) */
ENTRY(_start)

SECTIONS
{
    /* OpenSBI 佔用 0x80000000 起的空間,S-mode kernel 慣例載入於 0x80200000 */
    . = 0x80200000;
    __kernel_start = .;                     /* kernel 映像的起點(記憶體管理員用) */

    .text   : { *(.text.boot) *(.text*) }   /* _start 所在的 .text.boot 放最前面 */
    /* 各區段對齊到頁(4KB):分頁的權限以頁為單位,
       程式碼、唯讀資料、可寫資料不能共用同一頁 */
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata*) *(.srodata*) }
    . = ALIGN(4096);
    __rodata_end = .;
    .data   : { *(.data*) *(.sdata*) }

    . = ALIGN(8);                           /* boot.S 以 8 位元組為單位清 BSS */
    .bss    : {
        __bss_start = .;
        *(.sbss*) *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __kernel_end = .;                       /* kernel 映像的終點;之後的記憶體才可配置 */
}
