/*
 * vfs.c - 虛擬檔案系統:路徑解析、開啟中的檔案、參考計數(與實際的檔案系統無關)
 *
 * 上鎖規則:
 *   in->lock(mutex)  保護一個 inode 的內容。呼叫 inode_ops 之前由這裡取得。
 *                      會睡覺(讀寫可能很久),所以用 mutex 而不是 spinlock。
 *                      需要同時鎖兩個時,一律「先上層目錄、後下層」,不會形成循環
 *   f->lock(mutex)   保護一次開啟的讀寫位置(同一個行程的多條執行緒可能共用)
 *   inode_lk(spinlock) 保護所有 inode 的 refs 與 nlink。只包住幾條指令
 *
 * inode 的生命週期:
 *   nlink  有幾個名稱指向它(在目錄裡)
 *   refs   有幾個使用中的參考(被開啟、或路徑解析正走到它)
 * 兩者都是 0 才釋放。所以「檔案被刪除時還有人開著」是允許的:
 * 名稱立刻消失,內容留到最後一個開啟者關閉為止。
 */
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "spinlock.h"

struct file {
    struct inode *in;
    uint64_t off;                /* 目前的讀寫位置;目錄則是下一筆紀錄的編號 */
    int flags;
    unsigned refs;               /* 有幾個檔案描述符指向這次開啟(受 inode_lk 保護) */
    struct mutex lock;
};

static struct inode *root;
static struct spinlock inode_lk = SPINLOCK_INIT("inode");

void inode_init(struct inode *in, const struct inode_ops *ops, int type)
{
    in->ops = ops;
    in->dev = NULL;
    in->type = type;
    in->size = 0;
    in->refs = 0;
    in->nlink = 0;
    in->exec_count = 0;
    mutex_init(&in->lock);
}

static struct inode *iget(struct inode *in)
{
    unsigned long f = spin_lock_irqsave(&inode_lk);
    in->refs++;
    spin_unlock_irqrestore(&inode_lk, f);
    return in;
}

static void iput(struct inode *in)
{
    unsigned long f = spin_lock_irqsave(&inode_lk);
    int dead = --in->refs == 0 && in->nlink == 0;
    spin_unlock_irqrestore(&inode_lk, f);
    if (dead)
        in->ops->release(in);                /* 在鎖外:釋放記憶體可能要花時間 */
}

/* 取出路徑的下一段到 name;回傳剩下的部分。沒有下一段回傳 NULL;太長時 *err 設為錯誤碼 */
static const char *next_part(const char *p, char *name, int *err)
{
    size_t n = 0;

    while (*p == '/')
        p++;
    if (!*p)
        return NULL;
    while (*p && *p != '/') {
        if (n >= FS_NAME_MAX) {
            *err = -E_NAMETOOLONG;
            return NULL;
        }
        name[n++] = *p++;
    }
    name[n] = '\0';
    return p;
}

/* 路徑後面是否已經沒有其他段了(只剩斜線) */
static int is_last(const char *p)
{
    while (*p == '/')
        p++;
    return *p == '\0';
}

/*
 * 路徑解析:從根目錄出發,一段一段往下找。
 *   want_parent = 0:回傳整條路徑指到的 inode
 *   want_parent = 1:回傳「最後一段所在的目錄」,最後一段的名稱放進 last
 *                    (建立、刪除時需要:要操作的是上層目錄)
 * 回傳的 inode 已經多一個參考,用完要 iput。
 * 每次只鎖住目前所在的那個目錄,查到下一層就放開。
 * 目前所有路徑都從根目錄開始(行程還沒有「目前的工作目錄」)。
 */
static int walk(const char *path, int want_parent, char *last, struct inode **out)
{
    char name[FS_NAME_MAX + 1];
    struct inode *cur = iget(root);
    int err = 0;

    if (strlen(path) > FS_PATH_MAX) {
        iput(cur);
        return -E_NAMETOOLONG;
    }
    for (;;) {
        const char *rest = next_part(path, name, &err);
        if (!rest) {
            if (err || want_parent) {        /* 路徑是空的(或只有 "/"):沒有「最後一段」 */
                iput(cur);
                return err ? err : -E_INVAL;
            }
            *out = cur;
            return 0;
        }
        if (want_parent && is_last(rest)) {
            if (cur->type != I_DIR) {
                iput(cur);
                return -E_NOTDIR;
            }
            for (size_t i = 0; i <= strlen(name); i++)
                last[i] = name[i];
            *out = cur;
            return 0;
        }
        path = rest;
        if (cur->type != I_DIR) {
            iput(cur);
            return -E_NOTDIR;
        }
        if (name[0] == '.' && name[1] == '\0')
            continue;                        /* "." 就是目前的目錄 */

        struct inode *next;
        mutex_lock(&cur->lock);
        err = cur->ops->lookup(cur, name, &next);
        if (err == 0)
            iget(next);                      /* 還鎖著目錄:它不可能在這一瞬間被刪掉 */
        mutex_unlock(&cur->lock);
        iput(cur);
        if (err)
            return err;
        cur = next;
    }
}

static int name_is_dots(const char *n)
{
    return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}

/* 在 path 建立一個新的 inode(檔案、目錄或裝置)。成功時 *out 已多一個參考 */
static int create_at(const char *path, int type, int excl, struct inode **out)
{
    char last[FS_NAME_MAX + 1];
    struct inode *dir, *in;
    int err = walk(path, 1, last, &dir);

    if (err)
        return err;
    if (name_is_dots(last)) {
        iput(dir);
        return -E_EXIST;
    }
    mutex_lock(&dir->lock);
    err = dir->ops->lookup(dir, last, &in);
    if (err == 0) {
        if (excl)
            err = -E_EXIST;
        else
            iget(in);
    } else if (err == -E_NOENT) {
        /* 目錄本身若已經被刪除(只是還有人參考著),不可以再往裡面放東西 */
        unsigned long f = spin_lock_irqsave(&inode_lk);
        int gone = dir->nlink == 0 && dir != root;
        spin_unlock_irqrestore(&inode_lk, f);
        err = gone ? -E_NOENT : dir->ops->create(dir, last, type, &in);
        if (err == 0) {
            f = spin_lock_irqsave(&inode_lk);
            in->nlink = 1;
            in->refs++;
            spin_unlock_irqrestore(&inode_lk, f);
        }
    }
    mutex_unlock(&dir->lock);
    iput(dir);
    if (err == 0)
        *out = in;
    return err;
}

int vfs_open(const char *path, int flags, struct file **out)
{
    struct inode *in;
    int err = (flags & O_CREAT) ? create_at(path, I_FILE, 0, &in) : walk(path, 0, NULL, &in);

    if (err)
        return err;
    /* 目錄只能以唯讀開啟(用來列出內容),也不能清空 */
    if (in->type == I_DIR && ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))) {
        iput(in);
        return -E_ISDIR;
    }
    if ((flags & O_TRUNC) && in->type == I_FILE) {
        mutex_lock(&in->lock);
        err = in->exec_count ? -E_TXTBSY : in->ops->truncate(in, 0);
        mutex_unlock(&in->lock);
        if (err) {
            iput(in);
            return err;
        }
    }

    struct file *f = kzalloc(sizeof(*f));
    if (!f) {
        iput(in);
        return -E_NOMEM;
    }
    f->in = in;
    f->flags = flags;
    f->refs = 1;
    mutex_init(&f->lock);
    *out = f;
    return 0;
}

struct file *vfs_dup(struct file *f)
{
    unsigned long fl = spin_lock_irqsave(&inode_lk);
    f->refs++;
    spin_unlock_irqrestore(&inode_lk, fl);
    return f;
}

void vfs_close(struct file *f)
{
    unsigned long fl = spin_lock_irqsave(&inode_lk);
    int last = --f->refs == 0;
    spin_unlock_irqrestore(&inode_lk, fl);
    if (last) {
        iput(f->in);
        kfree(f);
    }
}

uint64_t vfs_size(struct file *f)
{
    return f->in->size;
}

long vfs_pread(struct file *f, void *buf, size_t n, uint64_t off)
{
    struct inode *in = f->in;

    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -E_BADF;
    if (in->type == I_DIR)
        return -E_ISDIR;
    if (in->type == I_DEV)
        return in->dev->read ? in->dev->read(buf, n) : -E_INVAL;
    mutex_lock(&in->lock);
    long r = in->ops->read(in, buf, n, off);
    mutex_unlock(&in->lock);
    return r;
}

long vfs_read(struct file *f, void *buf, size_t n)
{
    mutex_lock(&f->lock);
    long r = vfs_pread(f, buf, n, f->off);
    if (r > 0)
        f->off += (uint64_t)r;
    mutex_unlock(&f->lock);
    return r;
}

long vfs_write(struct file *f, const void *buf, size_t n)
{
    struct inode *in = f->in;
    long r;

    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -E_BADF;
    if (in->type == I_DIR)
        return -E_ISDIR;
    if (in->type == I_DEV)
        return in->dev->write ? in->dev->write(buf, n) : -E_INVAL;

    mutex_lock(&f->lock);
    mutex_lock(&in->lock);
    if (f->flags & O_APPEND)
        f->off = in->size;                   /* 鎖著 inode 才決定位置:兩個人同時附加不會互相覆蓋 */
    /* 正被執行的程式檔不可修改:它的頁直接對映在行程裡,改了就等於改了執行中的程式 */
    r = in->exec_count ? -E_TXTBSY : in->ops->write(in, buf, n, f->off);
    mutex_unlock(&in->lock);
    if (r > 0)
        f->off += (uint64_t)r;
    mutex_unlock(&f->lock);
    return r;
}

int vfs_exec_hold(struct file *f)
{
    struct inode *in = f->in;

    if (in->type != I_FILE)
        return -E_NOEXEC;
    mutex_lock(&in->lock);
    in->exec_count++;
    mutex_unlock(&in->lock);
    return 0;
}

void vfs_exec_release(struct file *f)
{
    struct inode *in = f->in;

    mutex_lock(&in->lock);
    in->exec_count--;
    mutex_unlock(&in->lock);
}

void *vfs_file_page(struct file *f, uint64_t off)
{
    struct inode *in = f->in;
    void *page = NULL;

    if (in->type != I_FILE || !in->ops->get_page)
        return NULL;
    mutex_lock(&in->lock);
    if (in->exec_count)                      /* 沒有先 hold 就不給:內容隨時可能被改掉 */
        page = in->ops->get_page(in, off);
    mutex_unlock(&in->lock);
    return page;
}

int vfs_readdir(struct file *f, struct dirent *out)
{
    struct inode *in = f->in;

    if (in->type != I_DIR)
        return -E_NOTDIR;
    mutex_lock(&f->lock);
    mutex_lock(&in->lock);
    int r = in->ops->readdir(in, f->off, out);
    mutex_unlock(&in->lock);
    if (r == 0)
        f->off++;
    mutex_unlock(&f->lock);
    return r;
}

int vfs_mkdir(const char *path)
{
    struct inode *in;
    int err = create_at(path, I_DIR, 1, &in);

    if (err == 0)
        iput(in);
    return err;
}

int vfs_mkdev(const char *path, const struct dev_ops *dev)
{
    struct inode *in;
    int err = create_at(path, I_DEV, 1, &in);

    if (err == 0) {
        in->dev = dev;
        iput(in);
    }
    return err;
}

int vfs_unlink(const char *path)
{
    char last[FS_NAME_MAX + 1];
    struct inode *dir, *in, *victim;
    int err = walk(path, 1, last, &dir);

    if (err)
        return err;
    if (name_is_dots(last)) {
        iput(dir);
        return -E_INVAL;
    }
    mutex_lock(&dir->lock);
    err = dir->ops->lookup(dir, last, &in);
    if (err == 0) {
        /* 先鎖上層、再鎖下層。鎖著要刪除的目錄檢查它是不是空的,
           並一直鎖到名稱移除為止:這段期間沒有人能在裡面建立新的東西 */
        mutex_lock(&in->lock);
        if (in->type == I_DIR && !in->ops->is_empty(in))
            err = -E_NOTEMPTY;
        else
            err = dir->ops->unlink(dir, last, &victim);
        int dead = 0;
        if (err == 0) {
            unsigned long f = spin_lock_irqsave(&inode_lk);
            dead = --victim->nlink == 0 && victim->refs == 0;
            spin_unlock_irqrestore(&inode_lk, f);
        }
        mutex_unlock(&in->lock);
        if (dead)
            victim->ops->release(victim);    /* 沒有人開著:立刻釋放;否則等最後一個關閉 */
    }
    mutex_unlock(&dir->lock);
    iput(dir);
    return err;
}

int vfs_stat(const char *path, struct stat *st)
{
    struct inode *in;
    int err = walk(path, 0, NULL, &in);

    if (err)
        return err;
    st->type = (uint32_t)in->type;
    st->size = in->size;
    unsigned long f = spin_lock_irqsave(&inode_lk);
    st->nlink = in->nlink;
    spin_unlock_irqrestore(&inode_lk, f);
    iput(in);
    return 0;
}

/* ---- /dev/console:把主控台包裝成一個檔案 ---- */

/* 寫入 = 輸出到主控台。這樣使用者程式的輸出就只是「寫入 1 號檔案描述符」,
   之後要把輸出導向檔案,只要換掉描述符指向的東西,程式不必修改 */
static long console_write(const void *buf, size_t n)
{
    const char *p = buf;
    char chunk[128];
    size_t done = 0;

    while (done < n) {
        size_t k = n - done;
        if (k > sizeof(chunk) - 1)
            k = sizeof(chunk) - 1;
        for (size_t i = 0; i < k; i++)
            chunk[i] = p[done + i];
        chunk[k] = '\0';
        kprintf("%s", chunk);
        done += k;
    }
    return (long)n;
}

/* 還沒有鍵盤 / 序列埠的輸入驅動:讀取一律回報「沒有資料」 */
static long console_read(void *buf, size_t n)
{
    (void)buf;
    (void)n;
    return 0;
}

static const struct dev_ops console_dev = { console_read, console_write };

void vfs_init(void)
{
    root = ramfs_create_root();
    if (!root)
        panic("vfs: cannot create the root file system");
    root->nlink = 1;                         /* 根目錄永遠存在 */
    if (vfs_mkdir("/dev") != 0 || vfs_mkdev("/dev/console", &console_dev) != 0)
        panic("vfs: cannot create /dev/console");
}
