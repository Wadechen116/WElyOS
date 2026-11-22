#ifndef WELYOS_UAPI_SYSCALL_H
#define WELYOS_UAPI_SYSCALL_H
/*
 * uapi/syscall.h - 系統呼叫的編號
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */

/* 系統呼叫編號。新的呼叫一律加在最後:已經編譯好的程式用的是數值 */
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
    SYS_spawn,              /* (path, argv)    建立新行程執行 path;回傳 pid。新行程繼承 0、1、2 號描述符 */
    SYS_exec,               /* (path, argv)    目前的行程改為執行 path;成功則不返回 */
    SYS_wait,               /* (pid)           等待自己的某個子行程結束,回傳它的結束碼 */
    SYS_dup2,               /* (oldfd, newfd)  讓 newfd 也指向 oldfd 的那次開啟;回傳 newfd */
    SYS_fork,               /* ()              複製目前的行程;父行程得到子行程的 pid,子行程得到 0 */
    SYS_pipe,               /* (fds)           建立管線;fds[0] 為讀端、fds[1] 為寫端的描述符 */
    SYS_waitpid,            /* (pid, status, options)  等子行程結束(pid = -1:任何一個);回傳它的 pid,
                                               結束碼寫入 *status。options 可為 WNOHANG、WUNTRACED */
    SYS_kill,               /* (pid, sig)      送訊號(終止 2、9、15;暫停 19、20、21;繼續 18);
                                               pid < 0 表示整個群組 -pid */
    SYS_setpgid,            /* (pid, pgid)     把自己(pid 0)或子行程放進行程群組;pgid 0 = 以它的 pid 為群組 */
    SYS_tcsetfg,            /* (pgid)          設定主控台的前景群組:Ctrl-C 終止、Ctrl-Z 暫停這一組,
                                               也只有這一組可以讀鍵盤。0 = 沒有(輪到呼叫者自己讀) */
    SYS_procinfo,           /* (after, info)   查詢編號大於 after 的下一個行程;0 = 取得,1 = 沒有更多了 */
    SYS_sync,               /* ()              把檔案系統還留在記憶體裡的修改寫到磁碟上 */
    SYS_statfs,             /* (path, info)    path 所在的檔案系統的用量(struct fsinfo,見 fs.h) */
    SYS_sysinfo,            /* (info)          整個系統的狀態:運作時間、各 CPU 的使用量、記憶體(struct sysinfo) */
    SYS_tcsetmode,          /* (mode)          設定主控台的輸入模式(CON_RAW、CON_NOECHO;0 = 一般) */
    SYS_lseek,              /* (fd, off, whence)  改變讀寫位置(SEEK_SET / SEEK_CUR / SEEK_END);回傳新的位置 */
    SYS_rename,             /* (old, new)      改名或搬移(同一個檔案系統內;目的已存在則失敗) */
    SYS_link,               /* (old, new)      硬連結:讓 new 成為檔案 old 的另一個名稱 */
    SYS_chdir,              /* (path)          改變目前的工作目錄(相對路徑的起點) */
    SYS_getcwd,             /* (buf, len)      取得目前的工作目錄(以 0 結尾的字串);回傳長度 */
    SYS_sbrk,               /* (incr)          把 heap 的結尾移動 incr 個位元組;回傳移動之前的結尾 */
    SYS_signal,             /* (sig, handler, restorer)  設定訊號的處理方式;回傳原本的。handler 為
                                               SIG_DFL(0)、SIG_IGN(1)或函式的位址 */
    SYS_sigreturn,          /* (frame)         處理函式結束時由 restorer 呼叫:還原被打斷時的暫存器 */
    SYS_time,               /* (tv)            現在的日期時間(struct timeval:1970-01-01 UTC 至今的秒與微秒) */
    /* ---- 身分與權限(見 cred.h、fs.h 的「權限」) ---- */
    SYS_getuid,             /* ()              自己的使用者編號 */
    SYS_getgid,             /* ()              自己的群組編號 */
    SYS_setuid,             /* (uid, gid)      改變自己的身分(需要 CAP_SETID);改成非 root 會失去所有特權 */
    SYS_capget,             /* ()              自己目前的特權(CAP_* 的組合) */
    SYS_capdrop,            /* (mask)          永久放棄 mask 裡的特權;回傳剩下的 */
    SYS_chmod,              /* (path, mode)    更改權限位元(擁有者或 CAP_FS) */
    SYS_chown,              /* (path, uid, gid) 更改擁有者與群組(CAP_FS) */
    /* ---- 記憶體 ---- */
    SYS_mmap,               /* (len)           要一塊內容為 0 的記憶體(匿名對映);回傳位址 */
    SYS_munmap,             /* (addr)          歸還 mmap 得到的一整塊 */
    SYS_memlimit,           /* (kb)            記憶體配額:0 = 查詢;調低隨時可以,調高需要 CAP_ADMIN。回傳配額(KB) */
    /* ---- 網路(見 netif.h;位址與埠號是主機順序的整數) ---- */
    SYS_ping,               /* (ip, timeout_ms)  送出 echo request 並等回答;回傳來回的毫秒數 */
    SYS_udp_open,           /* (port)          建立綁定在 port 的 UDP socket(0 = 自動挑選);回傳檔案描述符 */
    SYS_udp_send,           /* (fd, msg)       送出一個封包(struct udpmsg:目的位址、埠、資料);回傳長度 */
    SYS_udp_recv,           /* (fd, msg, timeout_ms)  等一個封包;填入來源與資料,回傳長度 */
    SYS_netinfo,            /* (info)          網路的設定與統計(struct netinfo,見 netif.h) */
    /* ---- 黑盒子(見 blackbox.h) ---- */
    SYS_bbread,             /* (index, rec)    讀取一筆事件紀錄(0 = 最新的);0 = 取得,1 = 沒有這麼多筆 */
    SYS_bblog,              /* (type, text)    留下一筆紀錄(BB_NOTE 或 BB_RESTART);需要 CAP_ADMIN */
    SYS_scrub,              /* (info)          立刻把整顆磁碟巡檢一遍(需要 CAP_ADMIN);結果寫入 struct scrubinfo */
    /* ---- 週期性的即時工作(見 sched.h) ---- */
    SYS_rt_set,             /* (period_ms, budget_ms)  把自己宣告為週期性工作(需要 CAP_ADMIN);period 為 0 = 取消 */
    SYS_rt_wait,            /* (stats)         這一次做完了,睡到下一個週期;stats 不是 0 時填入 struct rt_stats */
    /* ---- 故障注入 ---- */
    SYS_seu,                /* (pid, addr, bit) 把行程 pid 的記憶體 addr 處的一個位元翻轉(測試用;需要 CAP_ADMIN) */
    /* ---- 檔案的持久化 ---- */
    SYS_fsync,              /* (fd)            把這個檔案寫回儲存媒體;回傳 0 才表示資料已經保存。
                                               管線、裝置、socket 回傳 -E_INVAL */
    /* ---- 效能量測 ---- */
    SYS_clock,              /* (info)          讀取高精度時鐘(struct clockinfo,見 uapi/proc.h);
                                               沒有可用的時鐘時回傳 -E_NOSYS */
    SYS_failinj,            /* (op, site, n)   配置失敗注入與計量(只給自我測試):只有以 selftest 開機、
                                               而且呼叫者有 CAP_ADMIN 時有效,否則 -E_PERM。
                                               op:FI_OP_ARM / DISARM(回傳是否注入過)/ DOMAIN / RESERVED / AVAIL_KB */
};


/* SYS_failinj 的 op 與地點(地點和 kernel 的 enum fi_site 相同) */
#define FI_OP_DISARM    0
#define FI_OP_ARM       1
#define FI_OP_DOMAIN    2     /* 呼叫者 domain 目前記帳的頁數 */
#define FI_OP_RESERVED  3     /* pmm 的預留頁數 */
#define FI_OP_AVAIL_KB  4     /* pmm 空閒 + heap 空閒 + 執行緒池(KB):不受 heap 成長、執行緒池影響 */
#define FI_OP_UNFREED   5     /* 還沒釋放的執行緒數(存活 + 已結束未回收):等它回到基準值,代表收拾完了 */
#define FI_SITE_KMALLOC 0
#define FI_SITE_PMM     1

/* SYS_sigreturn 的數值:使用者端 crt0.c 裡的組合語言要用(寫不了 enum) */
#define SYS_SIGRETURN_NUM 40
_Static_assert(SYS_sigreturn == SYS_SIGRETURN_NUM, "update SYS_SIGRETURN_NUM");
#endif
