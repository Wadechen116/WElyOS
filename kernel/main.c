/*
 * main.c - kernel 的 C 語言進入點(與架構無關)
 *
 * 由各架構的 arch_start()(arch/<名稱>/start.c)在完成最基本的
 * 硬體初始化、並把開機資訊轉成 struct boot_info 後呼叫。
 */
#include <stdint.h>
#include "arch.h"
#include "blackbox.h"
#include "clksync.h"
#include "blk.h"
#include "boot_info.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "membudget.h"
#include "failinj.h"
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
 * 開機參數(cmdline)中是否含有指定的「完整單字」(以空白分隔)。
 * 例:cmdline = "kernel.elf autoexit",word = "autoexit" -> 1。
 * 整字比對可避免 "autoexit2" 誤判。
 */
int cmdline_has(const char *cmd, const char *word)
{
    while (*cmd) {
        while (*cmd == ' ')                  /* 跳過前導空白 */
            cmd++;
        const char *a = cmd, *b = word;
        while (*a && *a != ' ' && *a == *b) {
            a++;
            b++;
        }
        /* 比對到 word 結尾,且 cmdline 該單字也剛好結束 -> 整字相符 */
        if ((*a == '\0' || *a == ' ') && *b == '\0')
            return 1;
        while (*cmd && *cmd != ' ')          /* 不符:跳到下一個單字 */
            cmd++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 使用者模式示範                                                      */
/* ------------------------------------------------------------------ */

/* cmdline 裡有沒有出現 needle 這段文字(不要求是完整的單字) */
static int strstr_simple(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 網路卡                                                              */
/* ------------------------------------------------------------------ */

struct netdev *boot_nic;

/* ------------------------------------------------------------------ */
/* 磁碟                                                                */
/* ------------------------------------------------------------------ */

/*
 * 巡檢執行緒:每 500ms 核對 16 個區塊的檢查碼,一遍又一遍地走過整顆磁碟
 * (16MB 的磁碟約兩分鐘一輪)。刻意做得很慢:它的工作不急,不該和正常的讀寫搶磁碟。
 * 發現損壞時記進黑盒子(每一輪最多記一次,免得同一個壞區塊灌滿紀錄)。
 */
static int scrub_running;

static void scrub_thread(void *arg)
{
    uint32_t pos = 1;
    unsigned bad_this_pass = 0;

    (void)arg;
    for (;;) {
        thread_sleep(500);
        uint32_t before = pos;
        unsigned bad = wfs_scrub(&pos, 16);
        if (bad && !bad_this_pass)
            blackbox_log(BB_DISK, "scrub: %u corrupted block(s) found near block %u", bad, before);
        bad_this_pass += bad;
        if (pos < before)                    /* 繞回開頭:新的一輪 */
            bad_this_pass = 0;
    }
}

/* 常駐的 kernel 執行緒數(main 以外):網路的接收執行緒、磁碟的巡檢執行緒。測試核對執行緒數時使用 */
unsigned resident_threads(void)
{
    return (boot_nic != NULL) + (unsigned)scrub_running;
}

/* 開機時找到的磁碟(包了一層可以故意出錯的那一個);沒有磁碟為 NULL */
static struct blkdev *boot_disk;
struct blkdev *arch_blk_probe_cached(void) { return boot_disk; }

/* 找出磁碟,把上面的檔案系統掛載到 /disk。沒有磁碟、或磁碟上不是我們的檔案系統時,
   只印出訊息,系統照常使用記憶體檔案系統 */
/*
 * ---- 安全模式 ----
 *
 * 開機本身也可能失敗:掛載磁碟時當掉、某個驅動程式在這台機器上出錯…。如果每次開機都走同一條路,
 * 系統就會永遠卡在「開機、當掉、開機、當掉」,而且因為根本到不了可以操作的地方,沒有人能介入。
 *
 * 做法:黑盒子裡每次開機有一筆紀錄,順利完成時再記一筆(BB_HEALTHY)。
 * 開機時數一數「最近連續幾次沒有順利完成」;到達 SAFE_AFTER 次就改以安全模式開機 —
 * 只啟動最少的東西:不掛載磁碟上的檔案系統、不啟動網路、其他 CPU 不參與排程、不跑背景工作。
 * 目的只有一個:讓人進得來,用 faultlog 看看發生了什麼事。
 * 在安全模式裡正常離開(exit)會記下「順利」,下一次開機再試一次完整的開機。
 *
 * 開機參數:safe = 強制以安全模式開機;nosafe = 不要自動進入(除錯用)。
 */
#define SAFE_AFTER 3
static int safe_mode;

static void disk_init(const char *cmdline)
{
    /* 外面包一層可以故意出錯的裝置(測試磁碟錯誤時使用;平常只是原樣轉送) */
    struct blkdev *dev = blk_fault_wrap(arch_blk_probe());
    struct inode *fsroot;
    struct wfs_stats st;
    struct bb_record last;
    unsigned failed = 0;
    /* 故意當機的測試(crash=、disktest=)不該被算成開機失敗;crash=boot 例外,它就是用來測這個的 */
    int test_boot = (strstr_simple(cmdline, "crash=") || strstr_simple(cmdline, "disktest=")) &&
                    !cmdline_has(cmdline, "crash=boot");

    boot_disk = dev;
    safe_mode = cmdline_has(cmdline, "safe");
    if (!dev) {
        kprintf("disk: none\n");
        if (safe_mode)
            kprintf("\n*** SAFE MODE (requested on the command line) ***\n\n");
        return;
    }

    /* 黑盒子:要在掛載之前啟用,才能在碰磁碟上的檔案系統之前決定要不要進安全模式 */
    blackbox_init(dev, wfs_probe(dev));
    if (!blackbox_enabled()) {
        kprintf("blackbox: disabled (no room on this disk, or its contents are not ours)\n");
    } else {
        failed = blackbox_failed_boots();
        if (blackbox_read(0, &last) != 0)
            kprintf("blackbox: empty (first boot with this disk)\n");
        else if (last.type == BB_SHUTDOWN || last.type == BB_HEALTHY)
            kprintf("blackbox: %u records; the previous boot %s\n", blackbox_count(),
                    last.type == BB_SHUTDOWN ? "shut down cleanly" : "came up but was not shut down");
        else if (last.type == BB_PANIC || last.type == BB_WATCHDOG)
            kprintf("blackbox: %u records; the previous boot ended with: %s\n", blackbox_count(), last.text);
        else
            kprintf("blackbox: %u records; the previous boot did not shut down (power loss, or the "
                    "emulator was closed)\n", blackbox_count());
        if (failed >= SAFE_AFTER && !cmdline_has(cmdline, "nosafe"))
            safe_mode = 1;
        blackbox_log(BB_BOOT, "boot #%u on %s%s%s", blackbox_boot(), arch_name(),
                     safe_mode ? ", safe mode" : "", test_boot ? " (test)" : "");
    }
    if (safe_mode) {
        kprintf("\n*** SAFE MODE ***\n");
        if (failed >= SAFE_AFTER)
            kprintf("The last %u boots did not complete.\n", failed);
        kprintf("The disk is not mounted, the network is off and only one CPU is used.\n"
                "Use \"faultlog\" to see what happened; \"exit\" to try a normal boot next time.\n\n");
        return;
    }

    int rc = wfs_mount(dev, &fsroot);
    if (rc < 0) {
        kprintf("disk: %s, %llu MB: not mounted (error %d)%s\n", dev->name,
                (unsigned long long)(dev->sectors / 2048), rc,
                rc == -E_INVAL ? "; it does not hold a WFS file system and was left untouched" :
                rc == -E_NOSPC ? "; it is too small" : "");
        return;
    }
    if (vfs_mkdir("/disk") != 0 || vfs_mount("/disk", fsroot) != 0)
        panic("disk: cannot mount on /disk");
    wfs_stats(&st);
    kprintf("disk: %s, %llu MB, %s on /disk (mount #%u, %u KB free)\n", dev->name,
            (unsigned long long)(dev->sectors / 2048),
            rc == 1 ? "new file system created and mounted" : "mounted",
            st.mounts, st.free_blocks * (unsigned)(PAGE_SIZE / 1024));
    if (st.sb_restored)
        kprintf("disk: superblock restored from its backup copy\n");
    if (st.checksums && thread_create_prio("scrub", scrub_thread, NULL, PRIO_LOW) > 0)
        scrub_running = 1;                   /* 低優先權:只在沒有別的事要做時才巡檢 */
    if (st.repairs)
        blackbox_log(BB_NOTE, "file system check repaired %u problem(s) at mount", st.repairs);
    if (st.sb_restored)
        blackbox_log(BB_DISK, "primary superblock was damaged; restored from the backup copy");
}

/* 關機(結束模擬器)。先把磁碟上的檔案系統正常卸載 */
static void shutdown(int code)
{
    struct wfs_stats st;

    wfs_stats(&st);
    if (st.failed)
        blackbox_log(BB_DISK, "shutting down with the file system read-only after a write error");
    if (safe_mode)
        blackbox_log(BB_HEALTHY, "safe mode session ended normally; the next boot will be a normal one");
    spin_order_report();                     /* 量測模式時印出鎖的巢狀關係(平常不印任何東西) */
    mutex_order_report();
    blackbox_log(BB_SHUTDOWN, "clean shutdown, exit code %d", code);
    wfs_shutdown();
    arch_exit(code);
}

/*
 * 開機標誌。由 kernel 自己印出,所以三個架構完全相同(走的是同一份程式),
 * 序列埠與 i386 的螢幕上都看得到。寬度 55 個字元,80 欄的文字畫面放得下。
 * 不交給各平台的韌體:只有 RISC-V 的韌體會印標誌,而且每個平台的韌體都不一樣。
 */
static const char boot_logo[] =
    "\n"
    " __          __  ______   _            ____     _____\n"
    " \\ \\        / / |  ____| | |          / __ \\   / ____|\n"
    "  \\ \\  /\\  / /  | |__    | |  _   _  | |  | | | (___\n"
    "   \\ \\/  \\/ /   |  __|   | | | | | | | |  | |  \\___ \\\n"
    "    \\  /\\  /    | |____  | | | |_| | | |__| |  ____) |\n"
    "     \\/  \\/     |______| |_|  \\__, |  \\____/  |_____/\n"
    "                               __/ |\n"
    "                              |___/\n";

/* kernel 主函式。主控台此時已可使用。此函式不會返回。 */
void kmain(const struct boot_info *bi)
{
    timer_clock_mark(CLOCK_MARK_KMAIN);      /* 效能量測:開機時間從這裡算起(之前是韌體與載入器) */
    kprintf("%s\n", boot_logo);
    kprintf("Hello WElyOS\n");
    kprintf("arch: %s (%u-bit)\n", arch_name(), (unsigned)(sizeof(void *) * 8));
    kprintf("cmdline: \"%s\"\n", bi->cmdline);
    kprintf("memory: base=0x%llx size=%llu KB\n",
            (unsigned long long)bi->mem_base,
            (unsigned long long)(bi->mem_size >> 10));
    kprintf("kmain=%p\n", (void *)kmain);

    /* autoexit:自動結束模擬器;並讓 panic 也結束模擬器(回傳失敗碼) */
    int autoexit = cmdline_has(bi->cmdline, "autoexit");
    panic_set_exit(autoexit);

    /* 記憶體管理:先建立頁面配置器,之後才能用 kmalloc */
    pmm_init(bi);
    vm_init(bi);                             /* 開啟分頁;之後的存取都經過頁表 */
    proc_mem_boot();                         /* 匿名記憶體的位址區域交給 VM(頁表票的上限) */
    /* 開機參數 selftest:執行各子系統的自我測試與示範。預設不執行,
       開機才不會被測試拖慢;回歸測試與 `run` target 會帶上這個參數 */
    int selftest = cmdline_has(bi->cmdline, "selftest");
    if (selftest) {
        failinj_enable();                    /* 配置失敗注入的系統呼叫只在自我測試時有效 */
        vm_demo();
        mem_selftest();
    }
    if (cmdline_has(bi->cmdline, "memtest"))
        mem_fulltest();

    crash_test(bi->cmdline);

    /*
     * kernel 的保留底線(I-240,第 52 步 C3.1)。取樣時點:分頁與 kernel 映像、位元圖、
     * 開機頁表都已配置,排程器、檔案系統、使用者程式都還沒開始 — 這時的空閒頁數就是
     * 「可用量」。保留 1/8,至少 1024 頁(4MB),但不超過一半:記憶體很小的機器上
     * 使用者仍有一半可用,而且算式不會下溢。可用量不到 2 頁時沒有可行的分法,拒絕開機。
     * 之後一般的配置都不能讓空閒頁數低於它(見 pmm.h);底線之上的部分照常使用。
     */
    {
        size_t usable = pmm_free_count(), floor = usable / 8;
        if (floor < 1024)
            floor = 1024;
        if (floor > usable / 2)
            floor = usable / 2;
        if (floor == 0 || pmm_reserve_enable(floor) != 0)
            panic("pmm: too little memory to keep a kernel reserve (%zu pages usable)", usable);
        kprintf("pmm: kernel reserve %zu pages (%zu KB) of %zu usable\n",
                floor, floor * (PAGE_SIZE / 1024), usable);
        /* ramfs 的 FS domain(I-240 C3.2 (d)):使用者可用量的一半。檔案比行程活得久,不記在寫入者的帳上 */
        struct mb_domain *fsd = mb_domain_new((usable - floor) / 2);
        if (!fsd)
            panic("ramfs: cannot create its memory domain");
        ramfs_set_domain(fsd);
        kprintf("ramfs: quota %zu pages for unprivileged writers\n", (usable - floor) / 2);
    }
    if (selftest) {
        pmm_floor_test();
        pmm_reservation_test();
        membudget_test();
        pte_encoding_test();
        vm_ledger_test();
        anon_ticket_test();
        anon_recommit_test();
        fork_ticket_test();
        stack_ticket_test();
        stack_release_exhausted_test();
        stack_concurrent_fail_test();
        stack_pt_boundary_test();
        stack_guard_test();
        anon_commit_map_test();
        image_ticket_test();
        exec_txn_test();
        r1_lifecycle_test();
        pmm_balance_test();
        image_class_test();
        exec_txn_end_test();
        exec_txn_lower_limit_test();
    }

    /* 啟動計時器(1000Hz,一個 tick 1ms)與排程器,再開中斷。
       timer_init 只是設定硬體,中斷要到 arch_irq_enable 才會真的進來。
       從開中斷那一刻起,main 也會和其他執行緒一樣被搶佔 */
    /* I-341 測試:clock=hpet32 把 HPET 當 32 位元計數器用(起始值設在繞回前約 2 秒),
       要在 timer_init 探測 HPET 之前設定。只在 selftest 時有效 */
    /* I-349 校準:stallinj=spike / steady(只在 selftest;只印診斷,不改任何判定) */
    if (selftest && cmdline_has(bi->cmdline, "stallinj=spike"))
        cpu0_stall_set_injection(CS_INJ_SPIKE);
    if (selftest && cmdline_has(bi->cmdline, "stallinj=steady")) {
        cpu0_stall_set_injection(CS_INJ_STEADY);
        timer_test_steady(1);
    }
    /* clock=nopm:不探測 PM timer,重現「沒有任何備援」(I-341 第二層備援的負對照) */
    if (selftest) {
        int m = ARCH_CLOCK_TEST_NONE;
        if (cmdline_has(bi->cmdline, "clock=hpet32"))
            m |= ARCH_CLOCK_TEST_HPET32;
        if (cmdline_has(bi->cmdline, "clock=nopm"))
            m |= ARCH_CLOCK_TEST_NOPM;
        arch_clock_test_mode(m);
    }
    timer_init(1000);
    sched_init();
    arch_irq_enable();
    timer_clock_init();                      /* 向硬體的即時時鐘問一次現在的時間 */
    kprintf("timer: %u Hz\n", timer_hz());
    console_init();                          /* 序列埠收到字元時發中斷(中斷控制器已由 timer_init 設定好) */

    /* 檔案系統:建立根目錄,把嵌在 kernel 裡的開機檔案包解開進去。
       要在排程器之後:檔案系統用 mutex 上鎖,而 mutex 需要知道「目前的執行緒」 */
    {
        extern const uint8_t rootfs_start[], rootfs_end[];
        vfs_init();
        int nfiles = initrd_unpack(rootfs_start, (size_t)(rootfs_end - rootfs_start));
        if (nfiles < 0)
            panic("fs: cannot unpack the boot archive (error %d)", nfiles);
        kprintf("fs: root file system ready: %d files unpacked from a %zu KB boot archive\n",
                nfiles, (size_t)(rootfs_end - rootfs_start) / 1024);
        if (selftest)
            wfs_mount_failinj_test();        /* 在真的磁碟掛載之前:wfs 一次只能掛一個(I-335) */
        disk_init(bi->cmdline);              /* 有磁碟的話,掛載到 /disk(或決定進入安全模式) */
        if (cmdline_has(bi->cmdline, "crash=boot"))
            panic("test: this boot fails before it completes");      /* 測試安全模式用 */
        if (safe_mode)
            selftest = 0;                    /* 安全模式只做最少的事:不跑自我測試 */
        boot_nic = safe_mode ? NULL : arch_net_probe();   /* 網路卡(安全模式不啟動) */
        if (boot_nic)
            kprintf("net: %s, address %02x:%02x:%02x:%02x:%02x:%02x\n", boot_nic->name,
                    boot_nic->mac[0], boot_nic->mac[1], boot_nic->mac[2], boot_nic->mac[3],
                    boot_nic->mac[4], boot_nic->mac[5]);
        else
            kprintf("net: none\n");
        disk_stage_test(bi->cmdline);
    }

    /* 啟動其他 CPU。selftest 時所有 CPU 一起跑一輪並行測試 */
    /* I-341 測試:讓開機的計數器驗證強制不合格,真的執行「換到備援計數器」或「只由第 0 顆推進」的路徑。
       只在 selftest 時有效(安全模式已在上面把 selftest 清成 0) */
    if (selftest) {
        if (cmdline_has(bi->cmdline, "clock=fallback") || cmdline_has(bi->cmdline, "clock=hpet32"))
            clksync_force(CLKSYNC_FORCE_FALLBACK);
        else if (cmdline_has(bi->cmdline, "clock=cpu0only"))
            clksync_force(CLKSYNC_FORCE_CPU0ONLY);
        else if (cmdline_has(bi->cmdline, "clock=pmtimer"))
            clksync_force(CLKSYNC_FORCE_PMTIMER);    /* 完整走 TSC → HPET → PM timer 兩次切換 */
        else if (cmdline_has(bi->cmdline, "clock=drift"))
            clksync_force(CLKSYNC_FORCE_DRIFT);      /* I-345:第一次再驗證注入 3/16 tick,只有跨輪比較抓得到 */
    }
    smp_init(bi, cmdline_has(bi->cmdline, "selftest"));

    sched_crash_test(bi->cmdline);
    if (selftest) {
        spin_selftest();
        sched_demo();
        sync_demo();
        prio_demo();
    }

    /* 以上的示範檢查的是「單一 CPU 上的排程行為」(誰先執行、誰搶佔誰),
       所以在其他 CPU 加入之前跑。從這裡起,執行緒會真正同時在多顆 CPU 上執行 */
    if (!safe_mode)
        smp_sched_start();                   /* 安全模式:其他 CPU 維持待命,只用開機的這一顆 */
    if (cmdline_has(bi->cmdline, "crash=wdog")) {
        if (smp_started() < 2) {
            /* 只有一顆 CPU 時沒有人可以看它(見 watchdog.c 的限制):這個測試無法進行 */
            panic("watchdog test needs at least 2 cpus");
        }
        watchdog_set_timeout(300);           /* 測試用較短的期限 */
        kprintf("one cpu now spins with interrupts disabled; the watchdog should notice...\n");
        thread_create("stuck", spin_irqs_off, NULL);
        thread_sleep(3000);
        kprintf("the watchdog did not fire\n");
        arch_exit(0);                        /* 預期不會到這裡:以「成功」結束會讓測試失敗 */
    }
    if (cmdline_has(bi->cmdline, "crash=smp")) {
        /* 其他 CPU 正忙的時候 panic:它們應該被叫停,系統乾淨地結束 */
        for (int i = 0; i < smp_started(); i++)
            thread_create("spinner", spin_forever, NULL);
        thread_sleep(5);
        panic("test panic while %d cpus are busy", smp_started());
    }
    if (selftest) {
        smp_sched_demo();
        rt_demo();
        affinity_test();                     /* I-343 */
        sleep_wall_test();
        cpu0_stall_wake_test();
        clksync_test();
        kstack_pool_test();                  /* I-339:要能建立執行緒,所以在排程器啟動之後 */
        pte_tear_test();
        thread_kobj_test();
        pipe_kobj_test();
        ramfs_eof_test();
        fs_demo();
        disk_demo();
        nic_selftest();                      /* 驅動程式單獨的測試:要在協定堆疊開始收訊框之前 */
    }
    netif_start(boot_nic);                   /* 啟動網路(沒有網路卡時不做任何事) */
    if (!safe_mode)
        disk_user_stage_test(bi->cmdline);   /* 需要使用者程式與網路(tftp)的磁碟階段 */
    if (selftest) {
        pipe_demo();
        user_demo();
        user_stack_rollback_test();
    }
    if (cmdline_has(bi->cmdline, "stress"))
        sched_stress();

    struct timer_stats ts;
    timer_get_stats(&ts);
    kprintf("uptime: %llu ticks, %lu context switches\n", (unsigned long long)timer_ticks(), sched_switch_count());
    kprintf("timer: late interrupts (ticks caught up at once): 2:%u 3:%u 4-5:%u 6:%u 7-9:%u 10+:%u, "
            "largest %u, resyncs %u\n", ts.burst[0], ts.burst[1], ts.burst[2], ts.burst[3], ts.burst[4],
            ts.burst[5], ts.max_burst, ts.resyncs);

    /*
     * 啟動 shell:從主控台讀命令並執行。
     *   一般開機(沒有 autoexit):一定啟動,這是使用者與系統互動的地方;
     *                              shell 結束(exit 或在空的提示符號下按 Ctrl-D)就關機
     *   自動化測試(autoexit)    :只有另外給了 shell 參數才啟動;shell 結束後
     *                              以它的結束碼結束模擬器
     */
    int exit_code = 0;
    /* 測試用:確認這次開機是不是安全模式 */
    if (cmdline_has(bi->cmdline, "expect=safe"))
        KASSERT(safe_mode && boot_nic == NULL && !scrub_running && cpu_count == 1);
    if (cmdline_has(bi->cmdline, "expect=normal"))
        KASSERT(!safe_mode);
    /* 走到這裡表示開機順利完成:記下來。之後即使被直接關掉電源,也不算「開機失敗」。
       安全模式不在這裡記:要等有人看過、正常離開之後(見 shutdown) */
    /* I-337:開機約 1 秒後再驗證一次計數器同步,抓漂移(selftest 已做過時不重做) */
    clksync_recheck();
    if (!safe_mode)
        blackbox_log(BB_HEALTHY, "boot completed");
    timer_clock_mark(CLOCK_MARK_READY);
    /* 開機參數 bench:執行效能基準 /bin/bench(docs/ISSUES.md N-004;scripts/bench.sh 使用)。
       它必須成功結束:量測途中有系統呼叫失敗,數字就不可信 */
    if (cmdline_has(bi->cmdline, "bench")) {
        int pid = proc_spawn("/bin/bench", NULL);
        KASSERT(pid > 0);
        int rc = proc_wait(pid);
        kprintf("bench: exit %d\n", rc);
        KASSERT(rc == 0);
    }
    if (cmdline_has(bi->cmdline, "memhog=file"))
        memhog_test("file");
    if (cmdline_has(bi->cmdline, "memhog=fork"))
        memhog_test("fork");
    if (cmdline_has(bi->cmdline, "memhog=threads"))
        memhog_test("threads");
    if (cmdline_has(bi->cmdline, "memhog=stack"))
        memhog_test("stack");
    if (cmdline_has(bi->cmdline, "memhog=maps"))
        memhog_test("maps");
    if (cmdline_has(bi->cmdline, "memhog=exec"))
        memhog_test("exec");
    int shell = !autoexit || cmdline_has(bi->cmdline, "shell");
    if (shell) {
        kprintf("starting /bin/sh (type \"help\" for a list of commands)\n");
        int pid = proc_spawn("/bin/sh", NULL);
        if (pid < 0)
            panic("cannot start /bin/sh");
        exit_code = proc_wait(pid);
        kprintf("shell exited with code %d\n", exit_code);
    }
    if (autoexit)
        shutdown(exit_code);
    if (shell) {
        /* 互動使用時 shell 結束了:沒有東西可以再接受命令,關機(結束模擬器)。
           結束碼固定為 0:這是正常的離開,不是測試失敗 */
        kprintf("powering off\n");
        shutdown(0);
    }
    for (;;)
        thread_sleep(1000);                  /* main 無事可做;CPU 交給 idle 執行緒休眠 */
}
