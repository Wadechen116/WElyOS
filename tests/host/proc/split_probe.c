/* Host model for the step-47 module seams; no scheduler or virtual-memory execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "errno.h"
#define PROC_MAX_MAPS 4
#define PROC_MAX_FD 4
#define PROC_DEFAULT_LIMIT_PAGES 8192
#define NSIG 32
#define SIG_DFL 0
#define SIG_IGN 1
#define PAGE_SIZE 4096
struct file { int id; };
struct mutex { int held; };
#define MX_PROC_MEM 20
static int mxheld;
static void mutex_init(struct mutex *m,const char *name,unsigned rank) {
    assert(!m->held && name && rank==MX_PROC_MEM);m->held=0;
}
static void mutex_lock(struct mutex *m) { assert(!m->held && !mxheld);m->held=1;mxheld=1; }
static void mutex_unlock(struct mutex *m) { assert(m->held && mxheld);m->held=0;mxheld=0; }
struct proc {
    struct { struct mutex mx; unsigned long commit, limit;
        struct { uintptr_t start; unsigned long pages; } maps[PROC_MAX_MAPS];
        uintptr_t mmap_next, brk;
    } mem;
    struct { uintptr_t handler[NSIG], restorer; uint32_t pending;
        int stopped, stop_reported;
    } sig;
    struct { struct file *fd[PROC_MAX_FD]; char cwd[32]; } files;
    void *space;
};
static unsigned long commit_total, commit_cap;
static int pstate_lk, held, sched_held, unmapped, tests;
static unsigned long spin_lock_irqsave(int *lock) {
    assert(lock == &pstate_lk && !held); held = 1; return 0;
}
static void spin_unlock_irqrestore(int *lock, unsigned long fl) {
    (void)fl; assert(lock == &pstate_lk && held); held = 0;
}
static unsigned long sched_lock(void) { assert(!sched_held); sched_held=1; return 0; }
static void sched_unlock(unsigned long f) { (void)f; assert(sched_held); sched_held=0; }
static unsigned long pmm_free_count(void) { assert(held); return 4096; }
static unsigned long pmm_total_pages(void) { assert(held); return 8192; }
static int vm_space_unmap(void *s, uintptr_t a) {
    (void)s; assert(!held && mxheld && (a==0x10000 || a==0x11000)); unmapped++; return 0;
}
#include "functions.c"
#define CHECK(c) do { assert(c); assert(!held && !sched_held && !mxheld); tests++; } while(0)
int main(void) {
    struct proc parent={0}, child={0};
    pmem_init(&parent, NULL); CHECK(parent.mem.limit==PROC_DEFAULT_LIMIT_PAGES);
    parent.mem.limit=100; parent.mem.commit=3; commit_total=3; commit_cap=1000;
    parent.mem.maps[0].start=0x10000; parent.mem.maps[0].pages=2;
    parent.mem.mmap_next=0x15000; parent.mem.brk=0x50000;
    pmem_init(&child,&parent); CHECK(child.mem.limit==100);
    CHECK(pmem_fork(&child,&parent)==0 && child.mem.commit==3 && commit_total==6 &&
          child.mem.brk==parent.mem.brk && child.mem.mmap_next==parent.mem.mmap_next &&
          !memcmp(child.mem.maps,parent.mem.maps,sizeof(child.mem.maps)));
    commit_release_all(&child); CHECK(!child.mem.commit && commit_total==3);
    memset(&child,0,sizeof(child));parent.mem.limit=2;
    CHECK(pmem_fork(&child,&parent)==-E_NOMEM && !child.mem.commit && commit_total==3);
    memset(&child,0,sizeof(child));parent.mem.limit=100;commit_cap=5;
    CHECK(pmem_fork(&child,&parent)==-E_NOMEM && !child.mem.commit && commit_total==3);
    commit_cap=1000; unmapped=0;
    CHECK(sys_munmap(&parent,0x10000)==0 && unmapped==2 && !parent.mem.maps[0].pages &&
          parent.mem.commit==1 && commit_total==1);
    CHECK(sys_munmap(&parent,0x10000)==-E_INVAL && unmapped==2 && commit_total==1);
    pmem_exec(&parent); CHECK(!parent.mem.commit && !commit_total && !parent.mem.brk &&
        !parent.mem.mmap_next && !parent.mem.maps[0].pages && parent.mem.limit==100);
    parent.sig.handler[2]=0x1234;parent.sig.handler[3]=SIG_IGN;parent.sig.restorer=0x5678;
    parent.sig.pending=4;parent.sig.stopped=19;parent.sig.stop_reported=1;
    memset(&child,0,sizeof(child));psig_fork(&child,&parent);
    CHECK(child.sig.handler[2]==0x1234 && child.sig.handler[3]==SIG_IGN &&
          child.sig.restorer==0x5678 && !child.sig.pending && !child.sig.stopped && !child.sig.stop_reported);
    psig_exec(&parent);CHECK(parent.sig.handler[2]==SIG_DFL && parent.sig.handler[3]==SIG_IGN && !parent.sig.pending);
    parent.sig.stop_reported=0;
    unsigned long f=sched_lock();assert(psig_stopped(&parent)==19 && psig_stop_unreported(&parent));
    assert(psig_report_stop(&parent)==19 && !psig_stop_unreported(&parent));sched_unlock(f);CHECK(parent.sig.stop_reported==1);
    memset(child.files.cwd,'x',sizeof(child.files.cwd));files_init_cwd(&child,NULL);
    CHECK(!strcmp(child.files.cwd,"/"));
    strcpy(parent.files.cwd,"/disk/test");files_init_cwd(&child,&parent);CHECK(!strcmp(child.files.cwd,"/disk/test"));
    CHECK(!fd_in_range(-1) && fd_in_range(0) && fd_in_range(PROC_MAX_FD-1) && !fd_in_range(PROC_MAX_FD));
    struct file a={1},b={2};child.files.fd[1]=&a;
    CHECK(fd_replace(&child,1,&b)==&a && child.files.fd[1]==&b);
    CHECK(fd_replace(&child,2,&a)==NULL && child.files.fd[2]==&a);
    printf("%d checks PASS\n",tests);return 0;
}
