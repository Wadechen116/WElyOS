/*
 * syscall.c - 系統呼叫層:把使用者傳來的參數取出、檢查,交給負責的模組
 *
 * 這一層不擁有任何狀態,也「看不到」struct proc 的內容:這裡只引入 proc_calls.h,
 * 其中 struct proc 是不透明的型別 — 想直接讀寫行程的欄位會編譯不過。
 * 要對行程做什麼,只能呼叫各模組的函式:
 *   行程本身(建立、等待、身分…) proc.c        訊號            proc_signal.c
 *   記憶體(複製、配額、mmap…)   proc_mem.c    描述符表、工作目錄 proc_fs.c
 * 檔案、網路、黑盒子等則直接呼叫它們各自對 kernel 公開的介面。
 */
#include <stddef.h>
#include "blackbox.h"
#include "console.h"
#include "cred.h"
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "net.h"
#include "netif.h"
#include "panic.h"
#include "proc.h"
#include "sched.h"
#include "timer.h"
#include "proc_calls.h"
#include "failinj.h"
#include "affinity_check.h"

/* rename / link:兩個路徑 */
static long sys_two_paths(struct proc *p, long num, uintptr_t uold, uintptr_t unew)
{
    char oldp[FS_PATH_MAX + 1], newp[FS_PATH_MAX + 1];
    int err = user_path(p, oldp, uold);

    if (err == 0)
        err = user_path(p, newp, unew);
    if (err)
        return err;
    return num == SYS_rename ? vfs_rename(oldp, newp) : vfs_link(oldp, newp);
}

/* ---- 網路 ---- */

static long sys_udp_open(struct proc *p, long port)
{
    struct file *f;

    if (port < 0 || port > 65535)
        return -E_INVAL;
    int err = netif_udp_open((uint16_t)port, &f);
    if (err)
        return err;
    int fd = fd_install(p, f);
    if (fd < 0)
        vfs_close(f);                        /* 描述符表滿了:socket 隨這次關閉釋放 */
    return fd;
}

/* 送出與接收共用:把使用者的 struct udpmsg 搬進來,資料經過一塊 kernel 的暫存區 */
static long sys_udp_io(struct proc *p, long fd, uintptr_t umsg, long timeout_ms, int recv)
{
    struct udpmsg m;
    long rc;

    if (copy_from_user(p, &m, umsg, sizeof(m)) != 0)
        return -E_FAULT;
    if (m.len > NET_UDP_MAX && !recv)
        return -E_INVAL;
    if (timeout_ms < 0 || timeout_ms > 60000)
        return -E_INVAL;
    struct file *f = fd_get(p, fd);
    if (!f)
        return -E_BADF;
    uint8_t *buf = kmalloc(NET_UDP_MAX);
    if (!buf) {
        vfs_close(f);
        return -E_NOMEM;
    }
    if (recv) {
        uint32_t ip = 0;
        uint16_t port = 0;
        size_t cap = m.len < NET_UDP_MAX ? m.len : NET_UDP_MAX;
        rc = netif_udp_recv(f, &ip, &port, buf, cap, (unsigned)timeout_ms);
        if (rc >= 0) {
            m.ip = ip;
            m.port = port;
            m.len = (uint32_t)rc;
            if (copy_to_user(p, (uintptr_t)m.data, buf, (size_t)rc) != 0 ||
                copy_to_user(p, umsg, &m, sizeof(m)) != 0)
                rc = -E_FAULT;
        }
    } else if (copy_from_user(p, buf, (uintptr_t)m.data, m.len) != 0) {
        rc = -E_FAULT;
    } else {
        rc = netif_udp_send(f, m.ip, m.port, buf, m.len);
    }
    kfree(buf);
    vfs_close(f);
    return rc;
}

static long sys_netinfo(struct proc *p, uintptr_t uinfo)
{
    struct netinfo ni;

    netif_info(&ni);
    return copy_to_user(p, uinfo, &ni, sizeof(ni)) == 0 ? 0 : -E_FAULT;
}

static long sys_lseek(struct proc *p, long fd, long off, long whence)
{
    struct file *f = fd_get(p, fd);

    if (!f)
        return -E_BADF;
    long r = vfs_seek(f, off, (int)whence);
    vfs_close(f);
    return r;
}

#define IO_CHUNK 256u                /* 讀寫時在 kernel 與使用者之間一次搬運的大小 */
#define IO_MAX   (1u << 20)          /* 單次讀寫的上限 */

static long sys_write(struct proc *p, long fd, uintptr_t ubuf, size_t len)
{
    char buf[IO_CHUNK];
    size_t done = 0;
    long rc = 0;
    struct file *f = fd_get(p, fd);

    if (!f)
        return -E_BADF;
    if (len > IO_MAX)
        len = IO_MAX;
    while (done < len) {
        size_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
        if (copy_from_user(p, buf, ubuf + done, n) != 0) {
            rc = -E_FAULT;           /* 位址不合法:拒絕 */
            break;
        }
        rc = vfs_write(f, buf, n);
        if (rc <= 0)
            break;
        done += (size_t)rc;
        if ((size_t)rc < n)
            break;                   /* 只寫進一部分(空間不足):回報已寫入的量 */
    }
    vfs_close(f);
    return done ? (long)done : rc;
}

static long sys_read(struct proc *p, long fd, uintptr_t ubuf, size_t len)
{
    char buf[IO_CHUNK];
    size_t done = 0;
    long rc = 0;
    struct file *f = fd_get(p, fd);

    if (!f)
        return -E_BADF;
    if (len > IO_MAX)
        len = IO_MAX;
    while (done < len) {
        size_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
        rc = vfs_read(f, buf, n);
        if (rc <= 0)
            break;
        if (copy_to_user(p, ubuf + done, buf, (size_t)rc) != 0) {
            rc = -E_FAULT;
            break;
        }
        done += (size_t)rc;
        if ((size_t)rc < n)
            break;                   /* 檔案結尾 */
    }
    vfs_close(f);
    return done ? (long)done : rc;
}

static long sys_open(struct proc *p, uintptr_t upath, int flags)
{
    char path[FS_PATH_MAX + 1];
    struct file *f;
    int err = user_path(p, path, upath);

    if (err)
        return err;
    /* 只接受認得的旗標:不認得的位元可能是之後才有意義的功能,悄悄忽略會造成誤會 */
    if (flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND))
        return -E_INVAL;
    err = vfs_open(path, flags, &f);
    if (err)
        return err;
    int fd = fd_install(p, f);
    if (fd < 0)
        vfs_close(f);
    return fd;
}

static long sys_close(struct proc *p, long fd)
{
    struct file *f = fd_remove(p, fd);
    int err = 0;

    if (!f)
        return -E_BADF;
    /*
     * 描述符在這裡就已經用掉了(fd_remove):不論下面有沒有錯誤,這個編號都不再屬於呼叫者。
     * 所以 close 回報錯誤之後「不可以重試」— 那個編號可能已經被重新使用,重試會關掉別的檔案。
     *
     * 可寫的檔案:把還留在記憶體裡的修改寫出去,寫不成功就讓呼叫者知道。
     * 這是 close 能回報錯誤的唯一時機:vfs_close 只是放掉參考(可能還有 dup、fork 出來的別人拿著),
     * 沒有回傳值。要確定資料到了儲存媒體上,用 sys_fsync。
     */
    err = vfs_flush(f);                          /* 唯讀的開啟、管線、裝置:沒有東西要寫,回傳 0 */
    vfs_close(f);
    return err;
}

/* 高精度時鐘:只讀取一個硬體計數器,不上任何鎖,也是量「系統呼叫本身要多久」的基準 */
static long sys_clock(struct proc *p, uintptr_t uinfo)
{
    struct clockinfo ci;

    ci.hz = timer_clock_hz();
    if (ci.hz == 0)
        return -E_NOSYS;
    ci.count = timer_clock_count();
    timer_clock_marks(&ci.kmain, &ci.ready);
    return copy_to_user(p, uinfo, &ci, sizeof(ci)) == 0 ? 0 : -E_FAULT;
}

static long sys_fsync(struct proc *p, long fd)
{
    struct file *f = fd_get(p, fd);

    if (!f)
        return -E_BADF;
    long r = vfs_fsync(f);
    vfs_close(f);
    return r;
}

/* dup2:讓 newfd 指向和 oldfd 同一次開啟。newfd 原本若開著,先關閉。
   重新導向靠它:把檔案「接到」1 號描述符上,之後再接回原本的 */
static long sys_dup2(struct proc *p, long oldfd, long newfd)
{
    if (!fd_in_range(newfd))
        return -E_BADF;
    struct file *f = fd_get(p, oldfd);           /* 多拿一個參考,交給 newfd */
    if (!f)
        return -E_BADF;
    if (oldfd == newfd) {
        vfs_close(f);
        return newfd;
    }
    struct file *old = fd_replace(p, newfd, f);
    if (old)
        vfs_close(old);
    return newfd;
}

/* pipe:建立一條管線,把讀端與寫端的描述符編號寫到使用者的 fds[0]、fds[1] */
static long sys_pipe(struct proc *p, uintptr_t ufds)
{
    struct file *rd, *wr;
    int fds[2];
    int err = vfs_pipe(&rd, &wr);

    if (err)
        return err;
    fds[0] = fd_install(p, rd);
    fds[1] = fds[0] >= 0 ? fd_install(p, wr) : -E_MFILE;
    if (fds[0] < 0 || fds[1] < 0) {
        err = -E_MFILE;
    } else if (copy_to_user(p, ufds, fds, sizeof(fds)) != 0) {
        err = -E_FAULT;
    }
    if (err) {
        /* 把已經放進描述符表的拿出來;沒放進去的直接關閉 */
        if (fds[0] >= 0)
            fd_remove(p, fds[0]);
        if (fds[1] >= 0)
            fd_remove(p, fds[1]);
        vfs_close(rd);
        vfs_close(wr);
        return err;
    }
    return 0;
}

static long sys_readdir(struct proc *p, long fd, uintptr_t uent)
{
    struct dirent d;
    struct file *f = fd_get(p, fd);

    if (!f)
        return -E_BADF;
    memset(&d, 0, sizeof(d));
    long rc = vfs_readdir(f, &d);
    vfs_close(f);
    /* 錯誤只關於「這一筆」時(名稱有填,位置已經移到它之後),也把名稱交給程式:
       它可以回報「哪一筆」讀不到,然後繼續列下一筆。名稱是空的 = 整個目錄讀不下去 */
    if ((rc == 0 || (rc < 0 && d.name[0])) && copy_to_user(p, uent, &d, sizeof(d)) != 0)
        rc = -E_FAULT;
    return rc;
}

static long sys_path_op(struct proc *p, long num, uintptr_t upath, uintptr_t uarg)
{
    char path[FS_PATH_MAX + 1];
    int err = user_path(p, path, upath);

    if (err)
        return err;
    if (num == SYS_statfs) {
        struct fsinfo fi;
        err = vfs_statfs(path, &fi);
        if (err == 0 && copy_to_user(p, uarg, &fi, sizeof(fi)) != 0)
            err = -E_FAULT;
        return err;
    }
    if (num == SYS_chmod)
        return vfs_chmod(path, (unsigned)uarg);
    if (num == SYS_mkdir)
        return vfs_mkdir(path);
    if (num == SYS_unlink)
        return vfs_unlink(path);

    struct stat st;                  /* SYS_stat */
    err = vfs_stat(path, &st);
    if (err == 0 && copy_to_user(p, uarg, &st, sizeof(st)) != 0)
        err = -E_FAULT;
    return err;
}

long syscall_dispatch(long num, long a0, long a1, long a2, void *tf)
{
    struct proc *p = thread_data();

    if (!p)
        panic("syscall %ld from a thread without a process", num);

    switch (num) {
    case SYS_write:
        return sys_write(p, a0, (uintptr_t)a1, (size_t)a2);
    case SYS_read:
        return sys_read(p, a0, (uintptr_t)a1, (size_t)a2);
    case SYS_open:
        return sys_open(p, (uintptr_t)a0, (int)a1);
    case SYS_close:
        return sys_close(p, a0);
    case SYS_fsync:
        return sys_fsync(p, a0);
    case SYS_clock:
        return sys_clock(p, (uintptr_t)a0);
    case SYS_dup2:
        return sys_dup2(p, a0, a1);
    case SYS_pipe:
        return sys_pipe(p, (uintptr_t)a0);
    case SYS_readdir:
        return sys_readdir(p, a0, (uintptr_t)a1);
    case SYS_mkdir:
    case SYS_unlink:
    case SYS_stat:
    case SYS_statfs:
    case SYS_chmod:
        return sys_path_op(p, num, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_chown: {
        char path[FS_PATH_MAX + 1];
        int err = user_path(p, path, (uintptr_t)a0);
        /* 磁碟上的編號只有 16 位元:超過的直接拒絕,不要悄悄截掉 */
        if (err == 0 && (a1 < 0 || a1 > 0xFFFF || a2 < 0 || a2 > 0xFFFF))
            err = -E_INVAL;
        return err ? err : vfs_chown(path, (uint32_t)a1, (uint32_t)a2);
    }
    case SYS_getuid:
    case SYS_getgid:
    case SYS_capget:
    case SYS_setuid:
    case SYS_capdrop:
        return proc_cred_call(p, num, a0, a1);   /* 身分與特權:見 proc.c */
    case SYS_sync:
        return vfs_sync();
    case SYS_failinj:
        if (!cred_current() || !(cred_current()->caps & CAP_ADMIN))
            return -E_PERM;
        return failinj_syscall((long)a0, (long)a1, (long)a2);
    case SYS_setaffinity: {          /* I-344:參數規則在 affinity_check.h(純函式,主機端可測) */
        unsigned m;
        if (a0 < 0 || a0 > 0x7fffffffL)
            return -E_INVAL;
        int rc = affinity_mask_check((unsigned long)a1, sched_online_mask(), &m);
        if (rc != 0)
            return rc;
        /* 線上遮罩在鎖內會再檢查一次:這裡只是先擋掉明顯不合法的輸入 */
        return thread_set_affinity_checked((int)a0, m, thread_domain(), proc_has_cap(p, CAP_ADMIN));
    }
    case SYS_getaffinity: {
        unsigned m = 0;
        if (a0 < 0 || a0 > 0x7fffffffL)
            return -E_INVAL;
        int rc = thread_get_affinity_checked((int)a0, thread_domain(), proc_has_cap(p, CAP_ADMIN), &m);
        return rc != 0 ? rc : (long)m;
    }
    case SYS_lseek:
        return sys_lseek(p, a0, a1, a2);
    case SYS_sbrk:
        return sys_sbrk(p, a0);
    case SYS_mmap:
        return sys_mmap(p, (uintptr_t)a0);
    case SYS_munmap:
        return sys_munmap(p, (uintptr_t)a0);
    case SYS_memlimit:
        return sys_memlimit(p, a0);
    case SYS_seu:                    /* 故障注入:見 proc_mem.c */
        return sys_seu(p, a0, (uintptr_t)a1, a2);
    case SYS_rt_set:                 /* 即時工作優先於所有人:只給有特權的行程(而且仍受預算與總量的限制) */
        if (!proc_has_cap(p, CAP_ADMIN))
            return -E_PERM;
        if (a0 < 0 || a1 < 0 || a0 > 60000 || a1 > 60000)
            return -E_INVAL;
        return thread_set_periodic((unsigned)a0, (unsigned)a1);
    case SYS_rt_wait: {
        struct rt_stats rs;
        int rc = thread_wait_period(&rs);
        if (rc == -E_INVAL)
            return rc;                           /* 不是週期性工作:rs 沒有內容 */
        if (a0 && copy_to_user(p, (uintptr_t)a0, &rs, sizeof(rs)) != 0)
            return -E_FAULT;
        return rc;
    }
    case SYS_bbread: {               /* 讀取黑盒子的一筆紀錄(0 = 最新的) */
        struct bb_record r;
        if (a0 < 0)
            return -E_INVAL;
        if (blackbox_read((unsigned)a0, &r) != 0)
            return 1;
        return copy_to_user(p, (uintptr_t)a1, &r, sizeof(r)) == 0 ? 0 : -E_FAULT;
    }
    case SYS_bblog: {                /* 在黑盒子留下一筆紀錄;需要 CAP_ADMIN(否則任何人都能把它灌滿) */
        char text[BB_TEXT_MAX + 1];
        if (!proc_has_cap(p, CAP_ADMIN))
            return -E_PERM;
        if (a0 != BB_NOTE && a0 != BB_RESTART)
            return -E_INVAL;
        if (!blackbox_enabled())
            return -E_IO;                    /* 沒有黑盒子(沒有磁碟,或磁碟沒有留出那一區) */
        size_t i = 0;
        for (; i < BB_TEXT_MAX; i++) {
            if (copy_from_user(p, &text[i], (uintptr_t)a1 + i, 1) != 0)
                return -E_FAULT;
            if (!text[i])
                break;
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7E)
                text[i] = '?';               /* 只留可以顯示的字元:紀錄會被原樣印到終端機上 */
        }
        text[i] = '\0';
        blackbox_log((int)a0, "%s", text);
        return 0;
    }
    case SYS_scrub: {                /* 讀整顆磁碟:只給有特權的行程 */
        struct scrubinfo si;
        struct wfs_stats ws;
        uint32_t pos = 1;
        if (!proc_has_cap(p, CAP_ADMIN))
            return -E_PERM;
        memset(&si, 0, sizeof(si));
        wfs_stats(&ws);
        si.checksums = (uint32_t)ws.checksums;
        si.blocks = ws.blocks;
        /* 分批做,每批之間放開檔案系統的鎖:巡檢期間別的行程照常可以讀寫 */
        for (uint32_t done = 0; ws.checksums && done < ws.blocks; done += 64)
            si.bad += wfs_scrub(&pos, 64);
        wfs_stats(&ws);
        si.read_errors = (uint32_t)ws.csum_errors;
        si.sb_restored = (uint32_t)ws.sb_restored;
        return copy_to_user(p, (uintptr_t)a0, &si, sizeof(si)) == 0 ? 0 : -E_FAULT;
    }
    case SYS_ping:
        if (a1 < 0 || a1 > 60000)
            return -E_INVAL;
        return netif_ping((uint32_t)a0, (unsigned)a1);
    case SYS_udp_open:
        return sys_udp_open(p, a0);
    case SYS_udp_send:
        return sys_udp_io(p, a0, (uintptr_t)a1, 0, 0);
    case SYS_udp_recv:
        return sys_udp_io(p, a0, (uintptr_t)a1, a2, 1);
    case SYS_netinfo:
        return sys_netinfo(p, (uintptr_t)a0);
    case SYS_time: {
        struct timeval tv;
        memset(&tv, 0, sizeof(tv));
        tv.sec = timer_epoch(&tv.usec);
        return copy_to_user(p, (uintptr_t)a0, &tv, sizeof(tv)) == 0 ? 0 : -E_FAULT;
    }
    case SYS_signal:
        return sys_signal(p, a0, (uintptr_t)a1, (uintptr_t)a2);
    case SYS_sigreturn:
        return sys_sigreturn(p, (uintptr_t)a0, tf);
    case SYS_rename:
    case SYS_link:
        return sys_two_paths(p, num, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_chdir:
        return sys_chdir(p, (uintptr_t)a0);
    case SYS_getcwd:
        return sys_getcwd(p, (uintptr_t)a0, (size_t)a1);
    case SYS_sysinfo:
        return sys_sysinfo(p, (uintptr_t)a0);
    case SYS_tcsetmode: {
        if (a0 & ~(long)(CON_RAW | CON_NOECHO))
            return -E_INVAL;
        unsigned long flags = sched_lock();
        long rc = console_set_mode((unsigned)a0);
        sched_unlock(flags);
        return rc;
    }
    case SYS_exit:
        /* 「被暫停」的回報借用了結束碼裡的一小段數值(見 WSTOPPED_BASE);
           程式自己用這段數值結束的話,只留下最低 8 個位元,免得被誤認 */
        if (WIFSTOPPED((int)a0))
            a0 &= 0xff;
        proc_exit((int)a0);
        return 0;                    /* 不會到達 */
    case SYS_getpid:
        return proc_pid(p);
    case SYS_yield:
        thread_yield();
        return 0;
    case SYS_sleep:
        if (a0 < 0 || a0 > 60000)
            return -1;
        thread_sleep((unsigned)a0);
        return 0;
    case SYS_thread_create:
        /* 進入點必須是使用者可執行的位址,否則新執行緒一開始就會出錯 */
        if (!proc_user_addr_ok(p, (uintptr_t)a0))
            return -1;
        return proc_add_thread(p, (uintptr_t)a0, (uintptr_t)a1, 0);
    case SYS_spawn:
        return sys_spawn(p, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_exec:
        return sys_exec(p, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_fork:
        return sys_fork(p, tf);
    case SYS_wait: {                 /* 舊的簡易介面:等指定的子行程,直接回傳它的結束碼 */
        int code = 0;
        long rc = proc_wait_child(p, a0, &code, 0, 0);
        return rc < 0 ? rc : code;
    }
    case SYS_waitpid: {              /* 回傳子行程的 pid,結束碼寫到使用者的 *status */
        int code = 0;
        if (a2 & ~(long)(WNOHANG | WUNTRACED))
            return -E_INVAL;
        long rc = proc_wait_child(p, a0, &code, (int)(a2 & WNOHANG), (int)(a2 & WUNTRACED));
        if (rc > 0 && a1 && copy_to_user(p, (uintptr_t)a1, &code, sizeof(code)) != 0)
            return -E_FAULT;
        return rc;
    }
    case SYS_procinfo:
        return sys_procinfo(p, a0, (uintptr_t)a1);
    case SYS_kill:
        return sys_kill(p, a0, a1);
    case SYS_setpgid:
        return sys_setpgid(p, a0, a1);
    case SYS_tcsetfg: {              /* 設定主控台的前景群組(Ctrl-C 的對象);0 = 沒有 */
        if (a0 < 0)
            return -E_INVAL;
        unsigned long flags = sched_lock();
        console_set_fg((int)a0, proc_pgid(p));
        sched_unlock(flags);
        return 0;
    }
    case SYS_thread_exit:
        proc_thread_exit();
        return 0;                    /* 不會到達 */
    case SYS_gettid:
        return thread_id();
    case SYS_futex_wait:
        return sys_futex_wait(p, (uintptr_t)a0, (int)a1);
    case SYS_futex_wake:
        return sys_futex_wake(p, (uintptr_t)a0);
    default:
        return -1;                   /* 不認得的編號:回報錯誤,不當成致命問題 */
    }
}
