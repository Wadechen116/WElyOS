#!/usr/bin/env python3
"""Extract real VM fork/create/destroy/walk code; inject allocator/ledger failures.

PMM, quota, PTE encodings, locks and shootdowns are fixtures, not real kernel
implementations. Reduced radix exercises caller unwind across all child tables.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
PREFIX = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define MMU_BITS 2
#define ENTRIES 4
#define IDX_MASK 3
#define MMU_VA_BITS (PAGE_SHIFT+MMU_LEVELS*MMU_BITS)
#define MMU_ASID_MAX 0
#define USER_BASE 0UL
#define USER_TOP (1UL<<MMU_VA_BITS)
#define VM_WRITE 2
#define VM_SHARED 32
#define VM_COW 64
#define VM_TICKET 128
#define TABLE_BIT 256
typedef uintptr_t pte_t;
enum mb_class {MB_ANON,MB_STACK,MB_IMAGE,MB_PGTABLE,MB_KOBJ,MB_RAMFS,MB_NCLASS};
struct mb_domain {unsigned long q[MB_NCLASS]; unsigned refs;};
struct mb_bundle {unsigned long quota[MB_NCLASS],reserve;};
struct vm_range {uintptr_t start,len;};
struct vm_space {pte_t *root;unsigned asid,used_cpus;unsigned long pages;
 struct mb_domain *dom;unsigned long quota[MB_NCLASS],data_tickets,pt_tickets,anon_tables;};
enum walk_use {WALK_KERNEL,WALK_LEGACY,WALK_ANON};
struct fork_anon {const struct vm_range *r;unsigned n;unsigned long marked;};
static pte_t kernel_root[PAGE_SIZE/sizeof(pte_t)],*root=kernel_root;
static unsigned long table_pages,anon_pt_max,reserved;
static int enabled,vm_lk,live_heap,depth;
static struct {uintptr_t base,len;} anon_win[1];static unsigned n_anon_win=1;
static int inject_kind,inject_at,calls[6];
static int fail(int kind){int n=calls[kind]++;return inject_kind==kind&&inject_at==n;}
static void panic(const char *s,...){fprintf(stderr,"panic: %s\n",s);exit(90);}
static void *kzalloc(size_t n){if(fail(1))return NULL;void*p=calloc(1,n);if(p)live_heap++;return p;}
static void kfree(void*p){live_heap--;free(p);}
static struct {void*p;unsigned refs;} blocks[512];static unsigned nb;
static void *pmm_alloc(void){if(fail(2))return NULL;void*p=aligned_alloc(PAGE_SIZE,PAGE_SIZE);if(!p)abort();blocks[nb++]=(typeof(blocks[0])){p,1};return p;}
static void pmm_ref(void*p){for(unsigned i=0;i<nb;i++)if(blocks[i].p==p){if(!blocks[i].refs)abort();blocks[i].refs++;return;}abort();}
static void pmm_free(void*p){for(unsigned i=0;i<nb;i++)if(blocks[i].p==p){if(!blocks[i].refs)abort();blocks[i].refs--;return;}abort();}
static int pmm_reserve(unsigned long n){if(fail(5))return -1;reserved+=n;return 0;}
static void pmm_unreserve(unsigned long n){if(n>reserved)abort();reserved-=n;}
static void *pmm_alloc_reserved(void){void*p=pmm_alloc();if(!p)abort();pmm_unreserve(1);return p;}
static int mb_domain_get(struct mb_domain*d){if(fail(3))return -1;d->refs++;return 0;}
static int mb_domain_put(struct mb_domain*d){if(!d->refs)abort();d->refs--;return 0;}
static int mb_quota_add(struct mb_domain*d,int c,unsigned long n){if(fail(4))return -1;d->q[c]+=n;return 0;}
static int mb_quota_sub(struct mb_domain*d,int c,unsigned long n){if(n>d->q[c])abort();d->q[c]-=n;return 0;}
static int mb_commit_bundle(struct mb_domain*d,const struct mb_bundle*b){if(pmm_reserve(b->reserve))return -1;for(int c=0;c<MB_NCLASS;c++)d->q[c]+=b->quota[c];return 0;}
static int mb_release_bundle(struct mb_domain*d,const struct mb_bundle*b){for(int c=0;c<MB_NCLASS;c++)mb_quota_sub(d,c,b->quota[c]);pmm_unreserve(b->reserve);return 0;}
static unsigned long spin_lock_irqsave(void*p){(void)p;if(depth++)abort();return 0;}
static void spin_unlock_irqrestore(void*p,unsigned long f){(void)p;(void)f;if(--depth)abort();}
static unsigned long arch_irq_save(void){return 0;}
static void arch_irq_restore(unsigned long f){(void)f;}
static void arch_asid_flush(unsigned n){(void)n;}
static void arch_mmu_root_changed(pte_t*t){(void)t;}
static void sched_space_dropped(struct vm_space*v){(void)v;}
static void space_flush_fn(void*p){(void)p;}
static void smp_call_mask(unsigned m,void(*fn)(void*),void*p){(void)m;(void)fn;(void)p;}
static int arch_pte_valid(pte_t p){return p!=0;}
static unsigned arch_pte_flags(pte_t p){return p&4095;}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static int arch_pte_is_table(pte_t p,int l){(void)l;return !!(p&TABLE_BIT);}
static pte_t arch_pte_table(uintptr_t p,int u,int l){(void)u;(void)l;return p|TABLE_BIT;}
static pte_t arch_pte_leaf(uintptr_t p,unsigned f,int l){(void)l;return p|f;}
static void arch_pte_set(pte_t*p,pte_t v){*p=v;}
static int split_large(pte_t*p,int l,uintptr_t b){(void)p;(void)l;(void)b;abort();}
'''
HARNESS = r'''
static unsigned cases,hits[6],successes,maxcalls[6];
static void run(int kind,int at,int tickets){
 inject_kind=0;nb=0;live_heap=depth=0;table_pages=reserved=0;memset(calls,0,sizeof calls);
 struct mb_domain pd={.refs=1},cd={.refs=1};
 anon_win[0].base=0;anon_win[0].len=USER_TOP;anon_pt_max=pt_need(0,USER_TOP);
 struct vm_space*p=vm_space_create(&pd);if(!p)abort();
 unsigned n=0;
 for(uintptr_t a=0;a<USER_TOP/2;a+=PAGE_SIZE*3){
  pte_t*e=walk(p,p->root,a,0,1,0,WALK_LEGACY,NULL);void*data=pmm_alloc();
  memset(data,(int)(n+1),PAGE_SIZE);
  arch_pte_set(e,arch_pte_leaf((uintptr_t)data,n%3==0?VM_COW|VM_TICKET:VM_WRITE,0));
  p->pages++;n++;
 }
 unsigned long q=USER_TOP/PAGE_SIZE;
 pd.q[MB_ANON]=p->quota[MB_ANON]=q;
 p->data_tickets=q-n+(n+2)/3;
 p->pt_tickets=tickets?anon_pt_max-p->anon_tables:0;
 p->quota[MB_PGTABLE]+=p->pt_tickets;pd.q[MB_PGTABLE]+=p->pt_tickets;
 reserved=p->data_tickets+p->pt_tickets;
 struct vm_space before=*p;struct mb_domain pdb=pd,cdb=cd;
 unsigned oldnb=nb,oldrefs[512];unsigned char saved[512][PAGE_SIZE];
 for(unsigned i=0;i<nb;i++){oldrefs[i]=blocks[i].refs;memcpy(saved[i],blocks[i].p,PAGE_SIZE);}
 unsigned long oldreserve=reserved,oldtables=table_pages;int oldheap=live_heap;
 memset(calls,0,sizeof calls);inject_kind=kind;inject_at=at;
 struct vm_range range={0,USER_TOP};struct vm_space*c=vm_space_fork(p,&cd,&range,1);
 int hit=kind&&calls[kind]>at;inject_kind=0;
 for(int k=1;k<=5;k++)if((unsigned)calls[k]>maxcalls[k])maxcalls[k]=calls[k];
 if(hit)hits[kind]++;else successes++;
 if(hit){
  if(c)return (void)panic("injected failure returned child");
  if(memcmp(p,&before,sizeof before)||memcmp(&pd,&pdb,sizeof pd)||memcmp(&cd,&cdb,sizeof cd))panic("parent/domain rollback");
  if(reserved!=oldreserve||table_pages!=oldtables||live_heap!=oldheap)panic("reserve/table/heap rollback");
  for(unsigned i=0;i<nb;i++){
   if(blocks[i].refs!=(i<oldnb?oldrefs[i]:0))panic("PMM refs rollback");
   if(i<oldnb&&memcmp(saved[i],blocks[i].p,PAGE_SIZE))panic("parent page content/PTE rollback");
  }
 }else{
  if(!c)panic("unexpected failure");
  unsigned writable=n-(n+2)/3;
  if(p->data_tickets!=before.data_tickets+writable)panic("successful parent tickets");
  vm_space_destroy(c);
  if(memcmp(&cd,&cdb,sizeof cd)||reserved!=oldreserve+writable||table_pages!=oldtables||live_heap!=oldheap)panic("successful child destroy");
  for(unsigned i=0;i<oldnb;i++)if(blocks[i].refs!=oldrefs[i])panic("successful child refs");
 }
 vm_space_destroy(p);
 if(reserved||table_pages||live_heap||pd.refs!=1)panic("final lifecycle leak");
 for(unsigned i=0;i<nb;i++){if(blocks[i].refs)panic("live final page");free(blocks[i].p);}
 cases++;
}
int main(void){
 for(int tickets=0;tickets<2;tickets++)for(int kind=1;kind<=5;kind++)for(int at=0;at<32;at++)run(kind,at,tickets);
 printf("PASS %u caller failure/lifecycle cases, levels=%d; injected kzalloc=%u PMM=%u get=%u quota=%u reserve=%u, successes=%u; max PMM calls=%u quota calls=%u\n",cases,MMU_LEVELS,hits[1],hits[2],hits[3],hits[4],hits[5],successes,maxcalls[2],maxcalls[4]);return 0;
}
'''

if __name__ == '__main__':
    src=(ROOT/'kernel/vm.c').read_text()
    names=('va_index','table_alloc','table_in_anon','anon_table_built','new_table','walk',
           'asid_alloc','asid_free','vm_space_create','free_table','space_gone_fn',
           'vm_space_destroy','tables_at','pt_need','in_anon','fork_table','vm_space_fork')
    code='\n'.join(function(src,n) for n in names)
    out=Path(tempfile.mkdtemp(prefix='welyos-fork-caller-'));rows=[]
    mutations={'none':code,
       'omit-parent-pending-rollback':code.replace('pmm_unreserve(pending);','(void)pending;'),
       'omit-destroy-child':code.replace('vm_space_destroy(child);','(void)child;'),
       'omit-root-quota-rollback':code.replace('mb_quota_sub(dom, MB_PGTABLE, 1) != 0','0')}
    for levels in (2,3):
        for name,body in mutations.items():
            c=out/f'{levels}-{name}.c';c.write_text(f'#define MMU_LEVELS {levels}\n'+PREFIX+body+HARNESS)
            exe=c.with_suffix('')
            cc=subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
            if cc.returncode:
                print(cc.stderr);raise SystemExit(cc.returncode)
            p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            ok=p.returncode==0 if name=='none' else p.returncode==90
            row=dict(levels=levels,mutation=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr)
            rows.append(row);print(row)
    changed=(ROOT/'kernel/vm.c').read_text()!=src
    (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
    print('artifacts:',out,'changed_input:',changed)
    raise SystemExit(int(changed or not all(r['passed'] for r in rows)))
