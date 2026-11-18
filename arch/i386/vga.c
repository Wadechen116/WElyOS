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

/*
 * 硬體捲動。顯示記憶體有 32KB(16384 格),一個畫面只用 2000 格。
 * 顯示卡有一個「顯示起點」暫存器,決定從顯示記憶體的第幾格開始畫。
 * 所以捲動一列不必把整個畫面往上搬(讀 1920 格、寫 2000 格),
 * 只要把起點往後移 80 格,再清空新露出來的最後一列(寫 80 格):
 *
 *   顯示記憶體  [ ......|<------ 目前的畫面(2000 格)------>|...... ]
 *                        ^ origin                捲動後 origin 往後 80 格
 *
 * 起點移到記憶體的尾端、放不下一整個畫面時,才把畫面搬回最前面一次
 * (大約每捲動 180 列一次)。
 * 為什麼在意:在模擬器裡,每次存取顯示記憶體都很慢(要離開客體、由模擬器處理),
 * 而輸出時是關著中斷的。捲動越慢,中斷被擋住越久,序列埠來不及取走的輸入就會遺失。
 */
#define VGA_CELLS 16384                /* 顯示記憶體總共幾格(32KB / 2) */
static unsigned origin;                /* 畫面左上角是顯示記憶體的第幾格 */
#define SCREEN    (VGA_MEM + origin)   /* 目前畫面的第 0 格 */

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
 *   索引 0x0D / 0x0C = 顯示起點的低 / 高位元組(硬體捲動用,見上方)。
 */
static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(origin + row * VGA_COLS + col);   /* 游標位置從顯示記憶體的開頭算起 */
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

static unsigned shown_origin;          /* 已經告訴顯示卡的起點;和 origin 不同時才需要更新 */

static void update_origin(void)
{
    if (shown_origin == origin)
        return;
    outb(0x3D4, 0x0D);
    outb(0x3D5, (uint8_t)(origin & 0xFF));
    outb(0x3D4, 0x0C);
    outb(0x3D5, (uint8_t)(origin >> 8));
    shown_origin = origin;
}

/* 捲動一列:顯示起點往後移一列,清空新的最後一列,游標停在最後一列(說明見檔案開頭) */
static void scroll(void)
{
    if (origin + VGA_COLS + VGA_COLS * VGA_ROWS > VGA_CELLS) {
        /* 到尾端了:把第 1~24 列搬回顯示記憶體的最前面,成為新畫面的第 0~23 列 */
        for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
            VGA_MEM[i] = SCREEN[VGA_COLS + i];
        origin = 0;
    } else {
        origin += VGA_COLS;
    }
    for (int c = 0; c < VGA_COLS; c++)
        SCREEN[(VGA_ROWS - 1) * VGA_COLS + c] = cell(' ');
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
    origin = 0;
    shown_origin = 1;                  /* 和 origin 不同:強制把起點寫進顯示卡一次 */
    update_origin();
    update_cursor();
}

/*
 * ---- 控制序列(escape sequence) ----
 *
 * 全螢幕的程式(例如 top)靠一種夾在文字裡的命令來移動游標、清除畫面、換顏色:
 * 以 ESC(0x1B)和 "[" 開頭,接著是以分號隔開的數字,最後一個字母決定做什麼。
 * 這是終端機通用的寫法(ANSI escape code);序列埠另一端的終端機本來就看得懂,
 * 這裡讓 VGA 螢幕也看得懂其中最常用的幾個,兩邊的畫面才會一致:
 *
 *   ESC [ 列 ; 欄 H    游標移到指定位置(從 1 算起;省略 = 1)
 *   ESC [ 2 J          清除整個畫面        ESC [ J   清除游標之後的部分
 *   ESC [ K            清除游標到這一列的結尾
 *   ESC [ … m          顏色:0 還原、1 加亮、7 反白、30~37 前景、40~47 背景、90~97 亮的前景
 * 其他不認得的序列整個略過(不會把它當成文字印出來)。
 */
#define ESC_MAX_PARAMS 4
static int esc_state;                  /* 0 = 一般文字;1 = 剛收到 ESC;2 = 收到 ESC [,正在讀數字 */
static int esc_param[ESC_MAX_PARAMS];
static int esc_count;
static const uint8_t default_attr = (VGA_BLACK << 4) | VGA_LIGHT_GREY;

/* 控制序列的顏色編號(黑紅綠黃藍洋紅青白)對應到 VGA 的顏色 */
static const uint8_t ansi_color[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN, VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LIGHT_GREY
};

static void clear_cells(int from, int to)      /* 清除畫面上第 from ~ to-1 格 */
{
    for (int i = from; i < to && i < VGA_COLS * VGA_ROWS; i++)
        SCREEN[i] = cell(' ');
}

static void esc_finish(char final)
{
    int p0 = esc_param[0], p1 = esc_param[1];

    switch (final) {
    case 'H':
    case 'f':
        row = p0 > 0 ? p0 - 1 : 0;
        col = p1 > 0 ? p1 - 1 : 0;
        if (row >= VGA_ROWS)
            row = VGA_ROWS - 1;
        if (col >= VGA_COLS)
            col = VGA_COLS - 1;
        break;
    case 'J':
        clear_cells(p0 == 2 ? 0 : row * VGA_COLS + col, VGA_COLS * VGA_ROWS);
        break;
    case 'K':
        clear_cells(row * VGA_COLS + col, (row + 1) * VGA_COLS);
        break;
    case 'm':
        for (int i = 0; i <= esc_count && i < ESC_MAX_PARAMS; i++) {
            int v = esc_param[i];
            if (v == 0)
                attr = default_attr;
            else if (v == 1)
                attr |= 0x08;                                  /* 加亮前景 */
            else if (v == 7)
                attr = (uint8_t)((attr << 4) | (attr >> 4));   /* 前景與背景對調 */
            else if (v >= 30 && v <= 37)
                attr = (uint8_t)((attr & 0xF8) | ansi_color[v - 30]);
            else if (v >= 40 && v <= 47)
                attr = (uint8_t)((attr & 0x0F) | (ansi_color[v - 40] << 4));
            else if (v >= 90 && v <= 97)
                attr = (uint8_t)((attr & 0xF0) | ansi_color[v - 90] | 0x08);
        }
        break;
    default:
        break;                         /* 不認得的:略過 */
    }
}

/* 處理控制序列中的一個字元。回傳 1 = 這個字元已經被吃掉了,不要當成文字顯示 */
static int esc_input(char c)
{
    if (esc_state == 0) {
        if (c != 0x1B)
            return 0;
        esc_state = 1;
        return 1;
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_count = 0;
            for (int i = 0; i < ESC_MAX_PARAMS; i++)
                esc_param[i] = 0;
        } else {
            esc_state = 0;             /* ESC 後面不是 "[":不支援,整個丟掉 */
        }
        return 1;
    }
    if (c >= '0' && c <= '9') {
        if (esc_count < ESC_MAX_PARAMS && esc_param[esc_count] < 1000)
            esc_param[esc_count] = esc_param[esc_count] * 10 + (c - '0');
    } else if (c == ';') {
        esc_count++;
    } else if (c >= 0x40 && c <= 0x7E) {       /* 結尾的字母 */
        esc_finish(c);
        esc_state = 0;
    }
    /* 其他(例如 "?"):序列的一部分,略過 */
    return 1;
}

void vga_putc(char c)
{
    if (esc_input(c))
        return;
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
            SCREEN[row * VGA_COLS + col] = cell(' ');
        }
        break;
    case '\t':                 /* Tab:移到下一個 8 的倍數欄位 */
        col = (col + 8) & ~7;
        break;
    default:                   /* 一般字元:寫入目前位置並右移 */
        SCREEN[row * VGA_COLS + col] = cell(c);
        col++;
    }
    if (col >= VGA_COLS) {     /* 寫到行尾:自動換行 */
        col = 0;
        row++;
    }
    if (row >= VGA_ROWS)       /* 超出最後一列:捲動 */
        scroll();
    /* 硬體游標不在這裡更新:那要寫 4 次 I/O 埠,每個字元都做的話,
       輸出一行就是幾百次。改由 vga_sync() 在一次輸出結束時更新一次 */
}

/* 把硬體游標與顯示起點更新到目前的狀態。一次輸出(一個 kprintf)結束時呼叫 */
void vga_sync(void)
{
    update_origin();
    update_cursor();
}
