/* linker.ld (AArch64, QEMU virt) */
ENTRY(_start)

SECTIONS
{
    /* virt 機器的 RAM 自 0x40000000 起;QEMU 把 DTB 放在 RAM 開頭,
       kernel 慣例從 +0x80000 開始,避開 DTB */
    . = 0x40080000;
    __kernel_start = .;                     /* kernel 映像的起點(記憶體管理員用) */

    .text   : { *(.text.boot) *(.text*) }   /* _start 所在的 .text.boot 放最前面 */
    /* 各區段對齊到頁(4KB):分頁的權限以頁為單位,
       程式碼、唯讀資料、可寫資料不能共用同一頁 */
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    __rodata_end = .;
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
