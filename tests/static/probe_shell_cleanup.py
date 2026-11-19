#!/usr/bin/env python3
"""摘取原始 shell run/job_wait，驗證保存 fd 及拒絕 SIGCONT 的清理。"""
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
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT)
    args=parser.parse_args()
    source=(args.source/'user/sh.c').read_text()
    hashes={'user/sh.c':hashlib.sha256(source.encode()).hexdigest()}
    selected={f.name:f for f in functions('user/sh.c',source)}
    lines=source.splitlines(keepends=True)
    out=Path(tempfile.mkdtemp(prefix='welyos-shell-cleanup-'))
    parts=[]
    for name in ('restore_fd','job_mark_done','job_wait','run'):
        if name=='restore_fd' and name not in selected:
            continue
        fn=selected[name];parts.append(''.join(lines[fn.line-1:fn.end]))
    (out/'shell_functions.c').write_text('\n'.join(parts))
    declarations=re.findall(r'^#define\s+(?:MAX_ARGS|MAX_STAGES|MAX_JOBS|SAVE_IN|SAVE_OUT|SAVE_SYNC)\b[^\n]*',source,re.M)
    for name in ('stage','job'):
        start=source.index('struct '+name+' {');end=source.index('};',start)+2
        declarations.append(source[start:end])
    (out/'shell_types.h').write_text('\n'.join(declarations)+'\n#include "uapi_fs.h"\n')
    for name in ('errno','proc','fs'):
        dest='errno.h' if name=='errno' else 'uapi_'+name+'.h'
        data=(args.source/f'include/uapi/{name}.h').read_bytes()
        hashes[f'include/uapi/{name}.h']=hashlib.sha256(data).hexdigest()
        (out/dest).write_bytes(data)
    (out/'probe.c').write_bytes((ROOT/'tests/host/shell/cleanup_probe.c').read_bytes())
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                    '-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    results=[]
    for kind in ('wait-normal','run-normal','resume-denied','resume-gone','wait-interrupted','wait-always-interrupted','wait-io-error','closed-stdin','closed-stdout',
                 'redirect-ok','redirect-sync-error','redirect-close-error','redirect-save-error','redirect-device','redirect-background'):
        p=subprocess.run([str(out/'probe'),kind],capture_output=True,text=True,timeout=3)
        row={'case':kind,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr};results.append(row)
        print(kind+': '+('PASS' if p.returncode==0 else p.stdout.strip() or p.stderr[:200]))
    changed=[name for name,h in hashes.items() if hashlib.sha256((args.source/name).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps({'source_hashes':hashes,'changed_inputs':changed,
                                              'results':results,'model':'deterministic fd/foreground environment'},indent=2)+'\n')
    print(f'Artifacts: {out}')
    return 2 if changed else int(any(r['returncode'] for r in results))


if __name__=='__main__':
    raise SystemExit(main())
