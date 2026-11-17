/* linker.ld (AArch64, QEMU virt) */
ENTRY(_start)

SECTIONS
{
    /* virt 機器的 RAM 自 0x40000000 起;QEMU 把 DTB 放在 RAM 開頭,
       kernel 慣例從 +0x80000 開始,避開 DTB */
    . = 0x40080000;
    __kernel_start = .;                     /* kernel 映像的起點(記憶體管理員用) */

    .text   : { *(.text.boot) *(.text*) }   /* _start 所在的 .text.boot 放最前面 */
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    . = ALIGN(8);                           /* boot.S 以 8 位元組為單位清 BSS */
    .bss    : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __kernel_end = .;                       /* kernel 映像的終點;之後的記憶體才可配置 */
}
