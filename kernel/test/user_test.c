/*
 * user_test.c - 使用者模式的測試:以各種模式啟動 /bin/init 並核對結束碼
 *
 * 這些程式原本和開機流程一起放在 kernel/main.c(第 43 步拆出)。它們只在開機參數
 * 要求時執行(selftest、crash=、disktest=、stress、memtest),對外的進入點宣告在 include/selftest.h。
 */
#include <stdint.h>
#include "arch.h"
#include "blackbox.h"
#include "blk.h"
#include "boot_info.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "sync.h"
#include "timer.h"
#include "vm.h"
#include "watchdog.h"
#include "mmu.h"
#include "netdev.h"
#include "netif.h"
#include "selftest.h"

/* 傳給使用者程式的 mode;數值必須與 user/init.c 一致 */
#include "uapi/selftest.h"       /* 測試模式的編號:和 user/init.c 共用同一份定義 */

/* 以測試編號 mode 啟動 /bin/init:編號轉成字串,當作 argv[1] 傳給它 */
int spawn_init(long mode)
{
    char num[12];
    int n = 0;
    const char *argv[] = { "init", num, NULL };

    if (mode >= 10)
        num[n++] = (char)('0' + mode / 10);
    num[n++] = (char)('0' + mode % 10);
    num[n] = '\0';
    return proc_spawn("/bin/init", argv);
}

/* 啟動一個行程、等它結束,確認結束碼符合預期 */
static void run_user(const char *what, long mode, int expect)
{
    int pid = spawn_init(mode);
    KASSERT(pid > 0);
    uint64_t t0 = timer_ticks();
    int code = proc_wait(pid);
    kprintf("user: %s -> exit code %d (%u ms)\n", what, code, (unsigned)(timer_ticks() - t0));
    if (code != expect)
        panic("user test \"%s\": exit code %d, expected %d", what, code, expect);
}

void user_demo(void)
{
    sched_trim();
    size_t free0 = pmm_free_count() + vm_table_pages();
    struct kheap_stats h0, h1;
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();       /* 執行緒結構留在池子裡不歸還,核對時要扣掉 */

    kprintf("user: running a program in user mode\n");
    run_user("hello, 2 threads, exit(42)", UMODE_HELLO, 42);

    /* 檔案系統呼叫:建立、讀寫、列目錄、刪除,以及位址與權限的檢查 */
    run_user("file system calls", UMODE_FS, 9);
    /* 程式檔不存在、或內容不是合法的程式:建立行程要失敗,而不是執行到一半出事 */
    KASSERT(proc_spawn("/bin/nope", NULL) == -1);
    KASSERT(proc_spawn("/etc/motd", NULL) == -1);

    /* 參數傳遞:kernel 直接啟動 /bin/echo,它的結束碼是收到的參數個數 */
    {
        const char *argv[] = { "echo", "  [user] /bin/echo started by the kernel with", "3 arguments", NULL };
        int pid = proc_spawn("/bin/echo", argv);
        KASSERT(pid > 0 && proc_wait(pid) == 3);
    }
    /* 使用者程式自己建立行程、等待、重新導向輸出;以及各種應該被拒絕的情況 */
    run_user("spawn, wait, arguments, redirection", UMODE_PROC, 12);
    /* exec:行程把自己換成 /bin/echo。結束碼來自 echo(3 個參數),不是原本的程式 */
    run_user("exec replaces the program", UMODE_EXEC, 3);
    run_user("exec and fork with a second thread are refused", UMODE_EXEC_THREADS, 13);

    /* fork:複製行程。記憶體以寫入時複製的方式共用,所以順便看複製了幾頁 */
    {
        unsigned long c0 = vm_cow_copies(), r0 = vm_cow_reuses();
        run_user("fork (copy-on-write)", UMODE_FORK, 14);
        kprintf("user: copy-on-write during that test: %lu pages copied, %lu times a page "
                "was reused without copying\n", vm_cow_copies() - c0, vm_cow_reuses() - r0);
    }
    /* 管線:父子行程之間傳資料、寫端全關閉後讀到結束、把別的程式的輸出接進管線 */
    run_user("pipes between processes", UMODE_PIPE, 16);
    /* 父子關係與訊號。其中一項會留下一個沒有人 wait 的孫行程,它要 5ms 後才結束,
       所以多等一下,讓最後的記憶體核對能確認它被自動回收了 */
    run_user("parent/child, signals, process groups", UMODE_SIGNAL, 17);
    /* 背景執行與 ps 的基礎:不等待的 waitpid、列出行程、/dev/null */
    run_user("waitpid without waiting, process list, /dev/null", UMODE_JOBS, 18);
    /* 工作控制的基礎:暫停、繼續、暫停的回報。最後一項留下一個被 kernel 終止的孫行程 */
    run_user("stop and continue (job control)", UMODE_STOP, 19);
    /* 磁碟(有掛載的話):把程式複製到磁碟上再執行、查詢用量、sync */
    {
        struct wfs_stats ws;
        wfs_stats(&ws);
        if (ws.mounted)
            run_user("run a program from the disk, statfs, sync", UMODE_DISK, 20);
    }
    /* 檔案介面的另一半:讀寫位置、改名、硬連結、工作目錄(記憶體檔案系統與磁碟上各測一次) */
    run_user("lseek, rename, link, working directory", UMODE_FILEOPS, 22);
    {
        /* 改名與硬連結動到磁碟上的目錄與名稱數:做完之後整個檔案系統必須仍然一致 */
        struct wfs_stats ws;
        wfs_stats(&ws);
        if (ws.mounted)
            KASSERT(wfs_check_now() == 0);
    }
    /* 使用者 heap(sbrk:用到才配置、縮小時歸還)與函式庫(malloc、printf、字串) */
    run_user("user heap and C library", UMODE_HEAP, 23);
    /* 訊號處理函式:暫存器存到使用者 stack、執行、還原;忽略;打斷等待;捏造的 sigreturn */
    run_user("signal handlers", UMODE_SIGHANDLER, 24);
    /* 日期時間:硬體時鐘、秒數與年月日的換算、檔案的修改時間 */
    run_user("date and time", UMODE_TIME, 25);
    /* 使用者與權限:檔案的擁有者與權限位元(記憶體與磁碟各測一次)、特權只能減少、送訊號的限制 */
    run_user("users and permissions", UMODE_PERM, 26);
    /* 記憶體配額(超過的要求當場失敗)與匿名對映 */
    run_user("memory quota and mmap", UMODE_MEMQUOTA, 27);
    /* 週期性的即時工作:參數檢查、負載上限、需要特權、準時 */
    run_user("periodic real-time tasks", UMODE_RT, 29);
    /* 三重執行加投票:對其中一份注入位元翻轉、讓一份卡住、讓兩份都出錯 */
    run_user("triple execution with voting", UMODE_TMR, 30);
    /* 網路(有網路卡的話):ping、UDP socket、以 TFTP 實際收送一個檔案 */
    if (boot_nic)
        run_user("network: ping and UDP", UMODE_NET, 28);
    /* top 的基礎:系統狀態、行程的記憶體用量、主控台的輸入模式 */
    run_user("system information for top", UMODE_SYSINFO, 21);
    thread_sleep(30);
    /* stack 自動成長:用掉約 180KB 的 stack(一開始只配置 16KB) */
    run_user("stack grows on demand", UMODE_STACK_GROW, 15);

    /* shell 執行一份 script:建立目錄、重新導向輸出到檔案、找不到的命令、最後 exit 7。
       除了結束碼,也檢查它留下的檔案內容 */
    {
        const char *argv[] = { "sh", "/etc/test.sh", NULL };
        struct file *f;
        char buf[80];
        /* 最後一行來自管線:cat /etc/motd | cat | wc(2 行、18 個字、104 位元組) */
        static const char want[] = "first line\nsecond line, with  two  spaces\n2 18 104\n";
        int pid = proc_spawn("/bin/sh", argv);
        KASSERT(pid > 0);
        int code = proc_wait(pid);
        kprintf("user: shell script /etc/test.sh -> exit code %d\n", code);
        KASSERT(code == 7);
        KASSERT(vfs_open("/work/result", O_RDONLY, &f) == 0);
        long n = vfs_read(f, buf, sizeof(buf));
        vfs_close(f);
        KASSERT(n == (long)sizeof(want) - 1 && memcmp(buf, want, (size_t)n) == 0);
        KASSERT(vfs_unlink("/work/result") == 0 && vfs_unlink("/work") == 0);
    }

    /* 正在等主控台輸入的行程也要能被終止:啟動一個互動式的 shell(沒有人打字,
       它會一直睡在讀取裡),過一下子把它終止,確認它真的結束了 */
    {
        int pid = proc_spawn("/bin/sh", NULL);
        KASSERT(pid > 0);
        thread_sleep(10);
        KASSERT(proc_kill(pid, 98) == 0);
        int code = proc_wait(pid);
        kprintf("\nuser: shell blocked on console input, killed -> exit code %d\n", code);
        KASSERT(code == 98);
    }

    /* 使用者模式的 mutex(futex):3 條執行緒搶同一把鎖,總和必須正確 */
    run_user("user-mode mutex (futex), 3 threads", UMODE_FUTEX, 11);

    /* 以下每一個都是使用者程式做錯事。預期:只有那個行程被終止(結束碼 -1),
       kernel 繼續正常運作並執行下一個測試 */
    run_user("read kernel memory", UMODE_READ_KERNEL, PROC_KILLED_FAULT);
    run_user("write to its own code", UMODE_WRITE_TEXT, PROC_KILLED_FAULT);
    run_user("privileged instruction", UMODE_PRIVILEGED, PROC_KILLED_FAULT);
    run_user("NULL dereference", UMODE_NULL, PROC_KILLED_FAULT);
    run_user("user stack overflow", UMODE_STACK, PROC_KILLED_FAULT);

    /* 系統呼叫的參數檢查:不合法的指標與編號要被拒絕,行程本身不受影響 */
    run_user("kernel pointer passed to write()", UMODE_BAD_POINTER, 7);
    run_user("unknown system call number", UMODE_BAD_SYSCALL, 9);

    /* 搶佔與強制終止:行程陷入無窮迴圈、從不呼叫 kernel。
       計時器中斷仍會把 CPU 拿回來,main 才有機會執行並終止它 */
    int pid = spawn_init(UMODE_SPIN);
    KASSERT(pid > 0);
    thread_sleep(20);
    KASSERT(proc_kill(pid, 99) == 0);
    int code = proc_wait(pid);
    kprintf("user: infinite loop, killed after 20 ms -> exit code %d\n", code);
    KASSERT(code == 99);

    /* 隔離:三個行程同時執行同一個程式,各自寫入「同一個虛擬位址」的變數。
       它們的頁表把這個位址對映到不同的實體頁,所以互不影響 */
    int p[3];
    for (int i = 0; i < 3; i++) {
        p[i] = spawn_init(UMODE_ISOLATION);
        KASSERT(p[i] > 0);
    }
    for (int i = 0; i < 3; i++)
        KASSERT(proc_wait(p[i]) == 0);
    kprintf("user: 3 processes wrote the same virtual address without interfering\n");

    KASSERT(proc_wait(12345) == -1);         /* 不存在的 pid */

    /* 所有行程結束後,它們的頁面、頁表、stack 都必須歸還 */
    thread_sleep(2);
    sched_trim();
    kheap_stats(&h1);
    KASSERT(sched_thread_count() == 1 + resident_threads());   /* main,加上網路的接收執行緒(有網路卡時) */
    /* heap 向頁面配置器要來的頁不會歸還(見 kmalloc.c),所以這段期間 heap 若長大了
       (例如管線的緩衝區第一次配置),要把那部分算進去:那些頁現在是 heap 的空閒空間 */
    size_t heap_grow = h1.heap_bytes - h0.heap_bytes;
    KASSERT(pmm_free_count() + vm_table_pages() == free0 - heap_grow / PAGE_SIZE);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + heap_grow);
    kprintf("user: all processes exited, memory reclaimed\n");
}
