/*
 * proc.c - 行程管理與系統呼叫(與架構無關)
 */
#include <stddef.h>
#include "arch.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
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

#define FUTEX_BUCKETS 16u
#define PROC_MAX_FD   16u            /* 每個行程最多同時開啟幾個檔案 */

struct proc {
    int pid;
    int ppid;                        /* 建立它的行程(父行程);0 = 由 kernel 建立,或父行程已經結束 */
    int pgid;                        /* 行程群組:shell 把同一條管線的行程放在同一組,
                                        Ctrl-C 一次終止整組(見 console.c) */
    int detached;                    /* 父行程已經結束:沒有人會來 wait,結束時自動回收 */
    char name[32];                   /* 程式檔的名稱(路徑的最後一段),訊息中使用 */
    struct vm_space *space;
    struct file *exe;                /* 正在執行的程式檔。一直開著:它的頁直接對映在行程裡 */
    unsigned nthreads;               /* 還活著的執行緒數 */
    unsigned nstacks;                /* 已分配的 stack 格數(不重複使用) */
    int exiting;                     /* 已開始結束流程 */
    int exit_code;
    int zombie;                      /* 所有執行緒都結束了,等待 proc_wait 來收 */
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
static int copy_to_user(struct proc *p, uintptr_t uaddr, const void *src, size_t len);

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
        if (thread_killed()) {
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
 * 這裡的訊號只有固定的效果,行程不能攔截或忽略(沒有訊號處理函式):
 *
 *   終止   SIGINT  (2)   使用者按了 Ctrl-C
 *          SIGKILL (9)   強制終止
 *          SIGTERM (15)  一般的「請結束」(kill 指令的預設)
 *   暫停   SIGSTOP (19)  暫停
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
    return sig == SIGINT || sig == SIGKILL || sig == SIGTERM ||
           sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN;
}

/* 對一個行程送出訊號。呼叫時必須持有排程器的鎖;q 不可以是 zombie */
static void proc_signal_locked(struct proc *q, int sig)
{
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

/* kill(pid, sig):pid > 0 是單一行程;pid < 0 是整個群組(編號為 -pid) */
static long sys_kill(long pid, long sig)
{
    long rc = -E_SRCH;

    if (!sig_valid(sig) || pid == 0)
        return -E_INVAL;
    unsigned long flags = sched_lock();
    if (pid < 0) {
        if (proc_signal_group_locked((int)-pid, (int)sig) > 0)
            rc = 0;
    } else {
        struct proc *q = proc_find((int)pid);
        if (q && !q->zombie) {
            proc_signal_locked(q, (int)sig);
            rc = 0;
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
    proc_exit(PROC_KILLED_FAULT);
    for (;;)
        ;                            /* 不會到達;讓編譯器知道本函式不返回 */
}

void user_return_check(void)
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
    int err = copy_path_from_user(p, path, upath);

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
    int err = copy_path_from_user(p, path, upath);

    if (err)
        return err;
    if (num == SYS_statfs) {
        struct fsinfo fi;
        err = vfs_statfs(path, &fi);
        if (err == 0 && copy_to_user(p, uarg, &fi, sizeof(fi)) != 0)
            err = -E_FAULT;
        return err;
    }
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
    int err = copy_path_from_user(p, path, upath);
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
    c->space = vm_space_fork(p->space);
    if (!c->space)
        goto nomem;

    memcpy(frame, tf, arch_frame_size());
    arch_frame_set_retval(frame, 0);         /* 子行程看到的回傳值 */
    st->p = c;
    st->frame = frame;

    memcpy(c->name, p->name, sizeof(c->name));
    c->exe = vfs_dup(p->exe);
    vfs_exec_hold(c->exe);                   /* 子行程也在執行同一個程式檔 */
    c->nstacks = p->nstacks;
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
        return sys_path_op(p, num, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_sync:
        return vfs_sync();
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
        return sys_kill(a0, a1);
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
