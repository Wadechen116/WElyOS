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

/* 磁碟上的檔案系統的自我測試。所有東西都放在 /disk/selftest 底下,結束時刪除,
   所以不影響磁碟上原有的內容,空閒的空間也要回到測試之前的數量 */
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
    vfs_unlink("/disk/selftest/big");
    vfs_unlink("/disk/selftest/prog");
    vfs_unlink("/disk/selftest");
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
