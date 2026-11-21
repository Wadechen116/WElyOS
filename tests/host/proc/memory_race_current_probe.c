/* Current real proc/VM entry functions; deterministic scheduling and bundle/PMM fixtures.
 * 第 50 步(D1)更新:配合 mem.mx(每個行程的 mutex)與 vm_space_copy(鎖內查表+複製)。
 * mutex 的模型:另一條執行緒想在 mutex 被持有時插進來,就延到 mutex_unlock 之後才執行
 * (真正的 mutex 會讓它睡到那時候)。沒有持有時立刻插進來。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "errno.h"
#include "vm_flags.h"
#define PAGE_SIZE 4096u
#define USER_BASE 0x40000000u
#define USER_TOP 0x80000000u
#define USER_HEAP_BASE (USER_BASE+0x10000000u)
#define USER_HEAP_MAX 0x10000000u
#define USER_MMAP_BASE (USER_BASE+0x20000000u)
#define USER_MMAP_MAX 0x0F000000u
#define USER_STACK_TOP (USER_BASE+0x30000000u)
#define USER_STACK_SLOT 0x100000u
#define PROC_MAX_THREADS 16u
#define PROC_MAX_MAPS 4u
#define CAP_ADMIN 1u
#define KASSERT assert
#define WALK_LEGACY 0
#define WALK_ANON 1
#define RELEASE_SEGMENT 64u
#define MB_ANON 0
#define MB_PGTABLE 1
#include <stdarg.h>
static void panic(const char *fmt, ...) { va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);fputc('\n',stderr);exit(10); }
#define VM_VALID 0x100u
#define MMU_ASID_MAX 0
#define MMU_VA_BITS 64
#define MMU_LEVELS 1
#define MMU_BITS 1
#define MMU_LARGE_LEVEL 0
#define ENTRIES 2u
#define IDX_MASK 1u
static void interleave(void);
static void interleave_body(void);
struct mutex { int held; };
static int mx_pending;
static int vm_lk, pstate_lk, vm_held, state_held, arm_query, arm_alloc, fired, test_case;
__attribute__((unused)) static void mutex_lock(struct mutex *m){assert(!m->held);m->held=1;}
__attribute__((unused)) static void mutex_unlock(struct mutex *m){
    assert(m->held);m->held=0;
    if(mx_pending){mx_pending=0;interleave_body();}   /* 等在 mutex 上的另一條執行緒現在才進來 */
}
static unsigned long spin_lock_irqsave(int *l) {
    if(l==&vm_lk && arm_alloc && !fired && !state_held) { fired=1;interleave(); }
    int *held=l==&vm_lk?&vm_held:&state_held; assert(!*held);*held=1;return 0;
}
static void spin_unlock_irqrestore(int *l,unsigned long f) {
    (void)f;int *held=l==&vm_lk?&vm_held:&state_held;assert(*held);*held=0;
    if(l==&vm_lk && arm_query && !fired) { fired=1;interleave(); }
}
typedef uintptr_t pte_t;
struct mb_domain { unsigned long used, reserved; };
struct mb_bundle { unsigned long reserve, quota[2]; };
struct vm_space { pte_t *root; unsigned long pages; struct mb_domain *dom; unsigned long data_tickets,pt_tickets,anon_tables,quota[2]; };
static struct mb_domain domain;
struct proc { struct vm_space *space; uint32_t stack_slots;
    struct { struct mutex mx; uintptr_t brk,mmap_next;
        struct {uintptr_t start;unsigned long pages;} maps[PROC_MAX_MAPS]; } mem; };
static struct proc p;
static struct vm_space vs;
static pte_t entry;
static int enabled=1;
static unsigned long anon_pt_max;
static unsigned long pt_need(uintptr_t a,uintptr_t n){(void)a;(void)n;return 0;}
static int mb_commit_bundle(struct mb_domain *d,const struct mb_bundle *b){d->used+=b->quota[MB_ANON];d->reserved+=b->reserve;return 0;}
static int mb_release_bundle(struct mb_domain *d,const struct mb_bundle *b){assert(d->used>=b->quota[MB_ANON]&&d->reserved>=b->reserve);d->used-=b->quota[MB_ANON];d->reserved-=b->reserve;return 0;}
static _Alignas(PAGE_SIZE) unsigned char pool[2][PAGE_SIZE];
static int refs[2];
static void *victim;
static long second_rc=-99;
static int page_index(void *ptr) {
    for(int i=0;i<2;i++)if(ptr==pool[i])return i;
    assert(!"bad page");return -1;
}
static void *pmm_alloc(void) {
    for(int i=0;i<2;i++)if(!refs[i]){refs[i]=1;return pool[i];}
    return NULL;
}
static void *pmm_alloc_reserved(void){assert(domain.reserved);domain.reserved--;void *q=pmm_alloc();assert(q);return q;}
static void pmm_free(void *ptr) {int i=page_index(ptr);assert(refs[i]>0);refs[i]--;}
static unsigned arch_pte_flags(pte_t e){return (unsigned)(e&(PAGE_SIZE-1))&~VM_VALID;}
static int arch_pte_valid(pte_t e){return (e&VM_VALID)!=0;}
static uintptr_t arch_pte_addr(pte_t e){return e&~(uintptr_t)(PAGE_SIZE-1);}
static pte_t arch_pte_leaf(uintptr_t pa,unsigned flags,int level){(void)level;return pa|flags|VM_VALID;}
static void arch_pte_set(pte_t *p,pte_t v){*p=v;}
static pte_t *walk(struct vm_space *v,pte_t *r,uintptr_t a,int t,int c,int s,int u,int *level) {
    (void)v;(void)a;(void)t;(void)c;(void)s;(void)u;assert(vm_held);if(level)*level=0;return r;
}
static void arch_tlb_flush(uintptr_t a){(void)a;}
static void tlb_shootdown_space(struct vm_space *v,uintptr_t a){(void)v;(void)a;}
static int vm_space_fault(struct vm_space *v,uintptr_t a,int w){(void)v;(void)a;(void)w;return -1;}
int vm_space_commit_anon(struct vm_space *,uintptr_t,uintptr_t);
void vm_space_release_anon(struct vm_space *,uintptr_t,uintptr_t);
int vm_space_fault_anon(struct vm_space *,uintptr_t);
int vm_space_copy(struct vm_space *,uintptr_t,void *,size_t,int);
int vm_space_map_new(struct vm_space *,uintptr_t,uintptr_t,unsigned);
int vm_space_unmap(struct vm_space *,uintptr_t);
long sys_munmap(struct proc *,uintptr_t);
long sys_sbrk(struct proc *,long);
#include "functions.c"
static void interleave(void) {
    if(p.mem.mx.held) mx_pending=1;              /* 另一條執行緒要拿 mem.mx:先睡 */
    else interleave_body();
}
static void interleave_body(void) {
    if(test_case==1 || test_case==2 || test_case==3) {
        assert(sys_munmap(&p,USER_MMAP_BASE)==0);
        if(test_case!=1) {victim=pmm_alloc();assert(victim);memset(victim,'V',PAGE_SIZE);}
    } else if(test_case==4) assert(sys_sbrk(&p,-(long)PAGE_SIZE)==USER_HEAP_BASE+PAGE_SIZE);
    else if(test_case==8) {                      /* 縮小後又長回來:同一頁變成「新的、內容為 0」 */
        assert(sys_sbrk(&p,-(long)PAGE_SIZE)==USER_HEAP_BASE+PAGE_SIZE);
        assert(sys_sbrk(&p,(long)PAGE_SIZE)==USER_HEAP_BASE);
    } else if(test_case==9) second_rc=proc_fault(&p,USER_MMAP_BASE,0);   /* 同一頁的第二個 fault */
}
int main(int argc,char **argv) {
    assert(argc==2);test_case=atoi(argv[1]);p.space=&vs;vs.root=&entry;
    vs.dom=&domain;vs.quota[MB_ANON]=1;vs.data_tickets=1;domain.used=1;domain.reserved=1;
    p.mem.maps[0].start=USER_MMAP_BASE;p.mem.maps[0].pages=1;
    p.mem.brk=USER_HEAP_BASE+PAGE_SIZE;
    long rc=0;int safe=0;unsigned char got=0,put='X';
    if(test_case==2 || test_case==3 || test_case==5 || test_case==6) {
        void *page=pmm_alloc();assert(page);memset(page,'U',PAGE_SIZE);
        assert(vm_space_map_new(&vs,USER_MMAP_BASE,(uintptr_t)page,VM_READ|VM_WRITE)==0);
        vs.data_tickets=0;domain.reserved=0;
        arm_query=test_case==2||test_case==3;
        if(test_case==2||test_case==5)rc=copy_to_user(&p,USER_MMAP_BASE,&put,1);
        else rc=copy_from_user(&p,&got,USER_MMAP_BASE,1);
        if(test_case==2)safe=rc!=0 || ((unsigned char *)victim)[0]=='V';
        if(test_case==3)safe=rc!=0 || got=='U';
        if(test_case==5)safe=rc==0&&pool[0][0]=='X';
        if(test_case==6)safe=rc==0&&got=='U';
    } else {
        int heap=test_case==4||test_case==8;
        arm_alloc=test_case==1||test_case==4||test_case==8||test_case==9;
        rc=proc_fault(&p,heap?USER_HEAP_BASE:USER_MMAP_BASE,0);
        int pages_used=refs[0]+refs[1];
        if(test_case==1||test_case==4)           /* 區域已經不在:不可以留下對映、頁或承諾 */
            safe=!arch_pte_valid(entry)&&vs.pages==0&&domain.used==0&&vs.quota[MB_ANON]==0&&vs.data_tickets==0&&domain.reserved==0&&pages_used==0;
        else if(test_case==8)                    /* 區域又在了:可以有對映,但必須是 0、而且記帳一致 */
            safe=domain.used==1&&vs.quota[MB_ANON]==1&&vs.pages==(unsigned long)pages_used&&vs.pages<=1&&
                 (!arch_pte_valid(entry)||((unsigned char*)arch_pte_addr(entry))[0]==0);
        else if(test_case==9)                    /* 兩個 fault 都成功,只留下一頁,沒有漏掉的頁 */
            safe=rc==0&&second_rc==0&&arch_pte_valid(entry)&&vs.pages==1&&pages_used==1;
        else safe=rc==0&&arch_pte_valid(entry)&&vs.pages==1;
    }
    printf("{\"case\":%d,\"safe\":%d,\"rc\":%ld,\"interleaved\":%d,\"mapped\":%d,\"vm_pages\":%lu,\"commit\":%lu,\"read_byte\":%u,\"victim_byte\":%u,\"refs\":[%d,%d]}\n",
        test_case,safe,rc,fired,arch_pte_valid(entry),vs.pages,domain.used,got,victim?((unsigned char*)victim)[0]:0,refs[0],refs[1]);
    return safe?0:1;
}
