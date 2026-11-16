#ifndef WELYOS_SERIAL_H
#define WELYOS_SERIAL_H

/* 序列埠(COM1)驅動介面。用於 log 輸出與除錯,不依賴顯示硬體。 */
void serial_init(void);            /* 初始化 UART,使用前必須先呼叫一次 */
void serial_putc(char c);          /* 輸出單一字元(會等待傳送緩衝區空) */
void serial_puts(const char *s);   /* 輸出字串,不自動換行、不轉換 \n */
#endif
