/*
 * proc_fs.c - 行程的「檔案環境」:檔案描述符表與目前的工作目錄
 *
 * 這兩樣東西決定了「這個行程說的 3 號檔案、說的 notes.txt 指的是什麼」。
 * 狀態放在 struct proc 的 files 成員(struct proc_files,見 proc_internal.h),
 * 只有這個檔案可以存取它;其他模組一律經由下面的函式。
 * 上鎖:pstate_lk(見 proc.c)。只包住查表與填表的幾條指令。
 */
#define PROC_OWNER_FILES              /* 這個檔案是這一組狀態的擁有者(見 proc_internal.h) */
#include "proc_internal.h"

/* ---- 檔案描述符表 ---- */

/* 把一次開啟放進編號最小的空位;回傳編號,滿了回傳 -E_MFILE */
int fd_install(struct proc *p, struct file *f)
{
    int fd = -E_MFILE;
    unsigned long fl = spin_lock_irqsave(&pstate_lk);

    for (unsigned i = 0; i < PROC_MAX_FD; i++) {
        if (!p->files.fd[i]) {
            p->files.fd[i] = f;
            fd = (int)i;
            break;
        }
    }
    spin_unlock_irqrestore(&pstate_lk, fl);
    return fd;
}

/* 取得描述符對應的檔案,並多拿一個參考:同一個行程的另一條執行緒
   可能同時關閉這個描述符,有了參考,檔案在我們用完之前不會消失。用完要 vfs_close */
struct file *fd_get(struct proc *p, long fd)
{
    struct file *f = NULL;

    if (fd < 0 || fd >= (long)PROC_MAX_FD)
        return NULL;
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    f = p->files.fd[fd];
    if (f)
        vfs_dup(f);                  /* 必須在放開 pstate_lk 之前:否則「查到指標」與「拿到參考」
                                        之間,另一條執行緒可能已經把它關閉並釋放了。
                                        鎖的順序固定為 pstate_lk -> VFS 內部的 inode_lk */
    spin_unlock_irqrestore(&pstate_lk, fl);
    return f;
}

struct file *fd_remove(struct proc *p, long fd)
{
    struct file *f = NULL;

    if (fd < 0 || fd >= (long)PROC_MAX_FD)
        return NULL;
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    f = p->files.fd[fd];
    p->files.fd[fd] = NULL;
    spin_unlock_irqrestore(&pstate_lk, fl);
    return f;
}

void fd_close_all(struct proc *p)
{
    for (unsigned i = 0; i < PROC_MAX_FD; i++) {
        struct file *f = fd_remove(p, i);
        if (f)
            vfs_close(f);
    }
}

/* 新行程的 0、1、2 號描述符(標準輸入、輸出、錯誤)都指向主控台 */
int fd_setup_console(struct proc *p)
{
    struct file *f;

    if (vfs_open("/dev/console", O_RDWR, &f) != 0)
        return -1;
    p->files.fd[0] = f;
    p->files.fd[1] = vfs_dup(f);
    p->files.fd[2] = vfs_dup(f);
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
void fd_inherit(struct proc *p, struct proc *parent, unsigned count)
{
    unsigned long fl = spin_lock_irqsave(&pstate_lk);

    for (unsigned i = 0; i < count && i < PROC_MAX_FD; i++)
        if (parent->files.fd[i])
            p->files.fd[i] = vfs_dup(parent->files.fd[i]);
    spin_unlock_irqrestore(&pstate_lk, fl);
}

/* fd 是不是描述符表範圍內的編號(不表示那一格有東西) */
int fd_in_range(long fd)
{
    return fd >= 0 && fd < (long)PROC_MAX_FD;
}

/* 把 f 放進指定的編號 fd(dup2 用);原本在那一格的那次開啟交還給呼叫者去關閉(沒有則為 NULL)。
   fd 必須在範圍內 */
struct file *fd_replace(struct proc *p, long fd, struct file *f)
{
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    struct file *old = p->files.fd[fd];
    p->files.fd[fd] = f;
    spin_unlock_irqrestore(&pstate_lk, fl);
    return old;
}

/* 新行程的工作目錄:沿用 parent 的;parent 為 NULL(由 kernel 建立)時從根目錄開始 */
void files_init_cwd(struct proc *p, struct proc *parent)
{
    if (parent) {
        unsigned long fl = spin_lock_irqsave(&pstate_lk);
        memcpy(p->files.cwd, parent->files.cwd, sizeof(p->files.cwd));
        spin_unlock_irqrestore(&pstate_lk, fl);
    } else {
        p->files.cwd[0] = '/';
        p->files.cwd[1] = '\0';
    }
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
int user_path(struct proc *p, char *dst, uintptr_t uaddr)
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
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    memcpy(dst, p->files.cwd, sizeof(p->files.cwd));
    spin_unlock_irqrestore(&pstate_lk, fl);
    size_t n = strlen(dst), m = strlen(rel);
    if (n + 1 + m > FS_PATH_MAX)
        return -E_NAMETOOLONG;
    if (n > 1)
        dst[n++] = '/';
    memcpy(dst + n, rel, m + 1);
    return 0;
}

long sys_chdir(struct proc *p, uintptr_t upath)
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
    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    memcpy(p->files.cwd, path, strlen(path) + 1);
    spin_unlock_irqrestore(&pstate_lk, fl);
    return 0;
}

long sys_getcwd(struct proc *p, uintptr_t ubuf, size_t len)
{
    char path[FS_PATH_MAX + 1];

    unsigned long fl = spin_lock_irqsave(&pstate_lk);
    memcpy(path, p->files.cwd, sizeof(path));
    spin_unlock_irqrestore(&pstate_lk, fl);
    size_t n = strlen(path);
    if (n + 1 > len)
        return -E_INVAL;                         /* 緩衝區放不下 */
    if (copy_to_user(p, ubuf, path, n + 1) != 0)
        return -E_FAULT;
    return (long)n;
}

