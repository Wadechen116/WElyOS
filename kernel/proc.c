/*
 * proc.c - 行程的生命週期:程式映像、執行緒、建立與等待、exec、fork、檔案描述符表、futex
 *
 * 行程管理內部共用的定義(struct proc 等)在 proc_internal.h;對外的介面在 include/proc.h。
 */
#include "proc_internal.h"

struct proc *procs;                  /* 所有行程(含 zombie);上鎖的規則見 proc_internal.h */
/*
 * 行程裡幾樣「小狀態」共用的鎖:檔案描述符表與工作目錄(proc_fs.c)、記憶體的記帳(proc_mem.c)。
 * 只包住幾條指令。持有它的時候不可以睡眠(它是 spinlock,中斷也關著),也不可以配置記憶體。
 *
 * 鎖的順序(2026-11-19 逐一檢查過全部 17 個持有它的區段,結果如下):
 *   - 唯一會在裡面再拿的鎖是 VFS 的參考計數鎖 inode_lk(經由 vfs_dup,共 2 處:
 *     查到描述符表裡的指標之後,要在放開 pstate_lk 之前拿到參考)。順序固定為 pstate_lk 在外。
 *     反方向不可能發生:inode_lk 只在 vfs.c 內部使用,而 vfs.c 看不到 pstate_lk。
 *   - 其餘區段在鎖裡只做欄位的讀寫、memcpy / strlen,以及 sbrk / mmap 的承諾
 *     (vm_space_commit_anon:依序取得 vm_lk → mb_lk → pmm_lk,層級都高於 LK_PSTATE,順序合法)。
 * 新增持有這把鎖的程式時必須維持這個清單;目前沒有機制在執行時檢查鎖的順序(見 docs/ISSUES.md)。
 */
struct spinlock pstate_lk = SPINLOCK_INIT("pstate", LK_PSTATE);
int next_pid = 1;

static struct proc *proc_became_zombie(struct proc *p);
static void proc_free_list(struct proc *list);

/* 目前這條執行緒代表誰(cred.h)。kernel 自己的執行緒回傳 NULL:不受限制。
   回傳的指標指向行程結構裡的欄位;行程在它自己的執行緒還在執行時不會消失 */
const struct cred *cred_current(void)
{
    struct proc *p = thread_data();

    return p ? &p->cred : NULL;
}

struct proc *proc_find(int pid)
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

/*
 * 使用者 stack(I-240 C3.2 (c1)):每一格 1MB,最低一頁是保護頁(從不承諾、從不對映),
 * 其餘 255 頁在建立執行緒時一次承諾(連同頁表票),之後 stack 往下長時用票兌現 — 一定成功。
 * 兩張位元圖:stack_slots = 已佔格,stack_ready = 已承諾(fault 只認這張)。
 */
#define USER_STACK_COMMIT_PAGES (USER_STACK_SLOT / PAGE_SIZE - 1)

static uintptr_t slot_top(unsigned slot)
{
    return USER_STACK_TOP - (uintptr_t)slot * USER_STACK_SLOT;
}

/* 承諾第 slot 格並以票兌現最上面 USER_STACK_PAGES 頁(參數要寫進去)。
   成功回傳 stack 頂端;失敗回傳 0,什麼都沒留下(承諾失敗不改任何狀態;兌現不會失敗) */
static uintptr_t commit_user_stack(struct vm_space *vs, unsigned slot)
{
    uintptr_t top = slot_top(slot), base = top - USER_STACK_SLOT + PAGE_SIZE;
    if (vm_space_commit(vs, MB_STACK, base, (uintptr_t)USER_STACK_COMMIT_PAGES * PAGE_SIZE) != 0)
        return 0;
    for (unsigned i = 1; i <= USER_STACK_PAGES; i++)
        if (vm_space_fault_anon(vs, top - i * PAGE_SIZE) < 0)
            panic("proc: committed stack page could not be backed");
    return top;
}

static void set_stack_bit(uint32_t *map, unsigned slot, int on)
{
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    if (on)
        __atomic_or_fetch(map, 1u << slot, __ATOMIC_RELEASE);
    else
        __atomic_and_fetch(map, ~(1u << slot), __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&pstate_lk, fl);
}

/* 替行程的一條新執行緒準備使用者 stack:佔一格空的 → 承諾 → 兌現最上面幾頁 → 設 ready。
   成功回傳 stack 頂端並以 *slot_out 交回格號;失敗回傳 0,格子與帳都沒有留下 */
static uintptr_t alloc_user_stack(struct proc *p, unsigned *slot_out)
{
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    unsigned slot = PROC_MAX_THREADS;
    for (unsigned i = 0; i < PROC_MAX_THREADS; i++)
        if (!(p->stack_slots & (1u << i))) {
            slot = i;
            break;
        }
    if (slot < PROC_MAX_THREADS)
        __atomic_or_fetch(&p->stack_slots, 1u << slot, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&pstate_lk, fl);
    if (slot == PROC_MAX_THREADS)
        return 0;

    uintptr_t top = commit_user_stack(p->space, slot);
    if (!top) {
        set_stack_bit(&p->stack_slots, slot, 0);
        return 0;
    }
    set_stack_bit(&p->stack_ready, slot, 1);     /* 從現在起 fault 才會在這一格兌現 */
    *slot_out = slot;
    return top;
}

/* 歸還一格(執行緒結束,或建立失敗):先清 ready、再逐頁退頁與票(R1)、最後放格。
   清 ready 與退頁在同一段 mem.mx 之內:stack 成長的 fault 也拿這把鎖,不會在中間插入 */
static void release_user_stack(struct proc *p, unsigned slot)
{
    uintptr_t top = slot_top(slot), base = top - USER_STACK_SLOT + PAGE_SIZE;
    pmem_lock(p);
    set_stack_bit(&p->stack_ready, slot, 0);
    vm_space_release(p->space, MB_STACK, base, (uintptr_t)USER_STACK_COMMIT_PAGES * PAGE_SIZE);
    pmem_unlock(p);
    set_stack_bit(&p->stack_slots, slot, 0);     /* 此後別的建立者才能拿這一格 */
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
static int image_build(const char *path, const struct args *a, struct image *img, struct mb_domain *dom)
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
    img->space = vm_space_create(dom);   /* 頁表記在行程的 domain 上 */
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

    uintptr_t top = commit_user_stack(img->space, 0);   /* 第 0 格:承諾 255 頁並兌現最上面幾頁 */
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
void proc_thread_exit(void)
{
    struct proc *p = thread_data();

    /* 歸還自己的 stack 格(I-330):這條執行緒不會再回到使用者模式,不會再在那塊 stack 上執行。
       在扣掉執行緒數之前做:位址空間此時一定還在(最後一條執行緒才會釋放它) */
    unsigned long tag = thread_tag();
    if (tag) {
        thread_set_tag(0);
        release_user_stack(p, (unsigned)(tag - 1));
    }

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
        vm_space_destroy(p->space);      /* 頁、承諾(票)與配額全部隨位址空間退回 domain */
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
void proc_begin_exit(struct proc *p, int code)
{
    if (p->exiting)
        return;
    p->exiting = 1;
    p->exit_code = code;
    sched_kill_group(p);
    /* 正睡在 kernel 裡的執行緒(等 futex、等輸入、等管線…)由 sched_kill_group 叫醒 */
}

/* 結束整個行程(由行程自己的執行緒呼叫) */
void proc_exit(int code)
{
    struct proc *p = thread_data();

    unsigned long flags = sched_lock();
    proc_begin_exit(p, code);
    sched_unlock(flags);
    proc_thread_exit();
}

/* ustart 在 prepare 成功後交給 user_thread_main,失敗則由建立者歸還。
   保存帳務欄位後先釋放物件,最後才放掉它自己的 domain 參考。 */
static void ustart_release(struct ustart *st)
{
    struct mb_domain *dom = st->dom;
    unsigned long kobj = st->kobj;
    kfree(st);
    if (dom && mb_release_object(dom, kobj) != 0)
        blackbox_log(BB_FAULT, "ustart kernel-object quota mismatch");
}

/* 行程中每條執行緒在 kernel 這一側的起點:設定好歸屬與位址空間後降到使用者模式 */
static void user_thread_main(void *arg)
{
    struct ustart st = *(struct ustart *)arg;

    ustart_release(arg);
    thread_set_data(st.p);
    thread_set_space(st.p->space);
    thread_set_tag(st.slot < PROC_MAX_THREADS ? st.slot + 1 : 0);   /* 結束時歸還這一格(I-330) */

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
/* 準備一條使用者執行緒(ustart、使用者 stack、kernel stack 與結構、KOBJ),還不啟動。失敗回傳 NULL,不留下東西 */
static struct thread *user_thread_prepare(struct proc *p, uintptr_t entry, uintptr_t arg, uintptr_t usp,
                                         unsigned owned_slot)
{
    struct ustart *st = kmalloc(sizeof(*st));
    if (!st)
        return NULL;
    st->dom = NULL;
    st->kobj = mb_pages_for_bytes(kmalloc_size(st));
    struct mb_domain *dom = pmem_domain(p);
    if (dom && mb_charge_object(dom, st->kobj) != 0) {
        kfree(st);                           /* charge 失敗沒有取得參考或配額 */
        return NULL;
    }
    st->dom = dom;
    st->p = p;
    st->entry = entry;
    st->arg = arg;
    /* owned_slot:呼叫者已經準備好的格(proc_create 的第 0 格);PROC_MAX_THREADS = 呼叫者自己提供的 stack。
       usp 為 0 時由這裡配置一格,失敗時也只退這一格 */
    unsigned slot = owned_slot, mine = 0;
    if (!usp) {
        usp = alloc_user_stack(p, &slot);
        mine = usp != 0;
    }
    st->usp = usp;
    st->slot = slot;
    if (!st->usp) {
        ustart_release(st);
        return NULL;
    }
    struct thread *t = thread_prepare(p->name, user_thread_main, st, PRIO_NORMAL, pmem_domain(p));
    if (!t) {
        /* 準備失敗:這次配置的 stack 連同承諾與格子一起退回(呼叫者提供或已擁有的格不碰) */
        if (mine) {
            release_user_stack(p, slot);
            __atomic_fetch_add(&p->stack_rollbacks, 1, __ATOMIC_RELAXED);   /* 每個行程各自計數(Codex 213) */
        }
        ustart_release(st);
    }
    return t;
}

/* 啟動準備好的執行緒:先算進行程的執行緒數(新執行緒可能立刻開始跑甚至結束),再排入佇列。不會失敗 */
static int user_thread_start(struct proc *p, struct thread *t)
{
    unsigned long flags = sched_lock();
    p->nthreads++;
    sched_unlock(flags);
    return thread_start(t);
}

int proc_add_thread(struct proc *p, uintptr_t entry, uintptr_t arg, uintptr_t usp)
{
    struct thread *t = user_thread_prepare(p, entry, arg, usp, PROC_MAX_THREADS);
    return t ? user_thread_start(p, t) : -1;
}

/* 建立行程。parent 為 NULL 表示由 kernel 建立。回傳 pid 或負的錯誤碼 */
static int proc_create(const char *path, const struct args *a, struct proc *parent)
{
    struct image img;
    struct proc *p = kzalloc(sizeof(*p));
    int err;

    if (!p)
        return -E_NOMEM;
    struct mb_domain *dom = pmem_domain_new(parent);   /* 配額沿用建立者的 */
    if (!dom) {
        kfree(p);
        return -E_NOMEM;
    }
    err = pmem_init(p, dom);
    if (err == 0)
        err = image_build(path, a, &img, dom);
    if (err) {
        pmem_release(p);
        kfree(p);
        return err;
    }
    if (parent) {
        fd_inherit(p, parent, 3);
    } else if (fd_setup_console(p) != 0) {
        image_discard(&img);
        pmem_release(p);
        kfree(p);
        return -E_IO;
    }
    proc_set_name(p, path);
    p->space = img.space;
    p->exe = img.exe;
    p->stack_slots = p->stack_ready = 1;   /* 第 0 格 stack 已由 image_build 承諾好 */
    /* 身分沿用建立者的;kernel 建立的行程是 root,擁有全部的特權 */
    p->cred = parent ? parent->cred : (struct cred){ 0, 0, CAP_ALL };
    files_init_cwd(p, parent);       /* 工作目錄沿用建立者的;kernel 建立的行程從根目錄開始 */

    /* 第一條執行緒在公布 pid 之前準備好(I-240 C3.2 (b2)):失敗時行程從沒出現過,直接拆掉 */
    struct thread *t = user_thread_prepare(p, img.entry, img.argp, img.usp, 0);   /* 第 0 格由 image_build 準備好 */
    if (!t) {
        image_discard(&img);
        fd_close_all(p);
        pmem_release(p);
        kfree(p);
        return -E_NOMEM;
    }

    unsigned long flags = sched_lock();
    int pid = p->pid = next_pid++;
    p->ppid = parent ? parent->pid : 0;
    p->pgid = parent ? parent->pgid : pid;   /* 沿用建立者的群組;kernel 建立的自成一組 */
    p->next = procs;
    procs = p;
    sched_unlock(flags);

    user_thread_start(p, t);
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
    if (p)
        pmem_release(p);                 /* 配額帳戶的最後一份參考(位址空間早已釋放) */
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
            if (psig_stopped(q) && !q->zombie)
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
        pmem_release(list);              /* 配額帳戶的最後一份參考(位址空間早已釋放) */
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
long proc_wait_child(struct proc *p, long pid, int *code, int nohang, int untraced)
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
            if (untraced && psig_stop_unreported(q) && !st)
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
            rc = st->pid;
            *code = (int)(WSTOPPED_BASE | (unsigned)psig_report_stop(st));
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
    if (z)
        pmem_release(z);                     /* 配額帳戶的最後一份參考 */
    kfree(z);
    return rc;
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
long sys_spawn(struct proc *p, uintptr_t upath, uintptr_t uargv)
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
long sys_exec(struct proc *p, uintptr_t upath, uintptr_t uargv)
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
        err = image_build(path, a, &img, pmem_domain(p));
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
    pmem_exec(p);                    /* 新的程式從空的 heap 開始;舊程式的 heap 與匿名對映都不在了 */
    psig_exec(p);                    /* 處理函式是舊程式裡的位址:全部還原成預設(「忽略」則保留) */
    p->exe = img.exe;
    p->stack_slots = p->stack_ready = 1;   /* 新程式只有第 0 格 stack(舊的隨舊位址空間消失) */
    thread_set_tag(1);                       /* 這條執行緒現在擁有第 0 格(fork 前可能是別格) */
    thread_set_space(img.space);     /* 換到新的位址空間;舊的已經沒有人使用 */
    vm_space_destroy(old_space);
    vfs_exec_release(old_exe);
    vfs_close(old_exe);

    arch_enter_user(img.entry, img.usp, img.argp);
}

/* fork 出來的子行程的第一條(也是唯一一條)執行緒在 kernel 這一側的起點 */
struct fork_start {
    unsigned long tag;               /* 父的呼叫執行緒擁有的 stack 格(+1),子行程唯一的執行緒沿用 */
    struct mb_domain *dom;           /* 暫時容器與 frame 共持一份參考 */
    unsigned long kobj;              /* 兩個 kmalloc 物件各取整後的和 */
    struct proc *p;
    void *frame;                     /* 父行程呼叫 fork 時的暫存器(回傳值已改成 0) */
};

/* fork_start 擁有 frame,兩者一起交接給子執行緒或由建立者回退。
   dom 為 NULL 表示尚未記帳,但兩個物件仍須釋放。 */
static void fork_start_release(struct fork_start *st)
{
    struct mb_domain *dom = st->dom;
    unsigned long kobj = st->kobj;
    kfree(st->frame);
    kfree(st);
    if (dom && mb_release_object(dom, kobj) != 0)
        blackbox_log(BB_FAULT, "fork temporary-object quota mismatch");
}

static void fork_child_main(void *arg)
{
    struct fork_start st = *(struct fork_start *)arg;
    /* trap frame 要放在自己的 kernel stack 上才能用來返回(見 arch_user_resume)。
       大小依架構而定,這裡取一個夠大的上限 */
    _Alignas(16) char frame[512];

    thread_set_data(st.p);
    thread_set_space(st.p->space);
    thread_set_tag(st.tag);                  /* 沿用父的呼叫執行緒那一格(子空間裡同一格已承諾) */
    memcpy(frame, st.frame, arch_frame_size());
    fork_start_release(arg);

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
long sys_fork(struct proc *p, void *tf)
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
    struct fork_start *st = kzalloc(sizeof(*st));
    void *frame = kmalloc(arch_frame_size());
    if (st)
        st->frame = frame;
    if (!c || !st || !frame)
        goto nomem;
    /* 順序(Codex 198):先做所有可能失敗、又不會改動父行程的配置 — 子的 domain、子的執行緒 —
       最後才複製位址空間(它會把父的頁改成 COW 並預留票)。複製成功之後只剩不會失敗的步驟 */
    if (pmem_fork_domain(c, p) != 0)
        goto nomem;
    st->kobj = mb_pages_for_bytes(kmalloc_size(st)) + mb_pages_for_bytes(kmalloc_size(frame));
    struct mb_domain *dom = pmem_domain(c);
    if (mb_charge_object(dom, st->kobj) != 0) {
        pmem_release(c);
        goto nomem;
    }
    st->dom = dom;

    memcpy(frame, tf, arch_frame_size());
    arch_frame_set_retval(frame, 0);         /* 子行程看到的回傳值 */
    st->p = c;
    st->tag = thread_tag();
    st->frame = frame;

    memcpy(c->name, p->name, sizeof(c->name));
    /* 所有可能失敗的配置都在公布 pid 之前完成(I-240 C3.2 (b2)):子行程唯一的執行緒先準備好,
       公布之後只剩 thread_start(不會失敗)。以前是公布之後才 thread_create,失敗時留下立刻死亡的子行程 */
    struct thread *ct = thread_prepare(c->name, fork_child_main, st, PRIO_NORMAL, pmem_domain(c));
    if (!ct) {
        pmem_release(c);
        goto nomem;
    }
    c->space = pmem_fork_space(c, p);        /* 最後一個可能失敗的步驟 */
    if (!c->space) {
        thread_abandon(ct);                  /* 還沒啟動:全部退回;父行程沒有被改過 */
        pmem_release(c);
        goto nomem;
    }
    c->cred = p->cred;
    files_init_cwd(c, p);
    c->exe = vfs_dup(p->exe);
    /* 子行程也在執行同一個程式檔。父行程已經持有同一個一般檔案的執行標記,所以不可能失敗;
       萬一失敗代表 VFS 的狀態不一致,不能讓子行程在沒有執行標記的情況下繼續 */
    if (vfs_exec_hold(c->exe) != 0)
        panic("fork: cannot mark the executable of pid %d as running", p->pid);
    c->stack_slots = __atomic_load_n(&p->stack_slots, __ATOMIC_ACQUIRE);   /* fork 只允許單一執行緒,不會同時變動 */
    psig_fork(c, p);                         /* 訊號的處理方式沿用;還沒交付的不帶過去 */
    fd_inherit(c, p, PROC_MAX_FD);

    flags = sched_lock();
    int pid = c->pid = next_pid++;
    c->ppid = p->pid;
    c->pgid = p->pgid;
    c->next = procs;
    procs = c;
    c->nthreads = 1;
    sched_unlock(flags);

    thread_start(ct);
    return pid;

nomem:
    kfree(c);
    if (st)
        fork_start_release(st);
    else
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

long sys_futex_wait(struct proc *p, uintptr_t uaddr, int val)
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

long sys_futex_wake(struct proc *p, uintptr_t uaddr)
{
    /* 同一桶裡可能有等別的位址的人,一併被叫醒也無妨:它們會重新檢查自己的條件 */
    unsigned long flags = sched_lock();
    long n = wq_wake_all(futex_bucket(p, uaddr));
    sched_preempt_point();
    sched_unlock(flags);
    return n;
}

/* ------------------------------------------------------------------ */
/* 給系統呼叫層使用的查詢與身分操作                                    */
/* (系統呼叫層看不到 struct proc 的內容,只能經由這些函式)              */
/* ------------------------------------------------------------------ */

int proc_pid(const struct proc *p)
{
    return p->pid;
}

int proc_pgid(const struct proc *p)
{
    return p->pgid;
}

/* p 有沒有 cap 這項特權(CAP_*) */
int proc_has_cap(const struct proc *p, uint32_t cap)
{
    return (p->cred.caps & cap) != 0;
}

/* addr 是不是 p 的位址空間裡已經對映的位址(建立執行緒時檢查進入點) */
int proc_user_addr_ok(const struct proc *p, uintptr_t addr)
{
    return vm_space_query(p->space, addr, NULL, NULL) == 0;
}

/*
 * 身分與特權的系統呼叫(SYS_getuid / getgid / capget / setuid / capdrop)。
 *
 * setuid:有 CAP_SETID 的行程可以改成任何人;其他行程只能「改成自己」(沒有作用)。
 * 改成非 root 時,所有特權一併清除,而且拿不回來:特權只會減少。
 * 這是整個權限模型的關鍵 — 沒有任何系統呼叫可以讓行程得到它原本沒有的特權,
 * 所以不需要密碼這類東西來把關;代價是「切換成另一個使用者再切回來」必須靠
 * 保留一個還有特權的父行程(su 就是這樣做的)。
 * capdrop:永久放棄指定的特權(最小權限:只留下真正需要的),回傳剩下的。
 */
long proc_cred_call(struct proc *p, long num, long a0, long a1)
{
    unsigned long flags;
    long rc = 0;

    switch (num) {
    case SYS_getuid:
        return (long)p->cred.uid;
    case SYS_getgid:
        return (long)p->cred.gid;
    case SYS_capget:
        return (long)p->cred.caps;
    case SYS_setuid:
        if (a0 < 0 || a0 > 0xFFFF || a1 < 0 || a1 > 0xFFFF)
            return -E_INVAL;
        flags = sched_lock();
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
    case SYS_capdrop:
        flags = sched_lock();
        p->cred.caps &= ~(uint32_t)a0;
        rc = (long)p->cred.caps;
        sched_unlock(flags);
        return rc;
    default:
        return -E_INVAL;
    }
}

/* SYS_procinfo / SYS_sysinfo:行程清單與系統狀態 */
/*
 * 查詢行程的資訊(ps 用):找出「編號大於 after 的行程中編號最小的那一個」,
 * 把它的資訊寫到使用者的 uinfo。回傳 0 = 取得,1 = 沒有更多了。
 *
 * 為什麼一次只給一個、用「上一個的編號」接續:行程隨時在建立與結束,
 * kernel 不必為了列表而記住「上次列到哪裡」,也不必一次配置一大塊記憶體;
 * 呼叫者從 0 開始,每次傳入上一次得到的 pid 即可。列表期間新建立或結束的行程
 * 可能出現也可能不出現,但不會重複、也不會讓 kernel 出錯。
 */
long sys_procinfo(struct proc *p, long after, uintptr_t uinfo)
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
        info.state = best->zombie ? PROC_ZOMBIE : psig_stopped(best) ? PROC_STOPPED :
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
long sys_sysinfo(struct proc *p, uintptr_t uinfo)
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
    struct timer_stats ts;
    timer_get_stats(&ts);
    _Static_assert(TIMER_BURST_BUCKETS == 6, "sysinfo.timer_burst");
    for (unsigned b = 0; b < TIMER_BURST_BUCKETS; b++)
        si.timer_burst[b] = ts.burst[b];
    si.timer_max_burst = ts.max_burst;
    si.timer_resyncs = ts.resyncs;
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

/*
 * 自我測試用(I-240 C3.2 (b2),Codex 191):讓 pid 這個行程「建立執行緒」連續失敗 rounds 次 —
 * 每一輪把它的配額調到「剛好夠 ustart 與 stack 的頁表、不夠 kernel 執行緒」,thread_prepare 在 stack 已經配置好之後失敗。
 * 回報前後的 stack 格位、位址空間頁數,以及確實走到「stack 之後才失敗」的次數;結束時配額還原。回傳成功建立的次數(應為 0)
 */
int proc_thread_failure_probe(int pid, unsigned rounds, uint32_t *slots0, uint32_t *slots1,
                              unsigned long *pages0, unsigned long *pages1, unsigned long *reached)
{
    unsigned long flags = sched_lock();
    struct proc *p = proc_find(pid);
    sched_unlock(flags);
    if (!p)
        return -1;
    struct mb_domain *d = pmem_domain(p);
    struct mb_stats st;
    mb_stats(d, &st);
    unsigned long limit = mb_limit(d);
    *slots0 = __atomic_load_n(&p->stack_slots, __ATOMIC_ACQUIRE);
    *pages0 = vm_space_pages(p->space);
    unsigned long rb0 = __atomic_load_n(&p->stack_rollbacks, __ATOMIC_RELAXED);
    int ok = 0;
    for (unsigned i = 0; i < rounds; i++) {
        /* 每一輪重新設上限:ustart 1 頁 + stack 承諾 255 頁 + 最多 3 張頁表票,
           kernel 執行緒的 KOBJ(6 頁)記不上去 → 一定是在 stack 建好之後、thread_prepare 失敗(Codex 211) */
        mb_stats(d, &st);
        mb_set_limit(d, st.total + 1 + USER_STACK_COMMIT_PAGES + 3);
        if (proc_add_thread(p, 0, 0, 0) >= 0)
            ok++;
    }
    *reached = __atomic_load_n(&p->stack_rollbacks, __ATOMIC_RELAXED) - rb0;
    *slots1 = __atomic_load_n(&p->stack_slots, __ATOMIC_ACQUIRE);
    *pages1 = vm_space_pages(p->space);
    mb_set_limit(d, limit);
    return ok;
}
