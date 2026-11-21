#!/usr/bin/env python3
"""Rebuild immutable c3-bind source; run true bind/create/destroy lifecycle."""
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
#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
#define PAGE_SIZE 4096UL
#define E_NOMEM 12
#define E_INVAL 22
#define MMU_LEVELS 2
#define USER_BASE 0UL
#define USER_TOP 16384UL
struct spinlock {pthread_mutex_t mx;};
#define SPINLOCK_INIT(n,l) {PTHREAD_MUTEX_INITIALIZER}
static unsigned long spin_lock_irqsave(struct spinlock *l){pthread_mutex_lock(&l->mx);return 0;}
static void spin_unlock_irqrestore(struct spinlock *l,unsigned long f){(void)f;pthread_mutex_unlock(&l->mx);}
enum mb_class {MB_ANON,MB_STACK,MB_IMAGE,MB_PGTABLE,MB_KOBJ,MB_RAMFS,MB_NCLASS};
struct mb_domain;
TXN
struct mb_bundle {unsigned long quota[MB_NCLASS],reserve;};
struct mb_stats {unsigned long used[MB_NCLASS],total,limit;unsigned refs;};
static atomic_uint live,fail_heap;
static unsigned long reserved,table_pages;
static int fail_root;
static void *kzalloc(size_t n){if(atomic_exchange(&fail_heap,0))return NULL;void *p=calloc(1,n);if(p)atomic_fetch_add(&live,1);return p;}
static void kfree(void *p){if(p){atomic_fetch_sub(&live,1);free(p);}}
static int pmm_reserve(unsigned long n){reserved+=n;return 0;}
static void pmm_unreserve(unsigned long n){if(n>reserved)abort();reserved-=n;}
int mb_quota_add_txn(struct mb_domain*,enum mb_class,unsigned long,const struct mb_exec_txn*);
int mb_commit_bundle_txn(struct mb_domain*,const struct mb_bundle*,const struct mb_exec_txn*);
MB
typedef uintptr_t pte_t;
struct vm_space {struct mb_domain *dom;struct mb_exec_txn *txn;unsigned long quota[MB_NCLASS],data_tickets,pt_tickets;pte_t *root;unsigned asid,used_cpus;};
static struct spinlock vm_lk=SPINLOCK_INIT("vm",0);
static pte_t kernel_root[PAGE_SIZE/sizeof(pte_t)];static pte_t *root=kernel_root;
static pte_t *table_alloc(void){if(fail_root){fail_root=0;return NULL;}pte_t *p=calloc(1,PAGE_SIZE);if(p)table_pages++;return p;}
static unsigned asid_alloc(void){return 1;}
static void asid_free(unsigned a){(void)a;}
static void pmm_free(void *p){free(p);}
static void panic(const char *s,...){fprintf(stderr,"panic %s\n",s);abort();}
static unsigned long arch_irq_save(void){return 0;}
static void arch_irq_restore(unsigned long f){(void)f;}
static void space_gone_fn(void *p){(void)p;}
static void smp_call_mask(unsigned n,void(*f)(void*),void *p){(void)n;(void)f;(void)p;}
static unsigned va_index(uintptr_t a,int l){(void)l;return (unsigned)(a>>12);}
static int arch_pte_valid(pte_t p){return p!=0;}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static void free_table(pte_t *p,int l){(void)p;(void)l;abort();} /* roots have no leaf mappings */
VM
#define CHECK(x,n) do{if(!(x)){printf("FAIL line %d code %d\n",__LINE__,n);return n;}}while(0)
static struct mb_domain *setup(struct mb_exec_txn *t){struct mb_domain *d=mb_domain_new(100);if(!d||mb_quota_add(d,MB_IMAGE,80)||mb_exec_begin(d,80,t))abort();return d;}
static void finish(struct mb_domain *d){if(mb_quota_sub(d,MB_IMAGE,80)||mb_domain_put(d)||atomic_load(&live)||reserved||table_pages||errors)abort();}
struct call {struct mb_domain *d;struct mb_exec_txn *t;struct vm_space *vs;};
static void *create_worker(void *p){struct call *c=p;c->vs=vm_space_create_txn(c->d,c->t);return NULL;}
int main(void){unsigned cases=0;
for(unsigned kind=0;kind<4;kind++){
 struct mb_exec_txn t;struct mb_domain *d=setup(&t);unsigned long total=d->total;unsigned refs=d->refs;unsigned heap=atomic_load(&live);
 if(kind==0)atomic_store(&fail_heap,1);
 if(kind==1)d->refs=MB_REFS_MAX;
 if(kind==2)mb_set_limit(d,0);
 if(kind==3)fail_root=1;
 unsigned expected_refs=d->refs;
 CHECK(vm_space_create_txn(d,&t)==NULL,10);
 CHECK(!t.bound&&t.active&&d->total==total&&d->refs==expected_refs&&atomic_load(&live)==heap&&!table_pages,11);
 d->refs=refs;mb_set_limit(d,100);
 struct vm_space *vs=vm_space_create_txn(d,&t);CHECK(vs&&t.bound==vs&&vs->txn==&t&&d->refs==refs+1&&d->total==total+1,12);
 vm_space_clear_txn(vs);CHECK(!vs->txn&&t.bound==vs,13);vm_space_destroy(vs);mb_exec_end(&t);finish(d);cases++;
}
puts("PASS heap/domain_get/quota/root-allocation failure: unbound, no owner side effects, same active credential retry succeeds");
{
 struct mb_exec_txn t;struct mb_domain *d=setup(&t),*foreign=mb_domain_new(1000);struct vm_space *a=vm_space_create_txn(d,&t);CHECK(a,20);
 struct mb_stats before={0},after={0},fbefore={0},fafter={0};mb_stats(d,&before);mb_stats(foreign,&fbefore);unsigned heap=atomic_load(&live);
 CHECK(!vm_space_create_txn(d,&t)&&!vm_space_create_txn(foreign,&t)&&!vm_space_create_txn(NULL,&t),21);
 mb_stats(d,&after);mb_stats(foreign,&fafter);CHECK(!memcmp(&before,&after,sizeof(before))&&!memcmp(&fbefore,&fafter,sizeof(fbefore))&&heap==atomic_load(&live)&&t.bound==a,22);
 char stranger;mb_exec_unbind(&t,&stranger);CHECK(t.bound==a,23);
 vm_space_clear_txn(a);CHECK(!a->txn&&t.bound==a&&!vm_space_create_txn(d,&t),24);
 vm_space_destroy(a);CHECK(t.bound==a&&!vm_space_create_txn(d,&t),25);
 mb_exec_end(&t);CHECK(!t.active&&!vm_space_create_txn(d,&t),26);
 CHECK(mb_exec_begin(d,80,&t)==0&&!t.bound,27);a=vm_space_create_txn(d,&t);CHECK(a,28);vm_space_destroy(a);mb_exec_end(&t);
 CHECK(mb_domain_put(foreign)==0,29);finish(d);cases++;
}
puts("PASS second/foreign/NULL-domain/ended rejects preserve both domains and binding; non-owner unbind ignored; clear/destroy retain tombstone, begin resets it");
for(unsigned run=0;run<100;run++){
 struct mb_exec_txn t;struct mb_domain *d=setup(&t);struct call a={d,&t,NULL},b={d,&t,NULL};pthread_t x,y;
 CHECK(!pthread_create(&x,NULL,create_worker,&a)&&!pthread_create(&y,NULL,create_worker,&b),30);pthread_join(x,NULL);pthread_join(y,NULL);
 struct vm_space *v=a.vs?a.vs:b.vs;CHECK(!!a.vs+!!b.vs==1&&v&&t.bound==v&&d->refs==2&&d->total==81,31);
 vm_space_destroy(v);mb_exec_end(&t);finish(d);cases++;
}
printf("PASS %u lifecycle cases including 100 real pthread simultaneous create attempts, exactly one binding each\n",cases);return 0;}
'''

def main():
    bundle=ROOT/'build/milestones/c32-c3-bind'
    patch=(bundle/'source.patch').read_bytes();out=Path(tempfile.mkdtemp(prefix='welyos-c3-bind-host-'))
    for p in re.findall(r'^diff --git a/(\S+) b/\S+$',patch.decode(),re.M):
        r=subprocess.run(['git','show','fbdfbe4:'+p],cwd=ROOT,capture_output=True,check=True)
        f=out/p;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(r.stdout)
    subprocess.run(['patch','-p1','--batch','--fuzz=0','-i',str(bundle/'source.patch')],cwd=out,check=True,capture_output=True)
    paths=['include/membudget.h','kernel/membudget.c','kernel/vm.c'];src={p:(out/p).read_text() for p in paths}
    expected=dict((p,h) for h,p in re.findall(r'^  ([a-f0-9]{64})  (\S+)$',(bundle/'manifest.txt').read_text(),re.M))
    hashes={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()}
    if any(hashes[p]!=expected[p] for p in paths):raise RuntimeError('fixed source hash mismatch')
    txn=re.search(r'struct mb_exec_txn \{.*?\n\};',src['include/membudget.h'],re.S).group()
    mb='\n'.join(x for x in src['kernel/membudget.c'].splitlines() if not x.startswith('#include'))
    vm='\n'.join(function(src['kernel/vm.c'],n) for n in ['vm_space_create_txn','vm_space_clear_txn','vm_space_quota_total','vm_space_destroy'])
    body=PRE.replace('TXN\n',txn+'\n').replace('MB\n',mb+'\n').replace('VM\n',vm+'\n')
    variants={'baseline':(body,0),'omit-get-failure-unbind':(body.replace('if (mb_domain_get(dom) != 0) {\n            if (txn)\n                mb_exec_unbind(txn, vs);','if (mb_domain_get(dom) != 0) {'),11),
              'allow-second-binding':(body.replace('&& !t->bound && owner','&& owner'),21)}
    rows=[]
    for name,(code,expected_exit) in variants.items():
        if name!='baseline' and code==body:raise RuntimeError('mutation missed')
        c=out/(name+'.c');c.write_text(code);exe=c.with_suffix('')
        cp=subprocess.run(['cc','-std=c11','-O1','-pthread','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30) if cp.returncode==0 else cp
        row=dict(variant=name,compile_exit=cp.returncode,exit=r.returncode,expected_exit=expected_exit,passed=cp.returncode==0 and r.returncode==expected_exit,stdout=r.stdout,stderr=r.stderr);rows.append(row);print(row)
    (out/'results.json').write_text(json.dumps(dict(source_hashes=hashes,patch_sha256=hashlib.sha256(patch).hexdigest(),cases=rows),indent=2)+'\n')
    print('artifacts:',out);return not all(r['passed'] for r in rows)

if __name__=='__main__':raise SystemExit(main())
