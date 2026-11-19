/* Exact kernel functions, deterministic scheduling seams, modeled PTE and PMM. */
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
#define VM_VALID 0x100u
#define MMU_ASID_MAX 0
#define MMU_VA_BITS 64
#define MMU_LEVELS 1
#define MMU_BITS 1
#define MMU_LARGE_LEVEL 0
#define ENTRIES 2u
#define IDX_MASK 1u
static void interleave(void);
static int vm_lk, pstate_lk, vm_held, state_held, arm_query, arm_alloc, fired, test_case;
static unsigned long spin_lock_irqsave(int *l) {
    int *held=l==&vm_lk?&vm_held:&state_held; assert(!*held);*held=1;return 0;
}
static void spin_unlock_irqrestore(int *l,unsigned long f) {
    (void)f;int *held=l==&vm_lk?&vm_held:&state_held;assert(*held);*held=0;
    if(l==&vm_lk && arm_query && !fired) { fired=1;interleave(); }
}
typedef uintptr_t pte_t;
struct vm_space { pte_t *root; unsigned long pages; };
struct proc { struct vm_space *space; unsigned nstacks;
    struct { uintptr_t brk,mmap_next;unsigned long commit,limit;
        struct {uintptr_t start;unsigned long pages;} maps[PROC_MAX_MAPS]; } mem; };
static struct proc p;
static struct vm_space vs;
static pte_t entry;
static int enabled=1;
static unsigned long commit_total,commit_cap=1000;
static _Alignas(PAGE_SIZE) unsigned char pool[2][PAGE_SIZE];
static int refs[2];
static void *victim;
static unsigned long pmm_free_count(void){return 1000;}
static unsigned long pmm_total_pages(void){return 1000;}
static int page_index(void *ptr) {
    for(int i=0;i<2;i++)if(ptr==pool[i])return i;
    assert(!"bad page");return -1;
}
static void *pmm_alloc(void) {
    if(arm_alloc && !fired) { assert(!vm_held&&!state_held);fired=1;interleave(); }
    for(int i=0;i<2;i++)if(!refs[i]){refs[i]=1;return pool[i];}
    return NULL;
}
static void pmm_free(void *ptr) {int i=page_index(ptr);assert(refs[i]>0);refs[i]--;}
static unsigned arch_pte_flags(pte_t e){return (unsigned)(e&(PAGE_SIZE-1))&~VM_VALID;}
static int arch_pte_valid(pte_t e){return (e&VM_VALID)!=0;}
static uintptr_t arch_pte_addr(pte_t e){return e&~(uintptr_t)(PAGE_SIZE-1);}
static pte_t arch_pte_leaf(uintptr_t pa,unsigned flags,int level){(void)level;return pa|flags|VM_VALID;}
static pte_t *walk(pte_t *r,uintptr_t a,int t,int c,int s,int u,int *level) {
    (void)a;(void)t;(void)c;(void)s;(void)u;assert(vm_held);if(level)*level=0;return r;
}
static void arch_tlb_flush(uintptr_t a){(void)a;}
static void tlb_shootdown_space(struct vm_space *v,uintptr_t a){(void)v;(void)a;}
static int vm_space_fault(struct vm_space *v,uintptr_t a,int w){(void)v;(void)a;(void)w;return -1;}
int vm_space_query(struct vm_space *,uintptr_t,uintptr_t *,unsigned *);
int vm_space_map_new(struct vm_space *,uintptr_t,uintptr_t,unsigned);
int vm_space_unmap(struct vm_space *,uintptr_t);
long sys_munmap(struct proc *,uintptr_t);
long sys_sbrk(struct proc *,long);
#include "functions.c"
static void interleave(void) {
    if(test_case==1 || test_case==2 || test_case==3) {
        assert(sys_munmap(&p,USER_MMAP_BASE)==0);
        if(test_case!=1) {victim=pmm_alloc();assert(victim);memset(victim,'V',PAGE_SIZE);}
    } else if(test_case==4) assert(sys_sbrk(&p,-(long)PAGE_SIZE)==USER_HEAP_BASE+PAGE_SIZE);
}
int main(int argc,char **argv) {
    assert(argc==2);test_case=atoi(argv[1]);p.space=&vs;vs.root=&entry;
    p.mem.limit=100;p.mem.commit=1;commit_total=1;
    p.mem.maps[0].start=USER_MMAP_BASE;p.mem.maps[0].pages=1;
    p.mem.brk=USER_HEAP_BASE+PAGE_SIZE;
    long rc=0;int safe=0;unsigned char got=0,put='X';
    if(test_case==2 || test_case==3 || test_case==5 || test_case==6) {
        void *page=pmm_alloc();assert(page);memset(page,'U',PAGE_SIZE);
        assert(vm_space_map_new(&vs,USER_MMAP_BASE,(uintptr_t)page,VM_READ|VM_WRITE)==0);
        arm_query=test_case==2||test_case==3;
        if(test_case==2||test_case==5)rc=copy_to_user(&p,USER_MMAP_BASE,&put,1);
        else rc=copy_from_user(&p,&got,USER_MMAP_BASE,1);
        if(test_case==2)safe=rc!=0 || ((unsigned char *)victim)[0]=='V';
        if(test_case==3)safe=rc!=0 || got=='U';
        if(test_case==5)safe=rc==0&&pool[0][0]=='X';
        if(test_case==6)safe=rc==0&&got=='U';
    } else {
        arm_alloc=test_case==1||test_case==4;
        rc=proc_fault(&p,test_case==4?USER_HEAP_BASE:USER_MMAP_BASE,0);
        safe=test_case==1||test_case==4 ? rc!=0&&!arch_pte_valid(entry)&&vs.pages==0&&commit_total==0 : rc==0&&arch_pte_valid(entry)&&vs.pages==1;
    }
    printf("{\"case\":%d,\"safe\":%d,\"rc\":%ld,\"interleaved\":%d,\"mapped\":%d,\"vm_pages\":%lu,\"commit\":%lu,\"read_byte\":%u,\"victim_byte\":%u,\"refs\":[%d,%d]}\n",
        test_case,safe,rc,fired,arch_pte_valid(entry),vs.pages,p.mem.commit,got,victim?((unsigned char*)victim)[0]:0,refs[0],refs[1]);
    return safe?0:1;
}
