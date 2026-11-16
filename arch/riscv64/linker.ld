/* linker.ld (RISC-V 64, QEMU virt) */
ENTRY(_start)

SECTIONS
{
    /* OpenSBI 佔用 0x80000000 起的空間,S-mode kernel 慣例載入於 0x80200000 */
    . = 0x80200000;

    .text   : { *(.text.boot) *(.text*) }   /* _start 所在的 .text.boot 放最前面 */
    .rodata : { *(.rodata*) *(.srodata*) }
    .data   : { *(.data*) *(.sdata*) }

    . = ALIGN(8);                           /* boot.S 以 8 位元組為單位清 BSS */
    .bss    : {
        __bss_start = .;
        *(.sbss*) *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
}
