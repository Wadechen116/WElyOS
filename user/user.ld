/*
 * user.ld - 使用者程式的 linker script
 *
 * 使用者程式是獨立連結的,和 kernel 沒有任何共用的符號;兩者之間唯一的
 * 介面是系統呼叫。USER_BASE 由建置系統以 --defsym 傳入(各架構不同)。
 *
 * 輸出的原始二進位開頭是一個 20 位元組的標頭(struct uimg_hdr,見 kernel/proc.c),
 * kernel 的載入器靠它知道各區段的範圍與進入點。數值都是相對於 USER_BASE 的偏移。
 */
ENTRY(_start)

SECTIONS
{
    . = USER_BASE;

    .hdr : {
        /* (連結器丟棄沒用到的區段時,由 LONG 直接產生的內容不受影響) */
        LONG(0x594C4557)                  /* magic "WELY" */
        LONG(_start - USER_BASE)          /* 進入點 */
        LONG(__text_end - USER_BASE)      /* 程式碼 + 唯讀資料的結尾 */
        LONG(__data_end - USER_BASE)      /* 已初始化資料的結尾 = 檔案結尾 */
        LONG(__bss_end - USER_BASE)       /* 未初始化資料的結尾 */
    }

    /* 程式碼與唯讀資料:載入後是「唯讀 + 可執行」 */
    .text : { *(.text.start) *(.text*) *(.rodata*) *(.srodata*) }
    . = ALIGN(4096);
    __text_end = .;

    /* 以下載入後是「可讀寫、不可執行」。結尾放 4 個位元組確保這一段不是空的,
       objcopy 產生的檔案才會延伸到 __data_end */
    .data : { *(.data*) *(.sdata*) LONG(0) }
    __data_end = .;

    .bss : { *(.sbss*) *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    __bss_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
