#!/usr/bin/env python3
"""Check that actual benchmark timing rejects failed/backward clocks instead of printing bogus numbers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/static'))
from rules import functions

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,default=ROOT)
    a=ap.parse_args();p=a.source/'user/bench.c';data=p.read_bytes();src=data.decode()
    fs={f.name:f for f in functions('user/bench.c',src)};lines=src.splitlines(keepends=True)
    out=Path(tempfile.mkdtemp(prefix='welyos-bench-clock-'))
    selected=['udiv64','now']
    if 'since' in fs:
        selected.append('since')
    selected += ['to_ns','sort_u64','run']
    globals_decl='static int failed, clock_failed;\n' if 'clock_failed' in src else 'static int failed;\n'
    if 'close_err' in '\n'.join(lines[fs['run'].line-1:fs['run'].end]):
        globals_decl += 'static int close_err;\n'
    (out/'functions.c').write_text(globals_decl+'\n'.join(''.join(lines[fs[n].line-1:fs[n].end]) for n in selected))
    (out/'probe.c').write_bytes((ROOT/'tests/host/bench/clock_probe.c').read_bytes())
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for name,case in [('normal',0),('start-error',1),('end-error',2),('backward',3)]:
        r=subprocess.run([str(out/'probe'),str(case)],capture_output=True,text=True,timeout=5)
        rows.append(dict(case=name,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr));print(name,r.stdout.strip(),r.stderr[:500])
    changed=p.read_bytes()!=data
    (out/'results.json').write_text(json.dumps(dict(results=rows,source_sha256=hashlib.sha256(data).hexdigest(),changed_inputs=changed),indent=2))
    print('artifacts:',out);return 2 if changed else int(any(r['returncode'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
