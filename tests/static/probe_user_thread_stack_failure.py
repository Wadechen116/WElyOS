#!/usr/bin/env python3
"""Known counterexample: user stack survives subsequent kernel-thread failure."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT=Path(__file__).resolve().parents[2]
FIXTURE=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define PROC_MAX_THREADS 16
#define PAGE_SIZE 4096UL
#define USER_STACK_TOP 0x70000000UL
#define USER_STACK_SLOT 0x100000UL
#define USER_STACK_PAGES 4u
#define VM_READ 1
#define VM_WRITE 2
#define PRIO_NORMAL 3
struct proc {unsigned nstacks;void *space;char name[16];};
struct thread {int unused;};
struct ustart {struct proc *p;uintptr_t entry,arg,usp;};
static struct ustart storage;static unsigned mapped_pages,allocs,frees,map_calls;
static int fail_map_at=-1;
static void *kmalloc(size_t n){(void)n;allocs++;return &storage;}
static void kfree(void *p){(void)p;frees++;}
struct vm_space;
static void *map_page(struct vm_space *s,uintptr_t a,unsigned f){
 (void)s;(void)a;(void)f;if((int)map_calls++==fail_map_at)return NULL;
 mapped_pages++;return (void*)1;
}
static void *pmem_domain(struct proc *p){(void)p;return NULL;}
static void user_thread_main(void *p){(void)p;}
static struct thread *thread_prepare(const char *n,void (*f)(void*),void *a,int pr,void *d){
 (void)n;(void)f;(void)a;(void)pr;(void)d;return NULL;
}
CODE
int main(void){
 struct proc p={.nstacks=1};
 fail_map_at=2;
 if(user_thread_prepare(&p,0x1000,0,0)!=NULL || p.nstacks!=1 || mapped_pages!=2)return 6;
 puts("REPRODUCED: third stack-page allocation failed; two earlier mappings retained");
 fail_map_at=-1;mapped_pages=map_calls=allocs=frees=0;
 for(unsigned i=0;i<PROC_MAX_THREADS-1;i++)
  if(user_thread_prepare(&p,0x1000,0,0)!=NULL)return 2;
 if(p.nstacks!=PROC_MAX_THREADS || mapped_pages!=4*(PROC_MAX_THREADS-1) || allocs!=frees)return 3;
 printf("REPRODUCED: %u failed preparations consumed %u stack slots and retained %u mapped pages; ustart freed\n",PROC_MAX_THREADS-1,p.nstacks-1,mapped_pages);
 unsigned old=mapped_pages;
 if(user_thread_prepare(&p,0x1000,0,0)!=NULL || mapped_pages!=old)return 4;
 puts("Subsequent auto-stack preparation refused: all stack slots consumed without a started thread");
 /* A caller-supplied stack is not newly allocated and must not be reclaimed here. */
 struct proc supplied={.nstacks=1};
 if(user_thread_prepare(&supplied,0x1000,0,0x200000)!=NULL || supplied.nstacks!=1 || mapped_pages!=old)return 5;
 return 0;
}
'''
if __name__=='__main__':
 src=(ROOT/'kernel/proc.c').read_text();out=Path(tempfile.mkdtemp(prefix='welyos-user-stack-failure-'))
 code='\n'.join(function(src,n) for n in ('map_user_stack','alloc_user_stack','user_thread_prepare'))
 c=out/'probe.c';c.write_text(FIXTURE.replace('CODE',code));exe=out/'probe'
 subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
 p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
 changed=(ROOT/'kernel/proc.c').read_text()!=src
 (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,exit=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n')
 print(p.stdout.strip());print('artifacts:',out,'changed_input:',changed)
 raise SystemExit(p.returncode or int(changed))
