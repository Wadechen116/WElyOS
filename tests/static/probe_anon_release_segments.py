#!/usr/bin/env python3
"""Production release loop with deterministic COW transitions at unlock boundaries."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT=Path(__file__).resolve().parents[2]
FIXTURE=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define RELEASE_SEGMENT 64u
#define MB_ANON 0
#define VM_SHARED 32
#define VM_TICKET 128
#define WALK_LEGACY 1
#define KASSERT(x) do{if(!(x)) abort();}while(0)
typedef uint64_t pte_t;
struct vm_space {void *dom;pte_t *root;unsigned long pages,data_tickets,quota[1];};
struct mb_bundle {unsigned long reserve,quota[1];};
static pte_t ptes[131];static unsigned long reserved,domain_used,freed,flushes;
static int vm_lk,enabled=1,locked,unlocks,target,phase,cow_mode;
static struct vm_space *current;
static void panic(const char *s,...){(void)s;abort();}
static unsigned long spin_lock_irqsave(int *l){(void)l;KASSERT(!locked);locked=1;return 0;}
static int arch_pte_valid(pte_t p){return p!=0;}
static unsigned arch_pte_flags(pte_t p){return p&4095;}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static void arch_pte_set(pte_t *p,pte_t v){KASSERT(locked);*p=v;}
static void pmm_free(void *p){(void)p;KASSERT(locked);freed++;}
static void tlb_shootdown_space(struct vm_space *v,uintptr_t a){(void)v;(void)a;KASSERT(locked);flushes++;}
static pte_t *walk(struct vm_space *v,pte_t *r,uintptr_t a,int t,int c,int s,int u,int *o){
 (void)v;(void)r;(void)t;(void)c;(void)s;(void)u;KASSERT(locked);*o=0;return &ptes[a/PAGE_SIZE];
}
static int mb_release_bundle(void *d,const struct mb_bundle *b){
 (void)d;KASSERT(locked && reserved>=b->reserve && domain_used>=b->quota[0]);
 reserved-=b->reserve;domain_used-=b->quota[0];return 0;
}
static void spin_unlock_irqrestore(int *l,unsigned long f){
 (void)l;(void)f;KASSERT(locked);locked=0;unlocks++;
 /* A separate actor resolves an outstanding COW before the next release segment.
    Mode 1 models copy, mode 2 sole-owner reuse; each removes its own ticket.
    This is an oracle transition, NOT execution of product vm_space_fault. */
 if(unlocks==phase && (ptes[target]&VM_TICKET)){
  KASSERT(current->data_tickets && reserved);
  current->data_tickets--;reserved--;
  ptes[target]=((target+1)*PAGE_SIZE)|1;
  if(cow_mode==1)freed++; /* Drop reference to old physical page during copy. */
 }
}
CODE
int main(void){
 unsigned cases=0;unsigned lens[]={1,63,64,65,127,128,130};
 for(unsigned li=0;li<7;li++)for(unsigned pattern=0;pattern<8;pattern++)
 for(int mode=1;mode<=2;mode++)for(int ph=1;ph<=2;ph++)for(unsigned victim=0;victim<lens[li];victim++){
  unsigned n=lens[li];struct vm_space vs={.dom=(void*)1,.quota={n}};current=&vs;
  reserved=domain_used=n;vs.data_tickets=n;freed=flushes=0;locked=unlocks=0;
  target=victim;phase=ph;cow_mode=mode;
  unsigned expected_mapped=0;
  for(unsigned i=0;i<n;i++){
   unsigned kind=(i+pattern)%4; /* absent, exclusive, COW(refs1), COW(refs>1) */
   ptes[i]=kind?((i+1)*PAGE_SIZE)|1|(kind>=2?VM_TICKET:0):0;
   if(kind){vs.pages++;expected_mapped++;}
   if(kind==1){vs.data_tickets--;reserved--;}
  }
  unsigned long original_tickets=reserved;
  vm_space_release_anon(&vs,0,n*PAGE_SIZE);
  if(vs.pages || vs.data_tickets || vs.quota[0] || reserved || domain_used || locked)return 10;
  if(unlocks!=(int)((n+63)/64) || flushes!=expected_mapped)return 11;
  unsigned extra=(mode==1 && victim>=(unsigned)(ph*64) &&
      (victim+pattern)%4>=2 && ph<(int)((n+63)/64));
  if(freed!=expected_mapped+extra)return 12;
  for(unsigned i=0;i<n;i++)if(ptes[i])return 13;
  (void)original_tickets;cases++;
 }
 printf("PASS %u segmented-release/interleaving schedules\n",cases);return 0;
}
'''

if __name__=='__main__':
 src=(ROOT/'kernel/vm.c').read_text();body=function(src,'vm_space_release_anon')
 out=Path(tempfile.mkdtemp(prefix='welyos-anon-release-'));rows=[]
 variants={'none':body,'omit-cow-return':body.replace('if (flags & VM_TICKET)','if (0)'),
           'omit-ledger-debit':body.replace('vs->data_tickets -= tickets;','(void)0;')}
 for name,b in variants.items():
  c=out/(name+'.c');c.write_text(FIXTURE.replace('CODE',b));exe=c.with_suffix('')
  subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
  p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
  ok=p.returncode==0 if name=='none' else p.returncode==10
  rows.append(dict(case=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name,ok,p.returncode,p.stdout.strip())
 changed=(ROOT/'kernel/vm.c').read_text()!=src
 (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_input:',changed)
 raise SystemExit(int(changed or not all(r['passed'] for r in rows)))
