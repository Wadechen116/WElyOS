/*
 * proc.c - 行程管理與系統呼叫(與架構無關)
 */
#include <stddef.h>
#include "arch.h"
#include "console.h"
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
    char name[32];                   /* 程式檔的名稱(路徑的最後一段),訊息中使用 */
    struct vm_space *space;
    struct file *exe;                /* 正在執行的程式檔。一直開著:它的頁直接對映在行程裡 */
    unsigned nthreads;               /* 還活著的執行緒數 */
    unsigned nstacks;                /* 已分配的 stack 格數(不重複使用) */
    int exiting;                     /* 已開始結束流程 */
    int exit_code;
    int zombie;                      /* 所有執行緒都結束了,等待 proc_wait 來收 */
    struct wait_queue waiters;       /* 正在 proc_wait 的執行緒 */
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
static struct proc *procs;           /* 所有行程(含 zombie) */
static int next_pid = 1;

/* 新執行緒進入使用者模式所需的資訊,由建立者交給新執行緒 */
struct ustart {
    struct proc *p;
    uintptr_t entry, usp, arg;
};

static void fd_close_all(struct proc *p);

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
    sched_unlock(flags);

    if (last) {
        /* 自己是最後一條:已經沒有執行緒屬於這個位址空間,可以在鎖外釋放 */
        vm_space_destroy(p->space);
        vfs_exec_release(p->exe);        /* 對映已經移除:程式檔不再「執行中」 */
        vfs_close(p->exe);
        fd_close_all(p);                 /* 行程結束時,它開啟的檔案由 kernel 代為關閉 */

        flags = sched_lock();
        p->space = NULL;
        p->zombie = 1;
        wq_wake_all(&p->waiters);
        sched_unlock(flags);
        /* 解鎖之後 proc_wait 隨時可能把 p 釋放,不可再存取 */
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
    /* 睡在 futex 上的執行緒不會自己醒來:叫醒它們,它們返回使用者模式前就會結束 */
    for (unsigned i = 0; i < FUTEX_BUCKETS; i++)
        wq_wake_all(&p->futex[i]);
    console_wake_readers();          /* 正在等主控台輸入的也一樣 */
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

/* 由另一個行程建立的新行程:繼承建立者所有開啟中的檔案(同一次開啟,共用讀寫位置)。
   這是之後做輸出入重新導向的基礎:建立者先安排好描述符,新行程照常讀寫 0、1、2 號 */
static void fd_inherit(struct proc *p, struct proc *parent)
{
    unsigned long fl = spin_lock_irqsave(&fd_lk);

    for (unsigned i = 0; i < PROC_MAX_FD; i++)
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
        fd_inherit(p, parent);
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
        p->zombie = 1;
        wq_wake_all(&p->waiters);
        sched_unlock(flags);
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
    while ((p = proc_find(pid)) != NULL && !p->zombie)
        wq_sleep(&p->waiters);
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
    if (thread_killed() && thread_data())
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
 * 把使用者位址 [uaddr, uaddr+len) 的內容複製到 kernel 緩衝區 dst。
 * 逐頁透過行程的頁表查出實體位址,並確認該頁是使用者可存取的;
 * 任何一頁不合格就回傳 -1。不直接解參考使用者的指標。
 */
static int copy_from_user(struct proc *p, void *dst, uintptr_t uaddr, size_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uintptr_t pa;
        if (vm_space_query(p->space, uaddr, &pa, NULL) != 0)
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
        if (vm_space_query(p->space, uaddr, &pa, &flags) != 0 || !(flags & VM_WRITE))
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
    p->space = img.space;
    p->exe = img.exe;
    p->nstacks = 1;
    thread_set_space(img.space);     /* 換到新的位址空間;舊的已經沒有人使用 */
    vm_space_destroy(old_space);
    vfs_exec_release(old_exe);
    vfs_close(old_exe);

    arch_enter_user(img.entry, img.usp, img.argp);
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

long syscall_dispatch(long num, long a0, long a1, long a2)
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
    case SYS_readdir:
        return sys_readdir(p, a0, (uintptr_t)a1);
    case SYS_mkdir:
    case SYS_unlink:
    case SYS_stat:
        return sys_path_op(p, num, (uintptr_t)a0, (uintptr_t)a1);
    case SYS_exit:
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
    case SYS_wait:
        if (a0 == p->pid)
            return -E_INVAL;         /* 等自己結束會永遠等下去 */
        return proc_wait((int)a0);
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
