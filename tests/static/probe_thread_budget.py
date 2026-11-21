#!/usr/bin/env python3
"""Extract production prepare/abandon/free with injected allocation/quota failures."""
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
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define STACK_PAGES 4
#define PRIO_MIN 0
#define PRIO_MAX 7
#define MB_KOBJ 0
struct mb_domain {unsigned long refs,used,limit;};
struct thread {struct mb_domain *dom;unsigned long kobj;void *stack;const char *name;};
static struct thread storage;static size_t alloc_size;
static int alloc_fail,get_fail,stacks_freed,structs_freed,alloc_calls;
static unsigned nzombies;
static void panic(const char *s,...){(void)s;abort();}
static struct thread *thread_alloc(const char *n,void(*f)(void*),void *a,int p){
 (void)f;(void)a;(void)p;alloc_calls++;if(alloc_fail)return NULL;
 memset(&storage,0,sizeof storage);storage.name=n;storage.stack=(void*)1;return &storage;
}
static size_t kmalloc_size(void *p){(void)p;return alloc_size;}
static int mb_domain_get(struct mb_domain *d){if(get_fail)return -1;d->refs++;return 0;}
static int mb_domain_put(struct mb_domain *d){if(!d->refs)return -1;d->refs--;return 0;}
static int mb_quota_add(struct mb_domain *d,int c,unsigned long n){
 (void)c;if(d->used>d->limit || n>d->limit-d->used)return -1;d->used+=n;return 0;
}
static int mb_quota_sub(struct mb_domain *d,int c,unsigned long n){
 (void)c;if(n>d->used)return -1;d->used-=n;return 0;
}
static void stack_free(void *p){(void)p;stacks_freed++;}
static void thread_struct_free(struct thread *t){(void)t;structs_freed++;}
CODE
static void fn(void *a){(void)a;}
int main(void){
 unsigned cases=0;const size_t sizes[]={1,4095,4096,4097,8192};
 for(unsigned si=0;si<5;si++)for(unsigned used=0;used<4;used++)
 for(unsigned limit=0;limit<16;limit++)for(int fail=0;fail<3;fail++)
 for(int dom=0;dom<2;dom++)for(int finish=0;finish<2;finish++){
  struct mb_domain d={1,used,limit};alloc_size=sizes[si];alloc_fail=fail==1;get_fail=fail==2;
  stacks_freed=structs_freed=alloc_calls=0;nzombies=0;
  unsigned long charge=5+(alloc_size+4095)/4096;
  int expected=!alloc_fail && (!dom || (!get_fail && used<=limit && charge<=limit-used));
  struct thread *t=thread_prepare("probe",fn,NULL,3,dom?&d:NULL);
  if(!!t!=expected)return 10;
  if(t){
   if(d.used!=used+(dom?charge:0) || d.refs!=(unsigned)(1+dom))return 11;
   if(finish){nzombies=1;thread_free(t);}else thread_abandon(t);
  }
  if(d.used!=used || d.refs!=1 || nzombies)return 12;
  int freed=!alloc_fail;
  if(stacks_freed!=freed || structs_freed!=freed)return 13;
  cases++;
 }
 for(int p=-1;p<=8;p+=9){
  alloc_calls=0;if(thread_prepare("invalid",fn,NULL,p,NULL) || alloc_calls)return 14;
 }
 printf("PASS %u prepare/failure/abandon/deferred-free quota and reference cases\n",cases);return 0;
}
'''

if __name__=='__main__':
 src=(ROOT/'kernel/sched.c').read_text();body='\n'.join(function(src,n) for n in ('thread_uncharge','thread_free','thread_prepare','thread_abandon'))
 variants={'none':body,'missing-domain-put':body.replace('|| mb_domain_put(d) != 0','|| 0'),
           'stack-undercharge':body.replace('STACK_PAGES + 1 +','STACK_PAGES +'),
           'quota-failure-leaks-ref':body.replace('if (mb_domain_put(dom) != 0)','if (0)')}
 out=Path(tempfile.mkdtemp(prefix='welyos-thread-budget-'));rows=[]
 for name,b in variants.items():
  c=out/(name+'.c');c.write_text(FIXTURE.replace('CODE',b));exe=c.with_suffix('')
  subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,check=True)
  p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
  ok=p.returncode==0 if name=='none' else p.returncode in range(10,15)
  rows.append(dict(case=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name,ok,p.returncode,p.stdout.strip())
 changed=(ROOT/'kernel/sched.c').read_text()!=src
 (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_input:',changed)
 raise SystemExit(int(changed or not all(r['passed'] for r in rows)))
