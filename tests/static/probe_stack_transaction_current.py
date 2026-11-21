#!/usr/bin/env python3
"""Current real stack helpers with pthread serialization and failure injection."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
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
struct proc {uint32_t stack_slots;struct vm_space *space;char name[16];struct {pthread_mutex_t mx;uintptr_t brk;struct {unsigned long pages;uintptr_t start;} maps[PROC_MAX_MAPS];}mem;};
struct thread {int unused;};
struct ustart {struct proc *p;uintptr_t entry,usp,arg;};
static pthread_mutex_t pstate_lk=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int spin_held,mem_held,role;
static _Thread_local uintptr_t prepared_top;
static unsigned long spin_lock_irqsave(pthread_mutex_t *m){pthread_mutex_lock(m);spin_held=1;return 0;}
static void spin_unlock_irqrestore(pthread_mutex_t *m,unsigned long f){(void)f;spin_held=0;pthread_mutex_unlock(m);}
static void mutex_lock(pthread_mutex_t *m){if(mem_held)abort();pthread_mutex_lock(m);mem_held=1;}
static void mutex_unlock(pthread_mutex_t *m){if(!mem_held)abort();mem_held=0;pthread_mutex_unlock(m);}
static void panic(const char *s){fprintf(stderr,"panic %s\n",s);exit(99);}
struct mapping {uintptr_t va;void *p;};
static struct mapping maps[256];
static unsigned nm,live_pages,alloc_calls,map_calls,fail_alloc,fail_map,replaced,prepare_fail;
static atomic_uint live_starts;
static int mode,arrived,map_paused,fault_attempted,fault_done,fault_rc;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static struct proc *target;
static struct thread th;
static void *kmalloc(size_t n){void *p=malloc(n);if(p)atomic_fetch_add(&live_starts,1);return p;}
static void kfree(void *p){if(p){atomic_fetch_sub(&live_starts,1);free(p);}}
static void *pmm_alloc(void){if(spin_held||!mem_held)abort();if(++alloc_calls==fail_alloc)return NULL;void *p=malloc(PAGE_SIZE);if(p)live_pages++;return p;}
static void pmm_free(void *p){if(!p||!mem_held)abort();live_pages--;free(p);}
static int vm_space_map(struct vm_space *s,uintptr_t va,uintptr_t pa,unsigned f){
 (void)s;(void)f;if(!mem_held)abort();
 if(mode==2&&!map_paused){pthread_mutex_lock(&gate);map_paused=1;pthread_cond_broadcast(&cv);while(!fault_attempted)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);}
 if(++map_calls==fail_map)return -1;
 for(unsigned i=0;i<nm;i++)if(maps[i].va==va){maps[i].p=(void*)pa;replaced++;return 0;}
 maps[nm++]=(struct mapping){va,(void*)pa};return 0;
}
static int vm_space_map_new(struct vm_space *s,uintptr_t va,uintptr_t pa,unsigned f){(void)s;(void)f;if(!mem_held)abort();for(unsigned i=0;i<nm;i++)if(maps[i].va==va)return 1;maps[nm++]=(struct mapping){va,(void*)pa};return 0;}
static int vm_space_unmap(struct vm_space *s,uintptr_t va){(void)s;if(!mem_held)abort();for(unsigned i=0;i<nm;i++)if(maps[i].va==va){pmm_free(maps[i].p);maps[i]=maps[--nm];return 0;}return -1;}
static int vm_space_fault_anon(struct vm_space *s,uintptr_t va){(void)s;(void)va;return -1;}
static int vm_space_fault(struct vm_space *s,uintptr_t va,int w){(void)s;(void)va;(void)w;return -1;}
static void *pmem_domain(struct proc *p){(void)p;return NULL;}
static void user_thread_main(void *p){(void)p;}
static struct thread *thread_prepare(const char *n,void(*fn)(void*),void *arg,int pr,void *d){
 (void)n;(void)fn;(void)pr;(void)d;prepared_top=((struct ustart*)arg)->usp;
 if(mode==1){pthread_mutex_lock(&gate);arrived++;pthread_cond_broadcast(&cv);while(arrived<2)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);if(role==1)return NULL;}
 if(mode==2){pthread_mutex_lock(&gate);while(!fault_done)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);return NULL;}
 if(prepare_fail)return NULL;kfree(arg);return &th;
}
static void free_user_stack_slot(struct proc*,unsigned);
static int proc_fault_locked(struct proc*,uintptr_t,int);
static void pmem_lock(struct proc*);
static void pmem_unlock(struct proc*);
CODE
static void reset(struct proc *p){pmem_lock(p);while(nm)vm_space_unmap(p->space,maps[0].va);pmem_unlock(p);if(live_pages||atomic_load(&live_starts))abort();alloc_calls=map_calls=fail_alloc=fail_map=prepare_fail=replaced=0;mode=arrived=map_paused=fault_attempted=fault_done=0;fault_rc=-99;p->stack_slots=1;target=p;}
struct creator {int id;struct thread *result;uintptr_t top;};
static void *creator(void *arg){struct creator *c=arg;role=c->id;c->result=user_thread_prepare(target,1,2,0);c->top=prepared_top;return NULL;}
static uintptr_t fault_offset;
static void *fault_worker(void *arg){(void)arg;pthread_mutex_lock(&gate);while(!map_paused)pthread_cond_wait(&cv,&gate);fault_attempted=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);fault_rc=proc_fault(target,USER_STACK_TOP-USER_STACK_SLOT-fault_offset,1);pthread_mutex_lock(&gate);fault_done=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);return NULL;}
int main(void){struct vm_space vs;struct proc p={.space=&vs,.stack_slots=1,.mem.mx=PTHREAD_MUTEX_INITIALIZER};unsigned cases=0;
for(unsigned kind=0;kind<3;kind++)for(unsigned at=1;at<=4;at++){
 reset(&p);prepare_fail=kind==2;fail_alloc=kind==0?at:0;fail_map=kind==1?at:0;
 if(user_thread_prepare(&p,1,2,0)||p.stack_slots!=1||nm||live_pages||atomic_load(&live_starts))return 10;
 fail_alloc=fail_map=prepare_fail=0;if(!user_thread_prepare(&p,1,2,0)||p.stack_slots!=3||nm!=4||live_pages!=4||atomic_load(&live_starts))return 11;cases++;
}
reset(&p);prepare_fail=1;if(user_thread_prepare(&p,1,2,0x123000)||p.stack_slots!=1||nm||live_pages)return 12;cases++;
printf("PASS %u sequential failure/retry/supplied-stack cases\n",cases);
for(unsigned run=0;run<100;run++){
 reset(&p);mode=1;struct creator a={.id=1},b={.id=2};pthread_t ta,tb;
 if(pthread_create(&ta,NULL,creator,&a)||pthread_create(&tb,NULL,creator,&b))return 90;
 pthread_join(ta,NULL);pthread_join(tb,NULL);
 unsigned bs=(unsigned)((USER_STACK_TOP-b.top)/USER_STACK_SLOT);
 if(a.result||!b.result||a.top==b.top||bs<1||bs>2||p.stack_slots!=(1u|(1u<<bs))||nm!=4||live_pages!=4||atomic_load(&live_starts))return 30;
 for(unsigned i=0;i<nm;i++)if(maps[i].va>=b.top||maps[i].va<b.top-USER_STACK_PAGES*PAGE_SIZE)return 31;
}
puts("PASS 100 real pthread two-creator one-failure/one-success cases, distinct slots and survivor mappings");
for(unsigned kind=0;kind<2;kind++)for(unsigned run=0;run<100;run++){
 reset(&p);mode=2;fault_offset=(kind==0?8:1)*PAGE_SIZE;struct creator a={.id=1};pthread_t ta,tf;
 if(pthread_create(&tf,NULL,fault_worker,NULL)||pthread_create(&ta,NULL,creator,&a))return 90;
 pthread_join(ta,NULL);pthread_join(tf,NULL);
 if(a.result||fault_rc||p.stack_slots!=1||nm||live_pages||replaced||atomic_load(&live_starts))return kind==0?20:21;
 if(proc_fault(&p,USER_STACK_TOP-USER_STACK_SLOT-fault_offset,1)!=-1||nm||live_pages)return 22;
}
puts("PASS 200 real pthread concurrent deep/initial-page fault rollback cases plus post-release fault rejection");
reset(&p);return 0;}
'''


def main():
    paths = ['kernel/proc.c', 'kernel/proc_mem.c', 'kernel/proc_internal.h']
    src = {p: (ROOT / p).read_text() for p in paths}
    helpers = ['map_page', 'map_user_stack', 'unmap_user_stack', 'alloc_user_stack',
               'free_user_stack_slot', 'user_thread_prepare']
    code = '\n'.join(function(src['kernel/proc.c'], n) for n in helpers)
    code += '\n' + '\n'.join(function(src['kernel/proc_mem.c'], n) for n in
                              ['pmem_lock', 'pmem_unlock', 'proc_fault', 'proc_fault_locked'])
    body = FIXTURE.replace('CODE', code)
    variants = {
        'baseline': body,
        'omit-partial-unmap': body.replace('while (--i >= 1)', 'while (0 && --i >= 1)'),
        'omit-slot-release': body.replace('__atomic_and_fetch(&p->stack_slots, ~(1u << slot), __ATOMIC_RELEASE);', '/* mutation */'),
        'only-initial-unmap': body.replace('for (uintptr_t va = top - USER_STACK_SLOT + PAGE_SIZE; va < top; va += PAGE_SIZE)', 'for (uintptr_t va = top - USER_STACK_PAGES * PAGE_SIZE; va < top; va += PAGE_SIZE)'),
    }
    expected = {'baseline': 0, 'omit-partial-unmap': 10, 'omit-slot-release': 10,
                'only-initial-unmap': 20}
    out = Path(tempfile.mkdtemp(prefix='welyos-stack-current-'))
    rows = []
    for name, text in variants.items():
        if name != 'baseline' and text == body:
            raise RuntimeError('mutation did not match: ' + name)
        c = out / (name + '.c')
        c.write_text(text)
        exe = c.with_suffix('')
        comp = subprocess.run(['cc', '-std=c11', '-O1', '-pthread', '-fsanitize=undefined',
                               '-fno-sanitize-recover=all', str(c), '-o', str(exe)],
                              capture_output=True, text=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=40) if comp.returncode == 0 else comp
        row = dict(variant=name, compile_exit=comp.returncode, exit=result.returncode,
                   expected_exit=expected[name], passed=comp.returncode == 0 and result.returncode == expected[name],
                   stdout=result.stdout, stderr=result.stderr)
        rows.append(row)
        print(name, row['passed'], result.returncode, result.stdout.strip(), result.stderr.strip())
    changed = [p for p, s in src.items() if (ROOT / p).read_text() != s]
    (out / 'results.json').write_text(json.dumps(dict(
        inputs={p: hashlib.sha256(s.encode()).hexdigest() for p, s in src.items()},
        changed_inputs=changed, cases=rows), indent=2) + '\n')
    print('artifacts:', out, 'changed_inputs:', changed)
    return bool(changed) or not all(r['passed'] for r in rows)


if __name__ == '__main__':
    raise SystemExit(main())
