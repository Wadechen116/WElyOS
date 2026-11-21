#!/usr/bin/env python3
"""Finite host proof checks for production table counting and ticket cap algebra."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT=Path(__file__).resolve().parents[2]
HARNESS=r'''
#include <stdint.h>
#include <stdio.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define MMU_BITS 9
#define MMU_LEVELS 3
CODE
int main(void) {
    unsigned long cases=0;
    const uintptr_t spans[]={1UL<<21,1UL<<30};
    for(unsigned l=0;l<2;l++) {
        uintptr_t span=spans[l];
        for(unsigned s=0;s<12;s++) for(unsigned n=1;n<=1100;n++) {
            uintptr_t start=(span*(s/4))+(s%4==0?0:s%4==1?4096:s%4==2?span-4096:span-8192);
            uintptr_t end=start+n*4096UL;
            unsigned long oracle=0;
            for(uintptr_t b=start/span;b<=(end-1)/span;b++) oracle++;
            if(tables_at(start,end,l)!=oracle) return 10;
            cases++;
        }
    }
    for(unsigned arch=0;arch<2;arch++) {
        uintptr_t base=arch?0x100000000UL:0x40000000UL;
        unsigned long m=0;
        for(unsigned l=0;l<2;l++) {
            uintptr_t span=spans[l];
            for(uintptr_t b=(base+0x10000000UL)/span;b<=(base+0x2effffffUL)/span;b++) m++;
        }
        /* Actual configured contiguous heap+mmap windows: count union. */
        unsigned long calculated=0;
        for(unsigned l=0;l<2;l++) {
            calculated+=tables_at(base+0x10000000UL,base+0x20000000UL,l);
            calculated+=tables_at(base+0x20000000UL,base+0x2f000000UL,l);
            if(((base+0x1fffffffUL)/(spans[l]))==((base+0x20000000UL)/spans[l]))calculated--;
        }
        if(m!=249 || calculated!=m) return 11;
        for(unsigned p=0;p<126976;p+=127) {
            uintptr_t start=base+0x10000000UL+p*4096UL;
            for(unsigned n=1;n<1024;n+=17) {
                unsigned long oracle=0;
                for(unsigned l=0;l<2;l++)
                    for(uintptr_t b=start/spans[l];b<=(start+n*4096UL-1)/spans[l];b++)oracle++;
                if(pt_need(start,n*4096UL)!=oracle) return 12;
                cases++;
            }
        }
    }
    printf("PASS %lu real-function counting cases; M=249\n",cases);
    return 0;
}
'''


def main():
    src=(ROOT/'kernel/vm.c').read_text();out=Path(tempfile.mkdtemp(prefix='welyos-anon-pt-bound-'))
    body='\n'.join(function(src,n) for n in ('tables_at','pt_need'))
    rows=[]
    for name,code in [('none',body),('end-off-by-one',body.replace('(end - 1) >> sh','end >> sh')),('omit-upper-table',body.replace('l < MMU_LEVELS - 1','l < MMU_LEVELS - 2'))]:
        c=out/(name+'.c');c.write_text(HARNESS.replace('CODE',code));exe=c.with_suffix('')
        subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
        p=subprocess.run([str(exe)],text=True,capture_output=True,timeout=20)
        ok=p.returncode==0 if name=='none' else p.returncode in (10,11,12)
        rows.append(dict(case=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name,ok,p.stdout.strip())
    # D = distinct unbuilt tables needed by old commitments; k = new demand.
    # D<=T and D+k<=M-E. Enumerate all E/T/D and maximal admissible k.
    count=0
    for m in (1,2,3,7,249):
        for e in range(m+1):
            for t in range(m-e+1):
                for d in range(t+1):
                    k=m-e-d
                    for need in {0,k,k+1,m+1}:
                        added=min(need,m-e-t)
                        assert d+min(k,need)<=t+added
                        assert e+t+added<=m
                        # A legacy table removes at most one outstanding demand.
                        if e<m:
                            after=t+added-int(e+1+t+added>m)
                            assert e+1+after<=m
                            assert max(0,d+min(k,need)-1)<=after
                        count+=1
    print('PASS',count,'cap/legacy algebra states')
    changed=(ROOT/'kernel/vm.c').read_text()!=src
    (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,count=count,cases=rows),indent=2)+'\n')
    print('artifacts:',out,'changed_input:',changed)
    return int(changed or not all(r['passed'] for r in rows))


if __name__=='__main__':raise SystemExit(main())
