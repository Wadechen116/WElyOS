/*
 * ramfs.c - 存在記憶體裡的檔案系統(實作 fs.h 的 inode_ops)
 *
 * 最簡單的檔案系統:沒有磁碟,所有東西都是 kernel 的資料結構。
 *   目錄  一條串列,每筆紀錄是「名稱 -> 節點」
 *   檔案  一張表,每一格指向一個 4KB 的頁;檔案的第 N 個位元組在
 *         第 N / 4096 格所指的頁裡。寫到哪裡才配置到哪裡,
 *         還沒配置的格子讀起來是 0
 * 關機後內容就消失。它的用途是讓 kernel 先有「檔案」可用(開機檔案包解開在這裡),
 * 並把 VFS 的介面確定下來;之後加入磁碟上的檔案系統時,VFS 以上的程式不必改。
 *
 * 上鎖:VFS 呼叫這裡的函式之前已經鎖住該 inode,所以這裡不必再上鎖。
 * 唯一的例外是全域的頁數統計,用原子操作。
 */
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "cred.h"
#include "panic.h"
#include "membudget.h"
#include "uapi/errno.h"

#define RAMFS_MAX_BLOCKS 4096u               /* 單一檔案的上限:4096 頁 = 16MB */

struct rnode;

struct rdirent {
    struct rdirent *next;
    struct rnode *node;
    char name[FS_NAME_MAX + 1];
};

struct rnode {
    struct inode in;                         /* 必須是第一個成員:inode 指標可以直接轉回 rnode */
    struct rnode *parent;                    /* 目錄:上層目錄(根目錄指向自己) */
    struct rdirent *ents;                    /* 目錄:紀錄的串列 */
    void **blocks;                           /* 檔案:頁的表 */
    size_t nblocks;                          /* 表有幾格 */
};

static const struct inode_ops ramfs_ops;
static unsigned long data_pages;             /* 所有檔案的資料頁總數 */

/*
 * ramfs 的記帳(I-240 C3.2 (d)):全系統共用一個 FS domain,在建立根目錄之前設定,所以每一個
 * 物件(包括開機時解開的檔案)都記帳。資料頁每頁 1;節點、目錄項、區塊表以 kmalloc 實際大小取整
 * (釋放時用同一個大小算,所以不必另外記)。一般使用者受 FS domain 的上限限制;有 CAP_ADMIN 的
 * (以及沒有身分的 kernel 執行緒)略過這個上限但照樣記帳 — 實體上仍受 pmm 的底線與預留保護。
 */
static struct mb_domain *fs_dom;
static int fs_strict;                        /* 測試用:kernel 執行緒也照一般使用者受上限限制 */

/* 回傳原本的 domain(測試換上自己的 domain 之後要換回去) */
struct mb_domain *ramfs_set_domain(struct mb_domain *d)
{
    struct mb_domain *old = fs_dom;
    fs_dom = d;
    return old;
}

void ramfs_test_strict(int on)
{
    fs_strict = on;
}

/* 記 n 頁;FS 上限不夠回傳 -E_NOSPC,什麼都不改 */
static int fs_charge(unsigned long n)
{
    if (!fs_dom || n == 0)
        return 0;
    const struct cred *c = cred_current();
    int rc = (!fs_strict && (!c || (c->caps & CAP_ADMIN))) ? mb_quota_force_add(fs_dom, MB_RAMFS, n)
                                           : mb_quota_add(fs_dom, MB_RAMFS, n);
    return rc == 0 ? 0 : -E_NOSPC;
}

static void fs_uncharge(unsigned long n)
{
    if (fs_dom && n && mb_quota_sub(fs_dom, MB_RAMFS, n) != 0)
        panic("ramfs: quota release mismatch");
}

static unsigned long obj_pages(void *p) { return p ? mb_pages_for_bytes(kmalloc_size(p)) : 0; }

static struct rnode *rn(struct inode *in) { return (struct rnode *)in; }

unsigned long ramfs_pages(void)
{
    return __atomic_load_n(&data_pages, __ATOMIC_RELAXED);
}

/* 失敗時 *err:-E_NOMEM = 實體記憶體不夠,-E_NOSPC = FS 上限(呼叫者照實回報,不混為一談) */
static struct rnode *node_new(int type, struct rnode *parent, int *err)
{
    struct rnode *n = kzalloc(sizeof(*n));
    if (!n) {
        *err = -E_NOMEM;
        return NULL;
    }
    if (fs_charge(obj_pages(n)) != 0) {
        kfree(n);
        *err = -E_NOSPC;
        return NULL;
    }
    inode_init(&n->in, &ramfs_ops, type);
    n->parent = parent ? parent : n;
    return n;
}

struct inode *ramfs_create_root(void)
{
    int err;
    struct rnode *n = node_new(I_DIR, NULL, &err);
    return n ? &n->in : NULL;
}

static struct rdirent *find(struct rnode *dir, const char *name)
{
    for (struct rdirent *e = dir->ents; e; e = e->next)
        if (!strcmp(e->name, name))
            return e;
    return NULL;
}

static int ramfs_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct rnode *d = rn(dir);

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        *out = &d->parent->in;
        return 0;
    }
    struct rdirent *e = find(d, name);
    if (!e)
        return -E_NOENT;
    *out = &e->node->in;
    return 0;
}

static int ramfs_create(struct inode *dir, const char *name, int type, struct inode **out)
{
    struct rnode *d = rn(dir);
    struct rdirent *e = kzalloc(sizeof(*e));
    if (!e)
        return -E_NOMEM;
    if (fs_charge(obj_pages(e)) != 0) {
        kfree(e);
        return -E_NOSPC;
    }
    int err;
    struct rnode *n = node_new(type, d, &err);
    if (!n) {
        fs_uncharge(obj_pages(e));
        kfree(e);
        return err;                          /* 和目錄項一致:記憶體不夠 E_NOMEM,FS 上限 E_NOSPC */
    }
    for (size_t i = 0; i <= strlen(name); i++)
        e->name[i] = name[i];
    e->node = n;
    /* 加在串列尾端:列出目錄時維持建立的順序 */
    struct rdirent **pp = &d->ents;
    while (*pp)
        pp = &(*pp)->next;
    *pp = e;
    d->in.size++;                            /* 目錄的「大小」= 紀錄數 */
    *out = &n->in;
    return 0;
}

/* 加一筆紀錄指向已經存在的節點(改名、硬連結) */
static int ramfs_link(struct inode *dir, const char *name, struct inode *in)
{
    struct rnode *d = rn(dir);
    struct rdirent *e = kzalloc(sizeof(*e));

    if (!e)
        return -E_NOMEM;
    if (fs_charge(obj_pages(e)) != 0) {
        kfree(e);
        return -E_NOSPC;
    }
    for (size_t i = 0; i <= strlen(name); i++)
        e->name[i] = name[i];
    e->node = rn(in);
    struct rdirent **pp = &d->ents;
    while (*pp)
        pp = &(*pp)->next;
    *pp = e;
    d->in.size++;
    if (in->type == I_DIR)
        rn(in)->parent = d;                      /* 目錄搬家:".." 改指向新的上層 */
    return 0;
}

static int ramfs_unlink(struct inode *dir, const char *name, struct inode **victim)
{
    struct rnode *d = rn(dir);

    for (struct rdirent **pp = &d->ents; *pp; pp = &(*pp)->next) {
        struct rdirent *e = *pp;
        if (strcmp(e->name, name))
            continue;
        *pp = e->next;
        *victim = &e->node->in;
        fs_uncharge(obj_pages(e));
        kfree(e);
        d->in.size--;
        return 0;
    }
    return -E_NOENT;
}

static int ramfs_is_empty(struct inode *dir)
{
    return rn(dir)->ents == NULL;
}

static int ramfs_readdir(struct inode *dir, uint64_t *pos, struct dirent *out)
{
    struct rdirent *e = rn(dir)->ents;
    uint64_t idx = *pos;                         /* 位置 = 第幾筆紀錄 */

    while (e && idx--)
        e = e->next;
    if (!e)
        return 1;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i <= strlen(e->name); i++)
        out->name[i] = e->name[i];
    out->type = (uint32_t)e->node->in.type;
    (*pos)++;
    return 0;
}

/* 記憶體檔案系統的「容量」就是整台機器的記憶體:以頁為單位回報 */
static int ramfs_statfs(struct inode *in, struct fsinfo *out)
{
    (void)in;
    memset(out, 0, sizeof(*out));
    out->block_size = PAGE_SIZE;
    out->blocks = pmm_total_pages();
    out->free_blocks = pmm_free_count();
    if (fs_dom) {                            /* 一般使用者能寫的量還受 FS domain 的上限限制 */
        struct mb_stats st;
        mb_stats(fs_dom, &st);
        unsigned long room = st.total < st.limit ? st.limit - st.total : 0;
        if (room < out->free_blocks)
            out->free_blocks = room;
    }
    memcpy(out->type, "ramfs", 6);
    return 0;
}

static long ramfs_read(struct inode *in, void *buf, size_t n, uint64_t off)
{
    struct rnode *f = rn(in);
    uint8_t *out = buf;
    size_t done = 0;

    if (off >= in->size)
        return 0;                            /* 已經到檔案結尾 */
    if (n > in->size - off)
        n = (size_t)(in->size - off);
    while (done < n) {
        size_t blk = (size_t)((off + done) / PAGE_SIZE);
        size_t in_blk = (size_t)((off + done) % PAGE_SIZE);
        size_t k = PAGE_SIZE - in_blk;
        if (k > n - done)
            k = n - done;
        if (blk < f->nblocks && f->blocks[blk])
            memcpy(out + done, (uint8_t *)f->blocks[blk] + in_blk, k);
        else
            memset(out + done, 0, k);        /* 沒有配置的部分(檔案中的「洞」)讀起來是 0 */
        done += k;
    }
    return (long)done;
}

/* 確保表至少有 want 格。表不夠大時換一張兩倍大的(複製的只是指標,不是資料) */
static int grow_table(struct rnode *f, size_t want)
{
    if (want <= f->nblocks)
        return 0;
    size_t cap = f->nblocks ? f->nblocks : 4;
    while (cap < want)
        cap *= 2;
    if (cap > RAMFS_MAX_BLOCKS)
        cap = RAMFS_MAX_BLOCKS;
    void **t = kzalloc(cap * sizeof(void *));
    if (!t)
        return -E_NOMEM;
    /* 只記新舊兩表的差額:換表之後留下來的只有新表。若先記整張新表,剩餘額度介於
       「差額」和「新表」之間時檔案會長不大(審查發現的假性 E_NOSPC)。不夠就保留舊表、什麼都不改 */
    unsigned long np = obj_pages(t), op = obj_pages(f->blocks);
    if (np > op && fs_charge(np - op) != 0) {
        kfree(t);
        return -E_NOSPC;
    }
    if (f->blocks)
        memcpy(t, f->blocks, f->nblocks * sizeof(void *));
    if (op > np)
        fs_uncharge(op - np);
    kfree(f->blocks);
    f->blocks = t;
    f->nblocks = cap;
    return 0;
}

static long ramfs_write(struct inode *in, const void *buf, size_t n, uint64_t off)
{
    struct rnode *f = rn(in);
    const uint8_t *src = buf;
    size_t done = 0;

    if (off + n > (uint64_t)RAMFS_MAX_BLOCKS * PAGE_SIZE || off + n < off)
        return -E_FBIG;
    if (n == 0)
        return 0;
    int gerr = grow_table(f, (size_t)((off + n + PAGE_SIZE - 1) / PAGE_SIZE));
    if (gerr)
        return gerr;
    long err = -E_NOMEM;                     /* 一個位元組都沒寫進去時回傳的錯誤 */

    while (done < n) {
        size_t blk = (size_t)((off + done) / PAGE_SIZE);
        size_t in_blk = (size_t)((off + done) % PAGE_SIZE);
        size_t k = PAGE_SIZE - in_blk;
        if (k > n - done)
            k = n - done;
        if (!f->blocks[blk]) {
            if (fs_charge(1) != 0) {
                err = -E_NOSPC;              /* FS 上限:回報已經寫入的部分 */
                break;
            }
            void *page = pmm_alloc();
            if (!page) {
                fs_uncharge(1);
                break;                       /* 記憶體用完:回報已經寫入的部分 */
            }
            memset(page, 0, PAGE_SIZE);      /* 新的頁一定清 0:不可以讓人讀到別人用過的內容 */
            f->blocks[blk] = page;
            __atomic_fetch_add(&data_pages, 1, __ATOMIC_RELAXED);
        }
        memcpy((uint8_t *)f->blocks[blk] + in_blk, src + done, k);
        done += k;
    }
    if (done > 0 && off + done > in->size)
        in->size = off + done;
    return done ? (long)done : err;
}

/* 檔案第 off / 4096 頁的內容所在的實體頁。只有「整頁都在檔案範圍內」才提供:
   最後不滿一頁的部分,頁裡多出來的位元組不屬於檔案,不該被對映出去 */
static void *ramfs_get_page(struct inode *in, uint64_t off)
{
    struct rnode *f = rn(in);
    size_t blk = (size_t)(off / PAGE_SIZE);

    if (off % PAGE_SIZE || off + PAGE_SIZE > in->size || blk >= f->nblocks)
        return NULL;
    return f->blocks[blk];
}

static int ramfs_truncate(struct inode *in, uint64_t size)
{
    struct rnode *f = rn(in);
    size_t keep = (size_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    for (size_t i = keep; i < f->nblocks; i++) {
        if (f->blocks[i]) {
            pmm_free(f->blocks[i]);
            f->blocks[i] = NULL;
            __atomic_fetch_sub(&data_pages, 1, __ATOMIC_RELAXED);
            fs_uncharge(1);
        }
    }
    /* 留下的最後一頁中,超出新大小的部分清 0:之後檔案再變大時,那裡應該讀到 0 */
    if (size % PAGE_SIZE && keep - 1 < f->nblocks && f->blocks[keep - 1])
        memset((uint8_t *)f->blocks[keep - 1] + size % PAGE_SIZE, 0,
               PAGE_SIZE - (size_t)(size % PAGE_SIZE));
    in->size = size;
    return 0;
}

static void ramfs_release(struct inode *in)
{
    struct rnode *n = rn(in);

    if (in->type == I_FILE)
        ramfs_truncate(in, 0);
    fs_uncharge(obj_pages(n->blocks) + obj_pages(n));
    kfree(n->blocks);
    kfree(n);                                /* 目錄此時一定是空的(非空的目錄不允許刪除) */
}

static const struct inode_ops ramfs_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .unlink = ramfs_unlink,
    .link = ramfs_link,
    .readdir = ramfs_readdir,
    .is_empty = ramfs_is_empty,
    .read = ramfs_read,
    .write = ramfs_write,
    .truncate = ramfs_truncate,
    .get_page = ramfs_get_page,
    .release = ramfs_release,
    .statfs = ramfs_statfs,
};
