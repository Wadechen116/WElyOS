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
