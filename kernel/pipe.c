/*
 * pipe.c - 管線:一條單向的資料通道
 *
 * 管線是一塊固定大小的環狀緩衝區,加上兩個等待佇列:
 *
 *     寫入者 ──▶ [ 緩衝區 4KB ] ──▶ 讀取者
 *                滿了:寫入者睡      空了:讀取者睡
 *
 * 它讓兩個程式可以接在一起,而且彼此不必知道對方的存在:前一個照常寫入標準輸出,
 * 後一個照常讀取標準輸入,shell 在建立它們之前把兩者接到同一條管線的兩端
 * (`ls | wc` 就是這樣運作的)。
 *
 * 緩衝區的大小也是一種流量控制:讀取者處理得慢,緩衝區滿了,寫入者自然被擋下來等,
 * 不會無限制地堆積資料。
 *
 * 怎麼知道「結束了」:管線記錄還有幾個寫端、幾個讀端開著。
 *   所有寫端都關閉 -> 讀取者把剩下的資料讀完後得到 0,和讀到檔案結尾一樣
 *   所有讀端都關閉 -> 寫入者得到錯誤(-E_PIPE):繼續寫已經沒有意義
 * 這裡的「端」是指一次開啟(struct file),不是檔案描述符:fork 之後父子共用
 * 同一次開啟,要兩邊都關閉才算關閉。VFS 在一次開啟完全關閉時通知我們(closed)。
 *
 * 上鎖:緩衝區與計數由排程器的鎖保護,因為讀寫雙方都要把「檢查狀態」與「睡下去」
 * 做成不可分割的一步(見 sched.h)。每次在鎖內複製的資料最多一個緩衝區的大小。
 */
#include <stddef.h>
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "sched.h"

#define PIPE_SIZE 4096u

struct pipe {
    struct inode in;                         /* 必須是第一個成員 */
    struct wait_queue readers_wq;            /* 等資料的讀取者 */
    struct wait_queue writers_wq;            /* 等空位的寫入者 */
    unsigned readers, writers;               /* 還開著的讀端 / 寫端數量 */
    unsigned head, count;                    /* 最舊資料的位置;目前有多少位元組 */
    char buf[PIPE_SIZE];
};

static struct pipe *pp(struct inode *in) { return (struct pipe *)in; }

static long pipe_read(struct inode *in, void *buf, size_t n, uint64_t off)
{
    struct pipe *p = pp(in);
    char *out = buf;
    size_t got = 0;

    (void)off;
    unsigned long flags = sched_lock();
    /* 沒有資料時:還有寫端開著就等;都關了就是結束。被終止的行程不再等 */
    while (p->count == 0 && p->writers > 0 && !thread_killed())
        wq_sleep(&p->readers_wq);

    while (got < n && p->count > 0) {
        size_t k = PIPE_SIZE - p->head;      /* 到緩衝區尾端為止連續的一段 */
        if (k > p->count)
            k = p->count;
        if (k > n - got)
            k = n - got;
        memcpy(out + got, p->buf + p->head, k);
        p->head = (p->head + (unsigned)k) % PIPE_SIZE;
        p->count -= (unsigned)k;
        got += k;
    }
    if (got) {
        wq_wake_all(&p->writers_wq);         /* 有空位了 */
        sched_preempt_point();
    }
    sched_unlock(flags);
    return (long)got;                        /* 0 = 結束(或被終止) */
}

static long pipe_write(struct inode *in, const void *buf, size_t n, uint64_t off)
{
    struct pipe *p = pp(in);
    const char *src = buf;
    size_t done = 0;
    long err = 0;

    (void)off;
    unsigned long flags = sched_lock();
    while (done < n) {
        if (p->readers == 0) {               /* 沒有人會來讀了 */
            err = -E_PIPE;
            break;
        }
        if (thread_killed())
            break;
        if (p->count == PIPE_SIZE) {
            wq_sleep(&p->writers_wq);        /* 滿了:等讀取者拿走一些 */
            continue;
        }
        size_t tail = (p->head + p->count) % PIPE_SIZE;
        size_t k = PIPE_SIZE - tail;         /* 到緩衝區尾端為止連續的空位 */
        if (k > PIPE_SIZE - p->count)
            k = PIPE_SIZE - p->count;
        if (k > n - done)
            k = n - done;
        memcpy(p->buf + tail, src + done, k);
        p->count += (unsigned)k;
        done += k;
        wq_wake_all(&p->readers_wq);         /* 有資料了 */
    }
    sched_preempt_point();
    sched_unlock(flags);
    return done ? (long)done : err;
}

/* 一次開啟被完全關閉:少一個讀端或寫端,並叫醒對面(它等的事情可能不會發生了) */
static void pipe_closed(struct inode *in, int oflags)
{
    struct pipe *p = pp(in);
    unsigned long flags = sched_lock();

    if ((oflags & O_ACCMODE) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    wq_wake_all(&p->readers_wq);
    wq_wake_all(&p->writers_wq);
    sched_unlock(flags);
}

static void pipe_release(struct inode *in)
{
    kfree(pp(in));
}

static const struct inode_ops pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .closed = pipe_closed,
    .release = pipe_release,
};

int vfs_pipe(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));

    if (!p)
        return -E_NOMEM;
    inode_init(&p->in, &pipe_ops, I_PIPE);
    p->readers = p->writers = 1;

    *rd = vfs_file_new(&p->in, O_RDONLY);
    *wr = *rd ? vfs_file_new(&p->in, O_WRONLY) : NULL;
    if (!*wr) {
        /* 配置失敗:把已經建立的部分收掉。計數先歸零,免得 closed 把它減成負的 */
        if (*rd) {
            p->writers = 0;
            vfs_close(*rd);                  /* 最後一個參考:管線會被釋放 */
        } else {
            kfree(p);
        }
        return -E_NOMEM;
    }
    return 0;
}
