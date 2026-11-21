#ifndef WELYOS_PROC_H
#define WELYOS_PROC_H
#include <stdint.h>

/*
 * 行程(process)= 一個獨立的位址空間 + 在裡面執行的一條或多條執行緒。
 *
 * 行程在「使用者模式」執行:CPU 處於低權限,不能執行特權指令、不能存取
 * kernel 的記憶體,也看不到其他行程的記憶體。它要做任何需要權限的事
 * (輸出、睡眠、建立執行緒…)都只能透過「系統呼叫」請 kernel 代勞。
 * 行程做錯事(存取不該碰的位址、執行特權指令)只會讓它自己被終止,
 * kernel 與其他行程不受影響。
 */

/* 建立行程,執行檔案系統中 path 指到的程式(例如 "/bin/init")。
   argv 是傳給程式 main 的參數:以 NULL 結尾的字串陣列;傳 NULL 表示只有程式名稱。
   回傳行程編號(pid);失敗(檔案不存在、不是合法的程式、記憶體不足)回傳 -1 */
void proc_mem_boot(void);                /* 開機時登記匿名記憶體的位址區域(vm_init 之後) */
int proc_spawn(const char *path, const char *const argv[]);

/* 等待行程結束並取得它的結束碼,同時釋放行程的紀錄。pid 不存在回傳 -1。
   因例外被終止的行程,結束碼為 PROC_KILLED_FAULT;被 proc_kill 終止的為指定的值 */
int proc_wait(int pid);

/* 強制終止行程(它的所有執行緒會在下次進入 kernel 時結束)。pid 不存在回傳 -1 */
int proc_kill(int pid, int code);

#define PROC_KILLED_FAULT (-1)

/* 對行程群組 pgid 的所有行程送出訊號 sig(終止它們)。回傳送到的行程數。
   呼叫時必須持有排程器的鎖;主控台收到 Ctrl-C 時使用 */
int proc_signal_group_locked(int pgid, int sig);

/* 主控台(console.c)判斷「誰可以讀鍵盤」時使用;呼叫時都必須持有排程器的鎖 */
int proc_self_pgid(void);                /* 目前執行緒所屬行程的群組;kernel 執行緒為 0 */
int proc_self_stopped(void);             /* 目前執行緒所屬的行程是否被暫停了 */
int proc_group_alive_locked(int pgid);   /* 群組裡還有沒有活著的行程 */

/* ---- 以下由各架構的例外處理程式呼叫 ---- */

/* 系統呼叫的進入點:num 為呼叫編號,a0~a2 為參數;回傳值會放回使用者的暫存器。
   tf 指向這次進入 kernel 時存下來的暫存器(fork 要複製它給子行程) */
long syscall_dispatch(long num, long a0, long a1, long a2, void *tf);

/* 使用者模式發生 page fault 時先呼叫:addr 為出錯的位址,write 表示是不是寫入。
   回傳 0 表示已經處理好(寫入時複製、stack 成長),讓程式重新執行那條指令即可;
   回傳 -1 表示是真正的錯誤,呼叫者應以 user_fault 終止行程 */
int proc_page_fault(uintptr_t addr, int write);

/* 使用者模式發生無法處理的例外:印出原因並終止該行程。不會返回 */
void user_fault(const char *what, uintptr_t pc, uintptr_t addr) __attribute__((noreturn));

/* 每次要從 kernel 返回使用者模式之前呼叫:若這條執行緒已被要求終止,就在這裡結束;
   行程被暫停就在這裡等;有訊號要交給處理函式,就把 tf(進入 kernel 時存下來的暫存器)
   改成去執行那個函式 */
void user_return_check(void *tf);

/* 系統呼叫的編號與經由它傳遞的結構:kernel 與使用者程式共用的定義 */
#include "uapi/syscall.h"
#include "uapi/proc.h"
#include "uapi/net.h"
#endif
