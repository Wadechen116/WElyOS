#!/usr/bin/env python3
"""Verify regress refuses stale artifacts after configure/build failures; no real QEMU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--script',type=Path,default=ROOT/'scripts/regress.sh');a=ap.parse_args()
    original=a.script.read_bytes();out=Path(tempfile.mkdtemp(prefix='welyos-regress-build-'));rows=[]
    for case in ('ok','configure-error','build-error','no-elf','warning','filter-error','transient-exit5','wrong-exit'):
        root=out/case;(root/'scripts').mkdir(parents=True);(root/'bin').mkdir()
        (root/'scripts/regress.sh').write_bytes(original)
        (root/'build/riscv64').mkdir(parents=True)
        if case!='no-elf':(root/'build/riscv64/welyos.elf').write_text('stale artifact\n')
        (root/'scripts/qemu.sh').write_text('#!/bin/sh\nprintf "launched\\n" >> qemu-runs\n'+('if [ ! -f once ]; then touch once; exit 5; fi\n' if case=='transient-exit5' else 'exit 7\n' if case=='wrong-exit' else '')+'exit 0\n');(root/'scripts/qemu.sh').chmod(0o755)
        cmake='#!/bin/sh\ncase "$1" in\n --preset) '+('exit 9' if case=='configure-error' else 'exit 0')+' ;;\n --build) '+('echo "compile failed"; exit 7' if case=='build-error' else 'echo "warning: fixture warning"; exit 0' if case=='warning' else 'exit 0')+' ;;\nesac\n'
        (root/'bin/cmake').write_text(cmake);(root/'bin/cmake').chmod(0o755)
        # Local fake toolchain prevents any external emulator cleanup or installation.
        for name,body in {'bc':'cat >/dev/null; echo 0','taskkill.exe':'exit 0',**({'grep':'exit 2'} if case=='filter-error' else {})}.items():
            p=root/'bin'/name;p.write_text('#!/bin/sh\n'+body+'\n');p.chmod(0o755)
        env=dict(os.environ,PATH=str(root/'bin')+':'+os.environ['PATH'])
        p=subprocess.run(['bash',str(root/'scripts/regress.sh'),'-a','riscv64','-q'],env=env,capture_output=True,text=True,timeout=10)
        launches=len((root/'qemu-runs').read_text().splitlines()) if (root/'qemu-runs').exists() else 0
        badbuild=case in ('configure-error','build-error','no-elf')
        good=(p.returncode!=0 and not launches) if badbuild else (p.returncode!=0 and launches==4) if case=='transient-exit5' else (p.returncode!=0 and launches==3) if case=='wrong-exit' else (p.returncode==0 and launches==3)
        # grep failures must not hide a build error or turn a success into build failure.
        rows.append(dict(case=case,returncode=p.returncode,launches=launches,passed=good,stdout=p.stdout,stderr=p.stderr))
        print(f'{case}: '+('PASS' if good else f'FAIL rc={p.returncode} launches={launches}'))
    changed=hashlib.sha256(original).hexdigest()!=hashlib.sha256(a.script.read_bytes()).hexdigest()
    (out/'results.json').write_text(json.dumps(dict(script_sha256=hashlib.sha256(original).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
    print(f'{sum(r["passed"] for r in rows)}/{len(rows)} PASS; artifacts: {out}')
    return 2 if changed else int(not all(r['passed'] for r in rows))

if __name__=='__main__':raise SystemExit(main())
