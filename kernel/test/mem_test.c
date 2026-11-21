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
    return hs.heap_bytes / PAGE_SIZE + vm_table_pages() + sched_stack_cache_pages();
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
    KASSERT(refusals > 0);                            /* 真的用到了底線,而不是別的上限先擋下 */
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
    KASSERT(st.used[MB_PGTABLE] == 1 && st.total == 1 && st.refs == 2);   /* 根表;空間持有一份參考 */

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
    KASSERT(st.used[MB_PGTABLE] == MMU_LEVELS && st.total == MMU_LEVELS);
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
    KASSERT(st.used[MB_KOBJ] >= 3 && st.refs == 4);   /* 2 個檔案 + 1 條管線,各持一份參考 */
    unsigned long k = st.used[MB_KOBJ];
    vfs_close(pobj_w);
    vfs_close(pobj_r);                            /* 最後一個參考:管線也釋放 */
    mb_stats(d, &st);
    KASSERT(st.used[MB_KOBJ] == 0 && st.refs == 1);
    KASSERT(mb_domain_put(d) == 0);
    kprintf("vfs: pipe and files stay charged to their creator after it exits (%lu pages), "
            "returned on the last close\n", k);
}
