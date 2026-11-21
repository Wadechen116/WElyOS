#!/usr/bin/env python3
"""Production new_table/table_alloc with fault-injecting host dependencies."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT=Path(__file__).resolve().parents[2]
FIXTURE=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define PAGE_SIZE 4096
#define MB_PGTABLE 0
typedef uint64_t pte_t;
struct mb_domain { unsigned long used,limit; };
struct vm_space { struct mb_domain *dom; unsigned long quota[1],pt_tickets; };
enum walk_use { WALK_KERNEL,WALK_LEGACY,WALK_ANON };
static unsigned long table_pages, reserved, ordinary_calls, reserved_calls;
static int fail_alloc;
static unsigned char page[PAGE_SIZE];
static jmp_buf trap;
static void panic(const char *s,...) { (void)s; longjmp(trap,1); }
static int mb_quota_add(struct mb_domain *d,int c,unsigned long n) {
    (void)c; if(d->used>d->limit || n>d->limit-d->used) return -1;
    d->used+=n;return 0;
}
static int mb_quota_sub(struct mb_domain *d,int c,unsigned long n) {
    (void)c;d->used-=n;return 0;
}
static void *pmm_alloc(void) { ordinary_calls++;return fail_alloc?NULL:page; }
static void *pmm_alloc_reserved(void) {
    reserved_calls++;if(!reserved) panic("no reservation");reserved--;return page;
}
CODE
int main(void) {
    unsigned count=0;
    for(int use=0;use<3;use++) for(int dom=0;dom<2;dom++)
    for(unsigned tickets=0;tickets<4;tickets++)
    for(unsigned used=0;used<4;used++) for(unsigned limit=0;limit<4;limit++)
    for(int fail=0;fail<2;fail++) {
        struct mb_domain d={used,limit};
        struct vm_space vs={.dom=dom?&d:NULL,.quota={used},.pt_tickets=tickets};
        table_pages=7;reserved=tickets;ordinary_calls=reserved_calls=0;fail_alloc=fail;
        memset(page,0xa5,sizeof page);
        if(use==WALK_ANON && tickets==0) {
            if(setjmp(trap)==0) {new_table(&vs,use);return 10;}
            if(d.used!=used || vs.pt_tickets || table_pages!=7 || reserved_calls) return 11;
        } else {
            pte_t *p=new_table(&vs,use);
            int success=use==WALK_ANON || (!fail && !(use==WALK_LEGACY && dom && used>=limit));
            if(!!p!=success) return 12;
            unsigned charge=success && use==WALK_LEGACY && dom;
            if(d.used!=used+charge || vs.quota[0]!=used+charge) return 13;
            if(vs.pt_tickets!=tickets-(use==WALK_ANON) || reserved!=vs.pt_tickets) return 14;
            if(reserved_calls!=(unsigned)(use==WALK_ANON) || table_pages!=7+(unsigned)success) return 15;
            if(use!=WALK_ANON && ordinary_calls!=(unsigned)!(use==WALK_LEGACY && dom && used>=limit)) return 16;
            if(success) for(unsigned i=0;i<PAGE_SIZE;i++) if(page[i]) return 17;
        }
        count++;
    }
    if(setjmp(trap)==0) {new_table(NULL,WALK_ANON);return 18;}
    puts("PASS 768 allocation/rollback/isolation combinations and NULL-ANON rejection");
    return count==768?0:19;
}
'''


def main():
    path=ROOT/'kernel/vm.c';src=path.read_text()
    code='\n'.join(function(src,n) for n in ('table_alloc','new_table'))
    out=Path(tempfile.mkdtemp(prefix='welyos-walk-budget-'));rows=[]
    variants={
        'none':code,
        'legacy-steals-ticket':code.replace('case WALK_LEGACY:', 'case WALK_LEGACY: if(vs && vs->pt_tickets) vs->pt_tickets--;'),
        'missing-rollback':code.replace('mb_quota_sub(vs->dom, MB_PGTABLE, 1);','(void)0;'),
        'anon-recharges':code.replace('vs->pt_tickets--;','vs->quota[MB_PGTABLE]++; vs->pt_tickets--;'),
    }
    for name,body in variants.items():
        c=out/(name+'.c');c.write_text(FIXTURE.replace('CODE',body));exe=c.with_suffix('')
        subprocess.run(['cc','-std=c11','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
        p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        ok=p.returncode==0 if name=='none' else p.returncode in range(10,20)
        rows.append(dict(case=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
        print(name,'PASS' if ok else 'FAIL',p.returncode,p.stdout.strip())
    changed=path.read_text()!=src
    (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
    print('artifacts:',out,'changed_input:',changed)
    return int(changed or not all(r['passed'] for r in rows))


if __name__=='__main__':raise SystemExit(main())
