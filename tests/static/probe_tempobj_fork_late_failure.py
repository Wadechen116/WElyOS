#!/usr/bin/env python3
"""Real sys_fork -> pmem_fork -> VM fork/destroy, with late prepare failure."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
from probe_pte_ticket import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#define PAGE_SIZE 4096UL
#define MMU_BITS 2
#define ENTRIES 4
#define MMU_LEVELS 2
#define MMU_ASID_MAX 0
#define USER_BASE 0UL
#define USER_TOP 65536UL
#define USER_HEAP_BASE 0UL
#define PROC_MAX_MAPS 2
#define PROC_MAX_FD 4
#define PRIO_NORMAL 3
#define MX_PROC_MEM 0
#define BB_FAULT 0
#define MB_REFS_MAX 0xFFFFFFF0u
#define E_NOMEM 12
#define E_INVAL 22
#define E_BUSY 16
#define VM_WRITE 2
#define VM_SHARED 32
#define VM_COW 64
#define VM_TICKET 128
#define WALK_LEGACY 1
enum mb_class { MB_ANON,MB_PGTABLE,MB_KOBJ,MB_NCLASS };
struct mb_bundle {unsigned long reserve,quota[MB_NCLASS];};
struct mb_domain {unsigned long used[MB_NCLASS],total,limit;unsigned refs;};
typedef uint64_t pte_t;
struct vm_range {uintptr_t start,len;};
struct vm_space {pte_t *root;struct mb_domain *dom;unsigned long pages,data_tickets,pt_tickets,quota[MB_NCLASS];unsigned used_cpus,asid;};
struct proc {char name[16];int pid,ppid,pgid,nthreads,exiting,cred;unsigned stack_slots;struct proc *next;struct vm_space *space;void *exe;
struct {struct mb_domain *dom;unsigned long kobj;int mx;uintptr_t brk,mmap_next;struct {uintptr_t start;unsigned long pages;} maps[PROC_MAX_MAPS];} mem;};
struct fork_start {struct proc *p;void *frame;};
struct fork_anon {const struct vm_range *r;unsigned n;unsigned long marked;};
static int mb_lk,vm_lk,pstate_lk;static unsigned long errors,table_pages,reserved,physical_refs;static unsigned long anon_pt_max=8;
static struct proc *procs;static int next_pid=100,started,force_thread_fail,resumed,exited;static jmp_buf thread_exit_jmp;
static int start_dying;
static pte_t *root;static unsigned live,alloc_calls,alloc_fail;
static void panic(const char *s,...){fprintf(stderr,"panic %s\n",s);exit(90);}
static void blackbox_log(int b,const char *s,...){(void)b;(void)s;panic("blackbox");}
static void *kmalloc(size_t n){if(++alloc_calls==alloc_fail)return NULL;size_t *p=malloc(sizeof(size_t)+n);if(!p)abort();*p=n;live++;return p+1;}
static void *kzalloc(size_t n){void *p=kmalloc(n);if(p)memset(p,0,n);return p;}
static size_t kmalloc_size(void *p){return ((size_t*)p)[-1];}
static void kfree(void *p){if(p){live--;free((size_t*)p-1);}}
static unsigned long spin_lock_irqsave(int *l){(void)l;return 0;}
static void spin_unlock_irqrestore(int *l,unsigned long f){(void)l;(void)f;}
static unsigned long sched_lock(void){return 0;}static void sched_unlock(unsigned long f){(void)f;}
static void mutex_init(int *m,const char *n,int c){(void)m;(void)n;(void)c;}
static void mutex_lock(int *m){(void)m;}static void mutex_unlock(int *m){(void)m;}
static unsigned long arch_irq_save(void){return 0;}static void arch_irq_restore(unsigned long f){(void)f;}
static void space_flush_fn(void *p){(void)p;}static void space_gone_fn(void *p){(void)p;}
static void smp_call_mask(unsigned c,void(*fn)(void*),void *p){(void)c;(void)fn;(void)p;}
static int pmm_reserve(unsigned long n){reserved+=n;return 0;}
static void pmm_unreserve(unsigned long n){if(n>reserved)panic("reserve underflow");reserved-=n;}
static void pmm_ref(void *p){(void)p;physical_refs++;}
static pte_t *table_alloc(void){void *p=aligned_alloc(4096,4096);if(!p)abort();memset(p,0,4096);table_pages++;return p;}
static void pmm_free(void *p){if((uintptr_t)p>=0x100000 && (uintptr_t)p<0x110000){if(!physical_refs)panic("refs underflow");physical_refs--;}else free(p);}
static unsigned asid_alloc(void){return 1;}static void asid_free(unsigned a){(void)a;}
static int arch_pte_valid(pte_t p){return p!=0;}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static unsigned arch_pte_flags(pte_t p){return p&4095;}
static pte_t arch_pte_leaf(uintptr_t pa,unsigned f,int l){(void)l;return pa|f;}
static void arch_pte_set(pte_t *p,pte_t v){*p=v;}
static unsigned va_index(uintptr_t a,int l){return (a>>(12+l*2))&3;}
static pte_t *walk(struct vm_space *v,pte_t *r,uintptr_t a,int t,int create,int s,int mode,int *level){
 (void)v;(void)t;(void)s;(void)mode;if(level)*level=0;unsigned hi=va_index(a,1);
 if(!r[hi]){if(!create)return NULL;r[hi]=(uintptr_t)table_alloc()|1;}
 return &((pte_t*)arch_pte_addr(r[hi]))[va_index(a,0)];}
static unsigned long pt_need(uintptr_t a,uintptr_t n){(void)a;return !!n;}
static unsigned long pages_for(uintptr_t n){return (n+4095)/4096;}
static size_t arch_frame_size(void){return 64;}
static void arch_frame_set_retval(void *p,int v){memcpy(p,&v,sizeof v);}
static void thread_set_data(struct proc *p){(void)p;}
static void thread_set_space(struct vm_space *v){(void)v;}
static int thread_killed(void){return start_dying==2;}
static void proc_thread_exit(void){exited++;longjmp(thread_exit_jmp,1);}
static void arch_user_resume(void *p){int v;memcpy(&v,p,sizeof v);if(v)panic("child retval");resumed++;longjmp(thread_exit_jmp,1);}
struct thread {void(*fn)(void*);void *arg;};
static struct thread *thread_prepare(const char *n,void(*fn)(void*),void *a,int pr,struct mb_domain *d){(void)n;(void)pr;(void)d;if(force_thread_fail)return NULL;struct thread *t=kmalloc(sizeof(*t));if(t){t->fn=fn;t->arg=a;}return t;}
static int thread_start(struct thread *t){if(!procs || procs->nthreads!=1)panic("published thread state");started++;if(start_dying==1)procs->exiting=1;if(!setjmp(thread_exit_jmp))t->fn(t->arg);kfree(t);return 1;}
static void files_init_cwd(struct proc *c,struct proc *p){(void)c;(void)p;}
static void *vfs_dup(void *p){return p;}static int vfs_exec_hold(void *p){(void)p;return 0;}
static void psig_fork(struct proc *c,struct proc *p){(void)c;(void)p;}
static void fd_inherit(struct proc *c,struct proc *p,unsigned n){(void)c;(void)p;(void)n;}
'''
MAIN=r'''
static int trial(unsigned n,int fail,int negative){
 alloc_calls=alloc_fail=live=errors=started=resumed=exited=0;table_pages=reserved=0;physical_refs=n;procs=NULL;force_thread_fail=fail==1;
 root=table_alloc();struct proc p={.pid=1,.pgid=1,.nthreads=1,.stack_slots=1};
 p.mem.dom=mb_domain_new(1000);p.space=vm_space_create(p.mem.dom);p.mem.brk=n*4096;
 if(mb_quota_add(p.mem.dom,MB_ANON,n))panic("parent quota");p.space->quota[MB_ANON]=n;p.space->pages=n;
 pte_t original[4]={0};for(unsigned i=0;i<n;i++){pte_t *e=walk(p.space,p.space->root,i*4096,0,1,0,0,NULL);original[i]=*e=(0x100000+i*4096)|VM_WRITE;}
 unsigned long before=reserved;unsigned baseline=live;char tf[64]={0};if(fail>=2){alloc_calls=0;alloc_fail=fail-1;}long rc=sys_fork(&p,tf);
 if(fail){
  if(rc!=-E_NOMEM || procs || started || live!=baseline || errors)return 10;
  unsigned changed=0;for(unsigned i=0;i<n;i++)changed+=*walk(p.space,p.space->root,i*4096,0,0,0,0,NULL)!=original[i];
  if(negative){if(p.space->data_tickets || reserved!=before || changed)return 20;}
  else if(p.space->data_tickets!=n || reserved!=before+n || changed!=n || physical_refs!=n)return 11;
  printf("%s n=%u rc=%ld parent_tickets=%lu reserved_delta=%lu changed_PTEs=%u child_live_delta=%d\n",negative?"CONTROL":"REPRODUCED",n,rc,p.space->data_tickets,reserved-before,changed,(int)live-(int)baseline);
 }else{
  if(rc<0 || !procs || started!=1 || resumed!=(start_dying==0) || exited!=(start_dying!=0) || live!=baseline+3 || errors)return 12;
  struct proc *c=procs;vm_space_destroy(c->space);pmem_release(c);kfree(c);procs=NULL;
 }
 vm_space_destroy(p.space);pmem_release(&p);pmm_free(root);table_pages--;if(live || reserved || physical_refs || table_pages || errors)return 13;return 0;
}
int main(int argc,char **argv){int negative=argc>1;(void)argv;for(unsigned n=1;n<=4;n++){int r=trial(n,1,negative);if(r)return r;for(int mode=0;mode<3;mode++){start_dying=mode;r=trial(n,0,0);if(r)return r;}start_dying=0;for(int early=2;early<=4;early++){r=trial(n,early,1);if(r)return r;}}puts("PASS: late failure, 3 early allocation failures, child-entry/dying/killed controls checked");return 0;}
'''
def main():
 paths=['kernel/proc.c','kernel/proc_mem.c','kernel/vm.c','kernel/membudget.c']
 src={p:(ROOT/p).read_text() for p in paths};p,pm,v,m=[src[x] for x in paths]
 body=PRE+'\n'
 body+='\n'.join(function(m,n) for n in ['mb_domain_new','mb_domain_get','mb_domain_put','fits_locked','mb_quota_add','sub_locked','mb_quota_sub','bundle_sum','mb_commit_bundle','mb_release_bundle'])+'\n'
 body+='static struct mb_domain *pmem_domain_new(struct proc *p){(void)p;return mb_domain_new(1000);}\n'
 body+='\n'.join(function(v,n) for n in ['vm_space_create','free_table','vm_space_destroy','in_anon','fork_table','vm_space_fork'])+'\n'
 body+='\n'.join(function(pm,n) for n in ['proc_kobj_pages','pmem_domain','pmem_release','pmem_fork'])+'\n'
 body+='\n'.join(function(p,n) for n in ['fork_child_main','sys_fork'])+'\n'+MAIN
 # Negative control: move forced prepare failure before pmem_fork's commit.
 mark='c->space = pmem_fork(c, p);'
 assert mark in body
 variants={'none':body,'prepare-failure-before-vm-fork':body.replace(mark,'if(force_thread_fail) goto nomem;\n    '+mark),'omit-fork-start-free':body.replace('kfree(arg);','/* omitted fork-start free */'),'omit-frame-free':body.replace('kfree(st.frame);','/* omitted frame free */')}
 out=Path(tempfile.mkdtemp(prefix='welyos-tempobj-fork-late-'));rows=[]
 for name,code in variants.items():
  c=out/(name+'.c');c.write_text(code);exe=c.with_suffix('')
  cp=subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
  if cp.returncode:raise RuntimeError(cp.stderr)
  args=[str(exe)]+(['control'] if name=='prepare-failure-before-vm-fork' else [])
  run=subprocess.run(args,capture_output=True,text=True,timeout=15)
  ok=run.returncode==(12 if name.startswith('omit-') else 0)
  rows.append(dict(variant=name,passed=ok,exit=run.returncode,stdout=run.stdout,stderr=run.stderr));print(name,ok,run.returncode,run.stdout.strip(),run.stderr)
 changed=[p for p,s in src.items() if (ROOT/p).read_text()!=s]
 (out/'results.json').write_text(json.dumps(dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()},changed_inputs=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_inputs:',changed)
 return int(bool(changed) or not all(r['passed'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
