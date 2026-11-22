/*
 * io_test.c - 網路卡驅動程式、磁碟與磁碟上的檔案系統、VFS、管線的測試
 *
 * 這些程式原本和開機流程一起放在 kernel/main.c(第 43 步拆出)。它們只在開機參數
 * 要求時執行(selftest、crash=、disktest=、stress、memtest),對外的進入點宣告在 include/selftest.h。
 */
#include <stdint.h>
#include "arch.h"
#include "blackbox.h"
#include "blk.h"
#include "boot_info.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "membudget.h"
#include "kmalloc.h"
#include "failinj.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "sync.h"
#include "timer.h"
#include "vm.h"
#include "watchdog.h"
#include "mmu.h"
#include "netdev.h"
#include "netif.h"
#include "selftest.h"

/*
 * 網路卡驅動程式的測試(不經過協定堆疊):手工組一個 ARP 詢問「10.0.2.2 的硬體位址是什麼」,
 * 送出去,然後等回答。模擬器的使用者模式網路會以閘道的身分回答,
 * 所以收得到就表示傳送與接收兩個方向都通。
 */
void nic_selftest(void)
{
    static uint8_t frame[1600];
    static const uint8_t my_ip[4] = { 10, 0, 2, 15 }, gw_ip[4] = { 10, 0, 2, 2 };
    struct netdev *d = boot_nic;

    if (!d) {
        kprintf("net: no network card, tests skipped\n");
        return;
    }
    memset(frame, 0, 42);
    memset(frame, 0xFF, 6);                  /* 目的:廣播 */
    memcpy(frame + 6, d->mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;      /* 類型:ARP */
    frame[14] = 0; frame[15] = 1;            /* 硬體:乙太網路 */
    frame[16] = 0x08; frame[17] = 0;         /* 協定:IPv4 */
    frame[18] = 6; frame[19] = 4;            /* 位址長度 */
    frame[20] = 0; frame[21] = 1;            /* 詢問 */
    memcpy(frame + 22, d->mac, 6);
    memcpy(frame + 28, my_ip, 4);
    memcpy(frame + 38, gw_ip, 4);
    KASSERT(d->send(d, frame, 42) == 0);
    KASSERT(d->send(d, frame, 10) == -E_INVAL && d->send(d, frame, 1515) == -E_INVAL);   /* 長度不合理 */

    uint64_t t0 = timer_ticks();
    int got = 0;
    while (!got && timer_ticks() - t0 < 500) {
        int n = d->recv(d, frame, sizeof(frame));
        KASSERT(n >= 0 && n <= 1514);
        /* 是 ARP 的回答、問的是閘道、而且是回給我們的 */
        if (n >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2 &&
            !memcmp(frame + 28, gw_ip, 4) && !memcmp(frame, d->mac, 6))
            got = 1;
        else if (n == 0)
            thread_sleep(1);
    }
    KASSERT(got);
    kprintf("net: ok (%s: sent an ARP request, the gateway 10.0.2.2 answered from "
            "%02x:%02x:%02x:%02x:%02x:%02x after %u ms)\n", d->name, frame[22], frame[23], frame[24],
            frame[25], frame[26], frame[27], (unsigned)(timer_ticks() - t0));
}

/* 測試資料:第 i 個位元組的內容(和位置有關,讀回來時可以核對) */
static char disk_pattern(size_t i, unsigned seed)
{
    return (char)('A' + (i * 7 + i / 4096 + seed) % 53);
}

static void disk_write_file(const char *path, size_t size, unsigned seed)
{
    static char buf[3000];
    struct file *f;

    KASSERT(vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
    for (size_t off = 0; off < size; ) {
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = disk_pattern(off + i, seed);
        KASSERT(vfs_write(f, buf, n) == (long)n);
        off += n;
    }
    vfs_close(f);                            /* 關閉時寫回磁碟 */
}

static void disk_verify_file(const char *path, size_t size, unsigned seed)
{
    static char buf[3000];
    struct file *f;
    size_t off = 0;
    long n;

    KASSERT(vfs_open(path, O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == size);
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            KASSERT(buf[i] == disk_pattern(off + (size_t)i, seed));
        off += (size_t)n;
    }
    KASSERT(n == 0 && off == size);
    vfs_close(f);
}

/* 多條執行緒同時在磁碟上建立、寫入、讀回、刪除各自的檔案 */
#define DISK_WORKERS 4
#define DISK_FILES   6

static void disk_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    char path[32] = "/disk/selftest/w0-0", data[600], back[600];
    struct file *f;

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (char)(id * 37 + i);
    for (int n = 0; n < DISK_FILES; n++) {
        path[16] = (char)('0' + id);
        path[18] = (char)('0' + n);
        KASSERT(vfs_open(path, O_CREAT | O_RDWR, &f) == 0);
        KASSERT(vfs_write(f, data, sizeof(data)) == (long)sizeof(data));
        KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
        KASSERT(memcmp(data, back, sizeof(data)) == 0);
        vfs_close(f);
        KASSERT(vfs_unlink(path) == 0);
    }
}

/*
 * I-335:磁碟檔案系統(wfs)的配置失敗注入掃描。由 disk_demo 在 /disk/selftest 還在時呼叫;
 * 沒有掛載磁碟時 disk_demo 已經提早返回,這裡不會執行(bare 模式自然略過)。
 *
 * wfs 的記憶體都在掛載時一次配好(nodes 表、scratch、檢查碼表、bcache 的 BCACHE_NBUF 頁),
 * 平常的操作不配置。可注入的點只有:
 *   - vfs_open 的 struct file(file_alloc 的 kzalloc;heap 要長大時另含一次 pmm)— 在建立檔案之前
 *   - wfs_check_now → wfs_check 的三塊工作區(pmm_alloc_pages ×3)
 * 核對:
 *   - create:失敗回 -E_NOMEM,檔案不存在,磁碟上空閒區塊 / inode 數與 heap 用量不變
 *   - write / link / unlink / sync:整段啟用注入也不能觸發(依設計不配置),而且全部成功
 *   - check_now:失敗回 -E_NOMEM,pmm 空閒頁數與 heap 不變,之後再檢查一次成功
 * bcache 的內容(哪些區塊在快取裡)依設計保留,不比較;只比磁碟上的計數(wfs_stats)。
 */
struct dfi_snap { uint32_t free_blocks, free_inodes; size_t heap_used, pmm_free; };

static void dfi_take(struct dfi_snap *x)
{
    struct wfs_stats st;
    struct kheap_stats h;
    wfs_stats(&st);
    kheap_stats(&h);
    x->free_blocks = st.free_blocks;
    x->free_inodes = st.free_inodes;
    x->heap_used = h.heap_bytes - h.free_bytes;
    x->pmm_free = pmm_free_count();
}

static void dfi_same(const struct dfi_snap *a, const char *what, int site, unsigned n)
{
    struct dfi_snap b;
    dfi_take(&b);
    if (a->free_blocks != b.free_blocks || a->free_inodes != b.free_inodes || a->heap_used != b.heap_used ||
        a->pmm_free != b.pmm_free)
        panic("disk: failinj %s site %d #%u leaked: blocks %u->%u inodes %u->%u heap %zu->%zu pmm %zu->%zu",
              what, site, n, a->free_blocks, b.free_blocks, a->free_inodes, b.free_inodes,
              a->heap_used, b.heap_used, a->pmm_free, b.pmm_free);
}

static void disk_failinj_test(void)
{
    static char data[3 * PAGE_SIZE];
    unsigned points[2][2] = { { 0 } };
    struct stat st;
    struct file *f;

    /* 暖身:成功做一次建立 / 寫入 / 刪除與檢查,讓 heap 的大小、目錄的區塊先穩定下來 */
    KASSERT(vfs_open("/disk/selftest/fiW", O_CREAT | O_RDWR, &f) == 0);
    KASSERT(vfs_write(f, data, sizeof(data)) == (long)sizeof(data));
    vfs_close(f);
    KASSERT(vfs_unlink("/disk/selftest/fiW") == 0 && vfs_sync() == 0 && wfs_check_now() == 0);

    for (int site = 0; site < FI_NSITES; site++) {
        /* 1. 建立檔案:唯一的配置點在建立之前,失敗時磁碟上什麼都沒發生 */
        for (unsigned n = 1;; n++) {
            KASSERT(n < 100);
            struct dfi_snap a;
            dfi_take(&a);
            f = NULL;
            failinj_arm((enum fi_site)site, n);
            int r = vfs_open("/disk/selftest/fiA", O_CREAT | O_WRONLY, &f);
            int fired = failinj_fired();
            failinj_disarm();
            if (!fired) {
                KASSERT(r == 0);
                vfs_close(f);
                KASSERT(vfs_unlink("/disk/selftest/fiA") == 0 && vfs_sync() == 0);
                dfi_same(&a, "create(cleanup)", site, n);
                points[site][0] = n - 1;
                break;
            }
            if (r != -E_NOMEM)
                panic("disk: failinj create site %d #%u returned %d, want -E_NOMEM", site, n, r);
            KASSERT(vfs_stat("/disk/selftest/fiA", &st) == -E_NOENT);
            dfi_same(&a, "create", site, n);
        }

        /* 2. 寫入(配置新區塊與間接區塊)、link、unlink、sync:依設計沒有任何配置 */
        KASSERT(vfs_open("/disk/selftest/fiB", O_CREAT | O_RDWR | O_TRUNC, &f) == 0);
        failinj_arm((enum fi_site)site, 1);
        long w = vfs_write(f, data, sizeof(data));
        int lr = vfs_link("/disk/selftest/fiB", "/disk/selftest/fiC");
        int ur = vfs_unlink("/disk/selftest/fiC");
        int sr = vfs_sync();
        int fired = failinj_fired();
        failinj_disarm();
        if (fired || w != (long)sizeof(data) || lr != 0 || ur != 0 || sr != 0)
            panic("disk: failinj site %d reached wfs write/link/unlink/sync (fired %d w %ld link %d unlink %d "
                  "sync %d)", site, fired, w, lr, ur, sr);
        vfs_close(f);
        KASSERT(vfs_unlink("/disk/selftest/fiB") == 0 && vfs_sync() == 0);

        /* 3. 一致性檢查的三塊工作區 */
        for (unsigned n = 1;; n++) {
            KASSERT(n < 100);
            struct dfi_snap a;
            dfi_take(&a);
            failinj_arm((enum fi_site)site, n);
            int r = wfs_check_now();
            fired = failinj_fired();
            failinj_disarm();
            if (!fired) {
                KASSERT(r == 0);
                dfi_same(&a, "check(ok)", site, n);
                points[site][1] = n - 1;
                break;
            }
            if (r != -E_NOMEM)
                panic("disk: failinj check site %d #%u returned %d, want -E_NOMEM", site, n, r);
            dfi_same(&a, "check", site, n);
        }
    }
    /* 依設計:create 在 kmalloc 至少 1 點;check 只用 pmm(3 點),kmalloc 0 點 */
    KASSERT(points[0][0] >= 1 && points[0][1] == 0 && points[1][1] == 3);
    kprintf("disk: allocation-failure sweep: create %u+%u, check %u+%u failure points (kmalloc+pmm) all "
            "returned E_NOMEM with nothing leaked; write/link/unlink/sync never allocate\n",
            points[0][0], points[1][0], points[0][1], points[1][1]);
}

/* 磁碟上的檔案系統的自我測試。所有東西都放在 /disk/selftest 底下,結束時刪除,
   所以不影響磁碟上原有的內容,空閒的空間也要回到測試之前的數量 */
/*
 * wfs 掛載 / 格式化路徑的配置失敗掃描(I-335)。掛載只在開機時跑一次,所以在 disk_init 之前、
 * 對一顆記憶體磁碟(pmm 頁,不碰真的磁碟)反覆「注入第 N 次配置失敗 → 掛載 → 核對 → 卸載」。
 * 三種磁碟狀態各掃一輪,涵蓋全部配置點:
 *   blank  全空白:bcache pool、nodes、scratch、wfs_format 的檢查碼表
 *   clean  正常卸載過:前三者 + 載入檢查碼表
 *   dirty  沒有正常卸載(模擬斷電):前三者 + 檢查碼表 + wfs_check(1) 的 bitmap / refs / queue
 * 每個 N 核對:回 -E_NOMEM(不是 -E_NOSPC / -E_IO);pmm 空閒頁數與 heap 用量不變;
 * wfs_stats 仍是「沒有掛載」;磁碟映像一個位元組都沒變(失敗的掛載不可以寫磁碟)。
 * 注入沒有觸發(N 超過配置次數)的那一次必須成功,之後再卸載、重新掛載確認檔案系統一致。
 * kmalloc 那一輪預期一次都不觸發(掛載路徑依設計不用 heap)。
 */
#define MD_BLOCKS 256u                           /* 1MB:超過 WFS_MIN_DEV_BLOCKS,有檢查碼表與間接區塊空間 */
static uint8_t *md_mem;
static int md_fail_writes;                       /* 1 = 所有寫入都回 -E_IO(讓檔案系統轉為唯讀) */
static int md_block_next_read;                   /* 1 = 下一次讀取停在 md_rw 裡,直到 md_release(I-338) */
static struct semaphore md_entered = SEMAPHORE_INIT(0), md_release = SEMAPHORE_INIT(0);

static int md_rw(struct blkdev *d, uint64_t lba, unsigned count, void *buf, int write)
{
    if (lba + count > d->sectors)
        return -E_IO;
    if (write && md_fail_writes)
        return -E_IO;
    if (!write && __atomic_exchange_n(&md_block_next_read, 0, __ATOMIC_ACQ_REL)) {
        sem_post(&md_entered);                   /* 呼叫者(檔案系統)此刻持有 wfs_lk */
        sem_wait(&md_release);
    }
    uint8_t *p = md_mem + lba * BLK_SECTOR;
    if (write)
        memcpy(p, buf, (size_t)count * BLK_SECTOR);
    else
        memcpy(buf, p, (size_t)count * BLK_SECTOR);
    return 0;
}
static int md_flush(struct blkdev *d) { (void)d; return 0; }
static struct blkdev md_dev = { "memdisk", (uint64_t)MD_BLOCKS * (PAGE_SIZE / BLK_SECTOR), md_rw, md_flush, NULL };

static uint32_t md_hash(void)                    /* FNV-1a:比較「失敗的掛載前後映像是否相同」 */
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)MD_BLOCKS * PAGE_SIZE; i++)
        h = (h ^ md_mem[i]) * 16777619u;
    return h;
}

/* avail = pmm 空閒 + heap 空閒 + 執行緒池:不受 heap 向 pmm 要頁、執行緒結構留在池子裡影響。
   整個測試前後用它比(I-338 (1) 建了兩條執行緒,結構留在池子裡);逐點掃描沒有執行緒,照樣分開嚴格比 */
struct mfi_snap { size_t heap_used, pmm_free, avail; unsigned long large; };
static void mfi_take(struct mfi_snap *x)
{
    struct kheap_stats h;
    kheap_stats(&h);
    x->heap_used = h.heap_bytes - h.free_bytes;
    x->pmm_free = pmm_free_count();
    x->avail = x->pmm_free * PAGE_SIZE + h.free_bytes + sched_pool_bytes();
    x->large = vm_large_pages();
}

/* 掃一種磁碟狀態;回傳成功掛載前觸發了幾個失敗點。成功之後依 leave 卸載(1 = 正常,0 = 斷電) */
static unsigned mfi_sweep(const char *what, int site, int expect_rc, int leave)
{
    struct inode *root;
    struct wfs_stats st;
    unsigned n;
    for (n = 1;; n++) {
        struct mfi_snap a, b;
        uint32_t h0 = md_hash();
        mfi_take(&a);
        failinj_arm((enum fi_site)site, n);
        int rc = wfs_mount(&md_dev, &root);
        failinj_disarm();
        int fired = failinj_fired();
        if (!fired) {
            if (rc != expect_rc)
                panic("wfs mount failinj %s site %d: unarmed mount returned %d (want %d)", what, site, rc, expect_rc);
            break;
        }
        mfi_take(&b);
        wfs_stats(&st);
        if (rc != -E_NOMEM)
            panic("wfs mount failinj %s site %d #%u: returned %d, want -E_NOMEM", what, site, n, rc);
        if (st.mounted)
            panic("wfs mount failinj %s site %d #%u: still marked mounted after failure", what, site, n);
        if (a.pmm_free != b.pmm_free || a.heap_used != b.heap_used)
            panic("wfs mount failinj %s site %d #%u leaked: pmm %zu->%zu heap %zu->%zu",
                  what, site, n, a.pmm_free, b.pmm_free, a.heap_used, b.heap_used);
        if (md_hash() != h0)
            panic("wfs mount failinj %s site %d #%u: the failed mount wrote to the disk", what, site, n);
        if (n > 64)
            panic("wfs mount failinj %s site %d: more than 64 allocation points?", what, site);
    }
    /* 成功的掛載:檔案系統一致,而且看得到上一輪留下的檔案 */
    wfs_stats(&st);
    KASSERT(st.mounted && st.checksums);
    KASSERT(wfs_check_now() == 0);               /* 0 = 不需要修復任何東西 */
    struct inode *f;
    if (expect_rc == 1) {                        /* 剛格式化:建立一個檔案,讓之後的狀態看得到它 */
        KASSERT(root->ops->create(root, "mfi", I_FILE, &f) == 0);
        KASSERT(root->ops->write(f, "mount-failinj", 13, 0) == 13);
    } else {
        char buf[16] = { 0 };
        KASSERT(root->ops->lookup(root, "mfi", &f) == 0);
        KASSERT(root->ops->read(f, buf, sizeof(buf), 0) == 13 && memcmp(buf, "mount-failinj", 13) == 0);
    }
    KASSERT(f->refs == 0);                       /* 沒有經過 VFS:沒有人持有它,才可以卸載 */
    KASSERT(wfs_test_unmount(leave) == 0);
    return n - 1;
}

/*
 * I-338 (1):已掛載、而且別的執行緒正持有 wfs_lk 時再呼叫 wfs_mount,必須回 -E_BUSY 且不碰鎖。
 *   holder:lookup 取得 wfs_lk 後讀磁碟,停在 md_rw 裡(持鎖)
 *   waiter:lookup 睡在 wfs_lk 的等待佇列
 *   本執行緒:wfs_mount → -E_BUSY;鎖仍被持有、等待者仍在;放行後兩條都正常完成。
 * 舊程式(mutex_init 在判斷之前)會把 owner / 等待佇列清掉:這裡的檢查立刻 panic;
 * 即使略過檢查,holder 解鎖時 mutex_unlock 也會因「非擁有者解鎖」panic,waiter 永遠不會被叫醒。
 */
static struct inode *mb_root;
static volatile int mb_rc[2] = { 1, 1 };

static void mb_lookup(void *arg)
{
    int i = (int)(uintptr_t)arg;
    struct inode *f;
    mb_rc[i] = mb_root->ops->lookup(mb_root, "busy", &f);
}

static void wfs_mount_busy_test(void)
{
    struct inode *root, *f, *r2 = NULL;
    int held, waiters;

    memset(md_mem, 0, (size_t)MD_BLOCKS * PAGE_SIZE);
    KASSERT(wfs_mount(&md_dev, &root) == 1);     /* 格式化 */
    KASSERT(root->ops->create(root, "busy", I_FILE, &f) == 0);
    KASSERT(f->refs == 0);
    KASSERT(wfs_test_drop_cache() == 0);         /* 之後的 lookup 一定要讀磁碟 */
    mb_root = root;
    mb_rc[0] = mb_rc[1] = 1;

    md_block_next_read = 1;
    int th = thread_create("wfs-holder", mb_lookup, (void *)0);
    KASSERT(th > 0);
    sem_wait(&md_entered);                       /* holder 已持有 wfs_lk,停在磁碟讀取裡 */
    int tw = thread_create("wfs-waiter", mb_lookup, (void *)1);
    KASSERT(tw > 0);
    for (int i = 0;; i++) {                      /* 等 waiter 真的睡在鎖上 */
        wfs_test_lock_state(&held, &waiters);
        if (held && waiters)
            break;
        if (i > 5000)
            panic("wfs mount busy: waiter never queued on wfs_lk (held %d waiters %d)", held, waiters);
        thread_sleep(1);
    }

    int rc = wfs_mount(&md_dev, &r2);
    if (rc != -E_BUSY)
        panic("wfs mount busy: second mount returned %d, want -E_BUSY", rc);
    KASSERT(r2 == NULL);
    wfs_test_lock_state(&held, &waiters);
    if (!held || !waiters)
        panic("wfs mount busy: second mount disturbed wfs_lk (held %d waiters %d); want 1 1", held, waiters);

    sem_post(&md_release);                       /* holder 繼續,解鎖時叫醒 waiter */
    thread_join(th);
    thread_join(tw);
    if (mb_rc[0] != 0 || mb_rc[1] != 0)
        panic("wfs mount busy: lookups returned %d / %d", mb_rc[0], mb_rc[1]);
    wfs_test_lock_state(&held, &waiters);
    KASSERT(!held && !waiters);
    KASSERT(wfs_check_now() == 0);
    KASSERT(wfs_test_unmount(1) == 0);
    kprintf("wfs: mount while mounted and the lock held returns -E_BUSY without touching the lock\n");
}

/*
 * I-338 (2):版面計算出錯(csum_alloc 回 -E_INVAL)時,格式化與載入都必須原樣回報 -E_INVAL,
 * 不可以偽裝成 -E_NOMEM;失敗的掛載不洩漏、不寫磁碟、不留下「已掛載」。
 * 舊的 wfs_format 把任何非 0 都改成 -E_NOMEM:blank 那一輪失敗。
 */
static void wfs_csum_inval_test(void)
{
    static const struct { const char *what; int prep; } c[] = { { "format", 0 }, { "load", 1 } };
    for (unsigned i = 0; i < 2; i++) {
        struct inode *root;
        struct wfs_stats st;
        struct mfi_snap a, b;
        memset(md_mem, 0, (size_t)MD_BLOCKS * PAGE_SIZE);
        if (c[i].prep) {                         /* 先做出一個正常卸載過的檔案系統(WFS2,有檢查碼表) */
            KASSERT(wfs_mount(&md_dev, &root) == 1);
            KASSERT(wfs_test_unmount(1) == 0);
        }
        uint32_t h0 = md_hash();
        mfi_take(&a);
        KASSERT(wfs_test_csum_inval_once() == 0);
        int rc = wfs_mount(&md_dev, &root);
        mfi_take(&b);
        wfs_stats(&st);
        if (rc != -E_INVAL)
            panic("wfs csum layout error (%s): mount returned %d, want -E_INVAL", c[i].what, rc);
        if (st.mounted)
            panic("wfs csum layout error (%s): still marked mounted", c[i].what);
        if (a.pmm_free != b.pmm_free || a.heap_used != b.heap_used)
            panic("wfs csum layout error (%s) leaked: pmm %zu->%zu heap %zu->%zu",
                  c[i].what, a.pmm_free, b.pmm_free, a.heap_used, b.heap_used);
        if (md_hash() != h0)
            panic("wfs csum layout error (%s): the failed mount wrote to the disk", c[i].what);
        KASSERT(wfs_mount(&md_dev, &root) == (c[i].prep ? 0 : 1));   /* 掛鉤只生效一次 */
        KASSERT(wfs_test_unmount(1) == 0);
    }
    kprintf("wfs: checksum-table layout error is reported as -E_INVAL on format and load\n");
}

/* 暖身用:兩條同時存在的執行緒。I-338 (1) 會讓兩條測試執行緒同時存在;第一次達到這個同時數時,
   kernel 會建一個之後保留的結構(實測 1 頁,第二次再跑同樣的測試損失為 0 — 是高水位,不是洩漏)。
   先在量測起點之前達到同一個高水位,整個測試前後才能嚴格比較 */
static struct semaphore mfi_warm_go;
static void mfi_warm_thread(void *arg)
{
    (void)arg;
    sem_wait(&mfi_warm_go);
}

void wfs_mount_failinj_test(void)
{
    {
        sem_init(&mfi_warm_go, 0);
        int t1 = thread_create("mfi-warm", mfi_warm_thread, NULL);
        int t2 = thread_create("mfi-warm", mfi_warm_thread, NULL);
        KASSERT(t1 > 0 && t2 > 0);
        sem_post(&mfi_warm_go);
        sem_post(&mfi_warm_go);
        thread_join(t1);
        thread_join(t2);
    }
    struct mfi_snap before, after;
    unsigned pts[3][FI_NSITES];

    sched_trim();                                /* 執行緒池與待回收的執行緒先收乾淨:前後量的是同一種狀態 */
    mfi_take(&before);
    md_mem = pmm_alloc_pages(MD_BLOCKS);
    KASSERT(md_mem);
    memset(md_mem, 0, (size_t)MD_BLOCKS * PAGE_SIZE);
    struct wfs_stats st;
    wfs_stats(&st);
    KASSERT(!st.mounted);                        /* 必須在 disk_init 之前 */

    for (int site = 0; site < FI_NSITES; site++) {
        memset(md_mem, 0, (size_t)MD_BLOCKS * PAGE_SIZE);
        pts[0][site] = mfi_sweep("blank", site, 1, 1);   /* 格式化,建立 mfi,正常卸載 */
        pts[1][site] = mfi_sweep("clean", site, 0, 0);   /* 掛載正常卸載過的;離開時模擬斷電 */
        pts[2][site] = mfi_sweep("dirty", site, 0, 1);   /* 斷電後的掛載(含 wfs_check(1)) */
    }
    /* pmm:blank 4(pool、nodes、scratch、csum)、clean 4、dirty 7;kmalloc 0 — 數目變了就要重新審查 */
    if (pts[0][FI_PMM] != 4 || pts[1][FI_PMM] != 4 || pts[2][FI_PMM] != 7 ||
        pts[0][FI_KMALLOC] || pts[1][FI_KMALLOC] || pts[2][FI_KMALLOC])
        panic("wfs mount failinj: unexpected allocation points pmm %u/%u/%u kmalloc %u/%u/%u",
              pts[0][FI_PMM], pts[1][FI_PMM], pts[2][FI_PMM],
              pts[0][FI_KMALLOC], pts[1][FI_KMALLOC], pts[2][FI_KMALLOC]);
    /* 唯讀狀態不延續到下一次掛載:寫入磁碟失敗使檔案系統轉為唯讀,卸載之後磁碟恢復正常,
       重新掛載必須可以寫入(以前掛載與卸載都不重設 fs_failed,新的掛載一開始就拒絕所有寫入) */
    {
        struct inode *root, *f;
        struct wfs_stats st;
        KASSERT(wfs_mount(&md_dev, &root) == 0);
        wfs_stats(&st);
        KASSERT(!st.failed);
        KASSERT(root->ops->create(root, "ro", I_FILE, &f) == 0);
        KASSERT(root->ops->write(f, "read-only-test", 14, 0) == 14);
        KASSERT(f->refs == 0);
        md_fail_writes = 1;                      /* 卸載時的寫回失敗 → 檔案系統轉為唯讀 */
        KASSERT(wfs_test_unmount(1) == -E_IO);
        md_fail_writes = 0;
        KASSERT(wfs_mount(&md_dev, &root) == 0);
        wfs_stats(&st);
        if (st.failed)
            panic("wfs: the read-only state of the previous mount carried over to a new mount");
        KASSERT(root->ops->create(root, "ro2", I_FILE, &f) == 0);
        KASSERT(root->ops->write(f, "writable-again", 14, 0) == 14);
        KASSERT(f->refs == 0);
        KASSERT(wfs_test_unmount(1) == 0);
        KASSERT(wfs_mount(&md_dev, &root) == 0);
        char buf[16] = { 0 };
        KASSERT(root->ops->lookup(root, "ro2", &f) == 0);
        KASSERT(root->ops->read(f, buf, sizeof(buf), 0) == 14 && memcmp(buf, "writable-again", 14) == 0);
        KASSERT(f->refs == 0);
        KASSERT(wfs_test_unmount(1) == 0);
    }
    wfs_mount_busy_test();                       /* I-338 (1) */
    wfs_csum_inval_test();                       /* I-338 (2) */
    pmm_free_pages(md_mem, MD_BLOCKS);
    md_mem = NULL;
    sched_trim();                                /* I-338 (1) 的兩條執行緒:join 之後結構與 kernel stack 進了池子 */
    mfi_take(&after);
    /* 唯一允許的差額:這段期間新的 kernel stack 落在還沒拆過的 2MB 直接對映區,保護頁把大分頁拆開,
       多出的那張頁表目前不會合併回去(I-339,實測大分頁數 60 -> 59 時剛好少 1 頁)。差額必須剛好等於它 */
    size_t split_bytes = (size_t)(before.large - after.large) * PAGE_SIZE;
    if (after.large > before.large || before.avail != after.avail + split_bytes)
        panic("wfs mount failinj: whole test leaked: available %zu->%zu bytes (pmm %zu->%zu heap used %zu->%zu, "
              "large pages %lu->%lu)", before.avail, after.avail, before.pmm_free, after.pmm_free,
              before.heap_used, after.heap_used, before.large, after.large);
    kprintf("wfs: mount/format allocation-failure sweep passed (pmm points %u/%u/%u, blank/clean/dirty); "
            "read-only state does not carry over to a new mount\n",
            pts[0][FI_PMM], pts[1][FI_PMM], pts[2][FI_PMM]);
}

void disk_demo(void)
{
    struct wfs_stats s0, s1, s2;
    struct file *f, *g;
    struct stat st;
    struct dirent e;
    char buf[16];

    wfs_stats(&s0);
    if (!s0.mounted) {
        kprintf("disk: no file system mounted, tests skipped\n");
        return;
    }
    /* 上一次測試如果中途被打斷,可能留下東西:先清掉。順便讓根目錄至少有一個區塊,
       之後的空間核對才有固定的基準 */
    static const char *const leftovers[] = { "/disk/selftest/big", "/disk/selftest/prog", "/disk/selftest" };
    for (unsigned i = 0; i < 3; i++) {
        int r = vfs_unlink(leftovers[i]);
        KASSERT(r == 0 || r == -E_NOENT);        /* 刪掉了,或本來就沒有 */
    }
    KASSERT(vfs_mkdir("/disk/selftest") == 0 && vfs_unlink("/disk/selftest") == 0);
    wfs_stats(&s0);

    /* 目錄與路徑:掛載點兩邊都走得通 */
    KASSERT(vfs_mkdir("/disk/selftest") == 0);
    KASSERT(vfs_mkdir("/disk/selftest") == -E_EXIST);
    KASSERT(vfs_stat("/disk/selftest/..", &st) == 0 && st.type == I_DIR);
    KASSERT(vfs_stat("/disk/../etc/motd", &st) == 0 && st.type == I_FILE);   /* 從磁碟往上回到記憶體檔案系統 */
    KASSERT(vfs_stat("/disk/selftest/../../disk/selftest", &st) == 0);
    KASSERT(vfs_unlink("/disk") == -E_BUSY);                                 /* 掛載點不可以刪除 */
    KASSERT(vfs_mkdev("/disk/selftest/dev", console_dev_ops()) == -E_INVAL); /* 裝置節點只能在記憶體裡 */
    wfs_stats(&s1);
    KASSERT(s1.free_inodes == s0.free_inodes - 1);

    /* 一個 100000 位元組的檔案:25 個資料區塊,超過 10 個之後要用到間接區塊 */
    disk_write_file("/disk/selftest/big", 100000, 1);
    disk_verify_file("/disk/selftest/big", 100000, 1);
    wfs_stats(&s2);
    /* 25 個資料 + 1 個間接 + 1 個給 selftest 目錄放紀錄(目錄也是存在區塊裡的) */
    KASSERT(s2.free_blocks == s1.free_blocks - 27);
    KASSERT(s2.free_inodes == s1.free_inodes - 1);
    KASSERT(vfs_stat("/disk/selftest/big", &st) == 0 && st.size == 100000 && st.nlink == 1);
    KASSERT(vfs_unlink("/disk/selftest") == -E_NOTEMPTY);

    /* 列出目錄:只有這一個檔案 */
    KASSERT(vfs_open("/disk/selftest", O_RDONLY, &f) == 0);
    KASSERT(vfs_readdir(f, &e) == 0 && !strcmp(e.name, "big") && e.type == I_FILE);
    KASSERT(vfs_readdir(f, &e) == 1);
    vfs_close(f);

    /* 換成比較短的內容:多出來的區塊要歸還 */
    disk_write_file("/disk/selftest/big", 5000, 2);
    disk_verify_file("/disk/selftest/big", 5000, 2);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 3);       /* 2 個資料 + 目錄的 1 個 */

    /* 附加:接在最後 */
    KASSERT(vfs_open("/disk/selftest/big", O_WRONLY | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "tail", 4) == 4);
    vfs_close(f);
    KASSERT(vfs_open("/disk/selftest/big", O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == 5004 && vfs_pread(f, buf, 8, 5000) == 4 && !memcmp(buf, "tail", 4));

    /* 開著的時候刪除:名稱立刻消失,內容留到關閉為止(和記憶體檔案系統相同的規則) */
    KASSERT(vfs_unlink("/disk/selftest/big") == 0);
    KASSERT(vfs_stat("/disk/selftest/big", &st) == -E_NOENT);
    KASSERT(vfs_open("/disk/selftest/big", O_RDONLY, &g) == -E_NOENT);
    KASSERT(vfs_pread(f, buf, 4, 5000) == 4 && !memcmp(buf, "tail", 4));
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 3);       /* 還沒釋放 */
    vfs_close(f);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 1 && s2.free_inodes == s1.free_inodes);

    /* 多條執行緒同時使用:各自建立、寫入、讀回、刪除自己的檔案 */
    int tid[DISK_WORKERS];
    for (int i = 0; i < DISK_WORKERS; i++)
        tid[i] = thread_create("disk-worker", disk_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < DISK_WORKERS; i++)
        thread_join(tid[i]);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 1 && s2.free_inodes == s1.free_inodes);

    /* 檢查碼:把一個檔案的資料在磁碟上翻轉一個位元(模擬儲存媒體的位元錯誤)。
       讀取必須失敗,而不是把壞掉的內容交出來;巡檢也要找得到它 */
    wfs_stats(&s2);
    if (s2.checksums) {
        static char cbuf[3000];
        uint32_t pos = 1;
        unsigned bad = 0;
        long n;

        disk_write_file("/disk/selftest/flip", 9000, 5);
        KASSERT(wfs_test_drop_cache() == 0);
        KASSERT(wfs_scrub(&pos, wfs_block_count()) == 0);            /* 目前整顆磁碟都是好的 */
        KASSERT(vfs_open("/disk/selftest/flip", O_RDONLY, &f) == 0);
        uint32_t blk = wfs_test_file_block(vfs_file_inode(f), 1);    /* 檔案的第 2 個區塊 */
        KASSERT(blk != 0);
        KASSERT(blk_fault_flip((uint64_t)blk * 8 + 3, 100, 5) == 0);
        while ((n = vfs_read(f, cbuf, sizeof(cbuf))) > 0)
            ;
        KASSERT(n == -E_IO);                                         /* 讀到壞掉的那一塊:拒絕 */
        vfs_close(f);
        wfs_stats(&s2);
        KASSERT(s2.csum_errors >= 1);
        pos = 1;
        for (uint32_t done = 0; done < wfs_block_count(); done += 256)
            bad += wfs_scrub(&pos, 256);
        KASSERT(bad == 1);                                           /* 巡檢找到的正好是那一塊 */
        /* 檔案系統本身沒有壞:刪掉那個檔案(壞掉的區塊被釋放)之後,巡檢又是乾淨的 */
        KASSERT(vfs_unlink("/disk/selftest/flip") == 0);
        pos = 1;
        KASSERT(wfs_scrub(&pos, wfs_block_count()) == 0);
        kprintf("disk: a single flipped bit on the disk was detected: the read failed with an I/O "
                "error and the scrub located 1 bad block among %lu checked\n",
                (unsigned long)(s2.scrub_blocks ? s2.scrub_blocks : 1));
    }

    disk_failinj_test();

    /* 全部刪除後,空間回到測試之前;再做一次完整檢查,應該找不到任何問題 */
    KASSERT(vfs_unlink("/disk/selftest") == 0);
    KASSERT(vfs_sync() == 0);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s0.free_blocks && s2.free_inodes == s0.free_inodes);
    KASSERT(wfs_check_now() == 0);
    kprintf("disk: ok (100000-byte file through an indirect block, truncate, append, unlink while "
            "open, %d threads x %d files; %lu block reads, %lu block writes; "
            "consistency check found nothing)\n", DISK_WORKERS, DISK_FILES,
            s2.disk_reads - s0.disk_reads, s2.disk_writes - s0.disk_writes);
}

/*
 * 跨開機的測試(回歸測試依序以 disktest=1 ~ 5 開機五次,用同一個磁碟映像):
 *   1  寫入一個檔案,正常關機
 *   2  確認檔案還在、內容正確(資料真的留在磁碟上);然後故意把檔案系統弄壞,不正常關機
 *   3  確認掛載時發現並修復了問題,原本的檔案完好
 *   4  磁碟錯誤:讀取失敗只影響那一次操作;寫入失敗之後檔案系統轉為唯讀
 *   5  確認上一次的唯讀狀態讓這次掛載做了檢查,檔案完好,又可以寫入;刪除測試檔
 *   6  黑盒子:留下一筆註記,然後故意 panic(這一階段預期以失敗的結束碼結束)
 *   7  確認黑盒子裡有上一次的註記與 panic 的訊息;把最新的一筆弄壞,確認它被認出來並略過
 *   8  寫一個檔案、正常卸載,然後把磁碟上的 superblock(區塊 0)整個清成 0
 *   9  確認掛載時改用備份的 superblock 並修好了區塊 0(而不是把磁碟當成新的重新格式化),檔案完好
 */
void disk_stage_test(const char *cmd)
{
    struct wfs_stats s;
    struct stat st;

    wfs_stats(&s);
    if (cmdline_has(cmd, "disktest=1")) {
        KASSERT(s.mounted);
        vfs_unlink("/disk/persist");
        disk_write_file("/disk/persist", 9000, 3);
        KASSERT(vfs_chown("/disk/persist", 1000, 1001) == 0 && vfs_chmod("/disk/persist", 0640) == 0);
        kprintf("disktest 1: wrote /disk/persist (9000 bytes, owner 1000:1001, mode 640)\n");
    } else if (cmdline_has(cmd, "disktest=2")) {
        KASSERT(s.mounted && s.repairs == 0);
        disk_verify_file("/disk/persist", 9000, 3);
        /* 修改時間也存在磁碟上:是上一次開機時寫入的時間,不會比現在晚 */
        KASSERT(vfs_stat("/disk/persist", &st) == 0 && st.mtime > 1700000000ull &&
                st.mtime <= timer_epoch(NULL));
        /* 擁有者與權限也是:重新開機後從磁碟讀回來的和當初設定的相同 */
        KASSERT(st.uid == 1000 && st.gid == 1001 && st.mode == 0640);
        KASSERT(wfs_test_damage("persist") == 0);
        kprintf("disktest 2: /disk/persist survived the reboot; damaged the file system on purpose, "
                "exiting without a clean shutdown\n");
        arch_exit(0);
    } else if (cmdline_has(cmd, "disktest=3")) {
        KASSERT(s.mounted);
        kprintf("disktest 3: the mount-time check repaired %u problem(s)\n", s.repairs);
        KASSERT(s.repairs >= 4);
        disk_verify_file("/disk/persist", 9000, 3);
        KASSERT(vfs_stat("/disk/persist", &st) == 0 && st.nlink == 1);
        KASSERT(vfs_stat("/disk/ghost", &st) == -E_NOENT);
        KASSERT(wfs_check_now() == 0);
        kprintf("disktest 3: /disk/persist is intact; a second check found nothing\n");
    } else if (cmdline_has(cmd, "disktest=4")) {
        struct file *f;
        char buf[64];
        unsigned long n0 = blk_fault_count();

        KASSERT(s.mounted && s.repairs == 0 && !s.failed);
        /* 讀取失敗:清空快取之後讀檔案,得到 -E_IO;磁碟恢復之後同一個檔案照常可讀 */
        KASSERT(wfs_test_drop_cache() == 0);
        blk_fault_set(1, 0);
        KASSERT(vfs_open("/disk/persist", O_RDONLY, &f) == -E_IO ||
                (vfs_read(f, buf, sizeof(buf)) == -E_IO && (vfs_close(f), 1)));
        KASSERT(blk_fault_count() > n0);
        blk_fault_set(0, 0);
        disk_verify_file("/disk/persist", 9000, 3);
        wfs_stats(&s);
        KASSERT(!s.failed);                              /* 讀取失敗不影響檔案系統的狀態 */

        /* 寫入成功(進了快取)、之後寫回磁碟才失敗:這個錯誤要傳得回來。
           write 回報成功時資料還在記憶體裡;fsync 與關閉時的寫回都必須回報 -E_IO,
           而且磁碟恢復之後也不會變回成功(見 wfs_flush) */
        KASSERT(vfs_open("/disk/wtest", O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
        KASSERT(vfs_fsync(f) == 0);                      /* 目前還是好的 */
        KASSERT(vfs_write(f, "written to the cache", 20) == 20);
        blk_fault_set(0, 1);
        KASSERT(vfs_fsync(f) == -E_IO);
        blk_fault_set(0, 0);
        KASSERT(vfs_fsync(f) == -E_IO && vfs_flush(f) == -E_IO);
        vfs_close(f);
        wfs_stats(&s);
        KASSERT(s.failed);

        /* 寫入失敗之後:建立目錄得到 -E_IO,檔案系統已經是唯讀 */
        blk_fault_set(0, 1);
        KASSERT(vfs_mkdir("/disk/broken") == -E_IO);
        blk_fault_set(0, 0);
        wfs_stats(&s);
        KASSERT(s.failed);
        /* 磁碟恢復了也一樣:記憶體和磁碟已經對不上,不再接受任何修改 */
        KASSERT(vfs_mkdir("/disk/again") == -E_IO);
        KASSERT(vfs_unlink("/disk/persist") == -E_IO);
        KASSERT(vfs_open("/disk/persist", O_WRONLY | O_APPEND, &f) == 0);
        KASSERT(vfs_write(f, "x", 1) == -E_IO);
        vfs_close(f);
        KASSERT(vfs_open("/disk/new", O_CREAT | O_WRONLY, &f) == -E_IO);
        KASSERT(vfs_rename("/disk/persist", "/disk/moved") == -E_IO);
        disk_verify_file("/disk/persist", 9000, 3);      /* 讀取照常 */
        /* 超出範圍的讀寫要求:驅動程式拒絕,不會碰到別的地方 */
        struct blkdev *dev = arch_blk_probe_cached();
        KASSERT(dev && dev->rw(dev, dev->sectors, 8, buf, 0) != 0);
        KASSERT(dev->rw(dev, dev->sectors - 1, 8, buf, 0) != 0);
        kprintf("disktest 4: a read error only failed that one read; after a write error the file "
                "system refused every change but could still be read\n");
    } else if (cmdline_has(cmd, "disktest=5")) {
        KASSERT(s.mounted && !s.failed);
        /* 上一次因為寫入失敗沒有記「正常卸載」:這次掛載時一定做過檢查 */
        kprintf("disktest 5: after the write error the mount-time check ran (%u repair(s))\n", s.repairs);
        disk_verify_file("/disk/persist", 9000, 3);
        KASSERT(vfs_stat("/disk/broken", &st) == -E_NOENT && vfs_stat("/disk/again", &st) == -E_NOENT);
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_mkdir("/disk/ok") == 0 && vfs_unlink("/disk/ok") == 0);      /* 又可以寫入了 */
int wt = vfs_unlink("/disk/wtest");              /* 上一階段建立的;內容沒寫成功,但名稱可能已經在 */
        KASSERT(wt == 0 || wt == -E_NOENT);
                KASSERT(vfs_unlink("/disk/persist") == 0);
        kprintf("disktest 5: /disk/persist is intact and the file system is writable again\n");
    } else if (cmdline_has(cmd, "disktest=6")) {
        KASSERT(s.mounted && blackbox_enabled());
        blackbox_log(BB_NOTE, "stage 6 marker %d", 4711);
        kprintf("disktest 6: left a note in the black box; now panicking on purpose\n");
        panic("black box test panic %d", 77);
    } else if (cmdline_has(cmd, "disktest=7")) {
        struct bb_record r;
        int saw_panic = 0, saw_note = 0, saw_boot = 0;
        uint32_t prev_seq = 0;

        KASSERT(s.mounted && blackbox_enabled());
        /* 由新到舊走一遍:序號遞減;找得到上一次開機的註記、panic 的訊息、以及開機紀錄 */
        for (unsigned i = 0; blackbox_read(i, &r) == 0; i++) {
            KASSERT(i == 0 || r.seq < prev_seq);
            prev_seq = r.seq;
            KASSERT(r.len <= BB_TEXT_MAX && r.time > 1700000000ull);
            if (r.boot == s.mounts - 1) {
                saw_panic |= r.type == BB_PANIC && !memcmp(r.text, "black box test panic 77", 23);
                saw_note |= r.type == BB_NOTE && !memcmp(r.text, "stage 6 marker 4711", 19);
                saw_boot |= r.type == BB_BOOT;
            }
        }
        KASSERT(saw_panic && saw_note && saw_boot);
        /* 最新的一筆是這次的開機紀錄。把它在磁碟上翻轉一個位元:檢查碼不符,載入時被略過 */
        unsigned before = blackbox_count();
        KASSERT(blackbox_read(0, &r) == 0 && r.type == BB_BOOT && r.boot == s.mounts);
        KASSERT(blackbox_test_corrupt_newest() == 0);
        KASSERT(blackbox_count() == before - 1);
        KASSERT(blackbox_read(0, &r) == 0 && r.boot == s.mounts - 1);   /* 現在最新的是上一次開機留下的 */
        blackbox_log(BB_NOTE, "stage 7 done");
        KASSERT(blackbox_read(0, &r) == 0 && !memcmp(r.text, "stage 7 done", 12));
        kprintf("disktest 7: the black box kept the note and the panic message across the reboot; "
                "a record with one flipped bit was rejected\n");
    } else if (cmdline_has(cmd, "disktest=8")) {
        static uint8_t zero[512];
        struct blkdev *dev = arch_blk_probe_cached();

        KASSERT(s.mounted && s.checksums && !s.sb_restored);
        vfs_unlink("/disk/sbtest");
        disk_write_file("/disk/sbtest", 20000, 9);
        blackbox_log(BB_SHUTDOWN, "clean shutdown before the superblock test");
        wfs_shutdown();                                  /* 正常卸載:兩份 superblock 都是最新的 */
        /* 把區塊 0 的 8 個磁區全部清成 0:最壞的情況,看起來就像一顆全新的磁碟 */
        for (unsigned i = 0; i < 8; i++)
            KASSERT(dev->rw(dev, i, 1, zero, 1) == 0);
        kprintf("disktest 8: wrote /disk/sbtest, unmounted, then wiped the primary superblock\n");
        arch_exit(0);
    } else if (cmdline_has(cmd, "disktest=9")) {
        KASSERT(s.mounted && s.checksums && s.sb_restored);
        disk_verify_file("/disk/sbtest", 20000, 9);      /* 沒有被重新格式化:檔案還在而且正確 */
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_unlink("/disk/sbtest") == 0);
        kprintf("disktest 9: the backup superblock was used and the primary repaired; "
                "/disk/sbtest is intact\n");
    } else if (cmdline_has(cmd, "disktest=10")) {
        /* 間接區塊讀不到時刪除檔案:不可以把它底下的資料區塊弄丟。
           預期:什麼都不釋放、檔案系統轉為唯讀;下一次開機的檢查把整個檔案的空間收回來(第 11 階段) */
        struct wfs_stats before, after;
        struct file *f;
        uint32_t base;

        KASSERT(s.mounted && s.checksums && !s.failed);
        vfs_unlink("/disk/indbase");
        vfs_unlink("/disk/indfile");
        KASSERT(vfs_open("/disk/indbase", O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
        KASSERT(vfs_write(f, &base, sizeof(base)) == sizeof(base));      /* 先佔好位置 */
        vfs_close(f);
        KASSERT(vfs_sync() == 0);
        wfs_stats(&before);
        base = before.free_blocks;                       /* 基準:indfile 還不存在時的可用區塊數 */
        KASSERT(vfs_open("/disk/indbase", O_WRONLY, &f) == 0);
        KASSERT(vfs_write(f, &base, sizeof(base)) == sizeof(base));
        vfs_close(f);
        disk_write_file("/disk/indfile", 12 * PAGE_SIZE, 11);            /* 12 個區塊:用到間接區塊 */
        KASSERT(wfs_test_drop_cache() == 0);
        wfs_stats(&before);
        KASSERT(before.free_blocks == base - 13);        /* 12 個資料 + 1 個間接 */
        KASSERT(vfs_open("/disk/indfile", O_RDONLY, &f) == 0);
        uint32_t ind = wfs_test_file_block(vfs_file_inode(f), 0xFFFFFFFFu);
        vfs_close(f);
        KASSERT(ind != 0 && wfs_test_drop_cache() == 0);
        KASSERT(blk_fault_flip((uint64_t)ind * 8, 100, 5) == 0);
        KASSERT(vfs_unlink("/disk/indfile") == 0);       /* 名稱拿掉了(刪除本身成功),但內容釋放不了 */
        wfs_stats(&after);
        KASSERT(after.failed && after.csum_errors >= 1);
        KASSERT(after.free_blocks == before.free_blocks);                /* 一個區塊都沒有被釋放 */
        KASSERT(vfs_mkdir("/disk/no") == -E_IO);         /* 已經轉為唯讀 */
        kprintf("disktest 10: the indirect block of a 12-block file was unreadable when the file was "
                "deleted; nothing was freed and the file system went read-only\n");
    } else if (cmdline_has(cmd, "disktest=11")) {
        struct file *f;
        uint32_t base = 0;

        KASSERT(s.mounted && !s.failed && s.repairs >= 1);               /* 開機時的檢查處理過了 */
        KASSERT(vfs_stat("/disk/indfile", &st) == -E_NOENT);
        KASSERT(vfs_open("/disk/indbase", O_RDONLY, &f) == 0);
        KASSERT(vfs_read(f, &base, sizeof(base)) == sizeof(base));
        vfs_close(f);
        KASSERT(s.free_blocks == base);                  /* 13 個區塊全部收回來了,沒有遺失 */
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_unlink("/disk/indbase") == 0);
        kprintf("disktest 11: the mount-time check reclaimed all 13 blocks of the deleted file\n");
    }
}

/*
 * 第 12~15 階段(第 50 步,D5 / ISSUES N-003):使用者程式在「寫進快取成功、寫回磁碟失敗」時
 * 必須回報失敗。以前只在 kernel 裡直接呼叫 VFS 驗證過 fsync 會回報錯誤,從沒有讓真的程式
 * 在這種情況下跑過 — 程式若忽略 fsync / close 的結果,使用者會以為檔案存好了。
 *   12  cp      複製到磁碟,寫回失敗           -> 結束碼不是 0
 *   13  sh      「echo ... > 磁碟上的檔案」      -> 整行的結束碼不是 0
 *   14  tftp    下載到磁碟(需要網路)          -> 結束碼不是 0
 *   15  對照:磁碟正常時三者都成功,內容正確
 * 每一階段之後檔案系統都轉為唯讀(寫入失敗過),下一次開機的檢查讓它恢復,所以各自一次開機。
 */
static int run_prog(const char *path, const char *const argv[])
{
    int pid = proc_spawn(path, argv);
    KASSERT(pid > 0);
    return proc_wait(pid);
}

/* 先在磁碟上建好(空的)目的檔並同步:之後注入的寫入失敗只會落在「寫回內容」這一步,
   而不是建立檔案的時候(建立本身也要寫磁碟,那條路徑已由第 4 階段涵蓋) */
static void precreate(const char *path)
{
    struct file *f;
    KASSERT(vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
    vfs_close(f);
    KASSERT(vfs_sync() == 0);
}

static void write_tmp(const char *path, const char *text)
{
    int mk = vfs_mkdir("/tmp");                  /* 開機到這裡時 /tmp 不一定已經建立 */
    KASSERT(mk == 0 || mk == -E_EXIST);
    struct file *f;
    size_t len = strlen(text);
    KASSERT(vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
    KASSERT(vfs_write(f, text, len) == (long)len);
    vfs_close(f);
}

/* 讀出一個小檔案的內容(以 0 結尾),供核對 shell 腳本寫下的結果 */
static void read_small(const char *path, char *buf, size_t cap)
{
    struct file *f;
    KASSERT(vfs_open(path, O_RDONLY, &f) == 0);
    long n = vfs_pread(f, buf, cap - 1, 0);
    vfs_close(f);
    KASSERT(n >= 0);
    buf[n] = '\0';
}

/* 在「資料區塊寫不進去」的情況下執行一個程式,回傳它的結束碼;之後檔案系統必須已經轉為唯讀。
   metadata(inode 表、位元圖)照常寫得進去:開檔、截斷都會成功,失敗只發生在內容寫回的時候 —
   也就是 write 回報成功、資料還在快取裡,要到 fsync / close 才知道沒存好的那種情況 */
static int run_with_write_faults(const char *path, const char *const argv[])
{
    struct wfs_stats s;
    unsigned long n0 = blk_fault_count();

    wfs_stats(&s);
    blk_fault_set_write_from((uint64_t)s.data_start * (PAGE_SIZE / BLK_SECTOR));
    int rc = run_prog(path, argv);
    blk_fault_set(0, 0);
    wfs_stats(&s);
    KASSERT(blk_fault_count() > n0 && s.failed);     /* 確實寫回過、而且失敗了 */
    return rc;
}

void disk_user_stage_test(const char *cmd)
{
    struct wfs_stats s;
    struct stat st;

    wfs_stats(&s);
    if (cmdline_has(cmd, "disktest=12")) {
        KASSERT(s.mounted && !s.failed);
        precreate("/disk/cpfail");
        int rc = run_with_write_faults("/bin/cp", (const char *const[]){ "cp", "/bin/echo", "/disk/cpfail", NULL });
        kprintf("disktest 12: cp to the disk while write-back failed exited with %d\n", rc);
        KASSERT(rc != 0);
    } else if (cmdline_has(cmd, "disktest=13")) {
        KASSERT(s.mounted && !s.failed);
        precreate("/disk/shfail");
        write_tmp("/tmp/save.sh", "echo saved > /disk/shfail\n");
        int rc = run_with_write_faults("/bin/sh", (const char *const[]){ "sh", "/tmp/save.sh", NULL });
        kprintf("disktest 13: the shell redirect to the disk while write-back failed exited with %d\n", rc);
        KASSERT(rc != 0);
    } else if (cmdline_has(cmd, "disktest=14")) {
        KASSERT(s.mounted && !s.failed);
        precreate("/disk/tfail");
        int rc = run_with_write_faults("/bin/tftp", (const char *const[]){ "tftp", "etc/motd", "/disk/tfail", NULL });
        kprintf("disktest 14: tftp to the disk while write-back failed exited with %d\n", rc);
        KASSERT(rc != 0);
    } else if (cmdline_has(cmd, "disktest=16")) {
        /* I-318:背景工作的重新導向。寫回失敗時,不能因為 shell 沒有等它就當成成功:
           背景工作結束、被回收(sleep 那一行之前就回收了)之後,wait 仍要回報非 0 */
        KASSERT(s.mounted && !s.failed);
        precreate("/disk/bgfail");
        write_tmp("/tmp/bg.sh", "echo saved > /disk/bgfail &\nsleep 300ms\nwait\necho $? > /tmp/w16\n");
        int rc = run_with_write_faults("/bin/sh", (const char *const[]){ "sh", "/tmp/bg.sh", NULL });
        char w[16];
        read_small("/tmp/w16", w, sizeof(w));
        kprintf("disktest 16: background redirect while write-back failed: wait returned %s", w);
        KASSERT(rc == 0 && strcmp(w, "0\n") != 0);
    } else if (cmdline_has(cmd, "disktest=15")) {
        KASSERT(s.mounted && !s.failed);
        /* 對照:同樣三個命令在磁碟正常時都成功,檔案確實存在 */
        write_tmp("/tmp/save.sh", "echo saved > /disk/shfail\n");
        int r1 = run_prog("/bin/cp", (const char *const[]){ "cp", "/bin/echo", "/disk/cpfail", NULL });
        int r2 = run_prog("/bin/sh", (const char *const[]){ "sh", "/tmp/save.sh", NULL });
        int r3 = run_prog("/bin/tftp", (const char *const[]){ "tftp", "etc/motd", "/disk/tfail", NULL });
        kprintf("disktest 15: exit codes cp %d, sh %d, tftp %d\n", r1, r2, r3);
        KASSERT(r1 == 0 && r2 == 0 && r3 == 0);
        struct stat src;
        KASSERT(vfs_stat("/bin/echo", &src) == 0 && vfs_stat("/disk/cpfail", &st) == 0 && st.size == src.size);
        KASSERT(vfs_stat("/disk/shfail", &st) == 0 && st.size == 6);         /* "saved\n" */
        KASSERT(vfs_stat("/disk/tfail", &st) == 0 && st.size > 0);
        KASSERT(vfs_unlink("/disk/cpfail") == 0 && vfs_unlink("/disk/shfail") == 0 && vfs_unlink("/disk/tfail") == 0);
        wfs_stats(&s);
        KASSERT(!s.failed);
        kprintf("disktest 15: with a healthy disk cp, the shell redirect and tftp all succeeded\n");

        /* I-318 對照:兩個背景工作各寫一個檔案,wait 回報 0、內容都完整 */
        write_tmp("/tmp/bg.sh", "echo one > /disk/bg1 &\necho two > /disk/bg2 &\nwait\necho $? > /tmp/w15\n");
        int r4 = run_prog("/bin/sh", (const char *const[]){ "sh", "/tmp/bg.sh", NULL });
        char w[16];
        read_small("/tmp/w15", w, sizeof(w));
        KASSERT(r4 == 0 && strcmp(w, "0\n") == 0);
        KASSERT(vfs_stat("/disk/bg1", &st) == 0 && st.size == 4 && vfs_stat("/disk/bg2", &st) == 0 && st.size == 4);
        KASSERT(vfs_unlink("/disk/bg1") == 0 && vfs_unlink("/disk/bg2") == 0);
        /* 輸出還沒確認寫回就 exit:shell 不能以 0 結束(背景工作仍在跑,不替它宣稱存好了) */
        write_tmp("/tmp/bg.sh", "sleep 1 > /tmp/slow &\nexit\n");
        int r5 = run_prog("/bin/sh", (const char *const[]){ "sh", "/tmp/bg.sh", NULL });
        kprintf("disktest 15: two background redirects, wait 0; exit with output pending -> %d\n", r5);
        KASSERT(r5 != 0);
        wfs_stats(&s);
        KASSERT(!s.failed);
    }
}

/* ------------------------------------------------------------------ */
/* 檔案系統的示範與測試(kernel 內部直接呼叫 VFS)                        */
/* ------------------------------------------------------------------ */

/* 遞迴列出目錄的內容(印在同一行) */
static void fs_list(const char *path, int depth)
{
    struct file *d;
    struct dirent e;

    if (depth > 4 || vfs_open(path, O_RDONLY, &d) != 0)
        return;
    while (vfs_readdir(d, &e) == 0) {
        char sub[FS_PATH_MAX + 1];
        size_t n = 0;
        for (const char *p = path; *p && n < FS_PATH_MAX; p++)
            sub[n++] = *p;
        if (n > 1 && n < FS_PATH_MAX)
            sub[n++] = '/';
        for (const char *p = e.name; *p && n < FS_PATH_MAX; p++)
            sub[n++] = *p;
        sub[n] = '\0';
        kprintf(" %s%s", sub, e.type == I_DIR ? "/" : "");
        if (e.type == I_DIR)
            fs_list(sub, depth + 1);
    }
    vfs_close(d);
}

#define FS_WORKERS 4
#define FS_FILES   25
#define FS_RECORD  32u                       /* 共用紀錄檔中每一筆的長度 */

/* 多條執行緒同時在同一個目錄建立、寫入、讀回、刪除各自的檔案,
   並各自往同一個紀錄檔附加固定長度的紀錄 */
static void fs_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    char path[32] = "/t/w0-00", rec[FS_RECORD], back[600], data[600];
    struct file *f, *log;

    KASSERT(vfs_open("/t/log", O_WRONLY | O_APPEND, &log) == 0);
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (char)(id * 31 + i);
    for (int n = 0; n < FS_FILES; n++) {
        path[4] = (char)('0' + id);
        path[6] = (char)('0' + n / 10);
        path[7] = (char)('0' + n % 10);
        KASSERT(vfs_open(path, O_CREAT | O_RDWR, &f) == 0);
        KASSERT(vfs_write(f, data, sizeof(data)) == (long)sizeof(data));
        KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
        KASSERT(memcmp(data, back, sizeof(data)) == 0);
        vfs_close(f);
        KASSERT(vfs_unlink(path) == 0);

        /* 一筆紀錄全部是同一個字元:之後檢查時,混到別人的內容就看得出來 */
        memset(rec, 'A' + id, sizeof(rec));
        KASSERT(vfs_write(log, rec, sizeof(rec)) == (long)sizeof(rec));
    }
    vfs_close(log);
}

/* ramfs 區塊表換大時只記新舊兩表的差額(審查發現:原本先記整張新表,剩餘額度介於
   「差額」與「新表」之間時檔案長不大,得到假的 E_NOSPC)。換上自己的 FS domain,
   把剩餘額度精確設成「差額 + 1 頁資料」:寫第 513 頁(表 512 → 1024 格)必須成功,
   之後額度剛好用完、下一頁 E_NOSPC;刪檔後帳回到原值 */
/* I-335:配置失敗注入掃過 ramfs 的每一個配置點。對每個操作、每個地點(kmalloc / pmm),
   令第 N 次配置失敗(N = 1, 2, ...),直到操作不再碰到注入為止。每次失敗都要:
   回傳 -E_NOMEM(或寫入時回報已寫的部分),而且 ramfs 頁數、FS 帳、heap 使用量完全回到原值 */
struct fi_snap { unsigned long pages, fs; size_t heap_used; };

static struct mb_domain *fi_fsdom(void)
{
    struct mb_domain *d = ramfs_set_domain(NULL);
    ramfs_set_domain(d);
    return d;
}

static void fi_take(struct fi_snap *x)
{
    struct mb_stats st;
    struct kheap_stats h;
    mb_stats(fi_fsdom(), &st);
    kheap_stats(&h);
    x->pages = ramfs_pages();
    x->fs = st.total;
    x->heap_used = h.heap_bytes - h.free_bytes;
}

static void fi_same(const struct fi_snap *a, const char *what, int site, unsigned n)
{
    struct fi_snap b;
    fi_take(&b);
    if (a->pages != b.pages || a->fs != b.fs || a->heap_used != b.heap_used)
        panic("fs: failinj %s site %d #%u leaked: pages %lu->%lu fs %lu->%lu heap %zu->%zu", what, site, n,
              a->pages, b.pages, a->fs, b.fs, a->heap_used, b.heap_used);
}

static void fs_failinj_test(void)
{
    static char data[3 * PAGE_SIZE];
    unsigned points[2][3] = { { 0 } };
    for (int site = 0; site < FI_NSITES; site++) {
        /* 1. 建立檔案 */
        for (unsigned n = 1;; n++) {
            KASSERT(n < 100);
            struct fi_snap a;
            struct file *f = NULL;
            fi_take(&a);
            failinj_arm((enum fi_site)site, n);
            int r = vfs_open("/fiA", O_CREAT | O_WRONLY, &f);
            int fired = failinj_fired();
            failinj_disarm();
            if (!fired) {
                KASSERT(r == 0);
                vfs_close(f);
                KASSERT(vfs_unlink("/fiA") == 0);
                fi_same(&a, "create(cleanup)", site, n);
                points[site][0] = n - 1;
                break;
            }
            if (r != -E_NOMEM)
                panic("fs: failinj create site %d #%u returned %d, want -E_NOMEM", site, n, r);
            fi_same(&a, "create", site, n);
        }
        /* 2. 寫 3 頁到空檔案 */
        for (unsigned n = 1;; n++) {
            KASSERT(n < 100);
            struct file *f;
            KASSERT(vfs_open("/fiB", O_CREAT | O_RDWR | O_TRUNC, &f) == 0);
            struct fi_snap a;
            fi_take(&a);
            failinj_arm((enum fi_site)site, n);
            long w = vfs_write(f, data, sizeof(data));
            int fired = failinj_fired();
            failinj_disarm();
            if (!fired)
                KASSERT(w == (long)sizeof(data));
            else if (w != -E_NOMEM && !(w > 0 && w < (long)sizeof(data) && w % PAGE_SIZE == 0))
                panic("fs: failinj write site %d #%u returned %ld", site, n, w);
            KASSERT(vfs_size(f) == (uint64_t)(w > 0 ? w : 0));
            vfs_close(f);
            KASSERT(vfs_unlink("/fiB") == 0);
            KASSERT(vfs_open("/fiB", O_CREAT | O_RDWR | O_TRUNC, &f) == 0);   /* 重新量:刪檔應歸還一切 */
            vfs_close(f);
            KASSERT(vfs_unlink("/fiB") == 0);
            if (!fired) {
                points[site][1] = n - 1;
                break;
            }
        }
        /* 3. 建立第二個名稱(link) */
        struct file *f;
        KASSERT(vfs_open("/fiC", O_CREAT | O_WRONLY, &f) == 0);
        vfs_close(f);
        for (unsigned n = 1;; n++) {
            KASSERT(n < 100);
            struct fi_snap a;
            fi_take(&a);
            failinj_arm((enum fi_site)site, n);
            int r = vfs_link("/fiC", "/fiD");
            int fired = failinj_fired();
            failinj_disarm();
            if (!fired) {
                KASSERT(r == 0 && vfs_unlink("/fiD") == 0);
                fi_same(&a, "link(cleanup)", site, n);
                points[site][2] = n - 1;
                break;
            }
            if (r != -E_NOMEM)
                panic("fs: failinj link site %d #%u returned %d, want -E_NOMEM", site, n, r);
            fi_same(&a, "link", site, n);
        }
        KASSERT(vfs_unlink("/fiC") == 0);
    }
    kprintf("fs: allocation-failure injection: create %u+%u, write %u+%u, link %u+%u failure points "
            "(kmalloc+pmm), each -E_NOMEM or a partial write, nothing leaked\n",
            points[0][0], points[1][0], points[0][1], points[1][1], points[0][2], points[1][2]);
}

static void fs_quota_table_test(void)
{
    static char page[PAGE_SIZE];
    struct mb_domain *d = mb_domain_new(1u << 20);
    KASSERT(d);
    struct mb_domain *old = ramfs_set_domain(d);
    ramfs_test_strict(1);
    struct mb_stats s0, s;
    mb_stats(d, &s0);

    struct file *f;
    KASSERT(vfs_open("/qtbl", O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
    for (int i = 0; i < 512; i++)
        KASSERT(vfs_write(f, page, sizeof(page)) == (long)sizeof(page));
    void *a = kmalloc(512 * sizeof(void *)), *b = kmalloc(1024 * sizeof(void *));
    KASSERT(a && b);
    unsigned long op = mb_pages_for_bytes(kmalloc_size(a)), np = mb_pages_for_bytes(kmalloc_size(b));
    kfree(a);
    kfree(b);
    KASSERT(np > op);
    mb_stats(d, &s);
    mb_set_limit(d, s.total + (np - op) + 1);       /* 容得下差額 + 1 頁,容不下整張新表 + 1 頁 */
    KASSERT(np + 1 > np - op + 1 && s.total + np + 1 > s.total + (np - op) + 1);
    long w = vfs_write(f, page, sizeof(page));
    if (w != (long)sizeof(page))
        panic("fs: table growth near the FS limit failed (%ld): charged the whole new table", w);
    mb_stats(d, &s);
    KASSERT(s.total == s.limit);                     /* 剛好用完 */
    KASSERT(vfs_write(f, page, sizeof(page)) == -E_NOSPC);
    vfs_close(f);
    KASSERT(vfs_unlink("/qtbl") == 0);
    mb_stats(d, &s);
    KASSERT(s.total == s0.total);

    ramfs_test_strict(0);
    KASSERT(ramfs_set_domain(old) == d);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("fs: table growth near the FS limit charges only the difference (%lu -> %lu pages)\n", op, np);
}

void fs_demo(void)
{
    struct file *f;
    struct stat st;
    static char buf[10000], back[10000];
    unsigned long pages0 = ramfs_pages();
    struct kheap_stats h0, h1;

    sched_trim();
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();

    kprintf("fs: tree:");
    fs_list("/", 0);
    kprintf("\n");

    /* 1. 讀取開機檔案包帶來的檔案 */
    KASSERT(vfs_open("/etc/motd", O_RDONLY, &f) == 0);
    long n = vfs_read(f, buf, 80);
    KASSERT(n > 0);
    for (long i = 0; i < n; i++)
        if (buf[i] == '\n')
            buf[i] = '\0';
    buf[n] = '\0';
    kprintf("fs: /etc/motd: \"%s\"\n", buf);
    KASSERT(vfs_write(f, "x", 1) == -E_BADF);         /* 唯讀開啟 */
    vfs_close(f);

    /* 2. 建立、寫入超過兩頁的資料、讀回比對 */
    KASSERT(vfs_mkdir("/t") == 0);
    KASSERT(vfs_mkdir("/t") == -E_EXIST);
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (char)(i * 13 + 1);
    KASSERT(vfs_open("/t/a", O_CREAT | O_RDWR, &f) == 0);
    KASSERT(vfs_write(f, buf, sizeof(buf)) == (long)sizeof(buf));
    KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
    KASSERT(memcmp(buf, back, sizeof(buf)) == 0);
    KASSERT(vfs_read(f, back, 10) == 0);              /* 寫完後位置在結尾:讀不到東西 */
    KASSERT(vfs_stat("/t/a", &st) == 0 && st.type == I_FILE && st.size == sizeof(buf));
    KASSERT(ramfs_pages() == pages0 + 3);             /* 10000 位元組 = 3 頁 */
    vfs_close(f);

    /* 3. 附加:以 O_APPEND 開啟時,每次寫入都接在檔案最後 */
    KASSERT(vfs_open("/t/b", O_CREAT | O_WRONLY | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "head", 4) == 4);
    vfs_close(f);
    KASSERT(vfs_open("/t/b", O_RDWR | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "tail", 4) == 4);
    KASSERT(vfs_pread(f, back, 16, 0) == 8 && memcmp(back, "headtail", 8) == 0);
    vfs_close(f);

    /* 4. 開著的時候被刪除:名稱立刻消失,內容留到關閉為止 */
    KASSERT(vfs_open("/t/a", O_RDONLY, &f) == 0);
    KASSERT(vfs_unlink("/t/a") == 0);
    KASSERT(vfs_stat("/t/a", &st) == -E_NOENT);
    KASSERT(vfs_pread(f, back, 100, 5000) == 100 && memcmp(back, buf + 5000, 100) == 0);
    KASSERT(ramfs_pages() == pages0 + 4);             /* a 的 3 頁還在,加上 b 的 1 頁 */
    vfs_close(f);
    KASSERT(ramfs_pages() == pages0 + 1);             /* 關閉後才真的釋放 */

    /* 5. O_TRUNC:重新開啟並清空 */
    KASSERT(vfs_open("/t/b", O_WRONLY | O_TRUNC, &f) == 0);
    KASSERT(vfs_size(f) == 0 && ramfs_pages() == pages0);
    vfs_close(f);

    /* 6. 錯誤情況 */
    KASSERT(vfs_open("/nope", O_RDONLY, &f) == -E_NOENT);
    KASSERT(vfs_open("/etc/motd/x", O_RDONLY, &f) == -E_NOTDIR);
    KASSERT(vfs_open("/t", O_WRONLY, &f) == -E_ISDIR);
    KASSERT(vfs_unlink("/t") == -E_NOTEMPTY);
    KASSERT(vfs_unlink("/") == -E_INVAL);
    KASSERT(vfs_mkdir("/t/..") == -E_EXIST);
    KASSERT(vfs_stat("/t/../etc/./motd", &st) == 0);  /* "." 與 ".." */
    {
        char longname[FS_NAME_MAX + 8];
        longname[0] = '/';
        memset(longname + 1, 'x', sizeof(longname) - 2);
        longname[sizeof(longname) - 1] = '\0';
        KASSERT(vfs_mkdir(longname) == -E_NAMETOOLONG);
    }

    /* 7. 多條執行緒(在多顆 CPU 上)同時操作同一個目錄與同一個紀錄檔 */
    int tid[FS_WORKERS];
    KASSERT(vfs_open("/t/log", O_CREAT | O_WRONLY, &f) == 0);
    vfs_close(f);
    uint64_t t0 = timer_ticks();
    for (int i = 0; i < FS_WORKERS; i++)
        tid[i] = thread_create("fs-worker", fs_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < FS_WORKERS; i++)
        thread_join(tid[i]);
    /* 紀錄檔:總長度正確,每一筆都完整(32 個相同的字元),每條執行緒的筆數正確 */
    unsigned count[FS_WORKERS] = { 0 };
    KASSERT(vfs_open("/t/log", O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == (uint64_t)FS_WORKERS * FS_FILES * FS_RECORD);
    char rec[FS_RECORD];
    while (vfs_read(f, rec, sizeof(rec)) == (long)sizeof(rec)) {
        for (unsigned i = 1; i < sizeof(rec); i++)
            KASSERT(rec[i] == rec[0]);
        KASSERT(rec[0] >= 'A' && rec[0] < 'A' + FS_WORKERS);
        count[rec[0] - 'A']++;
    }
    vfs_close(f);
    for (int i = 0; i < FS_WORKERS; i++)
        KASSERT(count[i] == FS_FILES);
    kprintf("fs: %d threads x %d files (create, write, read back, delete) plus a shared "
            "append-only log: all intact, %llu ticks\n", FS_WORKERS, FS_FILES,
            (unsigned long long)(timer_ticks() - t0));

    /* 8. 全部刪掉之後,佔用的記憶體要回到原本的樣子 */
    KASSERT(vfs_unlink("/t/log") == 0 && vfs_unlink("/t/b") == 0 && vfs_unlink("/t") == 0);
    sched_trim();
    kheap_stats(&h1);
    KASSERT(ramfs_pages() == pages0);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + (h1.heap_bytes - h0.heap_bytes));
    fs_quota_table_test();
    fs_failinj_test();
    kprintf("fs: selftest ok (all files removed, memory returned)\n");
}

/* ------------------------------------------------------------------ */
/* 管線的測試(kernel 內部直接呼叫 VFS)                                  */
/* ------------------------------------------------------------------ */
#define PIPE_TOTAL 50000u                    /* 遠大於管線的緩衝區(4KB):寫入者一定會被擋下來等 */

static void pipe_writer(void *arg)
{
    struct file *wr = arg;
    char chunk[700];

    for (unsigned done = 0; done < PIPE_TOTAL; ) {
        unsigned n = PIPE_TOTAL - done < sizeof(chunk) ? PIPE_TOTAL - done : sizeof(chunk);
        for (unsigned i = 0; i < n; i++)
            chunk[i] = (char)((done + i) * 7 + 5);
        KASSERT(vfs_write(wr, chunk, n) == (long)n);
        done += n;
    }
    vfs_close(wr);                           /* 寫完就關閉寫端:讀取者會因此讀到結束 */
}

void pipe_demo(void)
{
    struct file *rd, *wr;
    char buf[300];
    struct kheap_stats h0, h1;

    sched_trim();
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();

    /* 1. 一條執行緒寫、另一條讀,資料量遠大於緩衝區:順序與內容都必須正確 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    KASSERT(vfs_read(wr, buf, 1) == -E_BADF && vfs_write(rd, buf, 1) == -E_BADF);   /* 方向固定 */
    uint64_t t0 = timer_ticks();
    int tid = thread_create("pipe-writer", pipe_writer, wr);
    unsigned got = 0;
    long n;
    while ((n = vfs_read(rd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            KASSERT(buf[i] == (char)((got + (unsigned)i) * 7 + 5));
        got += (unsigned)n;
    }
    KASSERT(n == 0 && got == PIPE_TOTAL);    /* 寫端關閉後讀到 0,而且一個位元組都沒少 */
    thread_join(tid);
    kprintf("pipe: %u bytes through a 4 KB pipe between two threads, in order, %llu ticks\n",
            got, (unsigned long long)(timer_ticks() - t0));
    vfs_close(rd);

    /* 2. 沒有人讀的管線:寫入要回報錯誤,而不是永遠等下去 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    vfs_close(rd);
    KASSERT(vfs_write(wr, "x", 1) == -E_PIPE);
    vfs_close(wr);

    /* 3. 寫端關閉後,緩衝區裡剩下的資料仍然讀得到,讀完才是結束 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    KASSERT(vfs_write(wr, "left over", 9) == 9);
    vfs_close(wr);
    KASSERT(vfs_read(rd, buf, sizeof(buf)) == 9 && memcmp(buf, "left over", 9) == 0);
    KASSERT(vfs_read(rd, buf, sizeof(buf)) == 0);
    vfs_close(rd);

    sched_trim();
    kheap_stats(&h1);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + (h1.heap_bytes - h0.heap_bytes));
    kprintf("pipe: selftest ok\n");
}
