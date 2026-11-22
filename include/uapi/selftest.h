#ifndef WELYOS_UAPI_SELFTEST_H
#define WELYOS_UAPI_SELFTEST_H
/*
 * uapi/selftest.h - 自我測試的模式編號
 *
 * kernel 的自我測試(kernel/main.c 的 run_user)以「模式編號」當參數啟動 /bin/init,
 * init(user/init.c)依編號執行對應的一組測試,再以結束碼回報結果。
 * 兩邊引入這同一份定義;新的模式加在最後。
 */
enum {
    UMODE_HELLO = 0,     /* 正常執行:輸出、多執行緒、結束碼 */
    UMODE_READ_KERNEL,   /* 讀取 kernel 的記憶體 */
    UMODE_WRITE_TEXT,    /* 寫入自己的程式碼 */
    UMODE_PRIVILEGED,    /* 執行特權指令 */
    UMODE_NULL,          /* 解參考 NULL */
    UMODE_BAD_POINTER,   /* 把 kernel 的位址傳給系統呼叫 */
    UMODE_SPIN,          /* 無窮迴圈,永不讓出 CPU */
    UMODE_STACK,         /* 使用者 stack 溢位 */
    UMODE_ISOLATION,     /* 檢查自己的資料不受其他行程影響 */
    UMODE_BAD_SYSCALL,   /* 不存在的系統呼叫編號 */
    UMODE_FUTEX,         /* 多執行緒以使用者模式的 mutex 保護共用資料 */
    UMODE_FS,            /* 檔案:建立、寫入、讀回、列出目錄、刪除,以及各種錯誤情況 */
    UMODE_PROC,          /* 建立其他行程並等待、參數傳遞、exec 失敗時原程式不受影響 */
    UMODE_EXEC,          /* 以 exec 把自己換成另一個程式 */
    UMODE_EXEC_THREADS,  /* 還有其他執行緒時呼叫 exec / fork:應被拒絕 */
    UMODE_FORK,          /* fork:複製行程,父子各自修改資料互不影響(寫入時複製) */
    UMODE_STACK_GROW,    /* 用掉遠超過 16KB 的 stack:應該自動成長而不是被終止 */
    UMODE_PIPE,          /* 管線:父子行程之間傳資料、把別的程式的輸出接進管線 */
    UMODE_SIGNAL,        /* 父子關係與訊號:終止子行程、等任何一個子行程、行程群組、孤兒行程 */
    UMODE_JOBS,          /* 不等待的 waitpid、列出行程、/dev/null(背景執行與 ps 的基礎) */
    UMODE_STOP,          /* 暫停與繼續(SIGSTOP、SIGCONT、waitpid 的 WUNTRACED) */
    UMODE_DISK,          /* 磁碟:把程式複製到 /disk 再執行、statfs、sync(有磁碟時才執行) */
    UMODE_SYSINFO,       /* top 用到的東西:系統狀態、行程的記憶體用量、主控台的輸入模式 */
    UMODE_FILEOPS,       /* lseek、rename、硬連結、目前的工作目錄 */
    UMODE_HEAP,          /* 使用者 heap(sbrk)與函式庫:malloc、printf、字串函式 */
    UMODE_SIGHANDLER,    /* 訊號處理函式:登記、執行、還原、忽略、打斷等待中的系統呼叫 */
    UMODE_TIME,          /* 日期時間:sys_time、秒數與年月日的換算、檔案的修改時間 */
    UMODE_PERM,          /* 使用者與權限:檔案的擁有者與權限位元、特權只能減少、誰可以對誰送訊號 */
    UMODE_MEMQUOTA,      /* 記憶體配額(要求當場成功或失敗,不會事後被終止)與匿名對映 */
    UMODE_NET,           /* 網路:ping、UDP socket(以模擬器內建的 TFTP 伺服器驗證收送) */
    UMODE_RT,            /* 週期性的即時工作:準時、負載檢查、需要特權 */
    UMODE_TMR,           /* 三重執行加投票:注入位元翻轉、卡住的那一份、兩份都錯 */
    UMODE_MEMRACE,       /* 同一個行程的多條執行緒同時改變對映(mmap/munmap、sbrk)並和 kernel 交換資料 */
    UMODE_THREADCHURN,   /* 反覆建立、結束執行緒(超過 16 格):結束時歸還 stack 格,每條都深用自己的 stack */
    UMODE_FSQUOTA,       /* ramfs 的 FS 配額:一般使用者寫到上限得到 E_NOSPC,管理者可越過,刪檔後歸還 */
    UMODE_THREADMIX,     /* 多條執行緒同時建立 / 結束;主執行緒先結束後,由其他執行緒 fork */
    UMODE_FAILSWEEP,     /* 配置失敗注入掃過 fork / 建立執行緒 / exec / spawn / pipe / mmap+sbrk / 管線讀寫 / UDP 的每個配置點 */
    UMODE_SIGRACE,       /* 訊號在返回使用者模式的空檔送達:之後的睡眠必須被叫醒(I-334) */
    UMODE_MEMLOWER,      /* 配額調到已承諾量以下:已承諾的頁照用、新要求 E_NOMEM、管理者調回後又可成長(C3.2 B7) */
};
#endif
