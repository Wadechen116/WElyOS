#!/usr/bin/env python3
"""Current commit/release/fault boundaries: deterministic mutex/copy probes.

Extracts real proc/VM bodies. Bundle counters, PMM and a one-PTE walk are
fixtures: this validates interleaving exclusion, not real reservation backing,
complete ticket accounting, hardware SMP or TLB execution.

--mutate 用來做負對照:把修正從抽出來的真函式裡拿掉,對應的案例必須失敗。
  none               目前的程式(全部應該 PASS)
  no-memmx           拿掉 mem.mx(fault 與 munmap/sbrk 可以交錯):案例 1、4 必須 FAIL
  copy-outside-lock  vm_space_copy 先放開 vm_lk 再複製(舊的做法):案例 2、3 必須 FAIL
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'scripts/static'))
from rules import functions

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,default=ROOT);ap.add_argument('--mutate',choices=('none','no-memmx','copy-outside-lock'),default='none');a=ap.parse_args()
    groups={'include/vm.h':('vm_sw_flags_ok',),'kernel/vm.c':('flags_ok','user_range','vm_space_copy','vm_space_map_new','vm_space_unmap','vm_space_commit_anon','vm_space_fault_anon','vm_space_release_anon'),
            'kernel/proc_mem.c':('pages_for','sys_munmap','sys_sbrk','proc_fault_locked','proc_fault','copy_user','copy_from_user','copy_to_user')}
    data={p:(a.source/p).read_bytes() for p in (*groups,'include/uapi/errno.h','include/vm.h')}
    out=Path(tempfile.mkdtemp(prefix='welyos-memory-races-current-'));parts=[]
    for path,names in groups.items():
        text=data[path].decode();lines=text.splitlines(keepends=True);fns={f.name:f for f in functions(path,text)}
        for name in names:
            fn=fns[name];parts.append(('/*@'+name+'*/\n')+''.join(lines[fn.line-1:fn.end]))
    src='\n'.join(parts)
    def sub(old,new,count):
        nonlocal src
        if src.count(old)!=count: raise SystemExit(f'mutation anchor {old!r}: expected {count}, found {src.count(old)}')
        src=src.replace(old,new)
    if a.mutate=='no-memmx':
        sub('mutex_lock(&p->mem.mx);',';',3);sub('mutex_unlock(&p->mem.mx);',';',4)
    elif a.mutate=='copy-outside-lock':
        sub('(va & (PAGE_SIZE - 1)));','(va & (PAGE_SIZE - 1)));\n            spin_unlock_irqrestore(&vm_lk, irq); irq = 12345;',1)
        head,_,rest=src.partition('/*@vm_space_copy*/');body,sep,tail=rest.partition('/*@')
        old='    spin_unlock_irqrestore(&vm_lk, irq);\n    return rc;\n}'
        if body.count(old)!=1: raise SystemExit('mutation anchor in vm_space_copy not found exactly once')
        src=head+'/*@vm_space_copy*/'+body.replace(old,'    if (irq != 12345) spin_unlock_irqrestore(&vm_lk, irq);\n    return rc;\n}')+sep+tail
    (out/'functions.c').write_text(src)
    (out/'errno.h').write_bytes(data['include/uapi/errno.h'])
    (out/'vm_flags.h').write_text('\n'.join(re.findall(r'^#define VM_\w+\s+0x[0-9A-Fa-f]+u',data['include/vm.h'].decode(),re.M))+'\n')
    fixture=(ROOT/'tests/host/proc/memory_race_current_probe.c').read_bytes();(out/'probe.c').write_bytes(fixture)
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for i,name in enumerate(('fault-after-munmap','write-after-unmap-reuse','read-after-unmap-reuse','fault-after-shrink','normal-write','normal-read','normal-fault','fault-shrink-regrow','double-fault-same-page'),1):
        p=subprocess.run([str(out/'probe'),str(i)],capture_output=True,text=True,timeout=5)
        rows.append(dict(case=name,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name+': '+('PASS' if p.returncode==0 else 'FAIL')+' '+p.stdout.strip()+p.stderr[:200])
    expected={'none':set(),'no-memmx':{0,3},'copy-outside-lock':{1,2}}[a.mutate]
    observed={i for i,r in enumerate(rows) if r['returncode']}
    mutation_oracle_ok=observed==expected
    if a.mutate=='no-memmx':
        mutation_oracle_ok &= all(rows[i]['returncode']==10 and 'without a ticket' in rows[i]['stderr'] for i in expected)
    elif a.mutate=='copy-outside-lock':
        mutation_oracle_ok &= all(rows[i]['returncode']==1 and '"safe":0' in rows[i]['stdout'] for i in expected)
    sanitizer_clean=not any('AddressSanitizer' in r['stderr'] or 'runtime error:' in r['stderr'] for r in rows)
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed=[p for p,h in hashes.items() if hashlib.sha256((a.source/p).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps(dict(source_hashes=hashes,changed_inputs=changed,fixture_sha256=hashlib.sha256(fixture).hexdigest(),mutate=a.mutate,cases=rows,mutation_oracle_ok=mutation_oracle_ok,sanitizer_clean=sanitizer_clean,model='Real current proc and VM copy/map/unmap/commit/fault/release bodies; fixed PTE walk, bundle quota/reservation and PMM fixtures, scheduling hooks before VM lock; does not validate real ticket backing, full ledger, TLB, hardware SMP or page-table tree'),indent=2)+'\n')
    print(f'{sum(r["returncode"]==0 for r in rows)}/{len(rows)} PASS; artifacts: {out}; changed_inputs={changed}')
    print(f'mutation_oracle_ok={mutation_oracle_ok}; sanitizer_clean={sanitizer_clean}')
    return 2 if changed or not mutation_oracle_ok or not sanitizer_clean else int(any(r['returncode'] for r in rows))

if __name__=='__main__':raise SystemExit(main())
