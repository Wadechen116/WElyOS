#ifndef WELYOS_CONSOLE_H
#define WELYOS_CONSOLE_H
#include "fs.h"

/*
 * 主控台:輸出到螢幕 / 序列埠,輸入來自序列埠(之後是鍵盤)。
 * 在檔案系統中它是 /dev/console 這個裝置節點;行程的 0、1、2 號檔案描述符
 * 一開始都指向它。
 */

/* /dev/console 的讀寫操作,交給 VFS 建立裝置節點時使用 */
const struct dev_ops *console_dev_ops(void);

/* 由計時器中斷定期呼叫(中斷關閉中):把硬體收到的字元取出來處理 */
void console_poll(void);

/* 叫醒所有正在等輸入的執行緒(行程被終止時用:讓它們有機會發現自己該結束了)。
   呼叫時必須持有排程器的鎖 */
void console_wake_readers(void);
#endif
