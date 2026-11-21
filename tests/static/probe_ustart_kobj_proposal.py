#!/usr/bin/env python3
"""Apply isolated ustart proposal, extract its true functions, test ownership."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
from probe_pte_ticket import function
from probe_stack_transaction_current import FIXTURE
ROOT=Path(__file__).resolve().parents[2]

EXTRA=r'''
static unsigned km_calls,km_fail,round_extra;
static unsigned long errors;
static pthread_mutex_t mb_lk=PTHREAD_MUTEX_INITIALIZER;
static void *kmalloc(size_t n){if(++km_calls==km_fail)return NULL;size_t *p=malloc(sizeof(*p)+n);if(!p)abort();*p=n+sizeof(*p)+round_extra;atomic_fetch_add(&live_starts,1);return p+1;}
static size_t kmalloc_size(void *p){return ((size_t*)p)[-1];}
static void kfree(void *p){if(p){atomic_fetch_sub(&live_starts,1);free((size_t*)p-1);}}
static unsigned long mb_pages_for_bytes(unsigned long n){return (n+PAGE_SIZE-1)/PAGE_SIZE;}
static void blackbox_log(int b,const char *s){(void)b;panic(s);}
static unsigned long sched_lock(void){return 0;}
static void sched_unlock(unsigned long n){(void)n;}
static void thread_set_data(struct proc *p){(void)p;}
static void thread_set_space(struct vm_space *p){(void)p;}
static int killed,entered,exited;
static jmp_buf leave;
static int thread_killed(void){return killed;}
static void proc_thread_exit(void){exited++;longjmp(leave,1);}
static void arch_enter_user(uintptr_t e,uintptr_t u,uintptr_t a){if(e!=1||!u||a!=2)abort();entered++;longjmp(leave,1);}
static void *queued;
static struct thread *thread_prepare(const char *n,void(*fn)(void*),void *arg,int pr,void *d){(void)n;(void)fn;(void)pr;(void)d;if(prepare_fail)return NULL;queued=arg;return &th;}
'''
MAIN=r'''
static void cleanup(struct proc *p){pmem_lock(p);while(nm)vm_space_unmap(p->space,maps[0].va);pmem_unlock(p);if(live_pages)abort();}
int main(void){unsigned cases=0;struct vm_space vs;
for(unsigned extra=0;extra<2;extra++)for(unsigned kind=0;kind<12;kind++){
 struct proc p={.space=&vs,.stack_slots=1,.mem.mx=PTHREAD_MUTEX_INITIALIZER};
 km_calls=km_fail=round_extra=fail_alloc=fail_map=alloc_calls=map_calls=prepare_fail=entered=exited=killed=0;queued=NULL;
 struct mb_domain *d=kmalloc(sizeof(*d));memset(d,0,sizeof(*d));d->refs=1;d->limit=100;p.dom=d;
 round_extra=extra*PAGE_SIZE;unsigned expected=extra+1;
 if(kind==0)km_fail=km_calls+1;
 if(kind==1)d->limit=expected-1;
 if(kind==2)d->refs=MB_REFS_MAX;
 if(kind==3)fail_alloc=3;
 if(kind==4)fail_map=4;
 if(kind==5||kind==6)prepare_fail=1;
 uintptr_t usp=kind==6?0x123000:0;
 struct thread *t=user_thread_prepare(&p,1,2,usp);
 if(kind<=6){
  unsigned refs=kind==2?MB_REFS_MAX:1;
  if(t||d->total||d->refs!=refs||p.stack_slots!=1||nm||live_pages||atomic_load(&live_starts)!=1||errors)return 10;
  d->refs=1;
 }else{
  if(!t||!queued||d->used[MB_KOBJ]!=expected||d->total!=expected||d->refs!=2||atomic_load(&live_starts)!=2)return 11;
  if(kind==8)p.exiting=1;if(kind==9)killed=1;
  /* Original owner can release its domain reference before the consumer. */
  if(kind==10){if(mb_domain_put(d))return 12;p.dom=NULL;}
  if(!setjmp(leave))user_thread_main(queued);
  if((kind==8||kind==9)?(exited!=1||entered):(entered!=1||exited))return 13;
  if(kind==10){if(atomic_load(&live_starts))return 14;d=NULL;}
  else if(d->total||d->refs!=1||atomic_load(&live_starts)!=1||errors)return 15;
 }
 cleanup(&p);if(d&&mb_domain_put(d))return 16;if(atomic_load(&live_starts)||errors)return 17;cases++;
}
printf("PASS %u ustart allocation/quota/ref/stack/prepare/consumer/dying/killed/last-domain-reference cases, two size-rounding costs\n",cases);return 0;}
'''

def main():
    paths=['kernel/proc.c','kernel/proc_internal.h','kernel/membudget.c','kernel/proc_mem.c']
    src={p:(ROOT/p).read_text() for p in paths}
    out=Path(tempfile.mkdtemp(prefix='welyos-ustart-kobj-probe-'))
    for p in paths:
        f=out/p;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(src[p])
    patch=ROOT/'tests/static/proposals/tempobj-kobj-20261121.patch'
    applied=subprocess.run(['patch','-p1','--batch','--fuzz=0','-i',str(patch)],cwd=out,capture_output=True,text=True)
    if applied.returncode:raise RuntimeError(applied.stdout+applied.stderr)
    proc=(out/'kernel/proc.c').read_text()
    pre=FIXTURE.split('CODE')[0].replace('#include <stdatomic.h>','#include <stdatomic.h>\n#include <setjmp.h>\n#define BB_FAULT 0\n#define E_NOMEM 12\n#define E_INVAL 22\n#define MB_REFS_MAX 0xFFFFFFF0u\nenum mb_class {MB_ANON,MB_STACK,MB_IMAGE,MB_PGTABLE,MB_KOBJ,MB_RAMFS,MB_NCLASS};\nstruct mb_domain {unsigned long used[MB_NCLASS],total,limit;unsigned refs;};')
    pre=pre.replace('struct proc {uint32_t','struct proc {struct mb_domain *dom;int exiting;uint32_t')
    pre=pre.replace('struct ustart {struct proc *p;uintptr_t entry,usp,arg;};','struct ustart {struct mb_domain *dom;unsigned long kobj;struct proc *p;uintptr_t entry,usp,arg;};')
    for name in ['kmalloc','kfree','thread_prepare']:
        pre=pre.replace(function(pre,name),'')
    pre=pre.replace('static void *pmem_domain(struct proc *p){(void)p;return NULL;}','static void *pmem_domain(struct proc *p){return p->dom;}')
    pre=pre.replace('static void user_thread_main(void *p){(void)p;}','static void user_thread_main(void *);')
    mb='\n'.join(function(src['kernel/membudget.c'],n) for n in ['mb_domain_get','mb_domain_put','fits_locked','mb_quota_add','sub_locked','mb_quota_sub','mb_charge_object','mb_release_object'])
    funcs='\n'.join(function(proc,n) for n in ['map_page','map_user_stack','unmap_user_stack','alloc_user_stack','free_user_stack_slot','ustart_release','user_thread_main','user_thread_prepare'])
    funcs+='\n'+ '\n'.join(function(src['kernel/proc_mem.c'],n) for n in ['pmem_lock','pmem_unlock'])
    body=pre+EXTRA+mb+funcs+MAIN
    variants={'baseline':body,'omit-consumer-release':body.replace('ustart_release(arg);','kfree(arg);'),'omit-failure-release':body.replace('ustart_release(st);','kfree(st);'),'omit-size-rounding':body.replace('st->kobj = mb_pages_for_bytes(kmalloc_size(st));','st->kobj = 1;')}
    expected={'baseline':0,'omit-consumer-release':15,'omit-failure-release':10,'omit-size-rounding':10}
    rows=[]
    for name,b in variants.items():
        if name!='baseline' and b==body:raise RuntimeError('mutation missed')
        c=out/(name+'.c');c.write_text(b);exe=c.with_suffix('')
        comp=subprocess.run(['cc','-std=c11','-O1','-pthread','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20) if comp.returncode==0 else comp
        rows.append(dict(variant=name,compile_exit=comp.returncode,exit=r.returncode,expected=expected[name],passed=comp.returncode==0 and r.returncode==expected[name],stdout=r.stdout,stderr=r.stderr));print(name,rows[-1])
    changed=[p for p,s in src.items() if (ROOT/p).read_text()!=s]
    (out/'results.json').write_text(json.dumps(dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()},changed_inputs=changed,patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),cases=rows),indent=2)+'\n')
    print('artifacts:',out,'changed_inputs:',changed)
    return bool(changed) or not all(r['passed'] for r in rows)

if __name__=='__main__':raise SystemExit(main())
