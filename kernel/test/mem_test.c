/*
 * mem_test.c - 記憶體、分頁、spinlock 的自我測試,以及故意觸發錯誤的 crash= 測試
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
#include "kmalloc.h"
#include "uapi/errno.h"
#include "membudget.h"
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
#include "failinj.h"
#include "mmu.h"
#include "netdev.h"
#include "netif.h"
#include "selftest.h"

/*
 * 測試用:由開機參數 crash=<種類> 故意觸發錯誤,驗證例外與 panic 處理。
 *   crash=panic    直接呼叫 panic()
 *   crash=fault    觸發記憶體/保護錯誤(方式依架構而定,見 arch_test_fault)
 *   crash=illegal  執行非法指令
 *   crash=break    執行軟體中斷點指令(可恢復的例外,處理後會繼續往下)
 *   crash=dfree    同一塊記憶體釋放兩次(驗證 kfree 的檢查)
 *   crash=null     讀取 NULL 指標(位址 0 沒有對映 -> page fault)
 *   crash=wtext    寫入 kernel 程式碼(唯讀頁 -> page fault)
 *   crash=exec     跳到資料頁執行(不可執行頁 -> page fault;i386 無此保護)
 *   crash=spin     同一顆 CPU 對同一把 spinlock 上鎖兩次(不偵測的話會永遠卡住)
 *   crash=spinsleep 持有 spinlock 時讓出 CPU(規則禁止;排程器應該擋下)
 *   crash=smp      其他 CPU 都在忙的時候 panic(它們應該被叫停)
 *   crash=wdog     一顆 CPU 關著中斷空轉(卡死):隔壁的 CPU 的看門狗應該發現並 panic
 * (crash=stack、crash=thread、crash=relock、crash=spin* 需要排程器,在 sched_demo 之前另外處理)
 */
void crash_test(const char *cmd)
{
    if (cmdline_has(cmd, "crash=panic")) {
        panic("test panic, value=%d", 42);
    } else if (cmdline_has(cmd, "crash=fault")) {
        kprintf("triggering fault...\n");
        arch_test_fault();
        kprintf("fault did not trap!\n");       /* 不應到達這裡 */
    } else if (cmdline_has(cmd, "crash=illegal")) {
        kprintf("executing illegal instruction...\n");
        arch_test_illegal();
    } else if (cmdline_has(cmd, "crash=break")) {
        kprintf("executing breakpoint instruction...\n");
        arch_test_breakpoint();
        kprintf("survived breakpoint\n");
    } else if (cmdline_has(cmd, "crash=dfree")) {
        void *p = kmalloc(32);
        kprintf("freeing %p twice...\n", p);
        kfree(p);
        kfree(p);
    } else if (cmdline_has(cmd, "crash=null")) {
        volatile int *p = NULL;
        kprintf("reading from NULL...\n");
        kprintf("read %d?!\n", *p);               /* 不應到達 */
    } else if (cmdline_has(cmd, "crash=wtext")) {
        volatile char *p = (volatile char *)(uintptr_t)kmain;
        kprintf("writing to kernel text at %p...\n", (void *)(uintptr_t)kmain);
        *p = 0;
        kprintf("write succeeded?!\n");           /* 不應到達 */
    } else if (cmdline_has(cmd, "crash=exec")) {
        if (!MMU_HAS_NX) {
            kprintf("crash=exec: this page-table format has no NX bit, skipped\n");
            return;
        }
        void *page = pmm_alloc();                 /* 一般的資料頁:可讀寫、不可執行 */
        memset(page, 0, PAGE_SIZE);
        kprintf("jumping to data page %p...\n", page);
        ((void (*)(void))(uintptr_t)page)();
        kprintf("execution returned?!\n");        /* 不應到達 */
    }
}

/*
 * 分頁示範:查詢權限、把一個實體頁對映到兩個不同的虛擬位址(別名)。
 */
static const char *perm_str(unsigned f, char buf[4])
{
    buf[0] = (f & VM_READ) ? 'r' : '-';
    buf[1] = (f & VM_WRITE) ? 'w' : '-';
    buf[2] = (f & VM_EXEC) ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

void vm_demo(void)
{
    static int data_var = 1;
    static const int ro_var = 2;
    uintptr_t pa;
    unsigned f1, f2, f3;
    char b1[4], b2[4], b3[4];

    /* 1. 各區段的權限 */
    KASSERT(vm_query((uintptr_t)kmain, &pa, &f1) == 0 && pa == (uintptr_t)kmain);
    KASSERT(vm_query((uintptr_t)&ro_var, NULL, &f2) == 0);
    KASSERT(vm_query((uintptr_t)&data_var, NULL, &f3) == 0);
    kprintf("vm: kmain %s, rodata %s, data %s\n",
            perm_str(f1, b1), perm_str(f2, b2), perm_str(f3, b3));
    KASSERT(!(f1 & VM_WRITE) && !(f2 & VM_WRITE) && (f3 & VM_WRITE));
    KASSERT(vm_query(0, NULL, NULL) != 0);             /* NULL 頁沒有對映 */

    /* 2. 別名:兩個虛擬位址指向同一個實體頁。這兩個位址都在 RAM 之外,
          原本沒有任何東西;對映之後才變得可以存取 */
    const uintptr_t va1 = 0xC0000000u, va2 = 0xC0001000u;
    unsigned long tables0 = vm_table_pages();
    volatile uint32_t *phys = pmm_alloc();
    KASSERT(phys != NULL);
    KASSERT(vm_query(va1, NULL, NULL) != 0);
    KASSERT(vm_map(va1, (uintptr_t)phys, VM_READ | VM_WRITE) == 0);
    KASSERT(vm_map(va2, (uintptr_t)phys, VM_READ) == 0);

    volatile uint32_t *v1 = (volatile uint32_t *)va1;
    volatile uint32_t *v2 = (volatile uint32_t *)va2;
    *v1 = 0x12345678;                        /* 從第一個虛擬位址寫入 */
    KASSERT(*v2 == 0x12345678);              /* 從第二個虛擬位址讀到 */
    KASSERT(*phys == 0x12345678);            /* 從實體位址(恆等對映)也讀到 */
    *phys = 0xCAFEF00D;
    KASSERT(*v1 == 0xCAFEF00D && *v2 == 0xCAFEF00D);
    KASSERT(vm_query(va2 + 0x10, &pa, &f1) == 0 && pa == (uintptr_t)phys + 0x10);
    kprintf("vm: %p and %p both map to physical %p (%lu new table pages)\n",
            (void *)va1, (void *)va2, (void *)(uintptr_t)phys,
            vm_table_pages() - tables0);

    /* 安全檢查:同時可寫又可執行的對映必須被拒絕 */
    KASSERT(vm_map(va2, (uintptr_t)phys, VM_READ | VM_WRITE | VM_EXEC) != 0);

    KASSERT(vm_unmap(va1) == 0 && vm_unmap(va2) == 0);
    KASSERT(vm_query(va1, NULL, NULL) != 0);
    KASSERT(vm_unmap(va1) != 0);             /* 已移除:再移除要回報失敗 */

    pmm_free((void *)(uintptr_t)phys);

    /* 3. 拆開大分頁:取主記憶體 1/4 處、對齊到大分頁邊界之後的第 2 頁,
          這個位置確定是以大分頁對映的。把它單獨改成唯讀,vm 必須自動把
          那一大塊拆成 4KB 頁(多用 1 頁表格),而且只影響這一頁,鄰居維持可寫 */
    uintptr_t large = (uintptr_t)PAGE_SIZE << (MMU_LARGE_LEVEL * MMU_BITS);
    uintptr_t tgt = ((pmm_mem_start() + pmm_total_pages() / 4 * PAGE_SIZE) & ~(large - 1))
                    + PAGE_SIZE;
    volatile uint32_t *tp = (volatile uint32_t *)tgt;
    volatile uint32_t *nb = (volatile uint32_t *)(tgt + PAGE_SIZE);
    uint32_t tp_old = *tp, nb_old = *nb;
    *tp = 0x600DF00D;
    tables0 = vm_table_pages();
    KASSERT(vm_map(tgt, tgt, VM_READ) == 0);
    KASSERT(vm_table_pages() - tables0 == 1);            /* 拆分用掉剛好 1 頁表格 */
    KASSERT(vm_query(tgt + 4, &pa, &f1) == 0 && pa == tgt + 4 && !(f1 & VM_WRITE));
    KASSERT(vm_query(tgt + PAGE_SIZE, &pa, &f2) == 0 && pa == tgt + PAGE_SIZE);
    KASSERT(f2 & VM_WRITE);
    KASSERT(*tp == 0x600DF00D);              /* 內容不受拆分影響 */
    *nb = nb_old;                            /* 鄰居仍可寫入 */
    kprintf("vm: split a %lu MB page to write-protect %p (1 new table page)\n",
            (unsigned long)(large >> 20), (void *)tgt);
    KASSERT(vm_map(tgt, tgt, VM_READ | VM_WRITE) == 0);
    *tp = tp_old;
}

/*
 * spinlock 與每顆 CPU 狀態的自我測試(目前只有一顆 CPU,檢查的是基本行為與記帳):
 *   - 上鎖後 spin_held 為真、這顆 CPU 的持有數加一;解鎖後恢復
 *   - 巢狀持有兩把「不同的」鎖是允許的
 *   - 解鎖後中斷狀態回到上鎖之前的樣子
 */
void spin_selftest(void)
{
    static struct spinlock a = SPINLOCK_INIT("test-a", LK_TEST_OUTER), b = SPINLOCK_INIT("test-b", LK_TEST_INNER);

    unsigned long fa = spin_lock_irqsave(&a);
    struct cpu *c = this_cpu();              /* 中斷已關閉,可以安全使用 */
    int base = c->nlocks;
    KASSERT(spin_held(&a) && !spin_held(&b) && base >= 1);
    unsigned long fb = spin_lock_irqsave(&b);
    KASSERT(spin_held(&b) && c->nlocks == base + 1);
    spin_unlock_irqrestore(&b, fb);
    KASSERT(!spin_held(&b) && c->nlocks == base);
    int id = c->id;
    spin_unlock_irqrestore(&a, fa);
    KASSERT(!a.locked && a.cpu == -1);

    /* 中斷有恢復:睡一下還醒得來就代表計時器中斷仍在進來 */
    thread_sleep(2);
    kprintf("spinlock: selftest ok (cpu %d of %d online, max %d)\n", id, cpu_count, MAX_CPUS);
}

/*
 * 記憶體管理的開機自我測試:任何一項不符就 panic。
 * 同時示範 pmm(以頁為單位)與 kmalloc(任意大小)的用法。
 */
void mem_selftest(void)
{
    size_t free0 = pmm_free_count();

    /* ---- pmm:頁面配置 ---- */
    char *a = pmm_alloc();
    char *b = pmm_alloc();
    KASSERT(a && b);
    KASSERT(b > a);                          /* first-fit:由低位址往高位址配置 */
    KASSERT((uintptr_t)a % PAGE_SIZE == 0);
    pmm_free(a);
    KASSERT(pmm_alloc() == (void *)a);       /* 釋放後最低的空閒頁會被重複使用 */
    char *c = pmm_alloc_pages(8);            /* 連續 8 頁 */
    KASSERT(c > b);                          /* 不與前面配置的頁重疊 */
    memset(c, 0xA5, 8 * PAGE_SIZE);          /* 整段都要能寫 */
    KASSERT(pmm_free_count() == free0 - 10);
    pmm_free_pages(c, 8);
    pmm_free(b);
    pmm_free(a);
    KASSERT(pmm_free_count() == free0);
    kprintf("pmm: selftest ok (first page %p)\n", (void *)a);

    /* ---- kmalloc:任意大小配置 ---- */
    struct kheap_stats st;
    char *p1 = kmalloc(24);
    char *p2 = kmalloc(100);
    char *p3 = kmalloc(5000);                /* 超過一頁 */
    KASSERT(p1 && p2 && p3);
    KASSERT((uintptr_t)p1 % 16 == 0 && (uintptr_t)p2 % 16 == 0 &&
            (uintptr_t)p3 % 16 == 0);
    memset(p1, 0x11, 24);                    /* 各自寫滿,互相不應覆蓋 */
    memset(p2, 0x22, 100);
    memset(p3, 0x33, 5000);
    KASSERT(p1[23] == 0x11 && p2[0] == 0x22 && p2[99] == 0x22 && p3[0] == 0x33);

    kfree(p2);
    char *p4 = kmalloc(64);                  /* 放得進 p2 留下的洞 -> 同一位址 */
    KASSERT(p4 == p2);
    char *z = kzalloc(64);
    KASSERT(z && z[0] == 0 && z[63] == 0);

    kheap_stats(&st);
    kprintf("kmalloc: p1=%p p2=%p p3=%p, heap %zu bytes, %zu free in %zu block(s)\n",
            (void *)p1, (void *)p2, (void *)p3,
            st.heap_bytes, st.free_bytes, st.free_blocks);

    kfree(p1);
    kfree(p3);
    kfree(p4);
    kfree(z);
    kfree(NULL);                             /* 必須安全地被忽略 */
    kheap_stats(&st);
    /* 全部釋放後,相鄰的空閒區塊應合併回一整塊 */
    KASSERT(st.free_blocks == 1 && st.free_bytes == st.heap_bytes);
    kprintf("kmalloc: selftest ok (all freed, coalesced into 1 block)\n");
}

/*
 * 開機參數 memtest:把所有空閒頁都配置出來、寫入再讀回驗證,最後全部釋放。
 * 用來確認 pmm 認為「可用」的每一頁真的是可讀寫的 RAM,
 * 而且沒有把 kernel、位元圖或 bootloader 的資料配置出去。
 * 配置到的頁用每頁開頭的指標串成串列,就不需要另外的空間來記錄。
 */
void mem_fulltest(void)
{
    struct page_node {
        struct page_node *next;
        uintptr_t tag;
    } *head = NULL, *n;
    size_t free0 = pmm_free_count(), count = 0;

    while ((n = pmm_alloc()) != NULL) {
        n->next = head;
        n->tag = ~(uintptr_t)n;              /* 每頁寫入與自己位址相關的值 */
        ((char *)n)[PAGE_SIZE - 1] = 0x5A;   /* 頁尾也要能寫 */
        head = n;
        count++;
    }
    KASSERT(count == free0 && pmm_free_count() == 0);
    KASSERT(kmalloc(100000) == NULL);        /* 記憶體用完:回傳 NULL 而不是當掉 */

    while (head) {
        n = head;
        head = n->next;
        KASSERT(n->tag == ~(uintptr_t)n);    /* 沒有兩頁其實是同一塊記憶體 */
        KASSERT(((char *)n)[PAGE_SIZE - 1] == 0x5A);
        pmm_free(n);
    }
    KASSERT(pmm_free_count() == free0);
    kprintf("memtest: %zu pages written and verified\n", count);
}

/*
 * I-090:頁表項目的 64 位元寫入不可被別顆 CPU 看到半新半舊的值。
 * 寫入者(目前執行緒)與讀取者(另一條執行緒)各自關著中斷,釘在兩顆不同的 CPU 上
 * 同時執行約 20 ms:寫入者以 arch_pte_set 在 A、B 兩個值之間來回寫同一個項目,
 * 讀取者以 arch_pte_get 一次讀出 64 位元,看到 A、B 以外的值就是撕裂。
 * A = 「存在、沒有 NX」,B = 「另一個位址、有 NX」:兩半都不同,任何撕裂都會現形。
 * 用的是私有變數而不是真的頁表:測的是寫入原語本身(vm.c 的每個寫點都只經過它)。
 * 負對照:把 i386 的 arch_pte_set 換回 *e = v,這個測試必定失敗(第 52 步實測)。
 * 64 位元架構的對齊儲存本來就不會撕裂,在那裡這是正向控制。
 */
#define TEAR_A 0x0000000011111003ull
#define TEAR_B 0x8000000022222003ull

static volatile pte_t tear_slot = TEAR_A;
static volatile int tear_ready, tear_stop, tear_reader_cpu = -1;
static volatile unsigned long tear_reads, tear_torn;
static volatile pte_t tear_bad;

static void tear_reader(void *arg)
{
    (void)arg;
    unsigned long f = arch_irq_save();       /* 釘在這顆 CPU 上,不被搶佔 */
    tear_reader_cpu = this_cpu()->id;
    uint64_t hz = timer_clock_hz(), guard = timer_clock_count() + hz;   /* 最多 1 秒,寫入者出事也不會卡死 */
    unsigned long reads = 0, torn = 0;
    __atomic_store_n(&tear_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&tear_stop, __ATOMIC_SEQ_CST) && timer_clock_count() < guard) {
        pte_t v = arch_pte_get(&tear_slot);
        if (v != TEAR_A && v != TEAR_B) {
            if (!torn)
                tear_bad = v;
            torn++;
        }
        reads++;
    }
    tear_reads = reads;
    tear_torn = torn;
    arch_irq_restore(f);
}

void pte_tear_test(void)
{
    if (cpu_count < 2) {
        kprintf("pte: tear test needs 2 cpus, skipped (single cpu cannot observe a torn store)\n");
        return;
    }
    KASSERT(timer_clock_hz() != 0);
    int tid = thread_create("pte-reader", tear_reader, NULL);
    KASSERT(tid > 0);
    uint64_t wait_dl = timer_clock_count() + timer_clock_hz();
    while (!__atomic_load_n(&tear_ready, __ATOMIC_SEQ_CST)) {
        KASSERT(timer_clock_count() < wait_dl);
        thread_yield();
    }

    unsigned long f = arch_irq_save();
    int me = this_cpu()->id;
    /* 讀取者關著中斷在自己的 CPU 上空轉,而我們正在執行:一定是不同的 CPU */
    KASSERT(me != tear_reader_cpu);
    uint64_t end = timer_clock_count() + timer_clock_hz() / 50;
    unsigned long writes = 0;
    while (timer_clock_count() < end) {
        arch_pte_set(&tear_slot, TEAR_B);
        arch_pte_set(&tear_slot, TEAR_A);
        writes += 2;
    }
    __atomic_store_n(&tear_stop, 1, __ATOMIC_SEQ_CST);
    arch_irq_restore(f);
    thread_join(tid);

    kprintf("pte: tear test cpu %d writes %lu, cpu %d reads %lu, torn %lu\n",
            me, writes, tear_reader_cpu, tear_reads, tear_torn);
    if (tear_torn)
        panic("pte: torn page-table entry seen: %08x%08x",
              (unsigned)(tear_bad >> 32), (unsigned)tear_bad);
    KASSERT(writes >= 1000 && tear_reads >= 1000);   /* 真的有同時執行,不是空跑 */
}

/*
 * I-240 C3.1:保留底線。在排程器啟動之前執行(只有這一條執行緒在配置,結果是確定的)。
 * 把一般配置能拿的頁全部拿走,核對剛好停在底線;邊界 floor+1 / floor / floor-1、
 * 一次多頁、超大的 n、應急配置可以動用底線,最後全部歸還、計數恢復。
 * 負對照:拿掉 pmm.c 的底線檢查,「剛好停在底線」那一條必定失敗(第 52 步實測)。
 */
void pmm_floor_test(void)
{
    size_t floor = pmm_reserve_count(), free0 = pmm_free_count();
    unsigned long ref0 = pmm_reserve_refusals();
    KASSERT(floor > 0 && free0 > floor);
    KASSERT(pmm_reserve_enable(floor) != 0);          /* 只能啟用一次 */

    /* 一頁一頁拿到被拒絕為止;拿到的頁串成鏈(每頁開頭存下一頁的位址) */
    void *chain = NULL;
    size_t got = 0;
    for (;;) {
        void **pg = pmm_alloc();
        if (!pg)
            break;
        *pg = chain;
        chain = pg;
        got++;
        if (pmm_free_count() == floor + 1)
            KASSERT(pmm_alloc_pages(2) == NULL);      /* floor+1:要 2 頁會跌破,拒絕 */
    }
    KASSERT(got == free0 - floor && pmm_free_count() == floor);   /* 剛好停在底線 */
    KASSERT(pmm_alloc_pages(1) == NULL && pmm_alloc_pages(8) == NULL);
    KASSERT(pmm_alloc_pages((size_t)-1) == NULL);      /* 超大的 n:不溢位、不改變任何東西 */
    KASSERT(pmm_free_count() == floor && pmm_reserve_refusals() > ref0);

    /* 應急配置可以動用底線以下(floor-1),用完歸還 */
    void *r = pmm_alloc_pages_reserve(1);
    KASSERT(r && pmm_free_count() == floor - 1);
    KASSERT(pmm_alloc() == NULL);
    pmm_free(r);
    KASSERT(pmm_free_count() == floor);

    /* 還一頁:又剛好能拿一頁,拿兩頁仍被拒絕 */
    void **top = chain;
    chain = *top;
    pmm_free(top);
    KASSERT(pmm_alloc_pages(2) == NULL);
    void **again = pmm_alloc();
    KASSERT(again && pmm_free_count() == floor);
    *again = chain;
    chain = again;

    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }
    KASSERT(pmm_free_count() == free0);
    kprintf("pmm: reserve floor test ok: general allocations stopped at %zu free pages after %zu pages, "
            "%lu refusals\n", floor, got, pmm_reserve_refusals() - ref0);
}

/*
 * I-240 C3.1 的使用者壓力測試(開機參數 memhog=file / memhog=fork)。
 * /bin/memhog 把記憶體用到被拒絕,保持 500ms 後全部歸還。這段期間另一條執行緒
 * 每 1ms 量一次空閒頁數,記下最低值:一般配置不可以讓它低於保留底線。
 * 之後核對:底線確實擋下過配置、程式以「明確被拒絕」結束(0)、kernel 仍能印診斷,
 * 而且所有頁都回來了 — 不回來的只能是看得到數量的:kernel heap 擴充、頁表、閒置 stack 快取。
 * 負對照:拿掉 pmm.c 的底線檢查,最低值會跌到底線以下(第 52 步實測)。
 */
static volatile int hog_done;
static volatile size_t hog_min;

static void hog_watch(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&hog_done, __ATOMIC_SEQ_CST)) {
        size_t f = pmm_free_count();
        if (f < hog_min)
            hog_min = f;
        thread_sleep(1);
    }
}

static size_t retained_pages(void)
{
    struct kheap_stats hs;
    kheap_stats(&hs);
    return hs.heap_bytes / PAGE_SIZE + vm_table_pages() + sched_kstack_pages();
}

void memhog_test(const char *mode)
{
    size_t floor = pmm_reserve_count();
    KASSERT(floor > 0);
    size_t free0 = pmm_free_count(), kept0 = retained_pages();
    unsigned long ref0 = pmm_reserve_refusals();
    hog_min = free0;
    hog_done = 0;
    int wt = thread_create("hog-watch", hog_watch, NULL);
    KASSERT(wt > 0);

    const char *argv[] = { "memhog", mode, NULL };
    int pid = proc_spawn("/bin/memhog", argv);
    KASSERT(pid > 0);
    int rc = proc_wait(pid);
    __atomic_store_n(&hog_done, 1, __ATOMIC_SEQ_CST);
    thread_join(wt);

    /* fork 模式:父行程可能先被終止(見下),子行程要在它關閉管線之後才各自結束。
       等到頁數平衡為止(最多 3 秒);沒有平衡就是真的漏了 */
    size_t free1 = pmm_free_count(), kept1 = retained_pages();
    for (int i = 0; i < 3000 && free1 + kept1 != free0 + kept0; i++) {
        thread_sleep(1);
        free1 = pmm_free_count();
        kept1 = retained_pages();
    }
    unsigned long refusals = pmm_reserve_refusals() - ref0;
    kprintf("memhog %s: exit %d, lowest free %zu pages (reserve floor %zu), %lu refusals; "
            "free %zu -> %zu, kept by heap/page tables/stack cache %zu -> %zu\n",
            mode, rc, hog_min, floor, refusals, free0, free1, kept0, kept1);
    if (strcmp(mode, "fork") == 0 && rc != 0) {
        /* C3.2 尚未完成的部分,如實印出而不假裝通過:stack 的惰性增長與寫入時複製的頁
           沒有事先記帳,記憶體到底線時是在 page fault 裡配置失敗,行程被終止,
           而不是在 fork 時就拿到 -E_NOMEM。底線本身仍然守住(下面的斷言) */
        kprintf("memhog fork: a process was terminated at a page fault instead of fork failing "
                "with E_NOMEM (stack / copy-on-write pages are not accounted yet: I-240 C3.2)\n");
    } else {
        KASSERT(rc == 0);
    }
    KASSERT(hog_min >= floor);                        /* 底線守住了 */
    /* 真的用到了底線,而不是別的上限先擋下。exec 模式例外:它只碰單一行程的配額(2MB),
       測的是「接近配額的 exec 鏈與低於用量時 exec 被拒」,碰不到系統底線 */
    if (strcmp(mode, "exec") != 0)
        KASSERT(refusals > 0);
    KASSERT(free1 + kept1 == free0 + kept0);          /* 其餘全部歸還(快取也可能縮小,所以兩邊相加比較) */
}

/*
 * I-240 C3.2 第 (a) 批:pmm 的預留。排程器啟動前執行,結果確定。
 * 預留到剛好滿、一般配置與應急配置都拿不到預留的頁、應急配置把底線用完(欠帳)時
 * 新預留與一般配置都被拒絕但兌現照常、全部歸還後計數恢復。
 * 負對照:一般配置不扣預留(第 52 步實測)時「預留剛好滿之後一般配置被拒絕」那條斷言失敗。
 */
static void *chain_take(void **chain, void *(*take)(void))
{
    void **pg = take();
    if (pg) {
        *pg = *chain;
        *chain = pg;
    }
    return pg;
}
static void *take_normal(void)    { return pmm_alloc(); }
static void *take_emergency(void) { return pmm_alloc_pages_reserve(1); }
static void *take_reserved(void)  { return pmm_alloc_reserved(); }
static size_t chain_free(void **chain)
{
    size_t n = 0;
    while (*chain) {
        void **pg = *chain;
        *chain = *pg;
        pmm_free(pg);
        n++;
    }
    return n;
}

void pmm_reservation_test(void)
{
    struct pmm_snapshot s0, s;
    pmm_snapshot(&s0);
    KASSERT(s0.reserved == 0 && s0.floor > 0 && s0.free > s0.floor);

    KASSERT(pmm_reserve(0) == 0 && pmm_reserved_count() == 0);          /* 0:成功、不改狀態 */
    KASSERT(pmm_reserve((size_t)-1) == -E_NOMEM && pmm_reserved_count() == 0);
    pmm_unreserve(0);

    /* 預留到剛好滿:未預留的空閒頁 = 底線 */
    size_t room = s0.free - s0.floor;
    KASSERT(pmm_reserve(room + 1) == -E_NOMEM && pmm_reserved_count() == 0);
    KASSERT(pmm_reserve(room) == 0);
    pmm_snapshot(&s);
    KASSERT(s.reserved == room && s.free == s0.free);                     /* 預留不配置任何頁 */
    KASSERT(pmm_reserve(1) == -E_NOMEM);
    KASSERT(pmm_alloc() == NULL);                                         /* 一般配置拿不到預留 */

    /* 應急配置:把底線用完,但拿不到預留 */
    void *emerg = NULL, *resv = NULL;
    size_t ne = 0;
    while (chain_take(&emerg, take_emergency))
        ne++;
    pmm_snapshot(&s);
    KASSERT(ne == s0.floor && s.free == s.reserved && s.reserved == room); /* N1 剛好成立,N2 欠帳 */

    /* 欠帳期間:新預留、一般配置拒絕;兌現照常(N1) */
    KASSERT(pmm_reserve(1) == -E_NOMEM && pmm_alloc() == NULL);
    for (int i = 0; i < 3; i++)
        KASSERT(chain_take(&resv, take_reserved));
    pmm_snapshot(&s);
    KASSERT(s.free == s.reserved && s.reserved == room - 3);

    /* 還掉應急配置:欠帳解除,但預留仍佔著 → 一般配置還是拿不到 */
    KASSERT(chain_free(&emerg) == ne);
    KASSERT(pmm_alloc() == NULL && pmm_reserve(1) == -E_NOMEM);
    /* 放棄一頁預留:一般配置剛好能拿一頁 */
    pmm_unreserve(1);
    void *one = NULL;
    KASSERT(chain_take(&one, take_normal) && pmm_alloc() == NULL);

    /* 全部歸還 */
    KASSERT(chain_free(&one) == 1 && chain_free(&resv) == 3);
    pmm_unreserve(room - 4);
    pmm_snapshot(&s);
    KASSERT(s.reserved == 0 && s.free == s0.free);
    kprintf("pmm: reservation test ok: %zu pages reserved, emergency allocations stopped at the "
            "reservation (%zu pages), reserved pages still allocatable\n", room, ne);
}

/*
 * I-240 C3.2 (b1):membudget 的純計數規則。排程器啟動前執行,結果確定。
 * 負對照:把 fits_locked 的「total <= limit」拿掉(第 53 步實測),limit 被調到 used 以下時
 * limit - total 下溢,「調低後正增量被拒絕」那條斷言失敗。
 */
void membudget_test(void)
{
    unsigned long err0 = mb_accounting_errors(), err_start = err0;
    struct mb_domain *d = mb_domain_new(10);
    struct mb_stats st;
    KASSERT(d);

    KASSERT(mb_quota_add(d, MB_ANON, 0) == 0);                    /* 0:成功、不改 */
    KASSERT(mb_quota_add(d, MB_ANON, 6) == 0 && mb_quota_add(d, MB_KOBJ, 4) == 0);
    KASSERT(mb_quota_add(d, MB_ANON, 1) == -E_NOMEM);             /* 剛好滿,再 1 拒絕 */
    KASSERT(mb_quota_add(d, MB_ANON, (unsigned long)-1) == -E_NOMEM);
    KASSERT(mb_quota_add(d, (enum mb_class)MB_NCLASS, 1) == -E_INVAL);

    /* 配額調到已用量以下:已記的不撤銷;正增量一律拒絕(不可因下溢而放行);歸還照常 */
    mb_set_limit(d, 5);
    KASSERT(mb_quota_add(d, MB_ANON, 1) == -E_NOMEM && mb_quota_add(d, MB_ANON, 0) == 0);
    KASSERT(mb_quota_sub(d, MB_ANON, 6) == 0);                    /* total 4 < limit 5 */
    KASSERT(mb_quota_add(d, MB_ANON, 1) == 0 && mb_quota_add(d, MB_ANON, 1) == -E_NOMEM);

    /* bundle:配額與預留一起;任一不成立都不改狀態 */
    mb_set_limit(d, 100);
    size_t r0 = pmm_reserved_count();
    struct mb_bundle big = { .reserve = 0 };
    big.quota[MB_ANON] = (unsigned long)-1;
    big.quota[MB_PGTABLE] = 2;                                     /* 總和溢位 */
    KASSERT(mb_commit_bundle(d, &big) == -E_NOMEM);
    struct mb_bundle nores = { .reserve = (size_t)-1 };
    nores.quota[MB_ANON] = 3;                                      /* 配額放得下,預留放不下 */
    KASSERT(mb_commit_bundle(d, &nores) == -E_NOMEM);
    mb_stats(d, &st);
    KASSERT(st.total == 5 && st.used[MB_ANON] == 1 && pmm_reserved_count() == r0);
    struct mb_bundle ok = { .reserve = 7 };
    ok.quota[MB_ANON] = 5;
    ok.quota[MB_PGTABLE] = 2;
    KASSERT(mb_commit_bundle(d, &ok) == 0);
    mb_stats(d, &st);
    KASSERT(st.total == 12 && st.used[MB_PGTABLE] == 2 && pmm_reserved_count() == r0 + 7);
    KASSERT(mb_release_bundle(d, &ok) == 0);
    mb_stats(d, &st);
    KASSERT(st.total == 5 && pmm_reserved_count() == r0);

    /* 整批歸還有一類超量:整批拒絕,配額與預留都不變(Codex 173 的反例 1) */
    KASSERT(mb_commit_bundle(d, &ok) == 0);
    struct mb_bundle bad = { .reserve = 1 };
    bad.quota[MB_ANON] = 1;
    bad.quota[MB_RAMFS] = 1;                                       /* RAMFS 從沒記過 */
    KASSERT(mb_release_bundle(d, &bad) == -1 && mb_accounting_errors() == err0 + 1);
    mb_stats(d, &st);
    KASSERT(st.total == 12 && st.used[MB_ANON] == 6 && pmm_reserved_count() == r0 + 7);
    KASSERT(mb_release_bundle(d, &ok) == 0 && pmm_reserved_count() == r0);
    err0++;

    /* 單類超量歸還:夾到 0、回傳 -1、計數 */
    KASSERT(mb_quota_sub(d, MB_IMAGE, 1) == -1 && mb_accounting_errors() == err0 + 1);
    mb_stats(d, &st);
    KASSERT(st.used[MB_IMAGE] == 0 && st.total == 5);

    /* 參考計數;帶著用量釋放 → -1、計數 */
    KASSERT(mb_domain_get(d) == 0);
    KASSERT(mb_domain_put(d) == 0);
    KASSERT(mb_domain_put(d) == -1 && mb_accounting_errors() == err0 + 2);
    struct mb_domain *e = mb_domain_new(1);
    KASSERT(e && mb_domain_put(e) == 0);
    kprintf("membudget: selftest ok (limit below used refuses, bundles all-or-nothing, "
            "%lu deliberate accounting errors counted)\n", mb_accounting_errors() - err_start);
}

/*
 * I-240 C3.2 (b1):PTE 軟體位元的編碼往返(VM_SHARED / VM_COW / VM_COW|VM_TICKET)。
 * 每一種合法的權限組合 × 四種軟體狀態 × 幾個實體位址:arch_pte_leaf 產生的項目,
 * 位址、R/W/X/U 與軟體狀態都要原樣解回來;非法組合(TICKET 沒有 COW、SHARED 與 COW 並存)
 * 在 vm_sw_flags_ok 與 vm_map 的入口就被拒絕,不會進到架構層。
 * 負對照:把 vm_sw_decode 的 3 解成 VM_SHARED | VM_COW(第 53 步實測)時,「11 不含 SHARED」的斷言失敗 —
 * 那種解法會讓 free_table 把有票的 COW 頁當成檔案的頁而不釋放。
 */
void pte_encoding_test(void)
{
    static const unsigned perms[] = {
        VM_READ, VM_READ | VM_WRITE, VM_READ | VM_EXEC,
        VM_READ | VM_USER, VM_READ | VM_WRITE | VM_USER, VM_READ | VM_EXEC | VM_USER,
    };
    static const unsigned sws[] = { 0, VM_SHARED, VM_COW, VM_COW | VM_TICKET };
    uintptr_t pas[] = { pmm_mem_start(), pmm_mem_start() + 0x1000, pmm_mem_start() + 0x3ff000 };
    unsigned checked = 0;

    for (unsigned i = 0; i < sizeof(perms) / sizeof(perms[0]); i++)
        for (unsigned j = 0; j < 4; j++)
            for (unsigned k = 0; k < 3; k++) {
                unsigned in = perms[i] | sws[j];
                KASSERT(vm_sw_flags_ok(in));
                pte_t e = arch_pte_leaf(pas[k], in, 0);
                unsigned out = arch_pte_flags(e);
                unsigned want = in;
                if (!MMU_HAS_NX)
                    want |= VM_EXEC;                 /* 沒有 NX 的 CPU:有對映就可執行 */
                KASSERT(arch_pte_valid(e) && arch_pte_addr(e) == pas[k]);
                KASSERT((out & (VM_READ | VM_WRITE | VM_EXEC | VM_USER)) ==
                        (want & (VM_READ | VM_WRITE | VM_EXEC | VM_USER)));
                KASSERT((out & (VM_SHARED | VM_COW | VM_TICKET)) == sws[j]);
                checked++;
            }
    /* 兩個位元都設的狀態:只能是 COW + TICKET,絕不帶 SHARED */
    KASSERT(vm_sw_decode(3) == (VM_COW | VM_TICKET) && !(vm_sw_decode(3) & VM_SHARED));

    static const unsigned bad[] = { VM_TICKET, VM_SHARED | VM_COW, VM_SHARED | VM_TICKET,
                                    VM_SHARED | VM_COW | VM_TICKET };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        KASSERT(!vm_sw_flags_ok(VM_READ | bad[i]));
        KASSERT(vm_map(0x40000000u, pmm_mem_start(), VM_READ | bad[i]) == -1);   /* 入口拒絕,不改頁表 */
    }
    kprintf("pte: software-bit encoding round-trip ok (%u leaf entries, 4 invalid combinations rejected)\n",
            checked);
}

/*
 * I-240 C3.2 (b1) 第 2 步:位址空間的帳本與「legacy」建表的配額。
 * 根表記 1 頁 PGTABLE;第一次對映到空的區域要建 MMU_LEVELS - 1 張中間表,各記 1 頁;
 * 同一張表底下的下一頁不再記;配額不夠時對映失敗、帳本與 domain 都不變;destroy 之後 domain 歸零。
 * 負對照:new_table 的 WALK_LEGACY 不記配額(第 54 步實測)時,「第一次對映後 PGTABLE = MMU_LEVELS」失敗。
 */
void vm_ledger_test(void)
{
    struct mb_domain *d = mb_domain_new(100);
    struct mb_stats st;
    KASSERT(d);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(vs);
    mb_stats(d, &st);
    const unsigned long ko = vm_space_kobj_pages();                       /* I-340 的承諾位元圖(KOBJ) */
    KASSERT(st.used[MB_PGTABLE] == 1 && st.used[MB_KOBJ] == ko && st.total == 1 + ko && st.refs == 2);   /* 根表;空間持有一份參考 */

    void *p1 = pmm_alloc(), *p2 = pmm_alloc(), *p3 = pmm_alloc();
    KASSERT(p1 && p2 && p3);
    KASSERT(vm_space_map(vs, USER_BASE, (uintptr_t)p1, VM_READ | VM_WRITE) == 0);
    mb_stats(d, &st);
    KASSERT(st.used[MB_PGTABLE] == MMU_LEVELS);                           /* 根 + 中間表 */
    KASSERT(vm_space_map(vs, USER_BASE + PAGE_SIZE, (uintptr_t)p2, VM_READ | VM_WRITE) == 0);
    mb_stats(d, &st);
    KASSERT(st.used[MB_PGTABLE] == MMU_LEVELS);                           /* 同一張表:不再記 */

    /* 配額只夠現有的:下一個 2MB 區域需要新的最底層表 → 對映失敗,什麼都不變 */
    mb_set_limit(d, st.total);
    uintptr_t far = USER_BASE + ((uintptr_t)PAGE_SIZE << MMU_BITS);
    KASSERT(vm_space_map(vs, far, (uintptr_t)p3, VM_READ | VM_WRITE) != 0);
    mb_stats(d, &st);
    KASSERT(st.used[MB_PGTABLE] == MMU_LEVELS && st.total == MMU_LEVELS + ko);
    pmm_free(p3);

    vm_space_destroy(vs);                                                  /* p1、p2 隨空間釋放 */
    mb_stats(d, &st);
    KASSERT(st.total == 0 && st.refs == 1);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("vm: address-space ledger test ok (root + %d intermediate tables charged, "
            "quota exhaustion refuses a new table without side effects)\n", MMU_LEVELS - 1);
}

/*
 * I-240 C3.2 (b1) 第 3 步:匿名記憶體的承諾、兌現、歸還與有票的 COW。
 * 核心是 I2:承諾成功之後,即使一般配置已經把記憶體用到預留邊界,第一次觸及
 * (包括需要新中間表的位址)仍然一定成功。每一步核對「這個空間的票 = pmm 增加的預留」。
 * 負對照:fault_anon 改用一般配置(第 54 步實測)時,飽和狀態下第一次觸及失敗。
 */
static void anon_check(struct vm_space *vs, size_t r0, const char *where)
{
    struct vm_ledger l;
    vm_space_ledger(vs, &l);
    if (l.data_tickets + l.pt_tickets != pmm_reserved_count() - r0)
        panic("anon: tickets %lu + %lu != reserved %zu at %s", l.data_tickets, l.pt_tickets,
              pmm_reserved_count() - r0, where);
}

void anon_ticket_test(void)
{
    uintptr_t heap, hlen, mm, mlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0 && vm_anon_window(1, &mm, &mlen) == 0);
    KASSERT(vm_anon_pt_max() > 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    struct vm_ledger l;
    KASSERT(d && vs);

    /* 承諾 600 頁:跨兩張最底層表(512 頁一張)+ 上一層 1 張 */
    const unsigned long n = 600;
    KASSERT(vm_space_commit_anon(vs, heap, n * PAGE_SIZE) == 0);
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == n && l.pt_tickets == 3 && l.anon_quota == n);
    anon_check(vs, r0, "commit");

    /* 一般配置用到預留邊界(拿不到任何一頁為止) */
    void *chain = NULL;
    size_t taken = 0;
    for (void **pg; (pg = pmm_alloc()) != NULL; taken++) {
        *pg = chain;
        chain = pg;
    }
    /* I2:每一頁第一次觸及都成功,含需要新中間表的 */
    for (unsigned long i = 0; i < n; i++)
        KASSERT(vm_space_fault_anon(vs, heap + i * PAGE_SIZE) == 0);
    KASSERT(vm_space_fault_anon(vs, heap) == 1);                 /* 已經有對映:不用票 */
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0 && l.pt_tickets == 0 && l.anon_tables == 3);
    anon_check(vs, r0, "faults");
    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }

    /* 有票的 COW:兩頁,一頁只剩自己(退票),一頁共用中(用票複製) */
    KASSERT(vm_space_commit_anon(vs, mm, 2 * PAGE_SIZE) == 0);
    anon_check(vs, r0, "commit2");
    void *solo = pmm_alloc(), *shared = pmm_alloc();
    KASSERT(solo && shared);
    pmm_ref(shared);                                              /* 模擬另一個擁有者 */
    KASSERT(vm_space_map(vs, mm, (uintptr_t)solo, VM_READ | VM_COW | VM_TICKET) == 0);
    KASSERT(vm_space_map(vs, mm + PAGE_SIZE, (uintptr_t)shared, VM_READ | VM_COW | VM_TICKET) == 0);
    vm_space_ledger(vs, &l);
    unsigned long dt = l.data_tickets;                            /* 2 張:對應兩筆 COW 映射 */
    KASSERT(dt == 2);
    KASSERT(vm_space_fault(vs, mm, 1) == 0);                      /* refcount 1:退票 */
    KASSERT(vm_space_fault(vs, mm + PAGE_SIZE, 1) == 0);          /* refcount 2:用票複製 */
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0 && pmm_refcount(shared) == 1);
    anon_check(vs, r0, "cow");
    pmm_free(shared);                                             /* 模擬的另一個擁有者放手 */

    /* 歸還:600 頁全部已配置 → 釋放頁、不退票;另一段承諾但沒碰的 → 退票 */
    KASSERT(vm_space_commit_anon(vs, heap + n * PAGE_SIZE, 100 * PAGE_SIZE) == 0);
    vm_space_release_anon(vs, heap, (n + 100) * PAGE_SIZE);
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0 && l.anon_quota == 2);
    anon_check(vs, r0, "release");

    vm_space_destroy(vs);
    struct mb_stats st;
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("anon: ticket test ok (%lu pages committed and first-touched after ordinary allocations "
            "took %zu pages; ticketed COW reuse and copy; M = %lu)\n", n, taken, vm_anon_pt_max());
}

/*
 * I-336:同一段範圍反覆「承諾 → 歸還」、從未碰觸,頁表票只預付一次(prepaid 位元圖)。
 * 每一輪之後 domain 的 PGTABLE 用量、pt_tickets、pmm 預留都與第一輪相同;
 * 之後全部觸及仍然成功(P1:票 = 位元數,fault 不會缺票),legacy 建表時精確退票,destroy 後全部歸零。
 * 負對照:改回第 61 步的 pt_missing(只看缺不缺)時,第二輪 pt_tickets 由 p 變成 2p,「不累積」斷言失敗。
 */
void anon_recommit_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    struct vm_ledger l;
    struct mb_stats st;
    KASSERT(d && vs);
    /* 這個測試的數字(600 頁跨兩張最底層表、p = 層數)假設每張表 512 項、heap 對齊一張最底層表
       涵蓋的範圍(2MB)。三個目標架構都成立;換了分頁格式就在這裡明確失敗,而不是誤判 */
    KASSERT(MMU_BITS == 9 && (heap & (((uintptr_t)PAGE_SIZE << MMU_BITS) - 1)) == 0);

    /* 配額不足使承諾失敗之後,調高配額再重試(Codex 197):失敗不可以留下已預付的位元 —
       若留下了,重試時那些表不會再收票,pt_tickets 會比該有的少 */
    {
        struct mb_domain *d2 = mb_domain_new(10);   /* 遠小於 600 頁 */
        struct vm_space *v2 = vm_space_create(d2);
        KASSERT(d2 && v2);
        struct mb_stats s0, s1;
        mb_stats(d2, &s0);
        KASSERT(vm_space_commit_anon(v2, heap, 600 * PAGE_SIZE) == -E_NOMEM);
        vm_space_ledger(v2, &l);
        mb_stats(d2, &s1);
        KASSERT(l.data_tickets == 0 && l.pt_tickets == 0 && s1.total == s0.total && pmm_reserved_count() == r0);
        mb_set_limit(d2, 100000);
        KASSERT(vm_space_commit_anon(v2, heap, 600 * PAGE_SIZE) == 0);
        vm_space_ledger(v2, &l);
        mb_stats(d2, &s1);
        if (l.pt_tickets != (unsigned long)MMU_LEVELS || s1.used[MB_PGTABLE] - s0.used[MB_PGTABLE] != (unsigned long)MMU_LEVELS)
            panic("recommit: retry after a quota failure prepaid %lu tables (PGTABLE +%lu), want %d",
                  l.pt_tickets, s1.used[MB_PGTABLE] - s0.used[MB_PGTABLE], MMU_LEVELS);
        vm_space_destroy(v2);
        mb_stats(d2, &s1);
        KASSERT(s1.total == 0 && pmm_reserved_count() == r0);
        KASSERT(mb_domain_put(d2) == 0);
    }

    const unsigned long n = 600;                     /* 跨兩張最底層表 */
    const unsigned rounds = 16;
    unsigned long p = 0, pg0 = 0;
    for (unsigned k = 0; k < rounds; k++) {
        KASSERT(vm_space_commit_anon(vs, heap, n * PAGE_SIZE) == 0);
        vm_space_ledger(vs, &l);
        mb_stats(d, &st);
        if (k == 0) {
            p = l.pt_tickets;
            pg0 = st.used[MB_PGTABLE];
            KASSERT(p > 0 && p == (unsigned long)(MMU_LEVELS - 1) + 1);   /* 2 張底層 + 每個上層 1 張 */
        }
        if (l.pt_tickets != p || st.used[MB_PGTABLE] != pg0 || l.anon_tables != 0)
            panic("recommit: round %u pt_tickets %lu (first %lu) PGTABLE %lu (first %lu)",
                  k, l.pt_tickets, p, st.used[MB_PGTABLE], pg0);
        anon_check(vs, r0, "recommit");
        vm_space_release_anon(vs, heap, n * PAGE_SIZE);
        vm_space_ledger(vs, &l);
        KASSERT(l.data_tickets == 0 && l.pt_tickets == p);           /* 頁表票保留到 destroy(有界保留) */
    }

    /* 窗外的範圍不能承諾 */
    KASSERT(vm_space_commit_anon(vs, heap - PAGE_SIZE, 2 * PAGE_SIZE) == -E_INVAL);

    /* 再承諾一次後全部觸及:不缺票,票全部消耗 */
    KASSERT(vm_space_commit_anon(vs, heap, n * PAGE_SIZE) == 0);
    for (unsigned long i = 0; i < n; i++)
        KASSERT(vm_space_fault_anon(vs, heap + i * PAGE_SIZE) == 0);
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0 && l.pt_tickets == 0 && l.anon_tables == p);
    anon_check(vs, r0, "recommit-touch");
    vm_space_release_anon(vs, heap, n * PAGE_SIZE);

    /* legacy 建表精確退票:承諾下一張底層表的範圍(預付 1 張),再用 vm_space_map 在那裡建表 */
    uintptr_t nx = heap + 1024 * PAGE_SIZE;          /* 第三張底層表(還不存在) */
    KASSERT(vm_space_commit_anon(vs, nx, PAGE_SIZE) == 0);
    vm_space_ledger(vs, &l);
    unsigned long q = l.pt_tickets;
    KASSERT(q >= 1);
    void *pg = pmm_alloc();
    KASSERT(pg && vm_space_map(vs, nx, (uintptr_t)pg, VM_READ | VM_WRITE) == 0);
    vm_space_ledger(vs, &l);
    KASSERT(l.pt_tickets == 0 && l.anon_tables == p + q);
    anon_check(vs, r0, "legacy-refund");
    /* nx 那一頁是 legacy 對映(沒有 VM_TICKET):release 不退它的資料票,那張票留到 destroy 才退
       (帳是平的;這裡不斷言 data_tickets == 0) */
    vm_space_release_anon(vs, nx, PAGE_SIZE);

    vm_space_destroy(vs);
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("anon: %u commit/release rounds of an untouched %lu-page range prepaid %lu table tickets once\n",
            rounds, n, p);
}

/*
 * ==== I-240 C3.2 R1:生命週期整合、實體平衡、6.2 的負對照(草稿)====
 *
 * r1_check:核對「所有活著的空間 Σ(data_tickets + pt_tickets) == pmm 增加的預留」,
 * 以及每個 domain(每個 domain 只有一個空間)的配額 == 該空間帳本:
 * used[ANON] == anon_quota、used[PGTABLE] == pt_quota、total == 兩者之和。
 */
static void r1_check(struct vm_space *const *vs, struct mb_domain *const *ds, unsigned n,
                     size_t r0, const char *where)
{
    unsigned long sum = 0;
    for (unsigned i = 0; i < n; i++) {
        struct vm_ledger l;
        struct mb_stats st;
        vm_space_ledger(vs[i], &l);
        mb_stats(ds[i], &st);
        sum += l.data_tickets + l.pt_tickets;
        /* 總數 = ANON + PGTABLE + 這個空間的承諾位元圖(KOBJ,I-340) */
        if (st.used[MB_ANON] != l.anon_quota || st.used[MB_PGTABLE] != l.pt_quota ||
            st.used[MB_KOBJ] != vm_space_kobj_pages() ||
            st.total != st.used[MB_ANON] + st.used[MB_PGTABLE] + st.used[MB_KOBJ])
            panic("r1: space %u domain ANON %lu PGTABLE %lu total %lu != ledger anon %lu pt %lu at %s",
                  i, st.used[MB_ANON], st.used[MB_PGTABLE], st.total, l.anon_quota, l.pt_quota, where);
    }
    if (sum != pmm_reserved_count() - r0)
        panic("r1: sum of tickets %lu != reserved %zu at %s", sum, pmm_reserved_count() - r0, where);
}

/* 由 R1 推導出的精確票數與配額(只看 Σ 抓不到「兩邊一起漏退」,要靠這個) */
static void r1_expect(struct vm_space *vs, unsigned long dt, unsigned long quota, const char *where)
{
    struct vm_ledger l;
    vm_space_ledger(vs, &l);
    if (l.data_tickets != dt || l.anon_quota != quota)
        panic("r1: data_tickets %lu anon_quota %lu, want %lu / %lu at %s",
              l.data_tickets, l.anon_quota, dt, quota, where);
}

/*
 * 實體平衡(設計「兩種平衡分開驗」):pmm 沒有「已配置」計數,所以掃描每頁的參考計數
 * (pmm 的每一條配置路徑都把 refs 設為 1,釋放到 0 才清位元圖),已配置 = refs > 0 的頁數。
 * usable 在基準點量一次(free + allocated),之後每個靜止點都必須相等 —
 * 與預留無關:預留的頁仍算在 free 裡。掃描前後 free 不變才採用(否則重試),避免與中斷中的配置交錯。
 */
static size_t r1_usable0;                       /* 0 = 不做實體核對 */

static size_t r1_pmm_allocated(void)
{
    size_t n = 0, np = pmm_total_pages();
    uintptr_t b = pmm_mem_start();
    for (size_t i = 0; i < np; i++)
        if (pmm_refcount((void *)(b + (uintptr_t)i * PAGE_SIZE)) != 0)
            n++;
    return n;
}

static size_t r1_pmm_usable(void)
{
    for (int tries = 0; tries < 3; tries++) {
        size_t f0 = pmm_free_count();
        size_t a = r1_pmm_allocated();
        if (pmm_free_count() == f0)
            return f0 + a;
    }
    panic("r1: pmm free count kept changing during the allocated-page scan");
}

static void r1_phys(const char *where)
{
    if (!r1_usable0)
        return;
    struct pmm_snapshot s;
    pmm_snapshot(&s);
    size_t u = r1_pmm_usable();
    if (u != r1_usable0 || s.reserved > s.free)
        panic("r1: free + allocated = %zu, usable %zu (reserved %zu free %zu) at %s",
              u, r1_usable0, s.reserved, s.free, where);
}

/*
 * R1 生命週期情境(r1_lifecycle_test 與 pmm_balance_test 共用)。頁號都相對於 heap:
 *   父承諾 [0,300)、觸及 [0,200) → fork(範圍 [0,300))→ 父 release [10,150)(140 頁,跨 3 段 64 頁)
 *   → 子寫 20(只剩子:退票)、子寫 170(共用:用票複製)、父寫 180、父寫 5(共用:複製)
 *   → 子 release [190,260)(10 頁共用 COW + 60 頁未兌現)→ 子觸及 270 → destroy 子
 *   → 父寫 151(只剩父:退票)→ 以 [0,10) + [150,300) 兩段再 fork → 父寫 0(複製)、子寫 0(退票)
 *   → 子 release [150,300)(150 頁,跨 3 段)→ 全部 destroy。
 * 每一步:Σ 核對 + domain 核對 + 由 R1 推導的精確票數;phys 為真時加上實體平衡。
 */
static void r1_scenario(uintptr_t heap, int phys)
{
    const uintptr_t PG = PAGE_SIZE;
    size_t r0 = pmm_reserved_count();
    struct mb_domain *dp = mb_domain_new(100000), *dc = mb_domain_new(100000), *dc2 = mb_domain_new(100000);
    struct vm_space *ps = vm_space_create(dp), *cs;
    struct vm_space *vs[2];
    struct mb_domain *ds[2];
    struct mb_stats st;
    KASSERT(dp && dc && dc2 && ps);
    vs[0] = ps;
    ds[0] = dp;
#define R1_STEP(n, where) do { r1_check(vs, ds, (n), r0, (where)); if (phys) r1_phys(where); } while (0)

    KASSERT(vm_space_commit_anon(ps, heap, 300 * PG) == 0);
    for (unsigned long i = 0; i < 200; i++)
        KASSERT(vm_space_fault_anon(ps, heap + i * PG) == 0);
    R1_STEP(1, "touch");
    r1_expect(ps, 100, 300, "touch");

    struct vm_range r = { heap, 300 * PG };
    cs = vm_space_fork(ps, dc, &r, 1);
    KASSERT(cs);
    vs[1] = cs;
    ds[1] = dc;
    R1_STEP(2, "fork");
    r1_expect(ps, 300, 300, "fork/parent");             /* 100 未兌現 + 200 COW + TICKET */
    r1_expect(cs, 300, 300, "fork/child");              /* 父的票 100 + pending 200 */

    /* munmap 式的部分歸還:140 頁共用中的 COW + TICKET,跨 3 段 RELEASE_SEGMENT,每頁退一張票 */
    vm_space_release_anon(ps, heap + 10 * PG, 140 * PG);
    R1_STEP(2, "parent-release");
    r1_expect(ps, 160, 160, "parent-release");
    r1_expect(cs, 300, 300, "parent-release/child");
    {
        uintptr_t pa;
        unsigned f;
        KASSERT(vm_space_query(cs, heap + 20 * PG, &pa, &f) == 0 && (f & VM_COW) && (f & VM_TICKET));
        KASSERT(pmm_refcount((void *)pa) == 1);         /* 父放手後只剩子 */
    }

    KASSERT(vm_space_fault(cs, heap + 20 * PG, 1) == 0);    /* 只剩子:退票 */
    KASSERT(vm_space_fault(cs, heap + 170 * PG, 1) == 0);   /* 共用:用子的票複製 */
    KASSERT(vm_space_fault(ps, heap + 180 * PG, 1) == 0);   /* 共用:用父的票複製 */
    KASSERT(vm_space_fault(ps, heap + 5 * PG, 1) == 0);
    R1_STEP(2, "cow-writes");
    r1_expect(ps, 158, 160, "cow-writes/parent");
    r1_expect(cs, 298, 300, "cow-writes/child");

    vm_space_release_anon(cs, heap + 190 * PG, 70 * PG);    /* 10 頁共用 COW + 60 頁未兌現,跨 2 段 */
    R1_STEP(2, "child-release");
    r1_expect(cs, 228, 230, "child-release");
    KASSERT(vm_space_fault_anon(cs, heap + 270 * PG) == 0);
    R1_STEP(2, "child-touch");
    r1_expect(cs, 227, 230, "child-touch");

    vm_space_destroy(cs);
    R1_STEP(1, "child-destroy");
    r1_expect(ps, 158, 160, "child-destroy");
    mb_stats(dc, &st);
    KASSERT(st.total == 0);

    KASSERT(vm_space_fault(ps, heap + 151 * PG, 1) == 0);   /* 子已不在:只剩父,退票 */
    R1_STEP(1, "parent-reuse");
    r1_expect(ps, 157, 160, "parent-reuse");

    /* 再 fork:範圍避開已歸還的 [10,150)。父的可寫獨佔頁 = 5、151、180 → pending 3 */
    struct vm_range r2[2] = { { heap, 10 * PG }, { heap + 150 * PG, 150 * PG } };
    cs = vm_space_fork(ps, dc2, r2, 2);
    KASSERT(cs);
    vs[1] = cs;
    ds[1] = dc2;
    R1_STEP(2, "refork");
    r1_expect(ps, 160, 160, "refork/parent");
    r1_expect(cs, 160, 160, "refork/child");            /* 子的票 = 子承諾的每一頁 */

    KASSERT(vm_space_fault(ps, heap, 1) == 0);              /* 共用:複製 */
    KASSERT(vm_space_fault(cs, heap, 1) == 0);              /* 只剩子:退票 */
    R1_STEP(2, "refork-writes");
    r1_expect(ps, 159, 160, "refork-writes/parent");
    r1_expect(cs, 159, 160, "refork-writes/child");

    vm_space_release_anon(cs, heap + 150 * PG, 150 * PG);   /* 跨 3 段:每頁都有票 */
    R1_STEP(2, "refork-release");
    r1_expect(cs, 9, 10, "refork-release");

    vm_space_destroy(cs);
    vm_space_destroy(ps);
#undef R1_STEP
    KASSERT(pmm_reserved_count() == r0);
    mb_stats(dp, &st);
    KASSERT(st.total == 0);
    mb_stats(dc2, &st);
    KASSERT(st.total == 0);
    KASSERT(mb_domain_put(dc2) == 0 && mb_domain_put(dc) == 0 && mb_domain_put(dp) == 0);
    if (phys)
        r1_phys("teardown");
}

/*
 * I-240 C3.2 R1 生命週期整合:部分歸還(跨多段)、父子交錯 COW 寫入、子 destroy、父再 fork。
 * 負對照:vm_space_release 對 VM_TICKET 的頁不 tickets++(COW 歸還不退票)→ Σ 仍平衡
 * (帳本與 pmm 一起沒退),但 "parent-release" 的 r1_expect 得到 data_tickets 300 ≠ 160 而 panic;
 * 只改帳本不改 pmm(漏 mb_release_bundle 的 reserve)→ r1_check 的 Σ panic。
 */
void r1_lifecycle_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0 && hlen >= 300 * PAGE_SIZE);
    r1_scenario(heap, 0);
    kprintf("r1: lifecycle ok (partial releases across 64-page segments, interleaved COW, "
            "child destroy, re-fork; tickets == reservation at every step)\n");
}

/*
 * 實體平衡與預留平衡分開驗:每個靜止點 free + allocated == usable(與預留無關),
 * 情境跑兩次(第一次吸收 kmalloc 等一次性成長),兩次結束後的 free 必須相同。
 * 負對照:vm_space_release 對獨佔頁漏掉 pmm_free(洩漏一頁)→ free + allocated 仍等於 usable
 * (洩漏的頁仍是已配置),但第二次結束後 free 少 1 →「leaked」panic。這正是兩種平衡要分開的原因。
 */
void pmm_balance_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0 && hlen >= 300 * PAGE_SIZE);
    r1_usable0 = r1_pmm_usable();
    r1_phys("baseline");
    r1_scenario(heap, 1);
    size_t f1 = pmm_free_count();
    r1_scenario(heap, 1);
    size_t f2 = pmm_free_count();
    if (f2 != f1)
        panic("r1: second lifecycle run leaked %ld pages (free %zu -> %zu)", (long)f1 - (long)f2, f1, f2);
    size_t u = r1_usable0;
    r1_usable0 = 0;
    kprintf("r1: physical balance ok (free + allocated == %zu usable pages at every quiescent point; "
            "repeat run returned every page)\n", u);
}

/*
 * 6.2「映像頁不記 IMAGE」:逐類核對 map_charged 的帳(只有 IMAGE 增加,ANON / STACK 不變,
 * total 只多 IMAGE + 新頁表),fork 後子 domain 照抄 IMAGE,destroy 後全部退回。
 * 既有測試中抓到這個回歸的是 image_ticket_test 的 KASSERT(st.used[MB_IMAGE] == 8)
 * (mem_test.c:1208,HEAD c8d68ed);這裡另外抓「記成別的類別」與「fork / destroy 不搬 / 不退」。
 * 限制:exec 真正的載入路徑(proc.c:289 呼叫 vm_space_map_charged(..., MB_IMAGE, ...))在這裡呼叫不到,
 * 改類別的回歸需要使用者層 exec 測試或靜態探針。
 */
void image_class_test(void)
{
    uintptr_t base = 0x40000000u + 0x00100000u;
#if UINTPTR_MAX > 0xffffffffu
    base = 0x100000000ul + 0x00100000u;
#endif
    size_t r0 = pmm_reserved_count();
    struct mb_domain *dp = mb_domain_new(100000), *dc = mb_domain_new(100000);
    struct vm_space *ps = vm_space_create(dp);
    struct mb_stats s0, s1;
    KASSERT(dp && dc && ps);
    mb_stats(dp, &s0);
    for (int i = 0; i < 8; i++)
        KASSERT(vm_space_map_charged(ps, MB_IMAGE, base + i * PAGE_SIZE, VM_READ | VM_WRITE));
    mb_stats(dp, &s1);
    if (s1.used[MB_IMAGE] - s0.used[MB_IMAGE] != 8 || s1.used[MB_ANON] != s0.used[MB_ANON] ||
        s1.used[MB_STACK] != s0.used[MB_STACK] ||
        s1.total - s0.total != 8 + (s1.used[MB_PGTABLE] - s0.used[MB_PGTABLE]))
        panic("image: map_charged charged IMAGE +%lu ANON +%lu STACK +%lu total +%lu",
              s1.used[MB_IMAGE] - s0.used[MB_IMAGE], s1.used[MB_ANON] - s0.used[MB_ANON],
              s1.used[MB_STACK] - s0.used[MB_STACK], s1.total - s0.total);
    struct vm_range r = { base, 8 * PAGE_SIZE };
    struct vm_space *cs = vm_space_fork(ps, dc, &r, 1);
    KASSERT(cs);
    mb_stats(dc, &s1);
    KASSERT(s1.used[MB_IMAGE] == 8 && s1.used[MB_ANON] == 0);
    vm_space_destroy(cs);
    mb_stats(dc, &s1);
    KASSERT(s1.total == 0);
    vm_space_destroy(ps);
    mb_stats(dp, &s1);
    KASSERT(s1.total == 0 && pmm_reserved_count() == r0);
    KASSERT(mb_domain_put(dc) == 0 && mb_domain_put(dp) == 0);
    kprintf("image: private pages charged to IMAGE only; fork copied and destroy returned them\n");
}

/*
 * 6.2「exec 不呼叫 mb_exec_end」:探針 x = limit - total + 1 頁,落在 limit 之上、limit + pending 之內。
 * 交易中帶憑證 → 成功(正對照,證明探針分得出兩種狀態);mb_exec_end 之後 → -E_NOMEM、帳不變。
 * 另外新空間刻意「不」先 vm_space_clear_txn:它仍綁著憑證,end 之後經由 VM 的承諾也必須照一般上限。
 * 負對照:拿掉 mb_exec_end(或 end 不清 active)→ 兩個「after end」斷言都失敗。
 * 限制:proc.c 的 exec 路徑是否每條出口都呼叫 end,要使用者層 exec 測試或靜態探針。
 */
void exec_txn_end_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(400);
    struct mb_exec_txn t;
    struct mb_stats st, s1;
    KASSERT(d);
    struct vm_space *old = vm_space_create(d);
    KASSERT(old && vm_space_commit_anon(old, heap, 300 * PAGE_SIZE) == 0);
    KASSERT(mb_exec_begin(d, vm_space_quota_total(old), &t) == 0);
    struct vm_space *nw = vm_space_create_txn(d, &t);
    KASSERT(nw && vm_space_commit_anon(nw, heap, 300 * PAGE_SIZE) == 0);
    vm_space_destroy(old);                                 /* 舊空間退完:total 回到上限內 */
    mb_stats(d, &st);
    KASSERT(st.total <= st.limit);
    unsigned long x = st.limit - st.total + 1;
    KASSERT(x <= t.pending);                               /* 探針落在放寬區間內 */
    KASSERT(mb_quota_add_txn(d, MB_KOBJ, x, &t) == 0);    /* 正對照:交易中放寬 */
    KASSERT(mb_quota_sub(d, MB_KOBJ, x) == 0);

    mb_exec_end(&t);
    mb_stats(d, &st);
    if (mb_quota_add_txn(d, MB_KOBJ, x, &t) != -E_NOMEM)
        panic("exec: charge after mb_exec_end still used the relaxed limit");
    if (vm_space_commit_anon(nw, heap + 300 * PAGE_SIZE, x * PAGE_SIZE) != -E_NOMEM)
        panic("exec: new space still bound to an ended transaction committed past the limit");
    mb_stats(d, &s1);
    KASSERT(s1.total == st.total && s1.used[MB_KOBJ] == st.used[MB_KOBJ]);
    vm_space_clear_txn(nw);
    vm_space_destroy(nw);
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("exec: after mb_exec_end the token no longer relaxes the limit (probe %lu pages)\n", x);
}

/*
 * 6.2 / 6.3「交易中途調低 limit」:最終檢查以檢查當下的 limit 為準。
 * 正對照:調低前通過;limit 恰好等於新空間用量時通過(邊界);再低 1 → -E_NOMEM、
 * 不算帳務錯誤、沒有副作用;交易中的放寬上限也跟著變低(新承諾被拒絕)。
 * 失敗路徑照 proc.c:清憑證、銷毀新空間、結束交易,舊空間的帳原封不動。
 * 負對照:check_final 改用 begin 時記下的 limit(或用 limit + pending 比較)→「lowered」斷言失敗。
 */
void exec_txn_lower_limit_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(400);
    struct mb_exec_txn t;
    struct mb_stats st, s1;
    KASSERT(d);
    struct vm_space *old = vm_space_create(d);
    KASSERT(old && vm_space_commit_anon(old, heap, 300 * PAGE_SIZE) == 0);
    KASSERT(mb_exec_begin(d, vm_space_quota_total(old), &t) == 0);
    struct vm_space *nw = vm_space_create_txn(d, &t);
    KASSERT(nw && vm_space_commit_anon(nw, heap, 300 * PAGE_SIZE) == 0);

    unsigned long old_now = vm_space_quota_total(old);
    mb_stats(d, &st);
    unsigned long need = st.total - old_now;               /* 舊空間退完之後的用量 = 新空間的帳 */
    KASSERT(need > 0 && need <= st.limit && mb_exec_check_final(&t, old_now) == 0);
    mb_set_limit(d, need);
    KASSERT(mb_exec_check_final(&t, old_now) == 0);       /* 邊界:恰好等於上限 */
    mb_set_limit(d, need - 1);
    unsigned long err0 = mb_accounting_errors();
    mb_stats(d, &st);
    if (mb_exec_check_final(&t, old_now) != -E_NOMEM)
        panic("exec: final check accepted after the limit was lowered to %lu (need %lu)", need - 1, need);
    KASSERT(mb_accounting_errors() == err0);
    KASSERT(vm_space_commit_anon(nw, heap + 300 * PAGE_SIZE, PAGE_SIZE) == -E_NOMEM);   /* 放寬上限也變低 */
    mb_stats(d, &s1);
    KASSERT(s1.total == st.total);

    vm_space_clear_txn(nw);
    vm_space_destroy(nw);
    mb_exec_end(&t);
    mb_stats(d, &st);
    KASSERT(st.total == vm_space_quota_total(old));
    vm_space_destroy(old);
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("exec: lowering the limit mid-transaction makes the final check refuse (need %lu)\n", need);
}

/*
 * I-240 C3.2 (b1) 第 4、5 步:fork 的兩筆帳(R1)。父空間承諾 15 頁、觸及其中 10 頁,fork 之後:
 * 子空間 15 張票(每一頁承諾一張);父空間 5 張未兌現 + 10 張(那 10 頁變成 COW + TICKET)。
 * 一般配置把記憶體用到預留邊界之後:父寫入 10 頁(共用中,用票複製)、子寫入同樣 10 頁
 * (只剩自己,退票)、子第一次觸及沒碰過的 5 頁 — 全部成功。每一步核對「兩個空間的票 = pmm 增加的預留」。
 * 另含失敗注入:子空間第一張表就建不出來時,父空間完全不變(先建子、後標父)。
 * 負對照:改回「建子時同步標記父」(第 54 步實測)時,失敗注入那段「父的頁仍是可寫獨佔」斷言失敗。
 */
void fork_ticket_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *dp = mb_domain_new(100000), *dc = mb_domain_new(100000);
    struct vm_space *ps = vm_space_create(dp);
    KASSERT(dp && dc && ps);
    KASSERT(vm_space_commit_anon(ps, heap, 15 * PAGE_SIZE) == 0);
    for (int i = 0; i < 10; i++)
        KASSERT(vm_space_fault_anon(ps, heap + i * PAGE_SIZE) == 0);

    struct vm_range r = { heap, 15 * PAGE_SIZE };

    /* 失敗注入(Codex 183):子 domain 的配額剛好只夠根表 + 15 張資料票 + 2 張頁表票,
       第一趟替子建第一張表時配額不足 → fork 失敗。父空間的 PTE、票與 pmm 預留必須完全不變 */
    struct vm_ledger before, after;
    vm_space_ledger(ps, &before);
    size_t rb = pmm_reserved_count();
    struct mb_domain *tight = mb_domain_new(1 + 15 + 2);
    KASSERT(tight);
    KASSERT(vm_space_fork(ps, tight, &r, 1) == NULL);
    vm_space_ledger(ps, &after);
    KASSERT(after.data_tickets == before.data_tickets && after.pt_tickets == before.pt_tickets);
    KASSERT(pmm_reserved_count() == rb);
    for (int i = 0; i < 10; i++) {
        unsigned f = 0;
        uintptr_t pa;
        KASSERT(vm_space_query(ps, heap + i * PAGE_SIZE, &pa, &f) == 0);
        KASSERT((f & VM_WRITE) && !(f & (VM_COW | VM_TICKET)));    /* 父的頁仍是可寫獨佔 */
    }
    KASSERT(mb_domain_put(tight) == 0);

    /* U1:範圍外的可寫私有頁 → fork 失敗,父不變(第 (c2) 批) */
    void *stray = pmm_alloc();
    KASSERT(stray);
    KASSERT(vm_space_map(ps, heap + 100 * PAGE_SIZE, (uintptr_t)stray, VM_READ | VM_WRITE) == 0);
    rb = pmm_reserved_count();
    KASSERT(vm_space_fork(ps, dc, &r, 1) == NULL && pmm_reserved_count() == rb);
    KASSERT(vm_space_unmap(ps, heap + 100 * PAGE_SIZE) == 0);   /* 拿掉,之後的 fork 才合法 */

    struct vm_space *cs = vm_space_fork(ps, dc, &r, 1);
    KASSERT(cs);
    struct vm_ledger lp, lc;
    vm_space_ledger(ps, &lp);
    vm_space_ledger(cs, &lc);
    KASSERT(lc.data_tickets == 15 && lp.data_tickets == 15);
    KASSERT(lp.data_tickets + lp.pt_tickets + lc.data_tickets + lc.pt_tickets == pmm_reserved_count() - r0);

    void *chain = NULL;
    for (void **pg; (pg = pmm_alloc()) != NULL; ) {
        *pg = chain;
        chain = pg;
    }
    for (int i = 0; i < 10; i++)
        KASSERT(vm_space_fault(ps, heap + i * PAGE_SIZE, 1) == 0);   /* 共用中:用父的票複製 */
    for (int i = 0; i < 10; i++)
        KASSERT(vm_space_fault(cs, heap + i * PAGE_SIZE, 1) == 0);   /* 只剩自己:退子的票 */
    for (int i = 10; i < 15; i++)
        KASSERT(vm_space_fault_anon(cs, heap + i * PAGE_SIZE) == 0); /* 未兌現:用子的票 */
    vm_space_ledger(ps, &lp);
    vm_space_ledger(cs, &lc);
    KASSERT(lp.data_tickets == 5 && lc.data_tickets == 0);
    KASSERT(lp.data_tickets + lp.pt_tickets + lc.data_tickets + lc.pt_tickets == pmm_reserved_count() - r0);
    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }

    vm_space_destroy(cs);
    vm_space_destroy(ps);
    KASSERT(pmm_reserved_count() == r0);
    KASSERT(mb_domain_put(dc) == 0 && mb_domain_put(dp) == 0);
    kprintf("anon: fork ticket test ok (parent and child each wrote shared pages and touched new ones "
            "after ordinary allocations reached the reservation)\n");
}

/*
 * I-240 C3.2 (b2):執行緒的 KOBJ 記帳與「先準備、後啟動」。
 * 準備時記 KOBJ(kernel stack 精確 + 結構取整)並持有 domain 參考;配額不夠時準備失敗、什麼都不留;
 * 放棄時全部退回;啟動、結束、被回收之後也全部退回。
 * 負對照:thread_free 不退配額(第 54 步實測)時,「回收後 KOBJ 歸零」斷言失敗。
 */
static volatile int kobj_ran;
static void kobj_fn(void *arg) { (void)arg; kobj_ran = 1; }

void thread_kobj_test(void)
{
    struct mb_domain *d = mb_domain_new(1000);
    struct mb_stats st;
    KASSERT(d);

    struct thread *t = thread_prepare("kobj", kobj_fn, NULL, PRIO_NORMAL, d);
    KASSERT(t);
    mb_stats(d, &st);
    unsigned long k = st.used[MB_KOBJ];
    /* 獨立算出應有的量(Codex 190):kernel stack 4 頁(與 sched.c 的 STACK_PAGES 一致)+ 保護頁 1 頁,
       加上結構以實際配置大小向上取整;精確比對,少算保護頁也會被抓到 */
    unsigned long expect = 4 + 1 + (kmalloc_size(t) + PAGE_SIZE - 1) / PAGE_SIZE;
    KASSERT(k == expect && st.refs == 2);
    thread_abandon(t);
    mb_stats(d, &st);
    KASSERT(st.used[MB_KOBJ] == 0 && st.refs == 1);

    mb_set_limit(d, expect - 1);                  /* 差一頁:準備失敗,不留任何東西 */
    KASSERT(thread_prepare("kobj", kobj_fn, NULL, PRIO_NORMAL, d) == NULL);
    mb_stats(d, &st);
    KASSERT(st.used[MB_KOBJ] == 0 && st.refs == 1);

    mb_set_limit(d, 1000);
    kobj_ran = 0;
    t = thread_prepare("kobj", kobj_fn, NULL, PRIO_NORMAL, d);
    KASSERT(t);
    int tid = thread_start(t);
    KASSERT(tid > 0);
    thread_join(tid);
    sched_trim();                                 /* 等延遲回收完成 */
    mb_stats(d, &st);
    KASSERT(kobj_ran && st.used[MB_KOBJ] == 0 && st.refs == 1);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("sched: thread kernel-object accounting ok (%lu pages per thread; refused at the limit, "
            "returned on abandon and after reaping)\n", k);
}

/*
 * I-240 C3.2 (b2):檔案與管線由建立者的 domain 持有。
 * 一條帶著 domain 的執行緒建立一條管線(兩個 struct file + 一個 struct pipe),不關閉就結束;
 * 執行緒被回收之後,domain 仍記著這些物件的 KOBJ、仍被它們持有參考(建立者先結束,物件還在);
 * 由別人關閉最後一個參考時,KOBJ 歸零、參考只剩 domain 自己的一份。
 * 負對照:vfs_close 不退配額(第 55 步實測)時,「關閉後 KOBJ 歸零」斷言失敗。
 */
static struct file *pobj_r, *pobj_w;
static void pobj_fn(void *arg)
{
    (void)arg;
    KASSERT(vfs_pipe(&pobj_r, &pobj_w) == 0);
}

void pipe_kobj_test(void)
{
    struct mb_domain *d = mb_domain_new(1000);
    struct mb_stats st;
    KASSERT(d);
    struct thread *t = thread_prepare("pobj", pobj_fn, NULL, PRIO_NORMAL, d);
    KASSERT(t);
    int tid = thread_start(t);
    thread_join(tid);
    sched_trim();                                 /* 建立者已結束並被回收 */
    mb_stats(d, &st);
    /* 獨立算出應有的量(Codex 196):兩個 struct file 與 struct pipe(它的第一個成員就是 inode)
       各以實際配置大小向上取整成頁,精確比對 */
    unsigned long expect = mb_pages_for_bytes(kmalloc_size(pobj_r)) + mb_pages_for_bytes(kmalloc_size(pobj_w)) +
                           mb_pages_for_bytes(kmalloc_size(vfs_file_inode(pobj_r)));
    KASSERT(st.used[MB_KOBJ] == expect && st.refs == 4);   /* 2 個檔案 + 1 條管線,各持一份參考 */
    unsigned long k = st.used[MB_KOBJ];
    vfs_close(pobj_w);
    vfs_close(pobj_r);                            /* 最後一個參考:管線也釋放 */
    mb_stats(d, &st);
    KASSERT(st.used[MB_KOBJ] == 0 && st.refs == 1);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("vfs: pipe and files stay charged to their creator after it exits (%lu pages), "
            "returned on the last close\n", k);
}

/*
 * Codex 209 的 ramfs 缺陷:在檔案結尾之後寫入,第一個資料頁就配置不到時,系統呼叫回報錯誤,
 * 但檔案長度卻被加大(什麼都沒寫進去,檔案變長了)。這裡把一般配置吃到預留邊界,
 * 在空檔案的 8192 位置寫入:必須回傳 -E_NOMEM,而且長度維持 0。
 * 負對照:拿掉 ramfs_write 的「done > 0」條件(第 55 步實測)時,長度變成 8193,斷言失敗。
 */
void ramfs_eof_test(void)
{
    struct file *f;
    struct stat st;
    KASSERT(vfs_open("/eoftest", O_CREAT | O_RDWR | O_TRUNC, &f) == 0);
    void *chain = NULL;
    for (void **pg; (pg = pmm_alloc()) != NULL; ) {
        *pg = chain;
        chain = pg;
    }
    char c = 'x';
    KASSERT(vfs_seek(f, 8192, SEEK_SET) == 8192);
    long rc = vfs_write(f, &c, 1);
    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }
    KASSERT(vfs_stat("/eoftest", &st) == 0);
    kprintf("ramfs: write past EOF with no memory -> %ld, size stays %llu\n", rc, (unsigned long long)st.size);
    KASSERT(rc == -E_NOMEM && st.size == 0);
    vfs_close(f);
    KASSERT(vfs_unlink("/eoftest") == 0);
}

/*
 * I-240 C3.2 (c1):stack 的 I2。承諾一整格 stack(255 頁,保護頁不承諾)之後,
 * 一般配置把記憶體用到預留邊界,整格 255 頁第一次觸及仍然全部成功(含需要新中間表的);
 * 歸還後票與配額全部退回。負對照見 DEVLOG(stack fault 改回一般配置 → 飽和時失敗)。
 */
void stack_ticket_test(void)
{
    uintptr_t sbase, slen;
    KASSERT(vm_anon_window(2, &sbase, &slen) == 0);      /* 第三個承諾窗 = stack 區 */
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(d && vs);
    uintptr_t base = sbase + PAGE_SIZE, len = 255 * PAGE_SIZE;   /* 第一格,略過保護頁 */
    KASSERT(vm_space_commit(vs, MB_STACK, base, len) == 0);
    struct mb_stats st;
    mb_stats(d, &st);
    KASSERT(st.used[MB_STACK] == 255);

    void *chain = NULL;
    for (void **pg; (pg = pmm_alloc()) != NULL; ) {
        *pg = chain;
        chain = pg;
    }
    for (uintptr_t va = base + len; va > base; va -= PAGE_SIZE)  /* 由上往下,像 stack 一樣長 */
        KASSERT(vm_space_fault_anon(vs, va - PAGE_SIZE) == 0);
    struct vm_ledger l;
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0);
    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }
    vm_space_release(vs, MB_STACK, base, len);
    mb_stats(d, &st);
    KASSERT(st.used[MB_STACK] == 0);
    vm_space_destroy(vs);
    KASSERT(pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("stack: a committed 255-page stack slot was fully touched after ordinary allocations "
            "reached the reservation\n");
}

/*
 * I-240 C3.2 stack 測試 T1/T2/T4/T5 補強(Codex 166)。
 * proc.c 的 alloc_user_stack / commit_user_stack / release_user_stack 與 proc_mem.c 的 stack fault
 * 過濾都是 static、而且要有使用者行程,kernel 自我測試(排程器啟動前)叫不到。下面的 ts_* 在
 * vm / stack 格層級逐行鏡射那些步驟(每個函式註明對應的產品行),帳本走的是真的 vm.c / membudget.c / pmm.c。
 * 因此:vm.c / membudget.c / pmm.c 的負對照會讓這裡失敗;proc.c / proc_mem.c 的負對照必須同時改鏡射
 * (或之後在 proc.c 加測試鉤子,像 proc_thread_failure_probe 那樣,讓這裡直接呼叫產品程式碼)。
 * 「併發」以確定的交錯模擬(A、B 都先佔格,再依序承諾),不是兩條真的執行緒。
 */
#define TS_SLOTS 16u                         /* = PROC_MAX_THREADS(kernel/proc_internal.h:57) */
#define TS_TOUCH_ON_COMMIT 4u                /* = USER_STACK_PAGES(kernel/proc_internal.h:56) */

struct ts_geo { uintptr_t sbase, slen, slot; unsigned long pages; };

static void ts_geo_get(struct ts_geo *g)
{
    KASSERT(vm_anon_window(2, &g->sbase, &g->slen) == 0);   /* 第三個承諾窗 = 16 格 stack 區 */
    g->slot = g->slen / TS_SLOTS;
    KASSERT(g->slen % TS_SLOTS == 0 && g->slot % PAGE_SIZE == 0 && g->slot >= 8 * PAGE_SIZE);
    g->pages = g->slot / PAGE_SIZE - 1;                       /* 保護頁不承諾(proc.c:50) */
}
static uintptr_t ts_top(const struct ts_geo *g, unsigned s)   { return g->sbase + g->slen - (uintptr_t)s * g->slot; }  /* proc.c:54 */
static uintptr_t ts_guard(const struct ts_geo *g, unsigned s) { return ts_top(g, s) - g->slot; }                      /* slot_base */
static uintptr_t ts_base(const struct ts_geo *g, unsigned s)  { return ts_guard(g, s) + PAGE_SIZE; }

/* proc.c:84-92:佔最低的空格 */
static int ts_claim(uint32_t *slots)
{
    for (unsigned i = 0; i < TS_SLOTS; i++)
        if (!(*slots & (1u << i))) {
            *slots |= 1u << i;
            return (int)i;
        }
    return -1;
}
/* proc.c:59-68:承諾整格並以票兌現最上面幾頁;失敗回傳非 0、什麼都沒改 */
static int ts_commit(struct vm_space *vs, const struct ts_geo *g, unsigned s)
{
    int rc = vm_space_commit(vs, MB_STACK, ts_base(g, s), g->pages * PAGE_SIZE);
    if (rc != 0)
        return rc;
    for (unsigned i = 1; i <= TS_TOUCH_ON_COMMIT; i++)
        KASSERT(vm_space_fault_anon(vs, ts_top(g, s) - i * PAGE_SIZE) == 0);
    return 0;
}
/* proc.c:99:承諾失敗,只放自己的格(負對照:改成 *slots = 0) */
static void ts_rollback(uint32_t *slots, unsigned s) { *slots &= ~(1u << s); }
/* proc.c:109-117:先清 ready、再退頁與票、最後放格 */
static void ts_release(struct vm_space *vs, const struct ts_geo *g, uint32_t *slots, uint32_t *ready, unsigned s)
{
    *ready &= ~(1u << s);
    vm_space_release(vs, MB_STACK, ts_base(g, s), g->pages * PAGE_SIZE);
    *slots &= ~(1u << s);
}
/* proc_mem.c:85-98:stack 區的 fault 過濾(格不在、保護頁、未 ready → -1;否則用票兌現) */
static int ts_fault(struct vm_space *vs, const struct ts_geo *g, uint32_t slots, uint32_t ready, uintptr_t addr)
{
    if (addr < g->sbase || addr >= g->sbase + g->slen)
        return -1;
    unsigned s = (unsigned)((g->sbase + g->slen - 1 - addr) / g->slot);
    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    if (s >= TS_SLOTS || !(slots & (1u << s)) || page == ts_guard(g, s))
        return -1;
    if (!(ready & (1u << s)))
        return -1;
    return vm_space_fault_anon(vs, page) < 0 ? -1 : 0;
}

/* 一般配置吃到底線,再用應急配置把底線也吃光:之後只剩預留(兌現)拿得到頁 */
static void ts_exhaust(void **normal, void **emerg)
{
    while (chain_take(normal, take_normal))
        ;
    if (emerg)
        while (chain_take(emerg, take_emergency))
            ;
    KASSERT(pmm_alloc() == NULL);
}

static int ts_ledger_eq(const struct vm_ledger *a, const struct vm_ledger *b)
{
    return a->data_tickets == b->data_tickets && a->pt_tickets == b->pt_tickets &&
           a->anon_tables == b->anon_tables && a->anon_quota == b->anon_quota && a->pt_quota == b->pt_quota;
}
static int ts_stats_eq(const struct mb_stats *a, const struct mb_stats *b)
{
    if (a->total != b->total)
        return 0;
    for (unsigned c = 0; c < MB_NCLASS; c++)
        if (a->used[c] != b->used[c])
            return 0;
    return 1;
}
static void ts_pt_bound(struct vm_space *vs, const char *where)
{
    struct vm_ledger l;
    vm_space_ledger(vs, &l);
    if (l.anon_tables + l.pt_tickets > vm_anon_pt_max())
        panic("stack: E %lu + T %lu > M %lu at %s", l.anon_tables, l.pt_tickets, vm_anon_pt_max(), where);
}

/*
 * T5 + Codex 166:記憶體耗盡時歸還(執行緒結束)。已承諾並觸及 n 頁的一格,在一般配置與應急底線都被吃光、
 * 而且 FI_PMM 第 1 次就注入失敗的情況下歸還:不 panic、不做任何一般配置(failinj 沒有觸發)、
 * 格與 ready 位元清掉、退 255-n 張資料票、n 頁回到 pmm、STACK 配額 -255、E + T <= M、
 * destroy 後 pmm 預留回到基準。
 * 負對照:vm_space_release(vm.c:1290 起)在歸還路徑做一次一般配置(例如段開始時 pmm_alloc 一頁暫存)
 *   → failinj_fired() 為 1(或拿到 NULL 而 panic),「歸還不配置」斷言失敗。
 */
void stack_release_exhausted_test(void)
{
    struct ts_geo g;
    ts_geo_get(&g);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(d && vs);
    uint32_t slots = 0, ready = 0;

    int s = ts_claim(&slots);
    KASSERT(s == 0 && ts_commit(vs, &g, (unsigned)s) == 0);
    ready |= 1u << s;
    const unsigned long touched = 64;                      /* 含 commit 已兌現的 4 頁 */
    for (unsigned long i = TS_TOUCH_ON_COMMIT + 1; i <= touched; i++)
        KASSERT(ts_fault(vs, &g, slots, ready, ts_top(&g, (unsigned)s) - i * PAGE_SIZE) == 0);

    struct vm_ledger l1, l2;
    struct mb_stats st1, st2;
    vm_space_ledger(vs, &l1);
    KASSERT(l1.data_tickets == g.pages - touched);
    anon_check(vs, r0, "release-exhausted: before");
    ts_pt_bound(vs, "release-exhausted: before");

    void *normal = NULL, *emerg = NULL;
    ts_exhaust(&normal, &emerg);
    mb_stats(d, &st1);
    KASSERT(st1.used[MB_STACK] == g.pages);
    size_t res1 = pmm_reserved_count(), free1 = pmm_free_count();

    failinj_arm(FI_PMM, 1);                                /* 歸還路徑若做一般配置就會被抓到 */
    ts_release(vs, &g, &slots, &ready, (unsigned)s);
    int fired = failinj_fired();
    failinj_disarm();
    if (fired)
        panic("stack: release under exhaustion made an ordinary allocation");

    vm_space_ledger(vs, &l2);
    mb_stats(d, &st2);
    KASSERT(slots == 0 && ready == 0);
    KASSERT(l2.data_tickets == 0 && l2.pt_tickets == l1.pt_tickets && l2.anon_tables == l1.anon_tables);
    KASSERT(st2.used[MB_STACK] == 0 && st2.total == st1.total - g.pages);
    KASSERT(pmm_reserved_count() == res1 - (g.pages - touched));   /* 只退未兌現的票,一張不多不少 */
    KASSERT(pmm_free_count() == free1 + touched);                   /* 觸及過的頁全部回到 pmm */
    anon_check(vs, r0, "release-exhausted: after");
    ts_pt_bound(vs, "release-exhausted: after");

    chain_free(&emerg);
    chain_free(&normal);
    KASSERT(ts_claim(&slots) == s);                         /* 格可重用 */
    vm_space_destroy(vs);
    mb_stats(d, &st2);
    KASSERT(st2.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("stack: slot with %lu touched pages released with memory exhausted: %lu tickets and %lu pages "
            "returned, no ordinary allocation\n", touched, g.pages - touched, touched);
}

/*
 * T4:兩個建立者都先佔格(A = 0、B = 1),A 承諾成功後 B 承諾失敗 — 一次因配額(mb_set_limit),
 * 一次因 pmm 預留(一般配置吃到底線)。B 只放自己的格:stack_slots / stack_ready == A 的位元,
 * A 的帳本、domain 用量、pmm 預留與 B 失敗前完全相同,A 整格仍可全部觸及;條件恢復後 B 可以重拿第 1 格。
 * failinj 不適用於注入 commit 失敗:vm_space_commit 不做一般配置(只呼叫 pmm_reserve),
 * 所以這裡反過來用 FI_PMM 證明「承諾路徑沒有一般配置」。
 * 負對照:alloc_user_stack 的失敗回退(proc.c:99)改成清掉整張 stack_slots(鏡射 ts_rollback 同改)
 *   → 「slots == A 的位元」斷言失敗;vm_space_commit 失敗時仍加了 data_tickets(vm.c:1246-1251 搬到 if 外)
 *   → 帳本相等斷言失敗。
 */
void stack_concurrent_fail_test(void)
{
    struct ts_geo g;
    ts_geo_get(&g);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(d && vs);
    uint32_t slots = 0, ready = 0;

    int a = ts_claim(&slots), b = ts_claim(&slots);         /* 兩者都已佔格,尚未承諾 */
    KASSERT(a == 0 && b == 1 && slots == 3u);
    KASSERT(ts_commit(vs, &g, (unsigned)a) == 0);
    ready |= 1u << a;

    struct vm_ledger la, l;
    struct mb_stats sa, st;
    vm_space_ledger(vs, &la);
    mb_stats(d, &sa);
    size_t ra = pmm_reserved_count();

    for (int round = 0; round < 2; round++) {
        void *normal = NULL;
        if (round == 0)
            mb_set_limit(d, sa.total + g.pages - 1);        /* 差一頁:B 的配額不夠 */
        else
            ts_exhaust(&normal, NULL);                      /* pmm_reserve 失敗 */
        failinj_arm(FI_PMM, 1);
        int rc = ts_commit(vs, &g, (unsigned)b);
        int fired = failinj_fired();
        failinj_disarm();
        KASSERT(rc == -E_NOMEM && !fired);
        ts_rollback(&slots, (unsigned)b);

        vm_space_ledger(vs, &l);
        mb_stats(d, &st);
        if (slots != (1u << a) || ready != (1u << a))
            panic("stack: T4 round %d failed creator left slots 0x%x ready 0x%x", round, slots, ready);
        KASSERT(ts_ledger_eq(&l, &la) && ts_stats_eq(&st, &sa) && pmm_reserved_count() == ra);
        anon_check(vs, r0, round ? "T4 reserve-fail" : "T4 quota-fail");

        chain_free(&normal);
        mb_set_limit(d, 100000);
        b = ts_claim(&slots);                               /* 重拿:仍是第 1 格 */
        KASSERT(b == 1);
    }

    /* A 不受影響:整格觸及成功、資料票用完 */
    for (unsigned long i = TS_TOUCH_ON_COMMIT + 1; i <= g.pages; i++)
        KASSERT(ts_fault(vs, &g, slots, ready, ts_top(&g, (unsigned)a) - i * PAGE_SIZE) == 0);
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == 0);

    /* 條件恢復後 B 承諾成功 */
    KASSERT(ts_commit(vs, &g, (unsigned)b) == 0);
    ready |= 1u << b;
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == g.pages - TS_TOUCH_ON_COMMIT);
    ts_pt_bound(vs, "T4 both");
    ts_release(vs, &g, &slots, &ready, (unsigned)b);
    ts_release(vs, &g, &slots, &ready, (unsigned)a);
    KASSERT(slots == 0 && ready == 0);
    mb_stats(d, &st);
    KASSERT(st.used[MB_STACK] == 0);
    vm_space_destroy(vs);
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("stack: T4 failed slot commit (quota, reservation) released only its own slot; "
            "the other slot's tickets were untouched\n");
}

/*
 * T1 完整性:新位址空間承諾一格(所有中間表都還不存在,承諾時預付 pt 票),再把一般配置與應急底線吃光、
 * FI_PMM 第 1 次就注入失敗,第一次觸及仍成功:建了幾張表就正好消耗幾張 pt 票(T 減 = E 增)、
 * 沒有一般配置;整格觸及後 T == 0、E + T <= M。
 * 負對照:vm_space_commit 不收 pt 票(vm.c:1240 的 pt 改成 0、或拿掉 vm.c:1249 的 pt_tickets += pt)
 *   → 第一次觸及在 new_table panic「anonymous-memory table ... without a prepaid ticket」(vm.c:270-271);
 *   new_table 的 WALK_ANON 改用 table_alloc → failinj 觸發 / 飽和時拿不到表。
 */
void stack_pt_boundary_test(void)
{
    struct ts_geo g;
    ts_geo_get(&g);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(d && vs);
    const unsigned s = 0;
    KASSERT(vm_space_commit(vs, MB_STACK, ts_base(&g, s), g.pages * PAGE_SIZE) == 0);  /* 先不兌現 */
    struct vm_ledger l0, l1;
    vm_space_ledger(vs, &l0);
    /* PGTABLE 配額 = 根表 1 頁(建立位址空間時就記,vm.c 的 vm_space_create)+ 預付的中間表票 */
    if (!(l0.pt_tickets > 0 && l0.anon_tables == 0 && l0.pt_quota == l0.pt_tickets + 1))
        panic("T1 commit ledger: pt_tickets %lu anon_tables %lu pt_quota %lu", l0.pt_tickets, l0.anon_tables, l0.pt_quota);
    ts_pt_bound(vs, "T1 commit");

    void *normal = NULL, *emerg = NULL;
    ts_exhaust(&normal, &emerg);
    uintptr_t va = ts_top(&g, s) - PAGE_SIZE;
    failinj_arm(FI_PMM, 1);
    int rc = vm_space_fault_anon(vs, va);
    int fired = failinj_fired();
    failinj_disarm();
    KASSERT(rc == 0 && !fired);
    vm_space_ledger(vs, &l1);
    KASSERT(l1.anon_tables > 0 && l0.pt_tickets - l1.pt_tickets == l1.anon_tables);   /* 每張表一張票 */
    KASSERT(l1.data_tickets == g.pages - 1 && l1.pt_quota == l0.pt_quota);           /* 配額在承諾時已記 */
    uintptr_t pa;
    unsigned fl;
    KASSERT(vm_space_query(vs, va, &pa, &fl) == 0 && (fl & VM_WRITE));
    anon_check(vs, r0, "T1 first touch");

    for (uintptr_t a = va; a > ts_base(&g, s); a -= PAGE_SIZE)
        KASSERT(vm_space_fault_anon(vs, a - PAGE_SIZE) == 0);
    vm_space_ledger(vs, &l1);
    KASSERT(l1.data_tickets == 0 && l1.pt_tickets == 0);
    ts_pt_bound(vs, "T1 full");
    anon_check(vs, r0, "T1 full");

    chain_free(&emerg);
    chain_free(&normal);
    vm_space_release(vs, MB_STACK, ts_base(&g, s), g.pages * PAGE_SIZE);
    vm_space_destroy(vs);
    struct mb_stats st;
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("stack: T1 first touch at the reservation boundary built %lu tables from %lu prepaid tickets\n",
            l0.pt_tickets, l0.pt_tickets);
}

/*
 * T2 保護頁:格的最低一頁不承諾、不對映。觸及它(頁內任一位址、讀或寫)在 VM 層與 stack fault 過濾層
 * 都回傳 -1(行程因此被終止),帳本、domain 用量、pmm 預留都不變;STACK 配額只有 255 頁。
 * 正對照:同一格的最低可用頁(保護頁上一頁)照常用一張票兌現。
 * 限制:真正的「終止行程」路徑(proc_page_fault → 訊號)要使用者行程,這裡只驗到過濾器回傳 -1;
 * 完整的 T2 還需要一個使用者模式測試(例如 crash=guard)。
 * 負對照:commit_user_stack 把保護頁也承諾(proc.c:61 的 base 少加 PAGE_SIZE,鏡射 ts_base 同改)
 *   → used[STACK] == 255 斷言失敗;拿掉 proc_mem.c:90 的 page == slot_base(鏡射 ts_fault 同改)
 *   → I-340 之後 VM 層仍拒絕(保護頁不在承諾位元圖裡),這個測試照樣通過 — 這正是縱深防禦的目的;
 *   要讓它失敗必須同時拿掉 vm.c vm_space_fault_anon 的 cmap_test(此時直接呼叫 VM 的那一行也失敗)。
 */
void stack_guard_test(void)
{
    struct ts_geo g;
    ts_geo_get(&g);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(100000);
    struct vm_space *vs = vm_space_create(d);
    KASSERT(d && vs);
    uint32_t slots = 0, ready = 0;
    int s = ts_claim(&slots);
    KASSERT(s == 0 && ts_commit(vs, &g, (unsigned)s) == 0);
    ready |= 1u << s;
    uint32_t s1 = (uint32_t)ts_claim(&slots);               /* 下面一格也在用:保護頁兩側都是有效 stack */
    KASSERT(s1 == 1 && ts_commit(vs, &g, s1) == 0);
    ready |= 1u << s1;

    struct vm_ledger l0, l;
    struct mb_stats st0, st;
    vm_space_ledger(vs, &l0);
    mb_stats(d, &st0);
    KASSERT(st0.used[MB_STACK] == 2 * g.pages);              /* 保護頁沒有承諾 */
    size_t res0 = pmm_reserved_count();

    uintptr_t guard = ts_guard(&g, (unsigned)s);
    const uintptr_t probes[] = { guard, guard + 8, guard + PAGE_SIZE - 1 };
    for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        uintptr_t pa;
        unsigned fl;
        KASSERT(vm_space_query(vs, probes[i], &pa, &fl) != 0);
        KASSERT(vm_space_fault(vs, probes[i], 1) == -1 && vm_space_fault(vs, probes[i], 0) == -1);
        KASSERT(ts_fault(vs, &g, slots, ready, probes[i]) == -1);
        KASSERT(vm_space_fault_anon(vs, probes[i]) == -1);   /* I-340:繞過過濾器,VM 層自己拒絕 */
    }
    vm_space_ledger(vs, &l);
    mb_stats(d, &st);
    if (!ts_ledger_eq(&l, &l0) || !ts_stats_eq(&st, &st0) || pmm_reserved_count() != res0)
        panic("stack: touching the guard page changed the ledger (data %lu -> %lu)", l0.data_tickets, l.data_tickets);

    /* 正對照:保護頁上一頁照常兌現,正好用一張票 */
    KASSERT(ts_fault(vs, &g, slots, ready, ts_base(&g, (unsigned)s)) == 0);
    vm_space_ledger(vs, &l);
    KASSERT(l.data_tickets == l0.data_tickets - 1);
    anon_check(vs, r0, "T2");

    ts_release(vs, &g, &slots, &ready, s1);
    ts_release(vs, &g, &slots, &ready, (unsigned)s);
    vm_space_destroy(vs);
    mb_stats(d, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("stack: T2 guard page faults without consuming tickets\n");
}

/*
 * I-340:VM 層自己拒絕「不在已承諾範圍內」的匿名 fault(縱深防禦;不靠 proc_mem.c 的過濾)。
 * 每一個拒絕都在「空間裡還有很多票」的狀態下做,並核對帳本、domain、pmm 預留、頁表與 anon_tables 全部不變
 * (未承諾的位址不能用掉別處的資料票,也不能用掉預付的頁表票)。
 *   1. 窗內但沒承諾的頁:heap 承諾段的下一頁、mmap 段之後的空頁、stack 格的保護頁(直接呼叫 VM,不經 ts_fault)
 *   2. 部分歸還:尾端(sbrk 縮小)與中段(一頁)歸還後,那些頁的 fault 失敗;沒歸還的照常兌現;
 *      尾端重新承諾後又能兌現;重複承諾同一頁被拒絕(-E_INVAL)、帳不變
 *   3. fork:子繼承父的承諾範圍(含中段的洞);子歸還一頁不影響父(位元圖是複本,不共用)
 * 負對照(各自單獨做,預期失敗的斷言):
 *   - 拿掉 vm.c vm_space_fault_anon 的 cmap_test → 第 1 步的 fault 回傳 0、消耗一張票,KASSERT(... == -1) 失敗
 *   - vm_space_release 的 CMAP_CLEAR 改成 CMAP_COUNT → 第 2 步歸還過的頁仍可兌現,斷言失敗
 *   - 拿掉 vm_space_fork 的 memcpy(child->cmap...) → 第 3 步子對已承諾頁的 fault 回傳 -1,斷言失敗
 *   - vm_space_commit 拿掉重複承諾的檢查 → 第 2 步的 -E_INVAL 斷言失敗
 */
static void cm_expect_refused(struct vm_space *vs, struct mb_domain *d, uintptr_t va, const char *what)
{
    struct vm_ledger l0, l;
    struct mb_stats st0, st;
    vm_space_ledger(vs, &l0);
    mb_stats(d, &st0);
    size_t res0 = pmm_reserved_count();
    unsigned long pages0 = vm_space_pages(vs);
    KASSERT(l0.data_tickets > 0);                            /* 有票可用:拒絕不是因為缺票 */
    if (vm_space_fault_anon(vs, va) != -1)
        panic("cmap: anonymous fault at %p (%s) outside every committed range was honoured", (void *)va, what);
    uintptr_t pa;
    KASSERT(vm_space_query(vs, va, &pa, NULL) != 0);
    vm_space_ledger(vs, &l);
    mb_stats(d, &st);
    if (!ts_ledger_eq(&l, &l0) || !ts_stats_eq(&st, &st0) || pmm_reserved_count() != res0 ||
        vm_space_pages(vs) != pages0)
        panic("cmap: refused fault at %p (%s) changed the ledger (data %lu -> %lu, pt %lu -> %lu)",
              (void *)va, what, l0.data_tickets, l.data_tickets, l0.pt_tickets, l.pt_tickets);
}

void anon_commit_map_test(void)
{
    uintptr_t heap, hlen, mm, mlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0 && vm_anon_window(1, &mm, &mlen) == 0);
    struct ts_geo g;
    ts_geo_get(&g);
    const uintptr_t PG = PAGE_SIZE;
    size_t r0 = pmm_reserved_count();
    struct mb_domain *dp = mb_domain_new(100000), *dc = mb_domain_new(100000);
    struct vm_space *ps = vm_space_create(dp);
    KASSERT(dp && dc && ps);

    /* 承諾:heap 8 頁、mmap 4 頁、stack 第 0 格(兌現最上面 4 頁) */
    KASSERT(vm_space_commit_anon(ps, heap, 8 * PG) == 0);
    KASSERT(vm_space_commit_anon(ps, mm, 4 * PG) == 0);
    KASSERT(ts_commit(ps, &g, 0) == 0);
    struct vm_ledger l;
    vm_space_ledger(ps, &l);
    KASSERT(l.data_tickets == 8 + 4 + g.pages - TS_TOUCH_ON_COMMIT);

    /* 1. 窗內、沒承諾 */
    cm_expect_refused(ps, dp, heap + 8 * PG, "past the committed heap");
    cm_expect_refused(ps, dp, heap + 600 * PG, "uncommitted heap, needs a new table");
    cm_expect_refused(ps, dp, mm + 4 * PG, "gap after an mmap block");
    cm_expect_refused(ps, dp, ts_guard(&g, 0), "stack guard page, filter bypassed");
    cm_expect_refused(ps, dp, ts_guard(&g, 0) + PG - 1, "last byte of the guard page");
    cm_expect_refused(ps, dp, ts_base(&g, 1), "stack slot 1, never committed");

    /* 2. 部分歸還 */
    KASSERT(vm_space_fault_anon(ps, heap + 3 * PG) == 0);
    vm_space_release_anon(ps, heap + 6 * PG, 2 * PG);        /* 尾端:sbrk 縮小 */
    vm_space_release_anon(ps, heap + 2 * PG, PG);            /* 中段:把一段切成兩段 */
    cm_expect_refused(ps, dp, heap + 6 * PG, "released heap tail");
    cm_expect_refused(ps, dp, heap + 7 * PG, "released heap tail");
    cm_expect_refused(ps, dp, heap + 2 * PG, "released heap middle");
    KASSERT(vm_space_fault_anon(ps, heap + 3 * PG) == 1);    /* 沒歸還、已兌現 */
    KASSERT(vm_space_fault_anon(ps, heap + 1 * PG) == 0);    /* 沒歸還、未兌現:用票 */
    vm_space_ledger(ps, &l);
    unsigned long dt = l.data_tickets;
    KASSERT(vm_space_commit_anon(ps, heap + 5 * PG, 2 * PG) == -E_INVAL);   /* heap+5 已承諾:整個拒絕 */
    vm_space_ledger(ps, &l);
    KASSERT(l.data_tickets == dt);
    cm_expect_refused(ps, dp, heap + 6 * PG, "after a refused double commit");
    KASSERT(vm_space_commit_anon(ps, heap + 6 * PG, 2 * PG) == 0);          /* 尾端重新承諾 */
    KASSERT(vm_space_fault_anon(ps, heap + 7 * PG) == 0);
    anon_check(ps, r0, "cmap-partial");

    /* 3. fork:範圍 = 父目前的承諾(heap 的洞在 [2,3)) */
    struct vm_range r[4] = {
        { heap, 2 * PG }, { heap + 3 * PG, 5 * PG }, { mm, 4 * PG }, { ts_base(&g, 0), g.pages * PG },
    };
    struct vm_space *cs = vm_space_fork(ps, dc, r, 4);
    KASSERT(cs);
    cm_expect_refused(cs, dc, heap + 2 * PG, "child: hole inherited from the parent");
    cm_expect_refused(cs, dc, ts_guard(&g, 0), "child: guard page");
    cm_expect_refused(cs, dc, mm + 4 * PG, "child: mmap gap");
    KASSERT(vm_space_fault_anon(cs, heap + 4 * PG) == 0);    /* 父也沒兌現:子用自己的票 */
    KASSERT(vm_space_fault_anon(cs, mm + 3 * PG) == 0);
    vm_space_release_anon(cs, heap + 5 * PG, PG);
    cm_expect_refused(cs, dc, heap + 5 * PG, "child: released after fork");
    KASSERT(vm_space_fault_anon(ps, heap + 5 * PG) == 0);    /* 父不受子的歸還影響 */

    vm_space_destroy(cs);
    vm_space_destroy(ps);
    struct mb_stats st;
    mb_stats(dp, &st);
    KASSERT(st.total == 0);
    mb_stats(dc, &st);
    KASSERT(st.total == 0 && pmm_reserved_count() == r0);
    KASSERT(mb_domain_put(dp) == 0 && mb_domain_put(dc) == 0);
    kprintf("cmap: anonymous faults outside committed ranges are refused by the VM layer "
            "(guard page, mmap gap, released tail and middle, fork child) without using a ticket\n");
}

/*
 * I-339:kernel stack 不在執行期拆直接對映的大分頁(只在建立新區塊時拆一次,區塊留著重複使用)。
 *   第 1 輪:同時存在的執行緒超過目前空閒的格數 10 條 → 至少多一個區塊,總共至少 2 個;每條都寫自己的 stack。
 *   基準 = 第 1 輪結束、回收之後(已長大)的 pmm 空閒頁、頁表頁數、大分頁數、區塊數。
 *   第 2 輪:完全相同的建立數;全部活著時與結束回收後,上述數字都必須「完全等於」基準(沒有容許差額)。
 *   churn:在既有容量內反覆建立 / 結束 400 條(每批 8 條),結束後仍完全等於基準。
 * 負對照(之後實測,見 DEVLOG):
 *   (1) stack_free 改成「vm_map 保護頁 + 把整個區塊還給 pmm」(或 stack_alloc 每次都 grow)→ 第 2 輪區塊數 /
 *       大分頁數 / 頁表頁數改變 → panic;
 *   (2) 把 stack_alloc 換回舊的「pmm_alloc_pages + vm_unmap 保護頁」、stack_free 換回「vm_map + pmm_free_pages」
 *       → stack 落在沒拆過的 2MB 區時大分頁數下降、頁表頁數上升 → 第 2 輪或 churn panic
 *       (舊的實作有 32 條的 stack 快取,churn 的批次要大於 32 才看得到,所以第 2 輪用超過一個區塊的數量)。
 */
static volatile int kpool_go;
static void kpool_worker(void *arg)
{
    volatile char probe[256];                    /* 真的用到 stack:確定可寫、沒有碰到保護頁 */
    for (unsigned i = 0; i < sizeof probe; i++)
        probe[i] = (char)(uintptr_t)arg;
    while (!__atomic_load_n(&kpool_go, __ATOMIC_ACQUIRE))
        thread_sleep(1);
    for (unsigned i = 0; i < sizeof probe; i++)
        KASSERT(probe[i] == (char)(uintptr_t)arg);
}

struct kpool_snap { size_t pmm_free; unsigned long large, tables; struct kstack_stats ks; };
static void kpool_take(struct kpool_snap *x)
{
    x->pmm_free = pmm_free_count();
    x->large = vm_large_pages();
    x->tables = vm_table_pages();
    sched_kstack_stats(&x->ks);
}
static void kpool_check(const struct kpool_snap *b, const char *when, int alive)
{
    struct kpool_snap x;
    kpool_take(&x);
    /* 全部活著時區塊會重新長出來(全空的區塊會還給 pmm,最多留一塊備用),所以那時只要求大分頁與頁表
       不變 — 重新長出的區塊要落在已經拆過的區域;全部結束後一切都要和基準完全相同 */
    if (x.large != b->large || x.tables != b->tables ||
        (!alive && (x.pmm_free != b->pmm_free || x.ks.chunks != b->ks.chunks || x.ks.used != b->ks.used)))
        panic("kstack pool: %s: pmm %zu->%zu large %lu->%lu tables %lu->%lu chunks %u->%u used %u->%u", when,
              b->pmm_free, x.pmm_free, b->large, x.large, b->tables, x.tables, b->ks.chunks, x.ks.chunks,
              b->ks.used, x.ks.used);
}

/* 同時建立 n 條、全部活著時呼叫 alive_check(可為 NULL)、放行、全部 join、回收 */
static void kpool_round(unsigned n, const struct kpool_snap *b, const char *when)
{
    static int tid[512];
    KASSERT(n <= sizeof tid / sizeof tid[0]);
    __atomic_store_n(&kpool_go, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; i++) {
        tid[i] = thread_create("kpool", kpool_worker, (void *)(uintptr_t)(i + 1));
        KASSERT(tid[i] > 0);
    }
    if (b)
        kpool_check(b, when, 1);
    __atomic_store_n(&kpool_go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; i++)
        thread_join(tid[i]);
    sched_trim();
}

void kstack_pool_test(void)
{
    struct kpool_snap s0, base;
    sched_trim();
    kpool_take(&s0);
    unsigned per = s0.ks.slots_per_chunk;
    KASSERT(per == 512u / (4u + 1u));            /* STACK_PAGES = 4 */
    unsigned free_slots = s0.ks.chunks * per - s0.ks.used;
    unsigned n = free_slots + 10u;               /* 一定要長大 */
    if (n < per + 10u)
        n = per + 10u;                           /* 超過一個區塊的容量 */

    kpool_round(n, NULL, NULL);                  /* 第 1 輪:可以長大 */
    kpool_take(&base);
    /* 高峰時需要的區塊數;結束後全空的區塊還給 pmm,最多留一塊備用(只增不減的話,一次高峰會永久
       佔住記憶體 — stress 測試 1000 條時約 20MB) */
    unsigned peak = (s0.ks.used + n + per - 1) / per;
    KASSERT(base.ks.used == s0.ks.used && base.ks.chunks <= s0.ks.chunks + 1);
    /* 拆大分頁、新增頁表的次數不超過高峰時新增的區塊數(頁表留著:合併回大分頁不安全,上限 = 實體記憶體 / 2MB) */
    unsigned grown = peak > s0.ks.chunks ? peak - s0.ks.chunks : 0;
    KASSERT(grown >= 1 && s0.large - base.large <= grown && base.tables - s0.tables <= grown);
    KASSERT(s0.pmm_free - base.pmm_free <= 512u + grown + 64u);   /* 備用區塊 + 頁表 + 執行緒結構池的成長 */

    kpool_round(n, &base, "second round, all alive");   /* 第 2 輪:什麼都不能變 */
    kpool_check(&base, "second round, after join", 0);

    for (unsigned k = 0; k < 50; k++)            /* churn:400 條,每批 8 條 */
        kpool_round(8, &base, "churn, alive");
    kpool_check(&base, "churn, after join", 0);

    kprintf("kstack: %u stacks/chunk, %u concurrent threads needed %u chunks, %u kept after (one spare at most); "
            "second round and 400-thread churn leave pmm / large pages (%lu) / page tables (%lu) exactly unchanged\n",
            per, n, peak, base.ks.chunks, base.large, base.tables);
}

/*
 * I-240 C3.2 (c2):映像私有頁的記帳與 fork。
 * vm_space_map_charged 每一頁記 IMAGE 配額;配額不夠時失敗、什麼都不變。
 * 把 8 頁當成映像的可寫範圍,fork 時交給 VM:父子各自的 COW 頁持有一張票,
 * 一般配置吃到預留邊界之後,父寫入(共用中,用票複製)與子寫入(只剩自己,退票)都成功。
 * 負對照:fork 不把映像可寫範圍放進範圍(第 56 步實測)→ U1 讓 fork 失敗。
 */
void image_ticket_test(void)
{
    uintptr_t base = 0x40000000u + 0x00100000u;           /* USER_BASE + 1MB:映像區 */
    size_t r0 = pmm_reserved_count();
    struct mb_domain *dp = mb_domain_new(100000), *dc = mb_domain_new(100000);
    struct vm_space *ps = vm_space_create(dp);
    KASSERT(dp && dc && ps);
#if UINTPTR_MAX > 0xffffffffu
    base = 0x100000000ul + 0x00100000u;
#endif
    for (int i = 0; i < 8; i++) {
        uint8_t *pg = vm_space_map_charged(ps, MB_IMAGE, base + i * PAGE_SIZE, VM_READ | VM_WRITE);
        KASSERT(pg);
        pg[0] = (uint8_t)(i + 1);
    }
    struct mb_stats st;
    mb_stats(dp, &st);
    KASSERT(st.used[MB_IMAGE] == 8);
    mb_set_limit(dp, st.total);                           /* 一頁都不能再加:配置失敗、帳不變 */
    KASSERT(vm_space_map_charged(ps, MB_IMAGE, base + 8 * PAGE_SIZE, VM_READ | VM_WRITE) == NULL);
    mb_stats(dp, &st);
    KASSERT(st.used[MB_IMAGE] == 8);
    mb_set_limit(dp, 100000);

    struct vm_range r = { base, 8 * PAGE_SIZE };
    struct vm_space *cs = vm_space_fork(ps, dc, &r, 1);
    KASSERT(cs);
    struct vm_ledger lp, lc;
    vm_space_ledger(ps, &lp);
    vm_space_ledger(cs, &lc);
    KASSERT(lp.data_tickets == 8 && lc.data_tickets == 8);
    void *chain = NULL;
    for (void **pg; (pg = pmm_alloc()) != NULL; ) {
        *pg = chain;
        chain = pg;
    }
    for (int i = 0; i < 8; i++)
        KASSERT(vm_space_fault(ps, base + i * PAGE_SIZE, 1) == 0);
    for (int i = 0; i < 8; i++)
        KASSERT(vm_space_fault(cs, base + i * PAGE_SIZE, 1) == 0);
    vm_space_ledger(ps, &lp);
    vm_space_ledger(cs, &lc);
    KASSERT(lp.data_tickets == 0 && lc.data_tickets == 0);
    while (chain) {
        void **pg = chain;
        chain = *pg;
        pmm_free(pg);
    }
    vm_space_destroy(cs);
    vm_space_destroy(ps);
    KASSERT(pmm_reserved_count() == r0 && mb_domain_put(dc) == 0 && mb_domain_put(dp) == 0);
    kprintf("image: 8 charged private pages; after fork both sides wrote them with ordinary "
            "allocations at the reservation\n");
}

/*
 * I-240 C3.2 (c3):exec 交易的上限。domain 上限 400 頁,舊空間承諾 300 頁;
 * 開始交易(pending = 舊空間帳本總和)後,新空間承諾 300 頁可以(暫時超過上限,但不超過 limit + pending);
 * 再多到超過 limit + pending 就拒絕;交易中不帶憑證的記帳照一般上限 → 拒絕;
 * 最終檢查(舊空間退完之後 ≤ limit)通過 → 解除綁定、銷毀舊空間、結束交易,用量回到上限內。
 * 負對照:新空間不綁憑證(第 56 步實測)→ 新空間的 300 頁承諾被拒絕。
 */
void exec_txn_test(void)
{
    uintptr_t heap, hlen;
    KASSERT(vm_anon_window(0, &heap, &hlen) == 0);
    size_t r0 = pmm_reserved_count();
    struct mb_domain *d = mb_domain_new(400);
    KASSERT(d);
    struct vm_space *old = vm_space_create(d);
    KASSERT(old && vm_space_commit_anon(old, heap, 300 * PAGE_SIZE) == 0);

    struct mb_exec_txn t;
    KASSERT(mb_exec_begin(d, vm_space_quota_total(old), &t) == 0);
    struct vm_space *nw = vm_space_create_txn(d, &t);
    KASSERT(nw);
    KASSERT(vm_space_commit_anon(nw, heap, 300 * PAGE_SIZE) == 0);          /* 放寬:暫時超過 400 */
    struct mb_stats st;
    /* 憑證只能綁一個新空間(Codex 223):第二個空間、別的 domain 都拒絕,而且帳完全不變 */
    struct mb_stats before;
    mb_stats(d, &before);
    KASSERT(vm_space_create_txn(d, &t) == NULL);
    struct mb_domain *other = mb_domain_new(1000);
    KASSERT(other && vm_space_create_txn(other, &t) == NULL);
    mb_stats(d, &st);
    KASSERT(st.total == before.total && st.refs == before.refs);
    mb_stats(d, &st);
    KASSERT(st.total > st.limit);
    KASSERT(vm_space_commit_anon(nw, heap + 300 * PAGE_SIZE, 200 * PAGE_SIZE) == -E_NOMEM);  /* 超過 limit + pending */
    KASSERT(mb_quota_add(d, MB_KOBJ, 1) == -E_NOMEM);   /* 不帶憑證的記帳:一般上限,不能借用放寬 */
    KASSERT(mb_exec_check_final(&t, vm_space_quota_total(old)) == 0);
    vm_space_clear_txn(nw);
    vm_space_destroy(old);
    mb_exec_end(&t);
    mb_stats(d, &st);
    KASSERT(st.total <= st.limit);
    KASSERT(mb_quota_add_txn(d, MB_KOBJ, 200, &t) == -E_NOMEM);   /* 交易結束後憑證不再放寬 */
    KASSERT(vm_space_create_txn(d, &t) == NULL);                  /* 已結束的憑證不能再綁 */
    KASSERT(mb_domain_put(other) == 0);
    vm_space_destroy(nw);
    KASSERT(pmm_reserved_count() == r0 && mb_domain_put(d) == 0);
    kprintf("exec: transaction let the new space exceed the limit by exactly what the old space returns\n");
}
