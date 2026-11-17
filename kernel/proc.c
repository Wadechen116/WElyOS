/*
 * proc.c - 行程管理與系統呼叫(與架構無關)
 */
#include <stddef.h>
#include "arch.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "timer.h"
#include "vm.h"

/*
 * 內建的使用者程式映像(user/init.c 編譯後的原始二進位,由 userimg.S 嵌入 kernel)。
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
extern const uint8_t user_image_start[], user_image_end[];

/* 使用者 stack:每條執行緒一格(1MB),從 USER_STACK_TOP 往下排;每格只對映最上面 16KB,
   其餘留空,所以 stack 用過頭會碰到沒有對映的位址而被終止 */
#define USER_STACK_TOP   (USER_BASE + 0x30000000u)
#define USER_STACK_SLOT  0x100000u
#define USER_STACK_PAGES 4u
#define PROC_MAX_THREADS 16u

#define FUTEX_BUCKETS 16u

struct proc {
    int pid;
    const char *name;
    struct vm_space *space;
    uintptr_t entry;                 /* 主執行緒的進入點 */
    unsigned nthreads;               /* 還活著的執行緒數 */
    unsigned nstacks;                /* 已分配的 stack 格數(不重複使用) */
    int exiting;                     /* 已開始結束流程 */
    int exit_code;
    int zombie;                      /* 所有執行緒都結束了,等待 proc_wait 來收 */
    struct wait_queue waiters;       /* 正在 proc_wait 的執行緒 */
    struct wait_queue futex[FUTEX_BUCKETS];  /* futex 的等待者,依位址分桶 */
    struct proc *next;
};

static struct proc *procs;           /* 所有行程(含 zombie) */
static int next_pid = 1;

/* 新執行緒進入使用者模式所需的資訊,由建立者交給新執行緒 */
struct ustart {
    struct proc *p;
    uintptr_t entry, usp, arg;
};

static struct proc *proc_find(int pid)
{
    for (struct proc *p = procs; p; p = p->next)
        if (p->pid == pid)
            return p;
    return NULL;
}

/* 配置一個清為 0 的實體頁並對映到行程的 va。回傳該頁在 kernel 中可用的位址 */
static void *map_page(struct proc *p, uintptr_t va, unsigned flags)
{
    void *page = pmm_alloc();
    if (!page)
        return NULL;
    memset(page, 0, PAGE_SIZE);      /* 清 0 也是安全措施:不把別人用過的資料交給行程 */
    if (vm_space_map(p->space, va, (uintptr_t)page, flags) != 0) {
        pmm_free(page);
        return NULL;
    }
    return page;
}

/* 把內建的程式映像載入行程的位址空間 */
static int load_image(struct proc *p)
{
    const struct uimg_hdr *h = (const struct uimg_hdr *)user_image_start;
    size_t file_size = (size_t)(user_image_end - user_image_start);

    if (file_size < sizeof(*h) || h->magic != UIMG_MAGIC || h->data_end > file_size ||
        h->text_end > h->data_end || h->data_end > h->bss_end || h->entry >= h->text_end)
        return -1;

    for (uint32_t off = 0; off < h->bss_end; off += PAGE_SIZE) {
        /* 程式碼:唯讀 + 可執行;資料:可讀寫、不可執行(使用者程式同樣遵守 W^X) */
        unsigned flags = off < h->text_end ? (VM_READ | VM_EXEC) : (VM_READ | VM_WRITE);
        uint8_t *page = map_page(p, USER_BASE + off, flags);
        if (!page)
            return -1;
        if (off < h->data_end) {
            size_t n = h->data_end - off;
            memcpy(page, user_image_start + off, n < PAGE_SIZE ? n : PAGE_SIZE);
        }
    }
    p->entry = USER_BASE + h->entry;
    return 0;
}

/* 替行程的一條新執行緒準備使用者 stack,回傳 stack 頂端的位址(失敗回傳 0) */
static uintptr_t alloc_user_stack(struct proc *p)
{
    if (p->nstacks >= PROC_MAX_THREADS)
        return 0;
    uintptr_t top = USER_STACK_TOP - (uintptr_t)p->nstacks * USER_STACK_SLOT;
    for (unsigned i = 1; i <= USER_STACK_PAGES; i++)
        if (!map_page(p, top - i * PAGE_SIZE, VM_READ | VM_WRITE))
            return 0;
    p->nstacks++;
    return top;
}

/* 目前這條執行緒離開行程;若是最後一條,行程變成 zombie。不會返回 */
static void proc_thread_exit(void)
{
    struct proc *p = thread_data();

    arch_irq_disable();
    if (--p->nthreads == 0) {
        /* 先換回 kernel 頁表,才能釋放自己正在使用的那一份 */
        thread_set_space(NULL);
        vm_space_destroy(p->space);
        p->space = NULL;
        p->zombie = 1;
        wq_wake_all(&p->waiters);
    }
    thread_set_data(NULL);
    thread_exit();
}

/* 開始結束行程:記下結束碼,要求所有執行緒結束。呼叫時中斷已關閉 */
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
}

/* 結束整個行程(由行程自己的執行緒呼叫) */
static void proc_exit(int code)
{
    struct proc *p = thread_data();

    arch_irq_disable();
    proc_begin_exit(p, code);
    proc_thread_exit();
}

/* 行程中每條執行緒在 kernel 這一側的起點:設定好歸屬與位址空間後降到使用者模式 */
static void user_thread_main(void *arg)
{
    struct ustart st = *(struct ustart *)arg;

    kfree(arg);
    thread_set_data(st.p);
    thread_set_space(st.p->space);
    if (thread_killed())             /* 還沒開始跑,行程就已經被終止 */
        proc_thread_exit();
    arch_enter_user(st.entry, st.usp, st.arg);
}

/* 在行程 p 中建立一條執行緒。回傳 tid,失敗回傳 -1 */
static int proc_add_thread(struct proc *p, uintptr_t entry, uintptr_t arg)
{
    struct ustart *st = kmalloc(sizeof(*st));
    if (!st)
        return -1;
    st->p = p;
    st->entry = entry;
    st->arg = arg;
    st->usp = alloc_user_stack(p);
    if (!st->usp) {
        kfree(st);
        return -1;
    }

    unsigned long flags = arch_irq_save();
    p->nthreads++;                   /* 先算進去:新執行緒可能立刻開始跑甚至結束 */
    int tid = thread_create(p->name, user_thread_main, st);
    if (tid < 0) {
        p->nthreads--;
        kfree(st);
    }
    arch_irq_restore(flags);
    return tid;
}

int proc_spawn(const char *name, long arg)
{
    struct proc *p = kzalloc(sizeof(*p));
    if (!p)
        return -1;
    p->name = name;
    p->space = vm_space_create();
    if (!p->space) {
        kfree(p);
        return -1;
    }
    if (load_image(p) != 0) {
        vm_space_destroy(p->space);
        kfree(p);
        return -1;
    }

    unsigned long flags = arch_irq_save();
    int pid = p->pid = next_pid++;
    p->next = procs;
    procs = p;
    arch_irq_restore(flags);

    if (proc_add_thread(p, p->entry, (uintptr_t)arg) < 0) {
        /* 沒有任何執行緒:直接當作已結束,讓 proc_wait 回收 */
        flags = arch_irq_save();
        vm_space_destroy(p->space);
        p->space = NULL;
        p->exit_code = PROC_KILLED_FAULT;
        p->zombie = 1;
        arch_irq_restore(flags);
    }
    return pid;
}

int proc_wait(int pid)
{
    unsigned long flags = arch_irq_save();
    struct proc *p = proc_find(pid);
    int code = -1;

    if (p) {
        while (!p->zombie)
            wq_sleep(&p->waiters);
        code = p->exit_code;
        for (struct proc **pp = &procs; *pp; pp = &(*pp)->next) {
            if (*pp == p) {
                *pp = p->next;
                break;
            }
        }
        kfree(p);
    }
    arch_irq_restore(flags);
    return code;
}

int proc_kill(int pid, int code)
{
    unsigned long flags = arch_irq_save();
    struct proc *p = proc_find(pid);
    int rc = -1;

    if (p && !p->zombie) {
        proc_begin_exit(p, code);
        rc = 0;
    }
    arch_irq_restore(flags);
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

static long sys_write(struct proc *p, uintptr_t ubuf, size_t len)
{
    char buf[128];
    size_t done = 0;

    if (len > 4096)                  /* 限制單次長度,避免長時間佔住輸出 */
        return -1;
    while (done < len) {
        size_t n = len - done;
        if (n > sizeof(buf) - 1)
            n = sizeof(buf) - 1;
        if (copy_from_user(p, buf, ubuf + done, n) != 0)
            return -1;               /* 位址不合法:拒絕,不輸出 */
        buf[n] = '\0';
        kprintf("%s", buf);
        done += n;
    }
    return (long)len;
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
 *   之後才睡下去就再也沒人叫了。把「再確認一次數值」交給 kernel 在關中斷下做,
 *   就不會有這個空檔:數值若已經變了,代表情況已改變,直接返回讓它重試。
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
    unsigned long flags = arch_irq_save();
    if (vm_space_query(p->space, uaddr, &pa, NULL) != 0) {
        rc = -1;                             /* 不是使用者可存取的位址 */
    } else if (*(volatile int *)pa != val || p->exiting) {
        rc = 1;                              /* 數值已經變了:不睡,讓使用者程式重新判斷 */
    } else {
        wq_sleep(futex_bucket(p, uaddr));
        rc = 0;
    }
    arch_irq_restore(flags);
    return rc;
}

static long sys_futex_wake(struct proc *p, uintptr_t uaddr)
{
    /* 同一桶裡可能有等別的位址的人,一併被叫醒也無妨:它們會重新檢查自己的條件 */
    unsigned long flags = arch_irq_save();
    long n = wq_wake_all(futex_bucket(p, uaddr));
    sched_preempt_point();
    arch_irq_restore(flags);
    return n;
}

long syscall_dispatch(long num, long a0, long a1, long a2)
{
    struct proc *p = thread_data();

    (void)a2;
    if (!p)
        panic("syscall %ld from a thread without a process", num);

    switch (num) {
    case SYS_write:
        return sys_write(p, (uintptr_t)a0, (size_t)a1);
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
        return proc_add_thread(p, (uintptr_t)a0, (uintptr_t)a1);
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
