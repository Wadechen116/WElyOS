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

/* 設定前景行程群組:使用者按 Ctrl-C 時這一組的行程會被終止、按 Ctrl-Z 時被暫停,
   也只有這一組可以讀鍵盤。0 表示沒有(輪到設定者自己讀;Ctrl-C 只會放棄正在打的那一行)。
   owner 是設定者(shell)自己的群組。呼叫時必須持有排程器的鎖 */
void console_set_fg(int pgid, int owner);

/* 設定輸入模式(CON_RAW、CON_NOECHO,見 proc.h;0 = 一般的整行模式)。
   只有目前可以讀鍵盤的行程可以設定;不是的話回傳 -E_INVAL。
   console_set_fg 會把模式設回 0:前景換人時,上一個程式留下的模式不該影響下一個。
   呼叫時必須持有排程器的鎖 */
int console_set_mode(unsigned mode);

/* 叫醒所有正在等鍵盤輸入的執行緒,讓它們重新檢查條件(行程從暫停中繼續時使用)。
   呼叫時必須持有排程器的鎖 */
void console_wake_readers(void);

/* 啟用主控台的輸入:讓序列埠在收到字元時發出中斷。要在中斷控制器設定好之後呼叫 */
void console_init(void);

/* 由序列埠的中斷處理程式呼叫(中斷關閉中):把硬體收到的字元全部取出來處理 */
void console_rx(void);
#endif
