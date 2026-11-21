#!/usr/bin/env python3
"""Actual stack helpers: sequential rollback checks and preparing-slot fault counterexamples."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
from probe_pte_ticket import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define USER_STACK_TOP 0x70000000UL
#define USER_STACK_SLOT 0x100000UL
#define USER_STACK_PAGES 4u
#define PROC_MAX_THREADS 16u
#define USER_HEAP_BASE 0x10000000UL
#define USER_HEAP_MAX 0x100000UL
#define USER_MMAP_BASE 0x20000000UL
#define USER_MMAP_MAX 0x100000UL
#define PROC_MAX_MAPS 2
#define VM_READ 1
#define VM_WRITE 2
#define PRIO_NORMAL 3
struct vm_space {int unused;};
struct proc {uint32_t stack_slots;struct vm_space *space;char name[16];struct {uintptr_t brk;struct {unsigned long pages;uintptr_t start;}maps[PROC_MAX_MAPS];}mem;};
struct thread {int unused;};
struct ustart {struct proc *p;uintptr_t entry,usp,arg;};
static int pstate_lk,locked;
static unsigned long spin_lock_irqsave(int *l){(void)l;if(locked)abort();locked=1;return 0;}
static void spin_unlock_irqrestore(int *l,unsigned long f){(void)l;(void)f;if(!locked)abort();locked=0;}
static void panic(const char *s){fprintf(stderr,"panic %s\n",s);exit(99);}
struct mapping {uintptr_t va;void *p;};static struct mapping maps[256];static unsigned nm,live_pages,alloc_calls,map_calls,fail_alloc,fail_map,live_starts,prepare_fail;
static unsigned hook,hook_done,replaced;static int fault_rc;static struct proc *target;static uintptr_t nested_top;
static uintptr_t alloc_user_stack(struct proc*,unsigned*);
static int proc_fault_locked(struct proc*,uintptr_t,int);
static void *kmalloc(size_t n){void *p=malloc(n);if(p)live_starts++;return p;}
static void kfree(void *p){if(p){live_starts--;free(p);}}
static void *pmm_alloc(void){if(locked)abort();if(++alloc_calls==fail_alloc)return NULL;void *p=malloc(PAGE_SIZE);if(p)live_pages++;return p;}
static void pmm_free(void *p){if(!p)abort();live_pages--;free(p);}
static void inject(void){if(hook&&!hook_done){hook_done=1;if(hook==1||hook==2){uintptr_t va=USER_STACK_TOP-USER_STACK_SLOT-(hook==1?8:1)*PAGE_SIZE;fault_rc=proc_fault_locked(target,va,1);}else{unsigned slot; nested_top=alloc_user_stack(target,&slot);}}}
static int vm_space_map(struct vm_space *s,uintptr_t va,uintptr_t pa,unsigned f){(void)s;(void)f;inject();if(++map_calls==fail_map)return -1;for(unsigned i=0;i<nm;i++)if(maps[i].va==va){/* Actual vm_space_map replaces without freeing previous PA. */maps[i].p=(void*)pa;replaced++;return 0;}maps[nm++]=(struct mapping){va,(void*)pa};return 0;}
static int vm_space_map_new(struct vm_space *s,uintptr_t va,uintptr_t pa,unsigned f){(void)s;(void)f;for(unsigned i=0;i<nm;i++)if(maps[i].va==va)return 1;maps[nm++]=(struct mapping){va,(void*)pa};return 0;}
static int vm_space_unmap(struct vm_space *s,uintptr_t va){(void)s;for(unsigned i=0;i<nm;i++)if(maps[i].va==va){pmm_free(maps[i].p);maps[i]=maps[--nm];return 0;}return -1;}
static int vm_space_fault_anon(struct vm_space *s,uintptr_t va){(void)s;(void)va;return -1;}
static void *pmem_domain(struct proc *p){(void)p;return NULL;}
static void user_thread_main(void *p){(void)p;}
static struct thread th;
static struct thread *thread_prepare(const char *n,void(*fn)(void*),void *arg,int pr,void *d){(void)n;(void)fn;(void)pr;(void)d;if(prepare_fail)return NULL;kfree(arg);return &th;}
static void free_user_stack_slot(struct proc*,unsigned);
CODE
static void reset(struct proc *p){while(nm)vm_space_unmap(p->space,maps[0].va);if(live_pages)abort();alloc_calls=map_calls=fail_alloc=fail_map=live_starts=prepare_fail=hook=hook_done=replaced=0;fault_rc=-99;nested_top=0;p->stack_slots=1;target=p;}
int main(void){struct vm_space vs;struct proc p={.space=&vs,.stack_slots=1};unsigned cases=0;
for(unsigned kind=0;kind<3;kind++)for(unsigned at=1;at<=4;at++){
 reset(&p);prepare_fail=kind==2;fail_alloc=kind==0?at:0;fail_map=kind==1?at:0;
 if(user_thread_prepare(&p,1,2,0)!=NULL||p.stack_slots!=1||nm||live_pages||live_starts)return 10;
 fail_alloc=fail_map=prepare_fail=0;
 if(!user_thread_prepare(&p,1,2,0)||p.stack_slots!=3||nm!=4||live_pages!=4||live_starts)return 11;
 cases++;
}
reset(&p);prepare_fail=1;if(user_thread_prepare(&p,1,2,0x123000)||p.stack_slots!=1||nm||live_pages)return 12;cases++;
/* Another creator runs after slot claim: it must get a distinct slot. */
reset(&p);hook=3;if(!user_thread_prepare(&p,1,2,0)||nested_top!=USER_STACK_TOP-2*USER_STACK_SLOT||p.stack_slots!=7||nm!=8||replaced)return 13;cases++;
printf("PASS %u sequential allocation/map/prepare rollback/retry/supplied/creator uniqueness cases\n",cases);
/* Another existing thread grows PREPARING slot, then preparation fails. */
reset(&p);hook=1;prepare_fail=1;
if(user_thread_prepare(&p,1,2,0)||fault_rc!=0||p.stack_slots!=1||nm!=1||live_pages!=1)return 20;
printf("REPRODUCED preparing-slot deep fault: fault rc=%d, slot rolled back, %u mapping/%u physical page retained\n",fault_rc,nm,live_pages);
reset(&p);hook=2;prepare_fail=1;
if(user_thread_prepare(&p,1,2,0)||fault_rc!=0||p.stack_slots!=1||nm!=0||live_pages!=1||replaced!=1)return 21;
printf("REPRODUCED preparing-slot initial-page fault: %u replacement, no mappings but %u physical page retained\n",replaced,live_pages);
return 0;}
'''
def main():
 paths=['kernel/proc.c','kernel/proc_mem.c','kernel/proc_internal.h'];src={p:(ROOT/p).read_text() for p in paths}
 code='\n'.join(function(src['kernel/proc.c'],n) for n in ['map_page','map_user_stack','unmap_user_stack','alloc_user_stack','free_user_stack_slot','user_thread_prepare'])+'\n'+function(src['kernel/proc_mem.c'],'proc_fault_locked')
 out=Path(tempfile.mkdtemp(prefix='welyos-stackfix-transaction-'));c=out/'probe.c';c.write_text(PRE.replace('CODE',code));exe=out/'probe'
 variants={'none':PRE.replace('CODE',code),'omit-partial-unmap':PRE.replace('CODE',code).replace('while (--i >= 1)', 'while (0 && --i >= 1)'), 'omit-slot-release':PRE.replace('CODE',code).replace('__atomic_and_fetch(&p->stack_slots, ~(1u << slot), __ATOMIC_RELEASE);','/* mutation */')}
 rows=[]
 for name,body in variants.items():
  if name!='none' and body==variants['none']:raise RuntimeError('mutation missed')
  c=out/(name+'.c');c.write_text(body);exe=c.with_suffix('')
  subprocess.run(['cc','-std=c11','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
  r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
  ok=r.returncode==(0 if name=='none' else 10)
  rows.append(dict(variant=name,exit=r.returncode,expected=ok,stdout=r.stdout,stderr=r.stderr));print(name,ok,r.returncode,r.stdout.strip())
 changed=[p for p,s in src.items() if (ROOT/p).read_text()!=s]
 (out/'results.json').write_text(json.dumps(dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()},changed_inputs=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_inputs:',changed);return bool(changed) or not all(r['expected'] for r in rows)
if __name__=='__main__':raise SystemExit(main())
