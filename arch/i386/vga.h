#ifndef WELYOS_VGA_H
#define WELYOS_VGA_H
#include <stdint.h>

/* VGA 文字模式的 16 色調色盤(數值即硬體色碼) */
enum vga_color {
    VGA_BLACK, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
    VGA_YELLOW, VGA_WHITE
};

void vga_init(void);                         /* 清除螢幕、游標歸位到左上角 */
void vga_putc(char c);                       /* 輸出字元;支援 \n \r \b \t,滿版自動捲動 */
void vga_sync(void);                         /* 把硬體游標移到目前位置(一次輸出結束時呼叫) */
void vga_set_color(enum vga_color fg, enum vga_color bg);  /* 設定之後輸出的顏色 */
#endif
