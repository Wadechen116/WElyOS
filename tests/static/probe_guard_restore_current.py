#!/usr/bin/env python3
"""Extract real stack/VM helpers; guard remap with all new allocations denied.

Host PTE/PMM/lock fixtures, not hardware MMU, real PMM exhaustion or SMP.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define MMU_BITS 9
#define MMU_VA_BITS 63
#define MMU_LARGE_LEVEL 1
#define ENTRIES 512
#define IDX_MASK 511
#define STACK_PAGES 4
#define VM_READ 2u
#define VM_WRITE 4u
#define VM_EXEC 8u
#define VM_USER 16u
#define MB_PGTABLE 0
enum walk_use { WALK_KERNEL, WALK_LEGACY, WALK_ANON };
typedef uintptr_t pte_t;
struct vm_space { void *dom; unsigned long anon_tables,pt_tickets,quota[1]; };
static int vm_lk,stack_lk,enabled=1,deny,allocations,attempts,freed,flushes;
static char *stack_cache,*block; static unsigned stack_cached;
static pte_t *root; static unsigned long table_pages,large_pages,anon_pt_max;
static struct {uintptr_t base,len;} anon_win[2]; static unsigned n_anon_win;
static void *owned[32]; static unsigned nowned;
static void panic(const char *s,...){fprintf(stderr,"panic: %s\n",s);exit(90);}
static unsigned long spin_lock_irqsave(int *p){assert(!*p);*p=1;return 0;}
static void spin_unlock_irqrestore(int *p,unsigned long f){(void)f;assert(*p);*p=0;}
static void *pmm_alloc(void){attempts++;if(deny)return NULL;void *p=aligned_alloc(PAGE_SIZE,PAGE_SIZE);assert(p);owned[nowned++]=p;allocations++;return p;}
static void *pmm_alloc_reserved(void){panic("unexpected reserved allocation");return NULL;}
static void pmm_unreserve(unsigned long n){(void)n;panic("unexpected unreserve");}
static int mb_quota_add(void *d,int c,unsigned long n){(void)d;(void)c;(void)n;panic("unexpected charge");return -1;}
static int mb_quota_sub(void *d,int c,unsigned long n){(void)d;(void)c;(void)n;panic("unexpected uncharge");return -1;}
static void *pmm_alloc_pages(unsigned n){assert(n==5);return block;}
static void pmm_free_pages(void *p,unsigned n){assert(p==block&&n==5);freed++;}
static int arch_pte_valid(pte_t e){return (e&1)!=0;}
static int arch_pte_is_table(pte_t e,int l){(void)l;return (e&32)!=0;}
static uintptr_t arch_pte_addr(pte_t e){return e&~4095UL;}
static unsigned arch_pte_flags(pte_t e){return (unsigned)e&30;}
static pte_t arch_pte_table(uintptr_t p,int u,int l){(void)u;(void)l;return p|33;}
static pte_t arch_pte_leaf(uintptr_t p,unsigned f,int l){(void)l;return p|f|1;}
static void arch_pte_set(pte_t *p,pte_t v){*p=v;}
static int vm_sw_flags_ok(unsigned f){(void)f;return 1;}
static void arch_tlb_flush(uintptr_t v){(void)v;flushes++;}
static void tlb_shootdown(uintptr_t v){(void)v;flushes++;}
static void arch_mmu_root_changed(pte_t *p){(void)p;}
'''
MAIN = r'''
int main(int argc,char **argv){
 assert(argc==2);int fail_split=atoi(argv[1]);
 block=aligned_alloc(2097152,2097152);assert(block);
 root=pmm_alloc();memset(root,0,PAGE_SIZE);
 assert(map_large((uintptr_t)block,(uintptr_t)block,VM_READ|VM_WRITE)==0);
 int before_attempts=attempts;
 deny=fail_split;char *st=stack_alloc();
 if(fail_split){
  assert(!st && freed==1 && attempts==before_attempts+1);
 }else{
  assert(st==block && freed==0);
  pte_t *e=walk(NULL,root,(uintptr_t)st,0,0,0,WALK_KERNEL,NULL);
  assert(e && !arch_pte_valid(*e));
  int before=attempts;deny=1;stack_release(st);
  assert(attempts==before && freed==1 && arch_pte_valid(*e));
  assert(arch_pte_addr(*e)==(uintptr_t)st);
  /* Repeat reuse/unmap/remap without allocating any intermediate table. */
  for(int i=0;i<100;i++){
   st=stack_alloc();assert(st==block && !arch_pte_valid(*e));
   stack_release(st);assert(arch_pte_valid(*e));
  }
  assert(attempts==before && freed==101);
 }
 printf("PASS depth=%d fail_split=%d allocation_attempts=%d frees=%d flushes=%d\n",MMU_LEVELS,fail_split,attempts,freed,flushes);
 for(unsigned i=0;i<nowned;i++)free(owned[i]);free(block);return 0;
}
'''


def main():
    paths = ('kernel/vm.c', 'kernel/sched.c')
    src = {p: (ROOT / p).read_text() for p in paths}
    names = ('va_index', 'table_alloc', 'split_large', 'table_in_anon',
             'anon_table_built', 'new_table', 'walk', 'flags_ok',
             'vm_map', 'map_large', 'vm_unmap')
    body = '\n'.join(function(src[paths[0]], n) for n in names)
    body += '\n' + '\n'.join(function(src[paths[1]], n)
                             for n in ('stack_alloc', 'stack_release'))
    out = Path(tempfile.mkdtemp(prefix='welyos-guard-restore-'))
    rows = []
    for depth in (2, 3):
        c = out / f'guard{depth}.c'
        c.write_text(f'#define MMU_LEVELS {depth}\n' + PRE + body + MAIN)
        exe = c.with_suffix('')
        cp = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                             '-Werror', '-Wno-misleading-indentation',
                             '-fsanitize=undefined', '-fno-sanitize-recover=all',
                             str(c), '-o', str(exe)], capture_output=True, text=True)
        if cp.returncode:
            raise RuntimeError(cp.stderr)
        for fail in (0, 1):
            r = subprocess.run([str(exe), str(fail)], capture_output=True,
                               text=True, timeout=10)
            rows.append(dict(depth=depth, fail_split=fail, rc=r.returncode,
                             stdout=r.stdout, stderr=r.stderr))
            print(r.stdout.strip(), r.stderr.strip())
    changed = [p for p, s in src.items() if (ROOT / p).read_text() != s]
    result = dict(source_hashes={p: hashlib.sha256(s.encode()).hexdigest()
                                 for p, s in src.items()},
                  changed_inputs=changed, cases=rows,
                  model='True helper bodies, modeled PTE/PMM/locks, no MMU or SMP')
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('artifacts:', out, 'changed_inputs:', changed)
    return int(bool(changed) or any(r['rc'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
