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
#include "console.h"
#include "cred.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "spinlock.h"
#include "sync.h"
#include "timer.h"

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
    in->uid = in->gid = 0;
    /* 預設的權限:目錄與檔案都是「擁有者可改,其他人可讀」;裝置誰都可以讀寫 */
    in->mode = type == I_DIR ? 0755 : type == I_DEV ? 0666 : 0644;
    in->mtime = 0;
    in->mounted = NULL;
    in->covered = NULL;
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

/*
 * 目前的行程對 in 是否有 want(PERM_R / PERM_W / PERM_X 的組合)的權限。
 * 依序看:是擁有者就用擁有者那一組位元;否則同群組用群組那一組;否則用「其他人」那一組。
 * 只看符合的那一組,不會往下找(擁有者那一組不允許時,即使「其他人」允許也不行)。
 * 有 CAP_FS 的行程不受限制,唯一的例外是「執行」:一個完全沒有執行位元的檔案
 * 連特權行程也不能執行,避免不小心把資料檔當成程式跑。
 */
static int may(struct inode *in, int want)
{
    const struct cred *c = cred_current();

    if (!c)
        return 0;                            /* kernel 自己 */
    if (c->caps & CAP_FS)
        return (want & PERM_X) && in->type == I_FILE && !(in->mode & 0111) ? -E_ACCES : 0;
    unsigned bits = c->uid == in->uid ? in->mode >> 6 : c->gid == in->gid ? in->mode >> 3 : in->mode;
    return ((bits & 7u) & (unsigned)want) == (unsigned)want ? 0 : -E_ACCES;
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
        err = may(cur, PERM_X);              /* 要經過一個目錄,必須有它的「執行」權限 */
        if (err) {
            iput(cur);
            return err;
        }

        struct inode *next;
        /* 掛載的根目錄往上走(".."):先回到它蓋住的那個目錄,再從那裡往上 */
        if (cur->covered && name[0] == '.' && name[1] == '.' && name[2] == '\0') {
            struct inode *under = iget(cur->covered);
            iput(cur);
            cur = under;
        }
        mutex_lock(&cur->lock);
        err = cur->ops->lookup(cur, name, &next);
        if (err == 0) {
            if (next->mounted)
                next = next->mounted;        /* 掛載點:改走掛在上面的那個檔案系統 */
            iget(next);                      /* 還鎖著目錄:它不可能在這一瞬間被刪掉 */
        }
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
    err = may(dir, PERM_X);
    if (err == 0)
        err = dir->ops->lookup(dir, last, &in);
    if (err == 0) {
        if (in->mounted)
            in = in->mounted;
        if (excl)
            err = -E_EXIST;
        else
            iget(in);
    } else if (err == -E_NOENT) {
        /* 目錄本身若已經被刪除(只是還有人參考著),不可以再往裡面放東西 */
        unsigned long f = spin_lock_irqsave(&inode_lk);
        int gone = dir->nlink == 0 && dir != root;
        spin_unlock_irqrestore(&inode_lk, f);
        err = gone ? -E_NOENT : may(dir, PERM_W);    /* 在目錄裡建立東西:要有它的「寫入」權限 */
        if (err == 0) {
            dir->mtime = timer_epoch(NULL);      /* 目錄的內容要變了(先設定:磁碟檔案系統寫回時一併存下) */
            err = dir->ops->create(dir, last, type, &in);
        }
        if (err == 0) {
            const struct cred *c = cred_current();
            if (!in->mtime)
                in->mtime = dir->mtime;
            if (c) {                             /* 新的檔案屬於建立它的人 */
                in->uid = c->uid;
                in->gid = c->gid;
                if (in->ops->setattr)
                    in->ops->setattr(in);
            }
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
    /* 權限:依開啟的方式需要讀、寫或兩者;清空內容算寫入。之後的讀寫不再檢查 —
       權限是在「開啟的那一刻」決定的,開啟之後即使檔案的權限被改掉,這次開啟仍然有效 */
    int acc = flags & O_ACCMODE;
    int want = (acc != O_WRONLY ? PERM_R : 0) | (acc != O_RDONLY || (flags & O_TRUNC) ? PERM_W : 0);
    mutex_lock(&in->lock);
    err = may(in, want);
    mutex_unlock(&in->lock);
    if (err) {
        iput(in);
        return err;
    }
    if ((flags & O_TRUNC) && in->type == I_FILE) {
        mutex_lock(&in->lock);
        if (!in->exec_count)
            in->mtime = timer_epoch(NULL);
        err = in->exec_count ? -E_TXTBSY : in->ops->truncate(in, 0);
        mutex_unlock(&in->lock);
        if (err) {
            iput(in);
            return err;
        }
    }

    struct file *f = vfs_file_new(in, flags);
    iput(in);                                /* vfs_file_new 自己拿了一個參考 */
    if (!f)
        return -E_NOMEM;
    *out = f;
    return 0;
}

struct file *vfs_file_new(struct inode *in, int flags)
{
    struct file *f = kzalloc(sizeof(*f));

    if (!f)
        return NULL;
    f->in = iget(in);
    f->flags = flags;
    f->refs = 1;
    mutex_init(&f->lock);
    return f;
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
        if (f->in->ops->closed)
            f->in->ops->closed(f->in, f->flags);
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
    /* 管線的讀取可能要等對方寫入,不能拿著 inode 的鎖等(對方寫入時也要這把鎖)。
       管線自己負責上鎖 */
    if (in->type == I_PIPE)
        return in->ops->read(in, buf, n, 0);
    mutex_lock(&in->lock);
    long r = in->ops->read(in, buf, n, off);
    mutex_unlock(&in->lock);
    return r;
}

long vfs_read(struct file *f, void *buf, size_t n)
{
    /* 裝置與管線沒有「讀寫位置」,而且讀取可能要等很久(等鍵盤、等對方寫入):
       不可以拿著這次開啟的鎖等。同一次開啟常常由好幾個行程共用(shell 和它啟動的命令
       共用同一個標準輸入),其中一個被暫停在讀取裡的話,其他人就全都讀不到了 */
    if (f->in->type == I_DEV || f->in->type == I_PIPE)
        return vfs_pread(f, buf, n, 0);
    mutex_lock(&f->lock);
    long r = vfs_pread(f, buf, n, f->off);
    if (r > 0)
        f->off += (uint64_t)r;
    mutex_unlock(&f->lock);
    return r;
}

long vfs_seek(struct file *f, long off, int whence)
{
    struct inode *in = f->in;
    long r = -E_INVAL;

    if (in->type == I_PIPE || in->type == I_DEV)
        return -E_SPIPE;
    mutex_lock(&f->lock);
    if (in->type == I_DIR) {
        if (off == 0 && whence == SEEK_SET)      /* 目錄只能「從頭再列一次」 */
            r = (long)(f->off = 0);
    } else if (whence == SEEK_SET || whence == SEEK_CUR || whence == SEEK_END) {
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off : (int64_t)in->size;
        int64_t pos = base + off;
        /* 可以移到結尾之後(之後寫入時,中間沒寫過的部分讀起來是 0),但不可以是負的;
           上限取 long 放得下的範圍,回傳值才不會被誤認成錯誤碼 */
        if (pos >= 0 && pos <= 0x7FFFFFFF) {
            f->off = (uint64_t)pos;
            r = (long)pos;
        }
    }
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
    if (in->type == I_PIPE)
        return in->ops->write(in, buf, n, 0);

    mutex_lock(&f->lock);
    mutex_lock(&in->lock);
    if (f->flags & O_APPEND)
        f->off = in->size;                   /* 鎖著 inode 才決定位置:兩個人同時附加不會互相覆蓋 */
    /* 正被執行的程式檔不可修改:它的頁直接對映在行程裡,改了就等於改了執行中的程式 */
    if (!in->exec_count)
        in->mtime = timer_epoch(NULL);       /* 先設定再寫:磁碟檔案系統會把它和大小一起存進 inode */
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
    int r = in->ops->readdir(in, &f->off, out);  /* 位置由檔案系統自己往後移 */
    mutex_unlock(&in->lock);
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
    err = may(dir, PERM_W | PERM_X);         /* 刪除看的是「目錄」的權限,不是檔案本身的 */
    if (err == 0)
        err = dir->ops->lookup(dir, last, &in);
    if (err == 0 && in->mounted)
        err = -E_BUSY;                       /* 掛載點不可以刪除 */
    if (err == 0) {
        /* 先鎖上層、再鎖下層。鎖著要刪除的目錄檢查它是不是空的,
           並一直鎖到名稱移除為止:這段期間沒有人能在裡面建立新的東西 */
        mutex_lock(&in->lock);
        if (in->type == I_DIR && !in->ops->is_empty(in)) {
            err = -E_NOTEMPTY;
        } else {
            dir->mtime = timer_epoch(NULL);
            err = dir->ops->unlink(dir, last, &victim);
        }
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

/*
 * ---- 改名與硬連結 ----
 *
 * 兩者都建立在同一個操作上:在目錄裡多加一個名稱,指向已經存在的 inode(inode_ops 的 link)。
 *   硬連結 = 加上新名稱
 *   改名   = 先加上新名稱,再移除舊名稱
 * 改名刻意分成這兩步、而且是這個順序:中途不論發生什麼事(另一條執行緒插進來、
 * 磁碟上寫到一半當機),檔案最少都還有一個名稱,不會消失;最壞的結果只是兩個名稱都在。
 * 兩步各自只鎖一個目錄,所以不必同時鎖兩個目錄(那樣要處理上鎖的順序,容易死結)。
 *
 * 搬移目錄時要防止「搬進自己的子目錄」(那會讓一段目錄樹從根目錄再也走不到)。
 * 檢查的方法是從目的目錄一路往上走到根目錄,途中不可以遇到被搬的那個目錄。
 * 這個檢查和實際的搬移之間不能有別的改名插進來,所以所有的改名由一把鎖排成一次一個。
 */
static struct mutex rename_mx = MUTEX_INIT;

/* dir 是不是 in 本身或它底下的某一層。兩者都已經有參考 */
static int is_inside(struct inode *dir, struct inode *in)
{
    struct inode *cur = iget(dir), *up;
    int inside = 0;

    for (int depth = 0; depth < 64; depth++) {
        if (cur == in) {
            inside = 1;
            break;
        }
        mutex_lock(&cur->lock);
        int err = cur->ops->lookup(cur, "..", &up);
        if (err == 0)
            iget(up);
        mutex_unlock(&cur->lock);
        if (err || up == cur) {                  /* 到這個檔案系統的根目錄了 */
            if (err == 0)
                iput(up);
            break;
        }
        iput(cur);
        cur = up;
    }
    iput(cur);
    return inside;
}

/* 在 dir 裡加上名稱 name 指向 in。名稱已經存在回傳 -E_EXIST */
static int link_at(struct inode *dir, const char *name, struct inode *in)
{
    struct inode *tmp;
    int err;

    if (dir->ops != in->ops || !dir->ops->link)
        return -E_XDEV;
    mutex_lock(&dir->lock);
    unsigned long f = spin_lock_irqsave(&inode_lk);
    int gone = dir->nlink == 0 && dir != root && !dir->covered;
    spin_unlock_irqrestore(&inode_lk, f);
    if (gone) {
        err = -E_NOENT;                          /* 目的目錄已經被刪除 */
    } else if ((err = may(dir, PERM_W | PERM_X)) != 0) {
        /* 沒有在目的目錄裡建立名稱的權限 */
    } else if (dir->ops->lookup(dir, name, &tmp) == 0) {
        err = -E_EXIST;
    } else {
        if (in->type == I_DIR)
            mutex_lock(&in->lock);               /* 它的「上層目錄」會被改掉 */
        err = dir->ops->link(dir, name, in);
        if (in->type == I_DIR)
            mutex_unlock(&in->lock);
        if (err == 0) {
            f = spin_lock_irqsave(&inode_lk);
            in->nlink++;
            spin_unlock_irqrestore(&inode_lk, f);
        }
    }
    mutex_unlock(&dir->lock);
    return err;
}

int vfs_link(const char *oldpath, const char *newpath)
{
    char name[FS_NAME_MAX + 1];
    struct inode *in, *dir;
    int err = walk(oldpath, 0, NULL, &in);

    if (err)
        return err;
    err = in->type == I_FILE ? walk(newpath, 1, name, &dir) : -E_INVAL;   /* 只有一般檔案可以有多個名稱 */
    if (err == 0) {
        err = name_is_dots(name) ? -E_EXIST : link_at(dir, name, in);
        iput(dir);
    }
    iput(in);
    return err;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    char oname[FS_NAME_MAX + 1], nname[FS_NAME_MAX + 1];
    struct inode *odir, *ndir, *in = NULL, *victim;
    int err = walk(oldpath, 1, oname, &odir);

    if (err)
        return err;
    err = walk(newpath, 1, nname, &ndir);
    if (err) {
        iput(odir);
        return err;
    }
    mutex_lock(&rename_mx);
    if (name_is_dots(oname) || name_is_dots(nname)) {
        err = -E_INVAL;
    } else {
        mutex_lock(&odir->lock);
        err = may(odir, PERM_W | PERM_X);        /* 要從來源目錄拿走一個名稱 */
        if (err == 0)
            err = odir->ops->lookup(odir, oname, &in);
        if (err == 0)
            iget(in);
        else
            in = NULL;
        mutex_unlock(&odir->lock);
    }
    if (err == 0 && in->mounted)
        err = -E_BUSY;                           /* 掛載點不可以搬走 */
    if (err == 0 && odir == ndir && !strcmp(oname, nname))
        goto out;                                /* 改成同一個名稱:什麼都不必做 */
    if (err == 0 && in->type == I_DIR && is_inside(ndir, in))
        err = -E_INVAL;                          /* 不可以把目錄搬進它自己底下 */
    if (err == 0)
        err = link_at(ndir, nname, in);          /* 第一步:加上新名稱 */
    if (err == 0) {
        /* 第二步:移除舊名稱。先確認舊名稱指的還是同一個東西(這段期間可能被別人刪掉或換掉) */
        struct inode *cur;
        mutex_lock(&odir->lock);
        if (odir->ops->lookup(odir, oname, &cur) == 0 && cur == in &&
            odir->ops->unlink(odir, oname, &victim) == 0) {
            unsigned long f = spin_lock_irqsave(&inode_lk);
            in->nlink--;                         /* 還有新名稱,不會降到 0 */
            spin_unlock_irqrestore(&inode_lk, f);
        }
        mutex_unlock(&odir->lock);
    }
out:
    mutex_unlock(&rename_mx);
    if (in)
        iput(in);
    iput(odir);
    iput(ndir);
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
    st->mtime = in->mtime;
    st->uid = in->uid;
    st->gid = in->gid;
    st->mode = in->mode;
    unsigned long f = spin_lock_irqsave(&inode_lk);
    st->nlink = in->nlink;
    spin_unlock_irqrestore(&inode_lk, f);
    iput(in);
    return 0;
}

/* ---- 權限 ---- */

int vfs_access(const char *path, int want)
{
    struct inode *in;
    int err = walk(path, 0, NULL, &in);

    if (err)
        return err;
    mutex_lock(&in->lock);
    err = may(in, want);
    mutex_unlock(&in->lock);
    iput(in);
    return err;
}

/* chmod 與 chown 共用:mode 為 -1 表示不改權限;change_owner 為 0 表示不改擁有者 */
static int set_attr(const char *path, long mode, int change_owner, uint32_t uid, uint32_t gid)
{
    const struct cred *c = cred_current();
    struct inode *in;
    int err = walk(path, 0, NULL, &in);

    if (err)
        return err;
    mutex_lock(&in->lock);
    int privileged = !c || (c->caps & CAP_FS);
    if (change_owner ? !privileged : !(privileged || c->uid == in->uid)) {
        err = -E_PERM;
    } else {
        uint32_t old_uid = in->uid, old_gid = in->gid;
        uint16_t old_mode = in->mode;
        if (mode >= 0)
            in->mode = (uint16_t)((unsigned)mode & S_PERM_MASK);
        if (change_owner) {
            in->uid = uid;
            in->gid = gid;
        }
        err = in->ops->setattr ? in->ops->setattr(in) : 0;
        if (err) {                           /* 存不進去:記憶體裡的也還原,兩邊保持一致 */
            in->uid = old_uid;
            in->gid = old_gid;
            in->mode = old_mode;
        }
    }
    mutex_unlock(&in->lock);
    iput(in);
    return err;
}

int vfs_chmod(const char *path, unsigned mode)
{
    return set_attr(path, (long)(mode & S_PERM_MASK), 0, 0, 0);
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    return set_attr(path, -1, 1, uid, gid);
}

/* ---- 掛載 ---- */

#define MAX_MOUNTS 4
static struct inode *mounts[MAX_MOUNTS];     /* 掛載上來的檔案系統的根目錄 */
static unsigned nmounts;

int vfs_mount(const char *path, struct inode *fsroot)
{
    struct inode *at;
    int err = walk(path, 0, NULL, &at);

    if (err)
        return err;
    mutex_lock(&at->lock);
    if (at->type != I_DIR || fsroot->type != I_DIR)
        err = -E_NOTDIR;
    else if (at->mounted || at->covered || at == root || nmounts >= MAX_MOUNTS)
        err = -E_BUSY;
    else if (!at->ops->is_empty(at))
        err = -E_NOTEMPTY;
    if (err == 0) {
        /* 兩邊各多拿一個參考,永遠不放:掛載點與掛上來的根目錄都不會消失 */
        iget(fsroot);
        iget(at);
        fsroot->covered = at;
        at->mounted = fsroot;
        mounts[nmounts++] = fsroot;
    }
    mutex_unlock(&at->lock);
    iput(at);
    return err;
}

int vfs_statfs(const char *path, struct fsinfo *out)
{
    struct inode *in;
    int err = walk(path, 0, NULL, &in);

    if (err)
        return err;
    err = in->ops->statfs ? in->ops->statfs(in, out) : -E_INVAL;
    iput(in);
    return err;
}

int vfs_sync(void)
{
    int rc = 0;

    for (unsigned i = 0; i < nmounts; i++)
        if (mounts[i]->ops->sync && mounts[i]->ops->sync(mounts[i]) != 0)
            rc = -E_IO;
    return rc;
}

/* /dev/null:讀取永遠是「結束」,寫入的東西直接丟掉。
   用途例如:背景執行的程式不該去讀鍵盤,shell 就把它的標準輸入接到這裡 */
static long null_read(void *buf, size_t n)
{
    (void)buf; (void)n;
    return 0;
}

static long null_write(const void *buf, size_t n)
{
    (void)buf;
    return (long)n;
}

static const struct dev_ops null_dev = { null_read, null_write };

void vfs_init(void)
{
    root = ramfs_create_root();
    if (!root)
        panic("vfs: cannot create the root file system");
    root->nlink = 1;                         /* 根目錄永遠存在 */
    if (vfs_mkdir("/dev") != 0 || vfs_mkdev("/dev/console", console_dev_ops()) != 0)
        panic("vfs: cannot create /dev/console");
    if (vfs_mkdev("/dev/null", &null_dev) != 0)
        panic("vfs: cannot create /dev/null");
}
