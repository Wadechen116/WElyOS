/*
 * vga.c - VGA 文字模式驅動(80x25)
 *
 * 文字模式的顯示記憶體映射在實體位址 0xB8000,硬體會持續把它畫到螢幕上。
 * 每個字元格 2 位元組:低位元組 = 字元碼,高位元組 = 屬性(顏色)。
 */
#include "io.h"
#include "vga.h"

/* volatile:避免編譯器把「對顯示記憶體的寫入」當成無作用而優化掉 */
#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

static int row, col;   /* 目前游標位置(第幾列、第幾欄) */

/* 屬性位元組:高 4 位 = 背景色,低 4 位 = 前景色。預設灰字黑底 */
static uint8_t attr = (VGA_BLACK << 4) | VGA_LIGHT_GREY;

/* 組出一個字元格:屬性在高位元組,字元在低位元組 */
static uint16_t cell(char c)
{
    return (uint16_t)((uint16_t)attr << 8) | (uint8_t)c;
}

/*
 * 移動硬體游標(閃爍的底線)。透過 CRTC 控制器:
 *   port 0x3D4 選擇內部暫存器索引,port 0x3D5 讀寫該暫存器。
 *   索引 0x0F = 游標位置低位元組,0x0E = 高位元組。
 * 位置以「第幾格」計,即 row * 80 + col。
 */
static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

/* 捲動一列:第 1~24 列複製到第 0~23 列,最後一列清空,游標停在最後一列 */
static void scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = cell(' ');
    row = VGA_ROWS - 1;
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    attr = (uint8_t)((bg << 4) | fg);   /* 只影響之後輸出的字元 */
}

void vga_init(void)
{
    /* 全螢幕填空白(使用目前屬性),游標歸位到左上角 */
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = cell(' ');
    row = col = 0;
    update_cursor();
}

void vga_putc(char c)
{
    switch (c) {
    case '\n':                 /* 換行:回到行首並往下一列 */
        col = 0;
        row++;
        break;
    case '\r':                 /* 歸位:只回到行首 */
        col = 0;
        break;
    case '\b':                 /* 退格:游標左移並清除該格(不會退到上一列) */
        if (col > 0) {
            col--;
            VGA_MEM[row * VGA_COLS + col] = cell(' ');
        }
        break;
    case '\t':                 /* Tab:移到下一個 8 的倍數欄位 */
        col = (col + 8) & ~7;
        break;
    default:                   /* 一般字元:寫入目前位置並右移 */
        VGA_MEM[row * VGA_COLS + col] = cell(c);
        col++;
    }
    if (col >= VGA_COLS) {     /* 寫到行尾:自動換行 */
        col = 0;
        row++;
    }
    if (row >= VGA_ROWS)       /* 超出最後一列:捲動 */
        scroll();
    update_cursor();
}
