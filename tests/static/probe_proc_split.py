#!/usr/bin/env python3
"""Run extracted step-47 module seams against bounded host state/lock models."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'scripts/static'))
from rules import functions


def main():
    groups={
        'kernel/proc_mem.c':('commit_change_locked','commit_release_all','sys_munmap','pmem_init','pmem_exec','pmem_fork'),
        'kernel/proc_signal.c':('psig_fork','psig_exec','psig_stopped','psig_stop_unreported','psig_report_stop'),
        'kernel/proc_fs.c':('fd_in_range','fd_replace','files_init_cwd'),
    }
    paths=(*groups,'include/uapi/errno.h','tests/host/proc/split_probe.c')
    data={p:(ROOT/p).read_bytes() for p in paths}
    out=Path(tempfile.mkdtemp(prefix='welyos-proc-split-'))
    parts=[]
    for path,names in groups.items():
        text=data[path].decode();lines=text.splitlines(keepends=True)
        fns={f.name:f for f in functions(path,text)}
        for name in names:
            f=fns[name];parts.append(''.join(lines[f.line-1:f.end]))
    (out/'functions.c').write_text('\n'.join(parts))
    (out/'errno.h').write_bytes(data['include/uapi/errno.h'])
    (out/'probe.c').write_bytes(data['tests/host/proc/split_probe.c'])
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    p=subprocess.run([str(out/'probe')],capture_output=True,text=True,timeout=5)
    hashes={path:hashlib.sha256(b).hexdigest() for path,b in data.items()}
    changed=[path for path,h in hashes.items() if hashlib.sha256((ROOT/path).read_bytes()).hexdigest()!=h]
    result=dict(source_hashes=hashes,changed_inputs=changed,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr,
                model='Extracted functions and bounded host structs/lock stubs; no real VM, scheduler, SMP, or sys_fork lifecycle.')
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(p.stdout,end='');print(p.stderr,end='');print(f'Artifacts: {out}; changed_inputs={changed}')
    return 2 if changed else int(p.returncode!=0)


if __name__=='__main__':raise SystemExit(main())
