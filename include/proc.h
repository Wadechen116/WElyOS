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
int proc_spawn(const char *path, const char *const argv[]);

/* 等待行程結束並取得它的結束碼,同時釋放行程的紀錄。pid 不存在回傳 -1。
   因例外被終止的行程,結束碼為 PROC_KILLED_FAULT;被 proc_kill 終止的為指定的值 */
int proc_wait(int pid);

/* 強制終止行程(它的所有執行緒會在下次進入 kernel 時結束)。pid 不存在回傳 -1 */
int proc_kill(int pid, int code);

#define PROC_KILLED_FAULT (-1)

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

/* 每次要從 kernel 返回使用者模式之前呼叫:若這條執行緒已被要求終止,就在這裡結束 */
void user_return_check(void);

/* 系統呼叫編號(使用者端的對應定義在 user/usys.h,兩邊必須一致) */
enum {
    SYS_write = 1,          /* (fd, buf, len)  寫入檔案描述符;回傳寫入的長度。1 = 標準輸出 */
    SYS_exit,               /* (code)          結束整個行程 */
    SYS_getpid,             /* ()              行程編號 */
    SYS_yield,              /* ()              讓出 CPU */
    SYS_sleep,              /* (ms)            睡眠 */
    SYS_thread_create,      /* (entry, arg)    在同一個行程內建立執行緒;回傳 tid */
    SYS_thread_exit,        /* ()              結束目前的執行緒 */
    SYS_gettid,             /* ()              執行緒編號 */
    SYS_futex_wait,         /* (addr, val)     若 *addr == val 就睡到被喚醒;否則立刻返回 1 */
    SYS_futex_wake,         /* (addr)          喚醒所有睡在 addr 上的執行緒;回傳喚醒的數量 */
    /* ---- 檔案(失敗時回傳負的錯誤碼,見 include/fs.h 的 E_*) ---- */
    SYS_open,               /* (path, flags)   開啟檔案;回傳檔案描述符 */
    SYS_close,              /* (fd) */
    SYS_read,               /* (fd, buf, len)  讀取;回傳讀到的長度,0 = 檔案結尾 */
    SYS_readdir,            /* (fd, dirent)    讀取目錄的下一筆;0 = 取得,1 = 沒有了 */
    SYS_mkdir,              /* (path) */
    SYS_unlink,             /* (path)          刪除檔案或空的目錄 */
    SYS_stat,               /* (path, stat)    查詢類型與大小 */
    /* ---- 行程 ---- */
    SYS_spawn,              /* (path, argv)    建立新行程執行 path;回傳 pid。新行程繼承開啟中的檔案 */
    SYS_exec,               /* (path, argv)    目前的行程改為執行 path;成功則不返回 */
    SYS_wait,               /* (pid)           等待行程結束,回傳它的結束碼 */
    SYS_dup2,               /* (oldfd, newfd)  讓 newfd 也指向 oldfd 的那次開啟;回傳 newfd */
    SYS_fork,               /* ()              複製目前的行程;父行程得到子行程的 pid,子行程得到 0 */
};
#endif
