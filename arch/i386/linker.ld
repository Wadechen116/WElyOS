/* linker.ld - 決定 kernel 各區段在記憶體中的位置 */

ENTRY(_start)                 /* 程式進入點:boot.S 的 _start */

SECTIONS
{
    . = 1M;                   /* 從 1MB 開始;低於 1MB 為 BIOS、VGA 等保留區 */
    __kernel_start = .;       /* kernel 映像的起點(記憶體管理員用) */

    /* 程式碼。.multiboot 必須最先放,bootloader 才能在檔案開頭找到 header */
    .text   : { *(.multiboot) *(.text*) }

    .rodata : { *(.rodata*) } /* 唯讀資料(字串常數等) */
    .data   : { *(.data*) }   /* 已初始化的全域變數 */
    .bss    : {                /* 未初始化的全域變數與 stack;載入時為 0 */
        __bss_start = .;
        *(COMMON) *(.bss*)
        __bss_end = .;
    }
    __kernel_end = .;         /* kernel 映像的終點;之後的記憶體才可配置 */
}
