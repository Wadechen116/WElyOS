#!/usr/bin/env python3
"""Host checks of extracted, unchanged production PTE codecs (no MMU execution)."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(src, name):
    match = re.search(r'^(?:static inline )?[\w *]+\b' + name + r'\([^;]*?\)\s*(?:/\*.*?\*/\s*)?\{', src, re.M)
    if not match:
        raise ValueError(name)
    end = match.end()
    depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[match.start():end]


HARNESS = r'''
static jmp_buf trap;
static void panic(const char *fmt, ...) { (void)fmt; longjmp(trap, 1); }
CODECS
int main(void) {
    unsigned count=0;
    const unsigned states[]={0,0x20,0x40,0xc0};
    const uintptr_t addresses[]={0,0x1000,0x7ffff000,0xfffff000};
    for(unsigned mode=0;mode<MODES;mode++) {
        SETMODE
        for(unsigned level=0;level<2;level++)
        for(unsigned ai=0;ai<4;ai++)
        for(unsigned perm=0;perm<16;perm++)
        for(unsigned si=0;si<4;si++) {
            unsigned flags=VM_READ | ((perm&1)?VM_WRITE:0) |
                ((perm&2)?VM_EXEC:0) | ((perm&4)?VM_USER:0) |
                ((perm&8)?VM_DEVICE:0) | states[si];
            uintptr_t pa=addresses[ai] & ~(level?0x1fffffUL:0xfffUL);
            unsigned expected=flags;
            NORMALIZE
            pte_t e=arch_pte_leaf(pa,flags,level);
            if(arch_pte_addr(e)!=pa || arch_pte_flags(e)!=expected) return 10;
            /* Independent raw-bit oracle; round-trip alone can hide paired bugs. */
            unsigned raw=((e&PTE_SW_SHARED)?1u:0u)|((e&PTE_SW_COW)?2u:0u);
            if(raw!=si || !vm_sw_flags_ok(flags)) return 11;
            count++;
        }
        for(unsigned sw=0;sw<8;sw++) {
            unsigned flags=sw<<5;
            int valid=(sw==0 || sw==1 || sw==2 || sw==6);
            if(!!vm_sw_flags_ok(flags)!=valid) return 12;
            if(!valid) {
                if(setjmp(trap)==0) { arch_pte_leaf(0x1000,VM_READ|flags,0); return 13; }
            }
        }
    }
    printf("PASS %u roundtrips plus illegal-state panic checks\n",count);
    return 0;
}
'''


def main():
    paths = ['include/vm.h'] + [f'arch/{a}/mmu.c' for a in ('riscv64','aarch64','i386')]
    inputs = {p:(ROOT/p).read_text() for p in paths}
    out = Path(tempfile.mkdtemp(prefix='welyos-pte-ticket-'))
    header = inputs['include/vm.h']
    vmdefs = '\n'.join(re.findall(r'^#define VM_\w+\s+0x[0-9a-fA-F]+u',header,re.M))
    swcode = '\n'.join(function(header,n) for n in ('vm_sw_flags_ok','vm_sw_encode','vm_sw_decode'))
    rows=[]
    for arch in ('riscv64','aarch64','i386'):
        src=inputs[f'arch/{arch}/mmu.c']
        defs='\n'.join(l.split('/*')[0] for l in src.splitlines() if l.startswith('#define '))
        funcs='\n'.join(function(src,n) for n in ('arch_pte_leaf','arch_pte_addr','arch_pte_flags'))
        harness=HARNESS.replace('CODECS',funcs).replace('MODES','4' if arch=='i386' else '1')
        harness=harness.replace('SETMODE','has_nx=mode&1; has_pge=mode>>1;' if arch=='i386' else '')
        norm=''
        if arch!='aarch64': norm+='expected &= ~VM_DEVICE;'
        if arch=='i386': norm+='if(!has_nx) expected |= VM_EXEC;'
        harness=harness.replace('NORMALIZE',norm)
        common='#include <stdint.h>\n#include <stdio.h>\n#include <setjmp.h>\ntypedef uint64_t pte_t;\n'
        common+=vmdefs+'\n'+defs+'\n'
        if arch=='i386': common+='static int has_nx, has_pge;\n'
        for mutation in ('none','decode-shared','drop-ticket','skip-validation'):
            sw=swcode
            h=harness
            if mutation=='decode-shared': sw=sw.replace('VM_COW | VM_TICKET };','VM_SHARED | VM_COW };')
            if mutation=='drop-ticket': sw=sw.replace('? 3 : 2','? 2 : 2')
            if mutation=='skip-validation': h=h.replace('if (!vm_sw_flags_ok(flags))','if (0)')
            code=common+sw+'\n'+h
            file=out/f'{arch}-{mutation}.c';file.write_text(code)
            exe=file.with_suffix('')
            subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all',str(file),'-o',str(exe)],check=True,capture_output=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
            ok=(run.returncode==0) if mutation=='none' else (run.returncode in (10,11,12,13))
            rows.append(dict(arch=arch,mutation=mutation,passed=ok,exit=run.returncode,stdout=run.stdout,stderr=run.stderr))
            print(arch,mutation,'PASS' if ok else 'FAIL',run.returncode,run.stdout.strip())
    changed=[p for p,s in inputs.items() if (ROOT/p).read_text()!=s]
    result=dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in inputs.items()},changed_inputs=changed,cases=rows)
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('artifacts:',out,'changed_inputs:',changed)
    return int(bool(changed) or not all(r['passed'] for r in rows))


if __name__=='__main__':
    raise SystemExit(main())
