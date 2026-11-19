#!/usr/bin/env python3
"""Deterministic unmap-vs-fault and unmap-vs-usercopy probes; not hardware SMP."""
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
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,default=ROOT);a=ap.parse_args()
    groups={'kernel/vm.c':('flags_ok','user_range','vm_space_query','vm_space_map_new','vm_space_unmap'),
            'kernel/proc_mem.c':('commit_change_locked','pages_for','sys_munmap','sys_sbrk','proc_fault','copy_from_user','copy_to_user')}
    data={p:(a.source/p).read_bytes() for p in (*groups,'include/uapi/errno.h','include/vm.h')}
    out=Path(tempfile.mkdtemp(prefix='welyos-memory-races-'));parts=[]
    for path,names in groups.items():
        text=data[path].decode();lines=text.splitlines(keepends=True);fns={f.name:f for f in functions(path,text)}
        for name in names:
            fn=fns[name];parts.append(''.join(lines[fn.line-1:fn.end]))
    (out/'functions.c').write_text('\n'.join(parts))
    (out/'errno.h').write_bytes(data['include/uapi/errno.h'])
    (out/'vm_flags.h').write_text('\n'.join(re.findall(r'^#define VM_\w+\s+0x[0-9A-Fa-f]+u',data['include/vm.h'].decode(),re.M))+'\n')
    fixture=(ROOT/'tests/host/proc/memory_race_probe.c').read_bytes();(out/'probe.c').write_bytes(fixture)
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for i,name in enumerate(('fault-after-munmap','write-after-unmap-reuse','read-after-unmap-reuse','fault-after-shrink','normal-write','normal-read','normal-fault'),1):
        p=subprocess.run([str(out/'probe'),str(i)],capture_output=True,text=True,timeout=5)
        rows.append(dict(case=name,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name+': '+('PASS' if p.returncode==0 else 'FAIL')+' '+p.stdout.strip()+p.stderr[:200])
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed=[p for p,h in hashes.items() if hashlib.sha256((a.source/p).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps(dict(source_hashes=hashes,changed_inputs=changed,fixture_sha256=hashlib.sha256(fixture).hexdigest(),cases=rows,model='Real proc/VM entry functions; fixed PTE walk, allocator reuse, and scheduling hooks; no real TLB, hardware SMP or page-table tree'),indent=2)+'\n')
    print(f'{sum(r["returncode"]==0 for r in rows)}/{len(rows)} PASS; artifacts: {out}; changed_inputs={changed}')
    return 2 if changed else int(any(r['returncode'] for r in rows))

if __name__=='__main__':raise SystemExit(main())
