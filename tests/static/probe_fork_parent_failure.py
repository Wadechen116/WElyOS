#!/usr/bin/env python3
"""Reproduce parent PTE mutation before child walk failure in production fork_table."""
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
#define PAGE_SIZE 4096UL
#define MMU_BITS 9
#define ENTRIES 512
#define VM_WRITE 2
#define VM_SHARED 32
#define VM_COW 64
#define VM_TICKET 128
#define WALK_LEGACY 1
typedef uint64_t pte_t;
struct vm_range {uintptr_t start,len;};
struct vm_space {pte_t *root;unsigned long pages;};
struct fork_anon {const struct vm_range *r;unsigned n;unsigned long marked;};
static int arch_pte_valid(pte_t p){return p!=0;}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static unsigned arch_pte_flags(pte_t p){return p&4095;}
static pte_t arch_pte_leaf(uintptr_t pa,unsigned f,int l){(void)l;return pa|f;}
static void arch_pte_set(pte_t *p,pte_t v){*p=v;}
static void pmm_ref(void *p){(void)p;}
static pte_t *walk(struct vm_space *v,pte_t *r,uintptr_t a,int t,int c,int s,int u,int *o)
{(void)v;(void)r;(void)a;(void)t;(void)c;(void)s;(void)u;(void)o;return NULL;}
CODE
int main(void){
    pte_t parent[1]={0x1000|VM_WRITE},original=parent[0];
    struct vm_space child={0};struct vm_range range={0,4096};
    struct fork_anon fa={&range,1,0};
    int rc=fork_table(&child,parent,0,0,0,0,&fa);
    if(rc!=-1) return 2;
    if(parent[0]!=original && fa.marked==1){
        puts("REPRODUCED: child table allocation failed; parent WRITE became COW|TICKET, marked=1");return 0;
    }
    puts("Parent unchanged: known counterexample no longer reproduced; review updated implementation");return 1;
}
'''
if __name__=='__main__':
    src=(ROOT/'kernel/vm.c').read_text();out=Path(tempfile.mkdtemp(prefix='welyos-fork-parent-failure-'))
    code='\n'.join(function(src,n) for n in ('in_anon','fork_table'))
    c=out/'probe.c';c.write_text(FIXTURE.replace('CODE',code));exe=out/'probe'
    subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    changed=(ROOT/'kernel/vm.c').read_text()!=src
    (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,exit=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n')
    print(p.stdout.strip());print('artifacts:',out,'changed_input:',changed)
    raise SystemExit(p.returncode or int(changed))
