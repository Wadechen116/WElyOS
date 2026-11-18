/*
 * serial.c - 16550 UART 序列埠驅動(COM1)
 *
 * QEMU 以 -serial 把 COM1 接到終端機或檔案,所以這是最早、最可靠的
 * 除錯輸出管道。
 */
#include "io.h"
#include "serial.h"

#define COM1 0x3F8   /* COM1 的基底 I/O port;其後 +0~+7 為各暫存器 */

/*
 * 暫存器(相對 COM1):
 *   +0 資料(DLAB=0)/ 鮑率除數低位元組(DLAB=1)
 *   +1 中斷致能 / 鮑率除數高位元組(DLAB=1)
 *   +2 FIFO 控制   +3 線路控制(含 DLAB 位元)
 *   +4 數據機控制  +5 線路狀態(bit 5 = 傳送緩衝區空)
 */
void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* 先關閉 UART 中斷;之後由 serial_irq_enable 開啟 */
    outb(COM1 + 3, 0x80);   /* 開啟 DLAB,以便寫入鮑率除數 */
    outb(COM1 + 0, 0x01);   /* 除數低位元組 = 1 -> 115200 baud(基頻 / 1) */
    outb(COM1 + 1, 0x00);   /* 除數高位元組 = 0 */
    outb(COM1 + 3, 0x03);   /* 關閉 DLAB;格式 8 資料位元、無同位、1 停止位元(8N1) */
    outb(COM1 + 2, 0x07);   /* 啟用 FIFO、清空收發緩衝、觸發門檻 1 位元組(一有字元就通知) */
    outb(COM1 + 4, 0x03);   /* 拉起 DTR 與 RTS */
}

/* 啟用「收到資料」中斷(COM1 接在 IRQ 4)。PIC 那一側的開關在 timer.c */
void serial_irq_enable(void)
{
    outb(COM1 + 4, 0x0B);   /* 再拉起 OUT2:PC 上 UART 的中斷線要經過它才接得到 PIC */
    outb(COM1 + 1, 0x01);   /* 中斷致能:bit 0 = 收到資料 */
}

void serial_putc(char c)
{
    /* 輪詢線路狀態暫存器 bit 5(THRE):為 1 才表示可寫入下一個位元組 */
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

/* 線路狀態暫存器 bit 0(DR,data ready):為 1 表示接收到的資料還沒被讀走。
   沒有資料時回傳 -1,不等待 */
int serial_getc(void)
{
    if (!(inb(COM1 + 5) & 0x01))
        return -1;
    return inb(COM1);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
