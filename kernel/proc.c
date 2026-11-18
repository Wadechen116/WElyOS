/*
 * proc.c - 行程管理與系統呼叫(與架構無關)
 */
#include <stddef.h>
#include "arch.h"
#include "blackbox.h"
#include "console.h"
#include "cpu.h"
#include "cred.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
#include "net.h"
#include "netif.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "vm.h"

/*
 * 使用者程式的檔案格式(user/init.c 編譯後的原始二進位,放在檔案系統的 /bin/init)。
 * 開頭是一個小標頭,描述各區段的範圍(由 user/user.ld 產生):
 */
struct uimg_hdr {
    uint32_t magic;         /* "WELY" */
    uint32_t entry;         /* 進入點(相對於 USER_BASE 的偏移,以下皆同) */
    uint32_t text_end;      /* 程式碼與唯讀資料的結尾(對齊到頁) */
    uint32_t data_end;      /* 已初始化資料的結尾 = 檔案內容的結尾 */
    uint32_t bss_end;       /* 未初始化資料的結尾;data_end 之後的部分要清為 0 */
};
#define UIMG_MAGIC 0x594C4557u

/* 使用者 stack:每條執行緒一格(1MB),從 USER_STACK_TOP 往下排;每格只對映最上面 16KB,
   其餘留空,所以 stack 用過頭會碰到沒有對映的位址而被終止 */
#define USER_STACK_TOP   (USER_BASE + 0x30000000u)
#define USER_STACK_SLOT  0x100000u
#define USER_STACK_PAGES 4u
#define PROC_MAX_THREADS 16u

/* 使用者 heap:程式的資料之後、stack 之前的一段固定範圍(程式本身最大 256MB,見 image_build)。
   行程以 sbrk 移動「結尾」(break)來取得或歸還記憶體;範圍內的頁用到才配置 */
#define USER_HEAP_BASE   (USER_BASE + 0x10000000u)
#define USER_HEAP_MAX    0x10000000u            /* 最多 256MB */

/* 匿名對映(mmap):heap 之後的另一段固定範圍,每次要求得到其中一塊,用完可以個別歸還 */
#define USER_MMAP_BASE   (USER_BASE + 0x20000000u)
#define USER_MMAP_MAX    0x0F000000u            /* 240MB;其上是各執行緒的 stack */
#define PROC_MAX_MAPS    16u                    /* 每個行程最多同時有幾塊 */

/* 記憶體配額的預設值:每個行程最多可以要 32MB(heap 加上匿名對映);可以用 memlimit 調低 */
#define PROC_DEFAULT_LIMIT_PAGES (32u * 1024 * 1024 / PAGE_SIZE)

#define FUTEX_BUCKETS 16u
#define PROC_MAX_FD   16u            /* 每個行程最多同時開啟幾個檔案 */

#define NSIG    32                   /* 訊號編號的上限(1 ~ NSIG-1) */
#define SIG_DFL 0u                   /* 預設的處理方式(終止或暫停) */
#define SIG_IGN 1u                   /* 忽略 */

struct proc {
    int pid;
    int ppid;                        /* 建立它的行程(父行程);0 = 由 kernel 建立,或父行程已經結束 */
    int pgid;                        /* 行程群組:shell 把同一條管線的行程放在同一組,
                                        Ctrl-C 一次終止整組(見 console.c) */
    int detached;                    /* 父行程已經結束:沒有人會來 wait,結束時自動回收 */
    struct cred cred;                /* 身分與特權(見 cred.h);受排程器的鎖保護 */
    char name[32];                   /* 程式檔的名稱(路徑的最後一段),訊息中使用 */
    char cwd[FS_PATH_MAX + 1];       /* 目前的工作目錄(整理過的絕對路徑);受 fd_lk 保護 */
    struct vm_space *space;
    struct file *exe;                /* 正在執行的程式檔。一直開著:它的頁直接對映在行程裡 */
    unsigned nthreads;               /* 還活著的執行緒數 */
    unsigned nstacks;                /* 已分配的 stack 格數(不重複使用) */
    unsigned long commit;            /* 已經答應給這個行程的頁數(heap 的範圍加上匿名對映);受 fd_lk 保護 */
    unsigned long limit;             /* commit 的上限(記憶體配額,以頁計) */
    struct { uintptr_t start; unsigned long pages; } maps[PROC_MAX_MAPS];   /* 匿名對映;pages 0 = 空位 */
    uintptr_t mmap_next;             /* 下一塊對映要放的位址(只往上走,不重複使用) */
    uintptr_t brk;                   /* heap 目前的結尾(0 = 還沒用過,等於 USER_HEAP_BASE);受 fd_lk 保護 */
    int exiting;                     /* 已開始結束流程 */
    int exit_code;
    int zombie;                      /* 所有執行緒都結束了,等待 proc_wait 來收 */
    uintptr_t sig_handler[NSIG];     /* 每個訊號的處理方式:SIG_DFL、SIG_IGN 或使用者函式的位址 */
    uintptr_t sig_restorer;          /* 處理函式返回時跳去的地方(使用者端的一小段程式,呼叫 sigreturn) */
    uint32_t sig_pending;            /* 已經送來、還沒交給處理函式的訊號(bit n = 訊號 n) */
    int stopped;                     /* 不為 0 = 被暫停了,值是造成暫停的訊號編號(見下方「暫停與繼續」) */
    int stop_reported;               /* 這次暫停已經告訴過父行程了(waitpid 的 WUNTRACED 只回報一次) */
    struct wait_queue stop_wq;       /* 暫停期間,它的執行緒睡在這裡,等 SIGCONT */
    unsigned long cpu_ticks;         /* 已經結束的執行緒用掉的 CPU 時間(還活著的另外算,見 sys_procinfo) */
    struct wait_queue waiters;       /* 正在 proc_wait 的執行緒(kernel 等特定的行程) */
    struct wait_queue child_wq;      /* 這個行程自己的執行緒,正在等「任何一個子行程結束」 */
    struct wait_queue futex[FUTEX_BUCKETS];  /* futex 的等待者,依位址分桶 */
    struct file *fd[PROC_MAX_FD];    /* 檔案描述符表:編號 -> 開啟中的檔案;NULL = 空位 */
    struct proc *next;
};

/*
 * 上鎖:行程的串列與每個行程的狀態(nthreads、exiting、zombie、等待佇列)
 * 都由排程器的鎖保護(sched_lock,見 sched.h),因為「等待行程結束」「futex」
 * 需要把「檢查狀態」與「睡下去」做成不可分割的一步。
 * 會花時間或需要其他鎖的工作(配置記憶體、建立執行緒、釋放位址空間)一律放在鎖外。
 */
/* 訊號編號(沿用 Unix 的定義;說明見下方「訊號」) */
#define SIGINT  2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21

static struct proc *procs;           /* 所有行程(含 zombie) */
static int next_pid = 1;

/* 新執行緒進入使用者模式所需的資訊,由建立者交給新執行緒 */
struct ustart {
    struct proc *p;
    uintptr_t entry, usp, arg;
};

static void fd_close_all(struct proc *p);
static struct proc *proc_became_zombie(struct proc *p);
static void proc_begin_exit(struct proc *p, int code);
static void proc_free_list(struct proc *list);
static int proc_fault(struct proc *p, uintptr_t addr, int write);
static int copy_from_user(struct proc *p, void *dst, uintptr_t uaddr, size_t len);
static void proc_exit(int code);
static void commit_release_all(struct proc *p);
static int copy_to_user(struct proc *p, uintptr_t uaddr, const void *src, size_t len);

/* 目前這條執行緒代表誰(cred.h)。kernel 自己的執行緒回傳 NULL:不受限制。
   回傳的指標指向行程結構裡的欄位;行程在它自己的執行緒還在執行時不會消失 */
const struct cred *cred_current(void)
{
    struct proc *p = thread_data();

    return p ? &p->cred : NULL;
}

static struct proc *proc_find(int pid)
{
    for (struct proc *p = procs; p; p = p->next)
        if (p->pid == pid)
            return p;
    return NULL;
}

/* 配置一個清為 0 的實體頁並對映到位址空間的 va。回傳該頁在 kernel 中可用的位址 */
static void *map_page(struct vm_space *vs, uintptr_t va, unsigned flags)
{
    void *page = pmm_alloc();
    if (!page)
        return NULL;
    memset(page, 0, PAGE_SIZE);      /* 清 0 也是安全措施:不把別人用過的資料交給行程 */
    if (vm_space_map(vs, va, (uintptr_t)page, flags) != 0) {
        pmm_free(page);
        return NULL;
    }
    return page;
}

/* 對映第 slot 格使用者 stack(最上面的 16KB),回傳 stack 頂端的位址(失敗回傳 0) */
static uintptr_t map_user_stack(struct vm_space *vs, unsigned slot)
{
    uintptr_t top = USER_STACK_TOP - (uintptr_t)slot * USER_STACK_SLOT;

    for (unsigned i = 1; i <= USER_STACK_PAGES; i++)
        if (!map_page(vs, top - i * PAGE_SIZE, VM_READ | VM_WRITE))
            return 0;
    return top;
}

/* 替行程的一條新執行緒準備使用者 stack,回傳 stack 頂端的位址(失敗回傳 0) */
static uintptr_t alloc_user_stack(struct proc *p)
{
    if (p->nstacks >= PROC_MAX_THREADS)
        return 0;
    uintptr_t top = map_user_stack(p->space, p->nstacks);
    if (top)
        p->nstacks++;
    return top;
}

/* 把資料寫進「某個位址空間」的使用者位址(不必是目前載入的那一個):
   逐頁查出實體位址再寫入。用來在新行程開始執行之前,先把參數放到它的 stack 上 */
static int space_write(struct vm_space *vs, uintptr_t uaddr, const void *src, size_t len)
{
    const uint8_t *in = src;

    while (len > 0) {
        uintptr_t pa;
        unsigned flags;
        if (vm_space_query(vs, uaddr, &pa, &flags) != 0 || !(flags & VM_WRITE))
            return -1;
        size_t n = PAGE_SIZE - (uaddr & (PAGE_SIZE - 1));
        if (n > len)
            n = len;
        memcpy((void *)pa, in, n);
        in += n;
        uaddr += n;
        len -= n;
    }
    return 0;
}

/* ---- 程式的參數(argv) ---- */

#define ARG_MAX_COUNT 16u
#define ARG_MAX_BYTES 1024u

/* 一組參數:字串一個接一個放在 buf 裡(各自以 0 結尾) */
struct args {
    unsigned argc;
    unsigned len;
    char buf[ARG_MAX_BYTES];
};

static int args_add(struct args *a, const char *s)
{
    size_t n = strlen(s) + 1;

    if (a->argc >= ARG_MAX_COUNT || a->len + n > ARG_MAX_BYTES)
        return -E_INVAL;
    memcpy(a->buf + a->len, s, n);
    a->len += (unsigned)n;
    a->argc++;
    return 0;
}

/*
 * ---- 程式映像:一個準備好、隨時可以開始執行的位址空間 ----
 *
 * 建立行程(spawn)與更換程式(exec)都要做同一件事:建立位址空間、載入程式、
 * 準備 stack 與參數。把它做成「先完整建好,成功了才使用」:
 * 中途任何一步失敗就整個丟掉,呼叫者原本的狀態完全不受影響。
 * 這對 exec 尤其重要:新程式載入失敗時,舊程式必須還能繼續執行。
 */
struct image {
    struct vm_space *space;
    struct file *exe;                /* 程式檔(已宣告為「執行中」) */
    uintptr_t entry;                 /* 進入點 */
    uintptr_t usp;                   /* 主執行緒一開始的 stack pointer */
    uintptr_t argp;                  /* 參數區塊的位址,傳給進入點 */
};

static void image_discard(struct image *img)
{
    if (img->space)
        vm_space_destroy(img->space);
    if (img->exe) {
        vfs_exec_release(img->exe);
        vfs_close(img->exe);
    }
    img->space = NULL;
    img->exe = NULL;
}

/*
 * 把參數放到新 stack 的頂端,格式是使用者程式的進入點所預期的:
 *
 *     高位址   字串本身("echo\0hello\0…")
 *              NULL
 *              argv[argc-1] 的位址
 *              …
 *              argv[0] 的位址
 *     argp ->  argc
 *     低位址   (stack 從這裡往下使用)
 *
 * 每一格是一個指標的大小。字串的位址是「在新行程裡」的使用者位址。
 */
static int image_put_args(struct image *img, uintptr_t top, const struct args *a)
{
    uintptr_t str_base = (top - a->len) & ~(uintptr_t)15;
    uintptr_t block = (str_base - (a->argc + 2) * sizeof(uintptr_t)) & ~(uintptr_t)15;
    uintptr_t words[ARG_MAX_COUNT + 2];
    unsigned off = 0;

    words[0] = a->argc;
    for (unsigned i = 0; i < a->argc; i++) {
        words[1 + i] = str_base + off;
        off += (unsigned)strlen(a->buf + off) + 1;
    }
    words[1 + a->argc] = 0;
    if (space_write(img->space, str_base, a->buf, a->len) != 0 ||
        space_write(img->space, block, words, (a->argc + 2) * sizeof(uintptr_t)) != 0)
        return -E_FAULT;
    img->argp = block;
    img->usp = block;                /* 已對齊 16 位元組(各架構的呼叫慣例要求) */
    return 0;
}

/* 從檔案系統載入 path 指到的程式,連同參數準備好。成功回傳 0,*img 可以直接使用;
   失敗回傳負的錯誤碼,*img 不留下任何東西 */
static int image_build(const char *path, const struct args *a, struct image *img)
{
    struct uimg_hdr hdr;
    const struct uimg_hdr *h = &hdr;
    int err;

    memset(img, 0, sizeof(*img));
    err = vfs_open(path, O_RDONLY, &img->exe);
    if (err) {
        img->exe = NULL;
        return err;
    }
    /* 宣告「執行中」:從現在起沒有人能修改這個檔案,它的頁才可以安全地共用 */
    err = vfs_exec_hold(img->exe);
    if (err) {
        vfs_close(img->exe);
        img->exe = NULL;
        return err;
    }
    uint64_t file_size = vfs_size(img->exe);

    /* 標頭來自檔案,內容不可信任:每個欄位都要檢查,否則一個損壞(或惡意)的檔案
       就能讓 kernel 讀寫到範圍以外 */
    err = -E_NOEXEC;
    if (vfs_pread(img->exe, &hdr, sizeof(hdr), 0) != (long)sizeof(hdr) ||
        h->magic != UIMG_MAGIC || h->data_end > file_size ||
        (h->text_end & (PAGE_SIZE - 1)) ||
        h->text_end > h->data_end || h->data_end > h->bss_end || h->entry >= h->text_end ||
        h->bss_end > 0x10000000u)
        goto fail;

    err = -E_NOMEM;
    img->space = vm_space_create();
    if (!img->space)
        goto fail;

    for (uint32_t off = 0; off < h->bss_end; off += PAGE_SIZE) {
        uintptr_t va = USER_BASE + off;

        if (off < h->text_end) {
            /*
             * 程式碼與唯讀資料:唯讀 + 可執行。內容永遠不會被行程修改,
             * 所以不必每個行程各複製一份:直接把「檔案的那一頁」對映進來,
             * 所有執行同一個程式的行程共用同一份實體頁。
             * 省下每次建立行程時的配置、清 0 與複製,也省記憶體。
             * VM_SHARED 讓位址空間釋放時不會把這一頁(它屬於檔案)一起釋放。
             */
            void *shared = vfs_file_page(img->exe, off);
            if (shared) {
                if (vm_space_map(img->space, va, (uintptr_t)shared,
                                 VM_READ | VM_EXEC | VM_SHARED) != 0)
                    goto fail;
                continue;
            }
        }
        /* 資料:可讀寫、不可執行(使用者程式同樣遵守 W^X)。每個行程要有自己的一份 */
        unsigned flags = off < h->text_end ? (VM_READ | VM_EXEC) : (VM_READ | VM_WRITE);
        uint8_t *page = map_page(img->space, va, flags);
        if (!page)
            goto fail;
        if (off < h->data_end) {
            size_t n = h->data_end - off;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            err = -E_IO;
            if (vfs_pread(img->exe, page, n, off) != (long)n)
                goto fail;
            err = -E_NOMEM;
        }
    }

    uintptr_t top = map_user_stack(img->space, 0);
    if (!top)
        goto fail;
    err = image_put_args(img, top, a);
    if (err)
        goto fail;
    img->entry = USER_BASE + h->entry;
    return 0;

fail:
    image_discard(img);
    return err;
}

/* 取路徑的最後一段當作行程的名稱 */
static void proc_set_name(struct proc *p, const char *path)
{
    const char *base = path;
    size_t n = 0;

    for (const char *c = path; *c; c++)
        if (*c == '/' && c[1])
            base = c + 1;
    while (base[n] && base[n] != '/' && n < sizeof(p->name) - 1) {
        p->name[n] = base[n];
        n++;
    }
    p->name[n] = '\0';
}

/* 目前這條執行緒離開行程;若是最後一條,行程變成 zombie。不會返回 */
static void proc_thread_exit(void)
{
    struct proc *p = thread_data();

    /* 先放掉位址空間(換回 kernel 頁表),再把自己從行程的執行緒數中扣掉。
       順序很重要:扣到 0 的那條執行緒會釋放位址空間,而此時其他執行緒可能還在
       別顆 CPU 上走完結束流程。只要它們都已經不再「屬於」這個位址空間,
       排程器就不會再替它們載入這份(即將被釋放的)頁表 */
    thread_set_space(NULL);

    unsigned long flags = sched_lock();
    int last = --p->nthreads == 0;
    p->cpu_ticks += thread_cpu_ticks();  /* 這條執行緒用掉的時間記到行程頭上(ps 會顯示) */
    sched_unlock(flags);

    if (last) {
        /* 自己是最後一條:已經沒有執行緒屬於這個位址空間,可以在鎖外釋放 */
        vm_space_destroy(p->space);
        commit_release_all(p);           /* 答應給它的記憶體全部歸還 */
        vfs_exec_release(p->exe);        /* 對映已經移除:程式檔不再「執行中」 */
        vfs_close(p->exe);
        fd_close_all(p);                 /* 行程結束時,它開啟的檔案由 kernel 代為關閉 */

        flags = sched_lock();
        p->space = NULL;
        struct proc *dead = proc_became_zombie(p);
        sched_unlock(flags);
        /* 解鎖之後等待者隨時可能把 p 釋放(p 也可能就在 dead 裡),不可再存取 */
        proc_free_list(dead);
    }
    thread_set_data(NULL);
    thread_exit();
}

/* 開始結束行程:記下結束碼,要求所有執行緒結束。呼叫時必須持有排程器的鎖 */
static void proc_begin_exit(struct proc *p, int code)
{
    if (p->exiting)
        return;
    p->exiting = 1;
    p->exit_code = code;
    sched_kill_group(p);
    /* 正睡在 kernel 裡的執行緒(等 futex、等輸入、等管線…)由 sched_kill_group 叫醒 */
}

/* 結束整個行程(由行程自己的執行緒呼叫) */
static void proc_exit(int code)
{
    struct proc *p = thread_data();

    unsigned long flags = sched_lock();
    proc_begin_exit(p, code);
    sched_unlock(flags);
    proc_thread_exit();
}

/* 行程中每條執行緒在 kernel 這一側的起點:設定好歸屬與位址空間後降到使用者模式 */
static void user_thread_main(void *arg)
{
    struct ustart st = *(struct ustart *)arg;

    kfree(arg);
    thread_set_data(st.p);
    thread_set_space(st.p->space);

    /*
     * 還沒開始跑,行程就已經開始結束了?兩種情況都要檢查:
     *   - 行程是在「上面登記歸屬之後」才開始結束的:結束流程會把我們標記為應終止
     *   - 行程在「我們登記之前」就開始結束了:那時我們還不算它的執行緒,
     *     沒被標記到,只能看行程本身的狀態
     * 少了第二項,這條執行緒會照常進入使用者模式執行,行程就永遠結束不了。
     */
    unsigned long flags = sched_lock();
    int dying = st.p->exiting;
    sched_unlock(flags);
    if (dying || thread_killed())
        proc_thread_exit();
    arch_enter_user(st.entry, st.usp, st.arg);
}

/* 在行程 p 中建立一條執行緒。usp 為它的使用者 stack pointer;
   傳 0 表示替它配置一格新的 stack。回傳 tid,失敗回傳 -1 */
static int proc_add_thread(struct proc *p, uintptr_t entry, uintptr_t arg, uintptr_t usp)
{
    struct ustart *st = kmalloc(sizeof(*st));
    if (!st)
        return -1;
    st->p = p;
    st->entry = entry;
    st->arg = arg;
    st->usp = usp ? usp : alloc_user_stack(p);
    if (!st->usp) {
        kfree(st);
        return -1;
    }

    unsigned long flags = sched_lock();
    p->nthreads++;                   /* 先算進去:新執行緒可能立刻開始跑甚至結束 */
    sched_unlock(flags);

    int tid = thread_create(p->name, user_thread_main, st);
    if (tid < 0) {
        flags = sched_lock();
        p->nthreads--;
        sched_unlock(flags);
        kfree(st);
    }
    return tid;
}

/* ---- 檔案描述符表 ---- */

/* 保護所有行程的 fd 表(只包住查表與填表的幾條指令) */
static struct spinlock fd_lk = SPINLOCK_INIT("fd");

/* 把一次開啟放進編號最小的空位;回傳編號,滿了回傳 -E_MFILE */
static int fd_install(struct proc *p, struct file *f)
{
    int fd = -E_MFILE;
    unsigned long fl = spin_lock_irqsave(&fd_lk);

    for (unsigned i = 0; i < PROC_MAX_FD; i++) {
        if (!p->fd[i]) {
            p->fd[i] = f;
            fd = (int)i;
            break;
        }
    }
    spin_unlock_irqrestore(&fd_lk, fl);
    return fd;
}

/* 取得描述符對應的檔案,並多拿一個參考:同一個行程的另一條執行緒
   可能同時關閉這個描述符,有了參考,檔案在我們用完之前不會消失。用完要 vfs_close */
static struct file *fd_get(struct proc *p, long fd)
{
    struct file *f = NULL;

    if (fd < 0 || fd >= (long)PROC_MAX_FD)
        return NULL;
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    f = p->fd[fd];
    if (f)
        vfs_dup(f);                  /* 必須在放開 fd_lk 之前:否則「查到指標」與「拿到參考」
                                        之間,另一條執行緒可能已經把它關閉並釋放了。
                                        鎖的順序固定為 fd_lk -> VFS 內部的 inode_lk */
    spin_unlock_irqrestore(&fd_lk, fl);
    return f;
}

static struct file *fd_remove(struct proc *p, long fd)
{
    struct file *f = NULL;

    if (fd < 0 || fd >= (long)PROC_MAX_FD)
        return NULL;
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    f = p->fd[fd];
    p->fd[fd] = NULL;
    spin_unlock_irqrestore(&fd_lk, fl);
    return f;
}

static void fd_close_all(struct proc *p)
{
    for (unsigned i = 0; i < PROC_MAX_FD; i++) {
        struct file *f = fd_remove(p, i);
        if (f)
            vfs_close(f);
    }
}

/* 新行程的 0、1、2 號描述符(標準輸入、輸出、錯誤)都指向主控台 */
static int fd_setup_console(struct proc *p)
{
    struct file *f;

    if (vfs_open("/dev/console", O_RDWR, &f) != 0)
        return -1;
    p->fd[0] = f;
    p->fd[1] = vfs_dup(f);
    p->fd[2] = vfs_dup(f);
    return 0;
}

/*
 * 新行程繼承另一個行程開啟中的檔案(同一次開啟,共用讀寫位置):編號 0 ~ count-1。
 *
 *   fork   全部繼承(子行程是父行程的複本)
 *   spawn  只繼承 0、1、2 號(標準輸入、輸出、錯誤)
 *
 * spawn 只給三個,是為了管線能正確結束:shell 手上通常還開著別的東西
 * (下一段管線的另一端、暫存的原本輸出…),新程式若連這些也繼承了,
 * 就會出現「管線的寫端其實還有人開著」,讀的那一方永遠等不到結束。
 * 新程式要用的東西,建立者先接到 0、1、2 號上就可以了(見 sh.c 的重新導向)。
 */
static void fd_inherit(struct proc *p, struct proc *parent, unsigned count)
{
    unsigned long fl = spin_lock_irqsave(&fd_lk);

    for (unsigned i = 0; i < count && i < PROC_MAX_FD; i++)
        if (parent->fd[i])
            p->fd[i] = vfs_dup(parent->fd[i]);
    spin_unlock_irqrestore(&fd_lk, fl);
}

/* 建立行程。parent 為 NULL 表示由 kernel 建立。回傳 pid 或負的錯誤碼 */
static int proc_create(const char *path, const struct args *a, struct proc *parent)
{
    struct image img;
    struct proc *p = kzalloc(sizeof(*p));
    int err;

    if (!p)
        return -E_NOMEM;
    err = image_build(path, a, &img);
    if (err) {
        kfree(p);
        return err;
    }
    if (parent) {
        fd_inherit(p, parent, 3);
    } else if (fd_setup_console(p) != 0) {
        image_discard(&img);
        kfree(p);
        return -E_IO;
    }
    proc_set_name(p, path);
    p->space = img.space;
    p->exe = img.exe;
    p->nstacks = 1;                  /* 第 0 格 stack 已由 image_build 準備好 */
    /* 身分沿用建立者的;kernel 建立的行程是 root,擁有全部的特權 */
    p->cred = parent ? parent->cred : (struct cred){ 0, 0, CAP_ALL };
    p->limit = parent ? parent->limit : PROC_DEFAULT_LIMIT_PAGES;    /* 配額沿用建立者的 */
    if (parent) {                    /* 工作目錄沿用建立者的;kernel 建立的行程從根目錄開始 */
        unsigned long fl = spin_lock_irqsave(&fd_lk);
        memcpy(p->cwd, parent->cwd, sizeof(p->cwd));
        spin_unlock_irqrestore(&fd_lk, fl);
    } else {
        p->cwd[0] = '/';
    }

    unsigned long flags = sched_lock();
    int pid = p->pid = next_pid++;
    p->ppid = parent ? parent->pid : 0;
    p->pgid = parent ? parent->pgid : pid;   /* 沿用建立者的群組;kernel 建立的自成一組 */
    p->next = procs;
    procs = p;
    sched_unlock(flags);

    if (proc_add_thread(p, img.entry, img.argp, img.usp) < 0) {
        /* 沒有任何執行緒:直接當作已結束,讓 proc_wait 回收 */
        image_discard(&img);
        fd_close_all(p);
        flags = sched_lock();
        p->space = NULL;
        p->exit_code = PROC_KILLED_FAULT;
        struct proc *dead = proc_became_zombie(p);
        sched_unlock(flags);
        proc_free_list(dead);
    }
    return pid;
}

int proc_spawn(const char *path, const char *const argv[])
{
    struct args *a = kzalloc(sizeof(*a));
    int rc = -1;

    if (!a)
        return -1;
    /* 沒有給參數時,依慣例 argv[0] 是程式自己的名稱 */
    if (!argv || !argv[0]) {
        if (args_add(a, path) != 0)
            goto out;
    } else {
        for (unsigned i = 0; argv[i]; i++)
            if (args_add(a, argv[i]) != 0)
                goto out;
    }
    rc = proc_create(path, a, NULL);
    if (rc < 0)
        rc = -1;
out:
    kfree(a);
    return rc;
}

int proc_wait(int pid)
{
    unsigned long flags = sched_lock();
    struct proc *p;
    int code = -1;

    /* 每次醒來都重新以編號尋找:如果有另一條執行緒也在等同一個行程,
       它可能先一步把行程回收並釋放了,不能沿用睡前拿到的指標(pid 不會重複使用) */
    while ((p = proc_find(pid)) != NULL && !p->zombie && !thread_killed())
        wq_sleep(&p->waiters);
    if (p && !p->zombie)
        p = NULL;                    /* 等待者自己被終止了:放棄等待,不回收對方 */
    if (p) {
        code = p->exit_code;
        for (struct proc **pp = &procs; *pp; pp = &(*pp)->next) {
            if (*pp == p) {
                *pp = p->next;
                break;
            }
        }
    }
    sched_unlock(flags);
    kfree(p);                        /* 已從串列移除,別人找不到它了;在鎖外釋放(NULL 無作用) */
    return code;
}

/*
 * ---- 父子關係 ----
 *
 * 行程結束後不會立刻消失:它的紀錄(結束碼)要留到「有人來問」為止,
 * 這種已經結束、等著被回收的行程稱為 zombie。誰負責來問?建立它的那個行程(父行程)。
 *
 *   父行程呼叫 wait      -> 取得結束碼,紀錄被回收
 *   父行程先結束了        -> 沒有人會來問了。子行程變成「沒有父行程」(detached),
 *                           它結束時直接回收,不留下紀錄。已經是 zombie 的當場回收
 * 這樣不論父行程有沒有好好 wait,紀錄都不會永遠留著。
 * (Unix 的做法是把這些行程過繼給 1 號行程;這裡沒有那樣的行程,所以由 kernel 直接回收。)
 */

/* 把行程從串列中拿掉(呼叫時持有排程器的鎖) */
static void proc_unlink(struct proc *p)
{
    for (struct proc **pp = &procs; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            return;
        }
    }
}

/*
 * p 的所有執行緒都結束了:標記為 zombie,通知等它的人,並處理它留下的子行程。
 * 呼叫時持有排程器的鎖。回傳「現在就可以釋放」的行程(以 next 串起來),
 * 呼叫者解鎖之後交給 proc_free_list — 釋放記憶體不在鎖內做。
 */
static struct proc *proc_became_zombie(struct proc *p)
{
    struct proc *dead = NULL;

    p->zombie = 1;
    wq_wake_all(&p->waiters);
    struct proc *parent = p->ppid ? proc_find(p->ppid) : NULL;
    if (parent)
        wq_wake_all(&parent->child_wq);

    /* 它的子行程從此沒有父行程 */
    for (struct proc **pp = &procs; *pp; ) {
        struct proc *q = *pp;
        if (q->ppid == p->pid) {
            q->ppid = 0;
            q->detached = 1;
            /* 暫停中的行程只有父行程(shell)能讓它繼續;父行程沒了,它會永遠停在那裡。
               所以直接終止它(Unix 對「失去管理者的暫停工作」也是這樣處理) */
            if (q->stopped && !q->zombie)
                proc_begin_exit(q, 128 + SIGKILL);
            if (q->zombie) {                 /* 已經結束、一直沒被 wait:現在回收 */
                *pp = q->next;
                q->next = dead;
                dead = q;
                continue;
            }
        }
        pp = &q->next;
    }
    if (p->detached) {                       /* 自己也沒有父行程:不留紀錄 */
        proc_unlink(p);
        p->next = dead;
        dead = p;
    }
    return dead;
}

static void proc_free_list(struct proc *list)
{
    while (list) {
        struct proc *next = list->next;
        kfree(list);
        list = next;
    }
}

/*
 * 行程等待自己的子行程結束。pid 為 -1 表示任何一個子行程。
 * 回傳被回收的子行程的 pid,它的結束碼放進 *code;
 * 沒有符合的子行程回傳 -E_CHILD;等待者自己被終止回傳 -E_INTR。
 * nohang 不為 0 時不等待:還沒有子行程結束就立刻回傳 0
 * (shell 用它在每次印提示符號之前,順手回收已經結束的背景工作)。
 * untraced 不為 0 時,子行程「被暫停」也算一個事件:回傳它的 pid,
 * *code 為 WSTOPPED_BASE | 訊號編號(見 proc.h 的 WIFSTOPPED);行程不會被回收。
 */
static long proc_wait_child(struct proc *p, long pid, int *code, int nohang, int untraced)
{
    struct proc *z = NULL;
    long rc;
    unsigned long flags = sched_lock();

    for (;;) {
        int found = 0;
        struct proc *st = NULL;
        for (struct proc *q = procs; q; q = q->next) {
            if (q->ppid != p->pid || (pid != -1 && q->pid != pid))
                continue;
            found = 1;
            if (q->zombie) {
                z = q;
                break;
            }
            if (untraced && q->stopped && !q->stop_reported && !st)
                st = q;
        }
        if (z) {
            proc_unlink(z);
            rc = z->pid;
            *code = z->exit_code;
            break;
        }
        if (st) {
            /* 子行程被暫停了:回報一次,但不回收(它還活著,之後可以繼續) */
            st->stop_reported = 1;
            rc = st->pid;
            *code = (int)(WSTOPPED_BASE | (unsigned)st->stopped);
            break;
        }
        if (!found) {
            rc = -E_CHILD;                   /* 只能等自己的子行程 */
            break;
        }
        if (nohang) {
            rc = 0;
            break;
        }
        if (thread_interrupted()) {              /* 被終止,或有訊號要處理 */
            rc = -E_INTR;
            break;
        }
        wq_sleep(&p->child_wq);              /* 任何一個子行程結束時會被叫醒,再檢查一次 */
    }
    sched_unlock(flags);
    kfree(z);
    return rc;
}

/*
 * ---- 訊號 ----
 *
 * 訊號(signal)是「從外面通知一個行程」的方法。編號沿用 Unix 的定義。
 * 每個訊號有預設的效果;除了 SIGKILL 與 SIGSTOP,行程可以改成「忽略」或
 * 「執行自己的處理函式」(見下方「訊號處理函式」):
 *
 *   終止   SIGINT  (2)   使用者按了 Ctrl-C
 *          SIGKILL (9)   強制終止(不能攔截)
 *          SIGUSR1 (10)、SIGUSR2 (12)  沒有固定意義,留給程式自己使用
 *          SIGTERM (15)  一般的「請結束」(kill 指令的預設)
 *   暫停   SIGSTOP (19)  暫停(不能攔截)
 *          SIGTSTP (20)  使用者按了 Ctrl-Z
 *          SIGTTIN (21)  背景的行程想讀鍵盤(見 console.c)
 *   繼續   SIGCONT (18)  讓暫停中的行程繼續
 *
 * 被訊號終止的行程,結束碼是 128 + 訊號編號(Unix shell 的慣例),
 * 所以從結束碼看得出它是自己結束的還是被終止的。
 *
 * ---- 暫停與繼續 ----
 *
 * 暫停 = 行程還在(記憶體、開啟的檔案都保留),只是不再執行使用者模式的程式碼。
 * 做法和「終止」用的是同一個檢查點:每條執行緒要從 kernel 返回使用者模式之前
 * 都會經過 user_return_check,看到行程被暫停,就睡在 p->stop_wq 上,等 SIGCONT。
 *
 *   - 正在使用者模式執行的執行緒:最晚在下一個計時器中斷(1ms)進入 kernel 時停下
 *   - 正睡在 kernel 裡的執行緒(等時間到、等管線…):不打斷它。它等的事情照常進行,
 *     醒來要回使用者模式時才停下。這樣不必讓每一種等待都處理「被暫停打斷」,
 *     系統呼叫也不會因為暫停而提早返回(程式看不出自己被暫停過)
 *   - 行程一被標記為暫停,父行程立刻得到通知(waitpid 的 WUNTRACED),
 *     不必等所有執行緒都真的停下:它們已經不可能再執行使用者模式的程式碼了
 *
 * 終止類的訊號對暫停中的行程立刻生效(不必先讓它繼續)。
 */
static int sig_valid(long sig)
{
    return sig == SIGINT || sig == SIGKILL || sig == SIGTERM || sig == SIGUSR1 || sig == SIGUSR2 ||
           sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN;
}

/* 行程可以自己決定怎麼處理的訊號 */
static int sig_catchable(long sig)
{
    return sig == SIGINT || sig == SIGTERM || sig == SIGUSR1 || sig == SIGUSR2 || sig == SIGTSTP;
}

/* 對一個行程送出訊號。呼叫時必須持有排程器的鎖;q 不可以是 zombie */
static void proc_signal_locked(struct proc *q, int sig)
{
    if (sig_catchable(sig) && q->sig_handler[sig] != SIG_DFL && !q->exiting) {
        if (q->sig_handler[sig] == SIG_IGN)
            return;                              /* 行程選擇忽略 */
        /* 行程有自己的處理函式:記下來,等它的某條執行緒要返回使用者模式時交給它
           (user_return_check)。正在 kernel 裡等待的執行緒先叫起來,不必等到它等的事情發生 */
        q->sig_pending |= 1u << sig;
        sched_interrupt_group(q);
        return;
    }
    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN) {
        if (q->exiting || q->stopped)
            return;
        q->stopped = sig;
        q->stop_reported = 0;
        struct proc *parent = q->ppid ? proc_find(q->ppid) : NULL;
        if (parent)
            wq_wake_all(&parent->child_wq);  /* 父行程可能正在 waitpid(WUNTRACED) */
    } else if (sig == SIGCONT) {
        if (!q->stopped)
            return;
        q->stopped = 0;
        wq_wake_all(&q->stop_wq);
        console_wake_readers();              /* 它可能是在等鍵盤輸入時被暫停的 */
    } else {
        proc_begin_exit(q, 128 + sig);
    }
}

int proc_signal_group_locked(int pgid, int sig)
{
    int n = 0;

    for (struct proc *q = procs; q; q = q->next) {
        if (q->pgid == pgid && !q->zombie) {
            proc_signal_locked(q, sig);
            n++;
        }
    }
    return n;
}

/* 目前這條執行緒所屬行程的群組;不屬於任何行程(kernel 執行緒)回傳 0。持有排程器的鎖時呼叫 */
int proc_self_pgid(void)
{
    struct proc *p = thread_data();

    return p ? p->pgid : 0;
}

/* 目前這條執行緒所屬的行程是不是被暫停了。持有排程器的鎖時呼叫 */
int proc_self_stopped(void)
{
    struct proc *p = thread_data();

    return p && p->stopped;
}

/* 群組 pgid 裡還有沒有活著的行程。持有排程器的鎖時呼叫 */
int proc_group_alive_locked(int pgid)
{
    for (struct proc *q = procs; q; q = q->next)
        if (q->pgid == pgid && !q->zombie)
            return 1;
    return 0;
}

/* p 可不可以對 q 送訊號:同一個使用者的行程之間可以;對別人的行程需要 CAP_KILL */
static int may_signal(struct proc *p, struct proc *q)
{
    return (p->cred.caps & CAP_KILL) || p->cred.uid == q->cred.uid;
}

/* kill(pid, sig):pid > 0 是單一行程;pid < 0 是整個群組(編號為 -pid)。
   沒有權限回傳 -E_PERM(群組:裡面只要有一個可以送就算成功,不能送的略過) */
static long sys_kill(struct proc *p, long pid, long sig)
{
    long rc = -E_SRCH;

    if (!sig_valid(sig) || pid == 0)
        return -E_INVAL;
    unsigned long flags = sched_lock();
    if (pid < 0) {
        for (struct proc *q = procs; q; q = q->next) {
            if (q->pgid != (int)-pid || q->zombie)
                continue;
            if (!may_signal(p, q)) {
                if (rc == -E_SRCH)
                    rc = -E_PERM;
                continue;
            }
            proc_signal_locked(q, (int)sig);
            rc = 0;
        }
    } else {
        struct proc *q = proc_find((int)pid);
        if (q && !q->zombie) {
            if (may_signal(p, q)) {
                proc_signal_locked(q, (int)sig);
                rc = 0;
            } else {
                rc = -E_PERM;
            }
        }
    }
    sched_preempt_point();
    sched_unlock(flags);
    return rc;
}

/* setpgid(pid, pgid):把自己(pid 為 0)或自己的子行程放進群組 pgid(0 表示以它自己的 pid 為群組) */
static long sys_setpgid(struct proc *p, long pid, long pgid)
{
    long rc = -E_SRCH;
    unsigned long flags = sched_lock();
    struct proc *q = pid == 0 ? p : proc_find((int)pid);

    if (pgid < 0) {
        rc = -E_INVAL;
    } else if (q && (q == p || q->ppid == p->pid)) {
        q->pgid = pgid ? (int)pgid : q->pid;
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

/*
 * 查詢行程的資訊(ps 用):找出「編號大於 after 的行程中編號最小的那一個」,
 * 把它的資訊寫到使用者的 uinfo。回傳 0 = 取得,1 = 沒有更多了。
 *
 * 為什麼一次只給一個、用「上一個的編號」接續:行程隨時在建立與結束,
 * kernel 不必為了列表而記住「上次列到哪裡」,也不必一次配置一大塊記憶體;
 * 呼叫者從 0 開始,每次傳入上一次得到的 pid 即可。列表期間新建立或結束的行程
 * 可能出現也可能不出現,但不會重複、也不會讓 kernel 出錯。
 */
static long sys_procinfo(struct proc *p, long after, uintptr_t uinfo)
{
    struct procinfo info;
    struct proc *best = NULL;
    unsigned runnable = 0;
    unsigned long ticks = 0;

    memset(&info, 0, sizeof(info));          /* 含欄位之間的空隙:不把 kernel 的資料帶出去 */
    unsigned long flags = sched_lock();
    for (struct proc *q = procs; q; q = q->next)
        if (q->pid > after && (!best || q->pid < best->pid))
            best = q;
    if (best) {
        sched_group_usage(best, &ticks, &runnable);
        info.pid = best->pid;
        info.ppid = best->ppid;
        info.pgid = best->pgid;
        info.nthreads = (int)best->nthreads;
        info.state = best->zombie ? PROC_ZOMBIE : best->stopped ? PROC_STOPPED :
                     runnable ? PROC_RUNNING : PROC_SLEEPING;
        info.cpu_ticks = (unsigned int)(best->cpu_ticks + ticks);
        /* 位址空間只在還有執行緒時才存在;exec 更換 p->space 時也拿著同一把鎖,
           所以這裡讀到的指標在解鎖之前不會被釋放 */
        if (best->nthreads > 0 && best->space)
            info.mem_kb = (unsigned int)(vm_space_pages(best->space) * (PAGE_SIZE / 1024));
        info.uid = best->cred.uid;
        memcpy(info.name, best->name, sizeof(info.name));
    }
    sched_unlock(flags);
    if (!best)
        return 1;
    if (copy_to_user(p, uinfo, &info, sizeof(info)) != 0)
        return -E_FAULT;
    return 0;
}

/* 整個系統的狀態(top 用) */
static long sys_sysinfo(struct proc *p, uintptr_t uinfo)
{
    struct sysinfo si;

    memset(&si, 0, sizeof(si));
    si.uptime_ticks = timer_ticks();
    si.hz = timer_hz();
    si.ncpu = (uint32_t)cpu_count;
    si.nthreads = sched_thread_count();
    si.mem_total_kb = (uint64_t)pmm_total_pages() * (PAGE_SIZE / 1024);
    si.mem_free_kb = (uint64_t)pmm_free_count() * (PAGE_SIZE / 1024);
    si.ctx_switches = sched_switch_count();
    /* 各 CPU 的計數由它們自己在計時器中斷裡累加;這裡不上鎖直接讀,差一兩個 tick 無妨 */
    for (int i = 0; i < cpu_count && i < SYSINFO_MAX_CPUS; i++) {
        si.cpu[i].busy = cpus[i].ticks_busy;
        si.cpu[i].idle = cpus[i].ticks_idle;
    }
    unsigned long flags = sched_lock();
    for (struct proc *q = procs; q; q = q->next)
        si.nprocs++;
    sched_unlock(flags);
    if (copy_to_user(p, uinfo, &si, sizeof(si)) != 0)
        return -E_FAULT;
    return 0;
}

int proc_kill(int pid, int code)
{
    unsigned long flags = sched_lock();
    struct proc *p = proc_find(pid);
    int rc = -1;

    if (p && !p->zombie) {
        proc_begin_exit(p, code);
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

void user_fault(const char *what, uintptr_t pc, uintptr_t addr)
{
    struct proc *p = thread_data();

    kprintf("[pid %d %s] killed: %s at pc=%p addr=%p\n", p ? p->pid : 0,
            p ? p->name : "?", what, (void *)pc, (void *)addr);
    blackbox_log(BB_FAULT, "pid %d %s: %s at pc=%p addr=%p", p ? p->pid : 0, p ? p->name : "?", what,
                 (void *)pc, (void *)addr);
    proc_exit(PROC_KILLED_FAULT);
    for (;;)
        ;                            /* 不會到達;讓編譯器知道本函式不返回 */
}

/*
 * ---- 訊號處理函式 ----
 *
 * 行程可以登記「收到某個訊號時,執行我的這個函式」。困難在於:訊號隨時可能來,
 * 程式當時正在做別的事,暫存器裡都是有用的資料。處理函式跑完之後,
 * 程式必須從被打斷的地方、以完全相同的暫存器內容繼續。
 *
 * kernel 沒有另外的地方保存這些暫存器,做法是借用「使用者自己的 stack」:
 *
 *   1. 交付(deliver_signal):執行緒要返回使用者模式時,把它當時的暫存器(trap frame)
 *      整份複製到使用者 stack 上,然後把 trap frame 改成「呼叫 handler(sig)」。
 *      返回使用者模式後,CPU 執行的就是處理函式,用的是同一條 stack 的下面一段
 *   2. 處理函式 return:返回位址被設成一小段使用者端的程式(restorer),它只做一件事 —
 *      呼叫 sigreturn 系統呼叫,並告訴 kernel 那份暫存器放在哪裡
 *   3. sigreturn:把那份暫存器讀回來,取代目前的 trap frame。返回使用者模式後,
 *      程式就從當初被打斷的地方繼續,完全不知道中間發生過什麼
 *
 *        使用者 stack(往下長)
 *        | 被打斷時正在用的部分 |
 *        +----------------------+ <- 被打斷時的 sp
 *        | 存下來的暫存器        |    <- kernel 寫入;sigreturn 時讀回
 *        +----------------------+ <- frame
 *        | 處理函式自己用的部分  |
 *
 * 安全:那份暫存器放在使用者的記憶體裡,程式可以任意改它(也可以隨便捏造一份來呼叫
 * sigreturn)。所以讀回來的時候,「只有 kernel 能決定的部分」— 特權等級、中斷開關 —
 * 一律不採用(arch_frame_restore)。程式能改的只有它本來就能改的一般暫存器,
 * 最壞的結果是它自己跳到奇怪的地方而被終止。
 *
 * 訊號屬於整個行程:哪一條執行緒先走到返回使用者模式的檢查點,就由它執行處理函式。
 */
static void deliver_signal(struct proc *p, void *tf)
{
    _Alignas(16) char saved[512];
    uintptr_t words[2], handler, restorer;
    int sig = 0;

    unsigned long flags = sched_lock();
    thread_clear_interrupt();
    for (int s = 1; s < NSIG && !sig; s++) {
        if (p->sig_pending & (1u << s)) {
            p->sig_pending &= ~(1u << s);
            sig = s;
        }
    }
    handler = sig ? p->sig_handler[sig] : SIG_DFL;
    restorer = p->sig_restorer;
    sched_unlock(flags);
    if (!sig || handler == SIG_DFL || handler == SIG_IGN)
        return;                                  /* 被別條執行緒拿走了,或處理方式剛被改掉 */

    size_t size = arch_frame_size();
    /* 存放位置:目前的 stack pointer 之下,留一點空隙(有些呼叫慣例允許程式使用
       stack pointer 以下的一小段),對齊 16 */
    uintptr_t frame = (arch_frame_user_sp(tf) - 128 - size) & ~(uintptr_t)15;
    memcpy(saved, tf, size);
    unsigned n = arch_frame_call(tf, frame, handler, (uintptr_t)sig, restorer, words);
    uintptr_t new_sp = arch_frame_user_sp(tf);
    if (size > sizeof(saved) || copy_to_user(p, frame, saved, size) != 0 ||
        (n && copy_to_user(p, new_sp, words, n * sizeof(words[0])) != 0)) {
        /* 使用者的 stack 壞了(或已經用完):沒有地方放,只能照預設的方式處理 — 終止 */
        proc_exit(128 + sig);
    }
}

/* sigreturn:處理函式結束,還原被打斷時的暫存器。回傳值會被放進「系統呼叫回傳值」的
   暫存器,所以回傳還原之後那個暫存器原本的值,等於沒有動到它 */
static long sys_sigreturn(struct proc *p, uintptr_t uframe, void *tf)
{
    _Alignas(16) char saved[512];
    size_t size = arch_frame_size();

    if (size > sizeof(saved) || copy_from_user(p, saved, uframe, size) != 0) {
        proc_exit(PROC_KILLED_FAULT);            /* 沒有可以還原的東西:無法繼續執行 */
        return 0;
    }
    arch_frame_restore(tf, saved);
    return arch_frame_get_retval(tf);
}

/* signal(sig, handler, restorer):設定處理方式,回傳原本的 */
static long sys_signal(struct proc *p, long sig, uintptr_t handler, uintptr_t restorer)
{
    if (sig <= 0 || sig >= NSIG || !sig_catchable(sig))
        return -E_INVAL;                         /* SIGKILL 與 SIGSTOP 不能改:永遠有辦法停下一個程式 */
    /* 處理函式必須是使用者可以執行的位址;restorer 也是 */
    if (handler != SIG_DFL && handler != SIG_IGN &&
        (vm_space_query(p->space, handler, NULL, NULL) != 0 ||
         vm_space_query(p->space, restorer, NULL, NULL) != 0))
        return -E_FAULT;

    unsigned long flags = sched_lock();
    long old = (long)p->sig_handler[sig];
    p->sig_handler[sig] = handler;
    if (handler != SIG_DFL && handler != SIG_IGN)
        p->sig_restorer = restorer;
    else
        p->sig_pending &= ~(1u << sig);          /* 不再由函式處理:還沒交付的就不算了 */
    sched_unlock(flags);
    return old;
}

void user_return_check(void *tf)
{
    struct proc *p = thread_data();

    if (!p)
        return;
    /* 行程被暫停了:睡到有人讓它繼續(或終止它)為止。
       先不上鎖看一眼:這條路徑每次返回使用者模式都會經過,絕大多數時候沒有被暫停。
       剛好錯過也無妨 — 最晚下一個計時器中斷又會經過這裡 */
    if (p->stopped && !thread_killed()) {
        unsigned long flags = sched_lock();
        while (p->stopped && !thread_killed())
            wq_sleep(&p->stop_wq);
        sched_unlock(flags);
    }
    if (thread_killed())
        proc_thread_exit();
    if (p->sig_pending)                          /* 不上鎖先看一眼,理由同上 */
        deliver_signal(p, tf);
    else if (thread_interrupted())
        thread_clear_interrupt();                /* 訊號已經由別條執行緒處理了 */
}

/* ------------------------------------------------------------------ */
/* 系統呼叫                                                            */
/*                                                                    */
/* 這是 kernel 的對外邊界:參數全部來自不可信任的使用者程式,            */
/* 每一個都要檢查。尤其是指標,使用者可以傳任何數值,包括 kernel 內部的  */
/* 位址;kernel 若不檢查就照著讀寫,等於替使用者突破了保護。            */
/* ------------------------------------------------------------------ */

/*
 * 處理一個「也許可以解決」的 page fault。回傳 0 表示已經解決,重試那次存取就會成功。
 * 能解決的有兩種:
 *
 * 1. 寫入時複製:寫入一個 fork 之後和別的行程共用的頁(見 vm_space_fault)。
 *
 * 2. stack 自動成長:每條執行緒的 stack 佔一格 1MB 的位址範圍,但一開始只配置
 *    最上面的 16KB。往下用到還沒配置的部分時,當場配置一頁(內容為 0)。
 *    大多數程式用不到 16KB,不必為它們預先配置;用得多的程式也不會因此被終止。
 *    每格最下面的一頁永遠不配置(保護頁):stack 真的用完整格時會在那裡出錯,
 *    而不是悄悄長進下一條執行緒的 stack。
 *
 * 其他情況(存取沒有對映的位址、寫入唯讀的頁)回傳 -1,呼叫者會終止行程。
 */
static int proc_fault(struct proc *p, uintptr_t addr, int write)
{
    if (write && vm_space_fault(p->space, addr, 1) == 0)
        return 0;

    /* heap:落在 [USER_HEAP_BASE, brk) 而還沒配置的頁,當場配置(內容為 0)。
       sbrk 只是移動結尾,不配置任何東西;程式要了很大一塊卻只用一點點時,只花用到的部分 */
    if (addr >= USER_HEAP_BASE && addr < USER_HEAP_BASE + USER_HEAP_MAX) {
        unsigned long fl = spin_lock_irqsave(&fd_lk);
        uintptr_t end = p->brk;
        spin_unlock_irqrestore(&fd_lk, fl);
        if (addr >= end)
            return -1;                           /* 超出目前的結尾:真正的錯誤 */
        void *hp = pmm_alloc();
        if (!hp)
            return -1;                           /* 記憶體用完:這個行程會被終止 */
        memset(hp, 0, PAGE_SIZE);
        int hrc = vm_space_map_new(p->space, addr & ~(uintptr_t)(PAGE_SIZE - 1), (uintptr_t)hp,
                                   VM_READ | VM_WRITE);
        if (hrc != 0)
            pmm_free(hp);
        return hrc < 0 ? -1 : 0;
    }

    /* 匿名對映:落在某一塊 mmap 的範圍內而還沒配置的頁,同樣當場配置 */
    if (addr >= USER_MMAP_BASE && addr < USER_MMAP_BASE + USER_MMAP_MAX) {
        int inside = 0;
        unsigned long fl = spin_lock_irqsave(&fd_lk);
        for (unsigned i = 0; i < PROC_MAX_MAPS && !inside; i++)
            inside = p->maps[i].pages && addr >= p->maps[i].start &&
                     addr - p->maps[i].start < p->maps[i].pages * PAGE_SIZE;
        spin_unlock_irqrestore(&fd_lk, fl);
        if (!inside)
            return -1;
        void *mp = pmm_alloc();
        if (!mp)
            return -1;
        memset(mp, 0, PAGE_SIZE);
        int mrc = vm_space_map_new(p->space, addr & ~(uintptr_t)(PAGE_SIZE - 1), (uintptr_t)mp,
                                   VM_READ | VM_WRITE);
        if (mrc != 0)
            pmm_free(mp);
        return mrc < 0 ? -1 : 0;
    }

    uintptr_t lowest = USER_STACK_TOP - (uintptr_t)PROC_MAX_THREADS * USER_STACK_SLOT;
    if (addr < lowest || addr >= USER_STACK_TOP)
        return -1;
    unsigned slot = (unsigned)((USER_STACK_TOP - 1 - addr) / USER_STACK_SLOT);
    uintptr_t slot_base = USER_STACK_TOP - (uintptr_t)(slot + 1) * USER_STACK_SLOT;
    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    if (slot >= p->nstacks || page == slot_base)     /* 沒有這條 stack,或碰到保護頁 */
        return -1;

    void *mem = pmm_alloc();
    if (!mem)
        return -1;
    memset(mem, 0, PAGE_SIZE);
    int rc = vm_space_map_new(p->space, page, (uintptr_t)mem, VM_READ | VM_WRITE);
    if (rc != 0)
        pmm_free(mem);       /* 1 = 同一行程的另一條執行緒搶先配置了同一頁:用它的 */
    return rc < 0 ? -1 : 0;
}

int proc_page_fault(uintptr_t addr, int write)
{
    struct proc *p = thread_data();

    return p ? proc_fault(p, addr, write) : -1;
}

/*
 * 把使用者位址 [uaddr, uaddr+len) 的內容複製到 kernel 緩衝區 dst。
 * 逐頁透過行程的頁表查出實體位址,並確認該頁是使用者可存取的;
 * 任何一頁不合格就回傳 -1。不直接解參考使用者的指標。
 */
static int copy_from_user(struct proc *p, void *dst, uintptr_t uaddr, size_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uintptr_t pa;
        /* 查不到對映時,先看是不是「用到才配置」的頁(還沒碰過的 stack);
           是的話配置後再查一次 */
        if (vm_space_query(p->space, uaddr, &pa, NULL) != 0 &&
            (proc_fault(p, uaddr, 0) != 0 || vm_space_query(p->space, uaddr, &pa, NULL) != 0))
            return -1;
        size_t n = PAGE_SIZE - (uaddr & (PAGE_SIZE - 1));    /* 這一頁剩下的長度 */
        if (n > len)
            n = len;
        memcpy(out, (const void *)pa, n);
        out += n;
        uaddr += n;
        len -= n;
    }
    return 0;
}

/* 反方向:把 kernel 的資料複製到使用者位址。除了要是使用者的頁,還必須是「可寫」的頁,
   否則使用者可以請 kernel 代為寫入它自己不能寫的地方(例如它的程式碼) */
static int copy_to_user(struct proc *p, uintptr_t uaddr, const void *src, size_t len)
{
    const uint8_t *in = src;

    while (len > 0) {
        uintptr_t pa;
        unsigned flags;
        if (vm_space_query(p->space, uaddr, &pa, &flags) != 0 || !(flags & VM_WRITE)) {
            /* kernel 是經由實體位址寫入的,不會像使用者程式那樣觸發 page fault,
               所以要自己做同樣的事:這一頁若是寫入時複製的(或還沒配置的 stack),
               先處理好再寫。否則會寫進和別的行程共用的那一頁 */
            if (proc_fault(p, uaddr, 1) != 0 ||
                vm_space_query(p->space, uaddr, &pa, &flags) != 0 || !(flags & VM_WRITE))
                return -1;
        }
        size_t n = PAGE_SIZE - (uaddr & (PAGE_SIZE - 1));
        if (n > len)
            n = len;
        memcpy((void *)pa, in, n);
        in += n;
        uaddr += n;
        len -= n;
    }
    return 0;
}

/* 從使用者位址複製一個以 0 結尾的字串(路徑)到 kernel。太長或位址不合法回傳負的錯誤碼 */
static int copy_path_from_user(struct proc *p, char *dst, uintptr_t uaddr)
{
    for (size_t i = 0; i <= FS_PATH_MAX; i++) {
        if (copy_from_user(p, &dst[i], uaddr + i, 1) != 0)
            return -E_FAULT;
        if (dst[i] == '\0')
            return 0;
    }
    return -E_NAMETOOLONG;
}

/*
 * ---- 目前的工作目錄 ----
 *
 * 每個行程記著一個目錄;使用者傳來的路徑如果不是以 "/" 開頭(相對路徑),就接在它後面。
 * 這件事完全在系統呼叫這一層做:把相對路徑接成絕對路徑,再交給 VFS。
 * VFS 仍然只認得從根目錄開始的路徑,不必知道「行程」是什麼。
 *
 * 工作目錄以「字串」保存,而不是抓著那個目錄的 inode:
 *   - 實作簡單,fork / spawn 時直接複製
 *   - 目錄可以照常被刪除(之後用相對路徑會得到「不存在」),不會有刪不掉的目錄
 * 代價:目錄被改名之後,行程記著的還是舊的名稱。
 */

/* 把絕對路徑整理成標準的樣子:拿掉重複的 "/"、"." 與 ".."(".." 往上一層;根目錄的上層還是根目錄) */
static void path_normalize(char *path)
{
    size_t out = 0, in = 0;

    while (path[in]) {
        while (path[in] == '/')
            in++;
        size_t start = in;
        while (path[in] && path[in] != '/')
            in++;
        size_t n = in - start;
        if (n == 0 || (n == 1 && path[start] == '.'))
            continue;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (out > 0 && path[--out] != '/')
                ;                                /* 退掉最後一段 */
            continue;
        }
        path[out++] = '/';                       /* out 永遠不超過 start:就地整理不會蓋到還沒讀的部分 */
        memmove(path + out, path + start, n);
        out += n;
    }
    if (out == 0)
        path[out++] = '/';
    path[out] = '\0';
}

/* 從使用者位址取得路徑;相對路徑接在行程的工作目錄後面。dst 至少 FS_PATH_MAX + 1 */
static int user_path(struct proc *p, char *dst, uintptr_t uaddr)
{
    char rel[FS_PATH_MAX + 1];
    int err = copy_path_from_user(p, rel, uaddr);

    if (err)
        return err;
    if (rel[0] == '\0')
        return -E_NOENT;                         /* 空的路徑不代表任何東西 */
    if (rel[0] == '/') {
        memcpy(dst, rel, strlen(rel) + 1);
        return 0;
    }
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    memcpy(dst, p->cwd, sizeof(p->cwd));
    spin_unlock_irqrestore(&fd_lk, fl);
    size_t n = strlen(dst), m = strlen(rel);
    if (n + 1 + m > FS_PATH_MAX)
        return -E_NAMETOOLONG;
    if (n > 1)
        dst[n++] = '/';
    memcpy(dst + n, rel, m + 1);
    return 0;
}

static long sys_chdir(struct proc *p, uintptr_t upath)
{
    char path[FS_PATH_MAX + 1];
    struct stat st;
    int err = user_path(p, path, upath);

    if (err == 0)
        err = vfs_stat(path, &st);               /* 先確認它存在(用還沒整理的路徑:".." 由 VFS 實際去走) */
    if (err)
        return err;
    if (st.type != I_DIR)
        return -E_NOTDIR;
    err = vfs_access(path, PERM_X);              /* 要能「經過」這個目錄才可以把它當成工作目錄 */
    if (err)
        return err;
    path_normalize(path);
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    memcpy(p->cwd, path, strlen(path) + 1);
    spin_unlock_irqrestore(&fd_lk, fl);
    return 0;
}

static long sys_getcwd(struct proc *p, uintptr_t ubuf, size_t len)
{
    char path[FS_PATH_MAX + 1];

    unsigned long fl = spin_lock_irqsave(&fd_lk);
    memcpy(path, p->cwd, sizeof(path));
    spin_unlock_irqrestore(&fd_lk, fl);
    size_t n = strlen(path);
    if (n + 1 > len)
        return -E_INVAL;                         /* 緩衝區放不下 */
    if (copy_to_user(p, ubuf, path, n + 1) != 0)
        return -E_FAULT;
    return (long)n;
}

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

/*
 * ---- 記憶體配額:答應了就一定給得出來 ----
 *
 * heap 與匿名對映的頁是「用到才配置」的。如果 kernel 對每個要求都說好,等到程式真的去用、
 * 實體記憶體卻不夠時,只剩下一個選擇:終止某個行程(第 30 步的做法,稱為過度承諾)。
 * 被終止的程式沒有做錯任何事,也沒有機會收拾;對要求可靠的系統,這是不能接受的行為。
 *
 * 這裡改成記帳:kernel 記著「已經答應出去的頁數」(commit),包括還沒真的配置的。
 * 每個要求在當下就決定成不成功:
 *   - 行程自己的配額(p->limit):一個行程失控,最多用掉它自己的額度
 *   - 整個系統的上限(commit_cap):答應出去的總量不超過實際有的記憶體,
 *     而且扣掉一塊保留給 kernel 自己(頁表、kernel heap、檔案快取…)
 * 超過就回傳 -E_NOMEM:程式在「要」的時候得到明確的答案,可以改做別的事、
 * 釋放其他東西、或把工作有秩序地結束。之後去使用那些已經答應的頁時,不會再失敗。
 *
 * 配額只能調低(memlimit);調高需要 CAP_ADMIN。子行程沿用父行程的配額。
 * fork 會讓子行程得到同樣多的承諾(寫入時複製:最壞情況下每一頁都要另外一份),
 * 所以 fork 也可能因為配額而失敗。
 */
static unsigned long commit_total;           /* 所有行程的 commit 總和;受 fd_lk 保護 */
static unsigned long commit_cap;             /* 上限;0 = 還沒算(第一次使用時依當時的空閒記憶體決定) */

/* 把 p 的承諾增減 delta 頁。增加時檢查兩個上限,不行就回傳 -E_NOMEM(什麼都不變)。
   呼叫時必須持有 fd_lk */
static int commit_change_locked(struct proc *p, long delta)
{
    if (!commit_cap) {
        /* 保留給 kernel 的部分:至少 2MB,或全部記憶體的 1/16(取大的) */
        unsigned long freep = pmm_free_count(), reserve = pmm_total_pages() / 16;
        if (reserve < 512)
            reserve = 512;
        commit_cap = freep > reserve ? freep - reserve : 1;
    }
    if (delta > 0) {
        unsigned long d = (unsigned long)delta;
        if (d > p->limit || p->commit > p->limit - d ||
            d > commit_cap || commit_total > commit_cap - d)
            return -E_NOMEM;
        p->commit += d;
        commit_total += d;
    } else {
        unsigned long d = (unsigned long)-delta;
        if (d > p->commit)
            d = p->commit;
        p->commit -= d;
        commit_total -= d;
    }
    return 0;
}

/* 行程結束或換成另一個程式:它的承諾全部歸還 */
static void commit_release_all(struct proc *p)
{
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    commit_change_locked(p, -(long)p->commit);
    spin_unlock_irqrestore(&fd_lk, fl);
}

static unsigned long pages_for(uintptr_t bytes)
{
    return (unsigned long)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* memlimit(kb):kb 為 0 = 查詢;否則把配額設成 kb。調低永遠允許,調高需要 CAP_ADMIN。回傳設定後的配額(KB) */
static long sys_memlimit(struct proc *p, long kb)
{
    if (kb < 0)
        return -E_INVAL;
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    long rc = 0;
    if (kb > 0) {
        unsigned long pages = (unsigned long)kb / (PAGE_SIZE / 1024);
        if (pages > p->limit && !(p->cred.caps & CAP_ADMIN))
            rc = -E_PERM;
        else
            p->limit = pages;
    }
    if (rc == 0)
        rc = (long)(p->limit * (PAGE_SIZE / 1024));
    spin_unlock_irqrestore(&fd_lk, fl);
    return rc;
}

/*
 * mmap(len):要一塊 len 位元組(進位到整頁)、內容為 0 的記憶體,回傳它的位址。
 * 和 heap 的差別:每一塊各自獨立,可以個別歸還(munmap),不必是「最後要的那一塊」。
 * 頁同樣是用到才配置;承諾在這裡就記帳。只支援「匿名」的對映(不對應任何檔案)。
 * 失敗回傳 -E_INVAL(長度為 0 或太大)、-E_NOMEM(超過配額、塊數已滿、位址範圍用完)。
 */
static long sys_mmap(struct proc *p, uintptr_t len)
{
    if (len == 0 || len > USER_MMAP_MAX)
        return -E_INVAL;
    unsigned long pages = pages_for(len);
    long rc = -E_NOMEM;

    unsigned long fl = spin_lock_irqsave(&fd_lk);
    uintptr_t start = p->mmap_next ? p->mmap_next : USER_MMAP_BASE;
    for (unsigned i = 0; i < PROC_MAX_MAPS; i++) {
        if (p->maps[i].pages)
            continue;
        if (start + pages * PAGE_SIZE <= USER_MMAP_BASE + USER_MMAP_MAX &&
            commit_change_locked(p, (long)pages) == 0) {
            p->maps[i].start = start;
            p->maps[i].pages = pages;
            p->mmap_next = start + pages * PAGE_SIZE + PAGE_SIZE;   /* 兩塊之間留一頁不對映:越界會立刻被發現 */
            rc = (long)start;
        }
        break;
    }
    spin_unlock_irqrestore(&fd_lk, fl);
    return rc;
}

/* munmap(addr):歸還 mmap 得到的一整塊(addr 必須是 mmap 回傳的位址) */
static long sys_munmap(struct proc *p, uintptr_t addr)
{
    unsigned long pages = 0;

    unsigned long fl = spin_lock_irqsave(&fd_lk);
    for (unsigned i = 0; i < PROC_MAX_MAPS; i++) {
        if (p->maps[i].pages && p->maps[i].start == addr) {
            pages = p->maps[i].pages;
            p->maps[i].pages = 0;            /* 先從表格拿掉:之後碰這一塊就是錯誤,不會再被配置 */
            commit_change_locked(p, -(long)pages);
            break;
        }
    }
    spin_unlock_irqrestore(&fd_lk, fl);
    if (!pages)
        return -E_INVAL;
    for (unsigned long i = 0; i < pages; i++)
        vm_space_unmap(p->space, addr + i * PAGE_SIZE);
    return 0;
}

/*
 * sbrk(incr):把 heap 的結尾移動 incr 個位元組(可以是負的),回傳「移動之前」的結尾。
 * 使用者端的 malloc 用它向 kernel 要記憶體。
 *   變大:只是把結尾往後移。新的範圍在第一次被碰到時才配置(見 proc_fault),內容保證是 0
 *   變小:結尾之後整頁落在範圍外的,立刻歸還給系統
 * 失敗(超出配額或系統的上限、或縮到起點之前)回傳 -E_NOMEM,結尾不變。
 */
static long sys_sbrk(struct proc *p, long incr)
{
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    uintptr_t old = p->brk ? p->brk : USER_HEAP_BASE;
    uintptr_t now = old + (uintptr_t)incr;
    int ok = incr >= 0 ? (uintptr_t)incr <= USER_HEAP_MAX && now <= USER_HEAP_BASE + USER_HEAP_MAX
                       : (uintptr_t)-incr <= old - USER_HEAP_BASE;
    /* 記帳:heap 佔用的頁數改變多少,就增減多少承諾。超過配額的話整個要求失敗 */
    if (ok && commit_change_locked(p, (long)pages_for(now - USER_HEAP_BASE) -
                                      (long)pages_for(old - USER_HEAP_BASE)) != 0)
        ok = 0;
    if (ok)
        p->brk = now;
    spin_unlock_irqrestore(&fd_lk, fl);
    if (!ok)
        return -E_NOMEM;

    /* 縮小:釋放不再屬於 heap 的整頁(第一個「完全在新結尾之後」的頁起) */
    uintptr_t first = (now + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    for (uintptr_t va = first; va < old; va += PAGE_SIZE)
        vm_space_unmap(p->space, va);
    return (long)old;
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

    if (!f)
        return -E_BADF;
    vfs_close(f);
    return 0;
}

/* dup2:讓 newfd 指向和 oldfd 同一次開啟。newfd 原本若開著,先關閉。
   重新導向靠它:把檔案「接到」1 號描述符上,之後再接回原本的 */
static long sys_dup2(struct proc *p, long oldfd, long newfd)
{
    if (newfd < 0 || newfd >= (long)PROC_MAX_FD)
        return -E_BADF;
    struct file *f = fd_get(p, oldfd);           /* 多拿一個參考,交給 newfd */
    if (!f)
        return -E_BADF;
    if (oldfd == newfd) {
        vfs_close(f);
        return newfd;
    }
    unsigned long fl = spin_lock_irqsave(&fd_lk);
    struct file *old = p->fd[newfd];
    p->fd[newfd] = f;
    spin_unlock_irqrestore(&fd_lk, fl);
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
    long rc = vfs_readdir(f, &d);
    vfs_close(f);
    if (rc == 0 && copy_to_user(p, uent, &d, sizeof(d)) != 0)
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

/* 從使用者位址讀入參數陣列(argv):一個以 NULL 結尾的指標陣列,每個指標指向一個字串。
   指標與字串都來自使用者,每一個都要檢查;數量與總長度有上限 */
static int args_from_user(struct proc *p, struct args *a, uintptr_t uargv)
{
    char str[FS_PATH_MAX + 1];

    if (!uargv)
        return 0;
    for (unsigned i = 0; i <= ARG_MAX_COUNT; i++) {
        uintptr_t ustr;
        if (copy_from_user(p, &ustr, uargv + i * sizeof(uintptr_t), sizeof(ustr)) != 0)
            return -E_FAULT;
        if (!ustr)
            return 0;
        int err = copy_path_from_user(p, str, ustr);
        if (err == 0)
            err = args_add(a, str);
        if (err)
            return err;
    }
    return -E_INVAL;                 /* 太多個參數 */
}

/* spawn 與 exec 共用的前半段:把路徑與參數從使用者位址搬進 kernel */
static int get_path_and_args(struct proc *p, uintptr_t upath, uintptr_t uargv,
                             char *path, struct args **out)
{
    int err = user_path(p, path, upath);
    if (err)
        return err;
    struct args *a = kzalloc(sizeof(*a));
    if (!a)
        return -E_NOMEM;
    err = args_from_user(p, a, uargv);
    if (err == 0 && a->argc == 0)
        err = args_add(a, path);     /* 沒有給參數:argv[0] 用路徑 */
    if (err) {
        kfree(a);
        return err;
    }
    *out = a;
    return 0;
}

/* 建立一個新行程執行 path。回傳 pid 或負的錯誤碼 */
static long sys_spawn(struct proc *p, uintptr_t upath, uintptr_t uargv)
{
    char path[FS_PATH_MAX + 1];
    struct args *a;
    int err = get_path_and_args(p, upath, uargv, path, &a);

    if (err == 0 && (err = vfs_access(path, PERM_X)) != 0)
        kfree(a);                    /* 沒有執行這個檔案的權限 */
    if (err)
        return err;
    long pid = proc_create(path, a, p);
    kfree(a);
    return pid;
}

/*
 * exec:目前的行程改為執行另一個程式。行程編號與開啟的檔案不變,
 * 記憶體內容(程式、資料、stack)全部換新。成功的話不會返回 —
 * 原本的程式已經不存在了,下一條被執行的指令是新程式的進入點。
 *
 * 做法:先把新的程式完整準備好(另一個位址空間),成功了才換過去並丟掉舊的。
 * 任何一步失敗都只是回傳錯誤,原本的程式照常繼續。
 */
static long sys_exec(struct proc *p, uintptr_t upath, uintptr_t uargv)
{
    char path[FS_PATH_MAX + 1];
    struct args *a;
    struct image img;
    int err = get_path_and_args(p, upath, uargv, path, &a);

    if (err)
        return err;

    /* 只有一條執行緒時才允許:其他執行緒正在舊的位址空間裡執行,
       把它換掉之前得先讓它們全部結束,目前不做這件事 */
    unsigned long flags = sched_lock();
    int single = p->nthreads == 1;
    sched_unlock(flags);
    if (!single) {
        kfree(a);
        return -E_BUSY;
    }

    err = vfs_access(path, PERM_X);  /* 要有執行這個檔案的權限 */
    if (err == 0)
        err = image_build(path, a, &img);
    kfree(a);
    if (err)
        return err;                  /* 舊程式完全沒被動到 */

    /* ---- 從這裡起不會失敗,也不會回到舊程式 ---- */
    struct vm_space *old_space = p->space;
    struct file *old_exe = p->exe;

    proc_set_name(p, path);
    flags = sched_lock();            /* 別的行程可能正透過 procinfo 讀取 p->space(見 sys_procinfo) */
    p->space = img.space;
    sched_unlock(flags);
    p->brk = 0;                      /* 新的程式從空的 heap 開始(舊的頁隨舊位址空間釋放) */
    commit_release_all(p);           /* 舊程式的 heap 與匿名對映都不在了 */
    memset(p->maps, 0, sizeof(p->maps));
    p->mmap_next = 0;
    flags = sched_lock();            /* 處理函式是舊程式裡的位址:全部還原成預設(「忽略」則保留) */
    for (int s = 0; s < NSIG; s++)
        if (p->sig_handler[s] != SIG_IGN)
            p->sig_handler[s] = SIG_DFL;
    p->sig_pending = 0;
    sched_unlock(flags);
    p->exe = img.exe;
    p->nstacks = 1;
    thread_set_space(img.space);     /* 換到新的位址空間;舊的已經沒有人使用 */
    vm_space_destroy(old_space);
    vfs_exec_release(old_exe);
    vfs_close(old_exe);

    arch_enter_user(img.entry, img.usp, img.argp);
}

/* fork 出來的子行程的第一條(也是唯一一條)執行緒在 kernel 這一側的起點 */
struct fork_start {
    struct proc *p;
    void *frame;                     /* 父行程呼叫 fork 時的暫存器(回傳值已改成 0) */
};

static void fork_child_main(void *arg)
{
    struct fork_start st = *(struct fork_start *)arg;
    /* trap frame 要放在自己的 kernel stack 上才能用來返回(見 arch_user_resume)。
       大小依架構而定,這裡取一個夠大的上限 */
    _Alignas(16) char frame[512];

    kfree(arg);
    thread_set_data(st.p);
    thread_set_space(st.p->space);
    memcpy(frame, st.frame, arch_frame_size());
    kfree(st.frame);

    unsigned long flags = sched_lock();      /* 和 user_thread_main 相同的檢查 */
    int dying = st.p->exiting;
    sched_unlock(flags);
    if (dying || thread_killed())
        proc_thread_exit();

    /* 從父行程呼叫 fork 的那個地方「返回」,只是回傳值是 0 */
    arch_user_resume(frame);
}

/*
 * fork:複製目前的行程。新行程(子行程)和原本的(父行程)在這一刻完全相同 —
 * 同樣的記憶體內容、同樣的開啟中檔案、執行到同一個地方 — 唯一的差別是
 * fork 的回傳值:父行程得到子行程的編號,子行程得到 0。
 *
 * 記憶體不是真的複製:兩邊先共用所有的頁,誰寫入才複製那一頁(寫入時複製)。
 * 所以 fork 的成本只和「頁表有多大」有關,和行程用了多少記憶體無關;
 * fork 之後馬上 exec 的常見用法,幾乎沒有複製任何資料。
 */
static long sys_fork(struct proc *p, void *tf)
{
    if (arch_frame_size() > 512)
        return -E_INVAL;

    /* 只有一條執行緒時才允許:其他執行緒可能正在寫入即將被改成唯讀的頁,
       而且子行程裡只會有呼叫 fork 的這一條,其他執行緒持有的鎖在那邊永遠解不開 */
    unsigned long flags = sched_lock();
    int single = p->nthreads == 1;
    sched_unlock(flags);
    if (!single)
        return -E_BUSY;

    struct proc *c = kzalloc(sizeof(*c));
    struct fork_start *st = kmalloc(sizeof(*st));
    void *frame = kmalloc(arch_frame_size());
    if (!c || !st || !frame)
        goto nomem;
    /* 子行程得到和父行程一樣多的承諾(最壞情況下,共用的每一頁都要各自一份):先記帳,不夠就不 fork */
    c->limit = p->limit;
    {
        unsigned long fl = spin_lock_irqsave(&fd_lk);
        int crc = commit_change_locked(c, (long)p->commit);
        memcpy(c->maps, p->maps, sizeof(c->maps));
        c->mmap_next = p->mmap_next;
        spin_unlock_irqrestore(&fd_lk, fl);
        if (crc != 0)
            goto nomem;
    }
    c->space = vm_space_fork(p->space);
    if (!c->space) {
        commit_release_all(c);
        goto nomem;
    }

    memcpy(frame, tf, arch_frame_size());
    arch_frame_set_retval(frame, 0);         /* 子行程看到的回傳值 */
    st->p = c;
    st->frame = frame;

    memcpy(c->name, p->name, sizeof(c->name));
    c->cred = p->cred;
    memcpy(c->cwd, p->cwd, sizeof(c->cwd));  /* 只有一條執行緒(上面檢查過),沒有人同時在改 */
    c->exe = vfs_dup(p->exe);
    vfs_exec_hold(c->exe);                   /* 子行程也在執行同一個程式檔 */
    c->nstacks = p->nstacks;
    c->brk = p->brk;                         /* heap 的內容已經隨位址空間複製(寫入時複製) */
    memcpy(c->sig_handler, p->sig_handler, sizeof(c->sig_handler));   /* 處理方式沿用;還沒交付的不帶過去 */
    c->sig_restorer = p->sig_restorer;
    fd_inherit(c, p, PROC_MAX_FD);

    flags = sched_lock();
    int pid = c->pid = next_pid++;
    c->ppid = p->pid;
    c->pgid = p->pgid;
    c->next = procs;
    procs = c;
    c->nthreads = 1;
    sched_unlock(flags);

    if (thread_create(c->name, fork_child_main, st) < 0) {
        /* 沒有任何執行緒:直接當作已結束,讓 wait 回收 */
        kfree(st);
        kfree(frame);
        vm_space_destroy(c->space);
        commit_release_all(c);
        vfs_exec_release(c->exe);
        vfs_close(c->exe);
        fd_close_all(c);
        flags = sched_lock();
        c->space = NULL;
        c->nthreads = 0;
        c->exit_code = PROC_KILLED_FAULT;
        struct proc *dead = proc_became_zombie(c);
        sched_unlock(flags);
        proc_free_list(dead);
    }
    return pid;

nomem:
    kfree(c);
    kfree(st);
    kfree(frame);
    return -E_NOMEM;
}

/*
 * futex(fast userspace mutex)的 kernel 這一半。
 *
 * 想法:鎖本身只是使用者記憶體裡的一個整數。沒有競爭時,上鎖與解鎖只是
 * 使用者模式下的一條原子指令,完全不必進 kernel,所以很快。只有在
 * 「拿不到鎖,需要睡覺」或「解鎖時有人在等,需要叫醒」這兩種情況才呼叫 kernel。
 *
 * futex_wait(addr, val):「如果 *addr 仍然等於 val,就讓我睡」。
 *   檢查與睡下去必須不可分割,否則會發生 lost wakeup(見 DEVLOG 第 11 步):
 *   使用者程式剛看到鎖被佔用、還沒睡下去,對方就解鎖並喚醒(此時沒人在睡),
 *   之後才睡下去就再也沒人叫了。把「再確認一次數值」交給 kernel 在持有排程器的鎖時做,
 *   而喚醒的一方也要先拿到同一把鎖才能叫人,就不會有這個空檔:數值若已經變了,代表情況已改變,直接返回讓它重試。
 */
static struct wait_queue *futex_bucket(struct proc *p, uintptr_t uaddr)
{
    return &p->futex[(uaddr >> 2) % FUTEX_BUCKETS];
}

static long sys_futex_wait(struct proc *p, uintptr_t uaddr, int val)
{
    uintptr_t pa;
    long rc;

    if (uaddr & 3)
        return -1;
    /* 位址轉換在鎖外做(它要用頁表的鎖)。行程的頁在行程結束前不會被移除,
       而呼叫者正是這個行程的執行緒,所以查到的實體位址在下面使用時仍然有效 */
    if (vm_space_query(p->space, uaddr, &pa, NULL) != 0)
        return -1;                           /* 不是使用者可存取的位址 */

    unsigned long flags = sched_lock();
    if (*(volatile int *)pa != val || p->exiting) {
        rc = 1;                              /* 數值已經變了:不睡,讓使用者程式重新判斷 */
    } else {
        wq_sleep(futex_bucket(p, uaddr));
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

static long sys_futex_wake(struct proc *p, uintptr_t uaddr)
{
    /* 同一桶裡可能有等別的位址的人,一併被叫醒也無妨:它們會重新檢查自己的條件 */
    unsigned long flags = sched_lock();
    long n = wq_wake_all(futex_bucket(p, uaddr));
    sched_preempt_point();
    sched_unlock(flags);
    return n;
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
        return (long)p->cred.uid;
    case SYS_getgid:
        return (long)p->cred.gid;
    case SYS_capget:
        return (long)p->cred.caps;
    case SYS_setuid: {
        /*
         * 改變自己的身分。有 CAP_SETID 的行程可以改成任何人;其他行程只能「改成自己」(沒有作用)。
         * 改成非 root 時,所有特權一併清除,而且拿不回來:特權只會減少。
         * 這是整個權限模型的關鍵 — 沒有任何系統呼叫可以讓行程得到它原本沒有的特權,
         * 所以不需要密碼這類東西來把關;代價是「切換成另一個使用者再切回來」必須靠
         * 保留一個還有特權的父行程(su 就是這樣做的)。
         */
        if (a0 < 0 || a0 > 0xFFFF || a1 < 0 || a1 > 0xFFFF)
            return -E_INVAL;
        unsigned long flags = sched_lock();
        long rc = 0;
        if (p->cred.caps & CAP_SETID) {
            p->cred.uid = (uint32_t)a0;
            p->cred.gid = (uint32_t)a1;
            if (a0 != 0)
                p->cred.caps = 0;
        } else if ((uint32_t)a0 != p->cred.uid || (uint32_t)a1 != p->cred.gid) {
            rc = -E_PERM;
        }
        sched_unlock(flags);
        return rc;
    }
    case SYS_capdrop: {
        /* 永久放棄指定的特權(最小權限:只留下真正需要的)。回傳剩下的 */
        unsigned long flags = sched_lock();
        p->cred.caps &= ~(uint32_t)a0;
        long rc = (long)p->cred.caps;
        sched_unlock(flags);
        return rc;
    }
    case SYS_sync:
        return vfs_sync();
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
        if (!(p->cred.caps & CAP_ADMIN))
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
        if (!(p->cred.caps & CAP_ADMIN))
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
        return p->pid;
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
        if (vm_space_query(p->space, (uintptr_t)a0, NULL, NULL) != 0)
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
        console_set_fg((int)a0, p->pgid);
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
