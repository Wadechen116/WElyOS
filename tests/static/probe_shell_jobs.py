#!/usr/bin/env python3
"""True shell lifecycle with identity-aware fd and child/wait model (I-318)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/static'))
from rules import functions

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,default=ROOT)
    ap.add_argument('--mutate',choices=('none','early-close','ignore-sync','borrow-path','lose-reaped-status'),default='none')
    a=ap.parse_args();root=a.source.resolve();data=(root/'user/sh.c').read_bytes();src=data.decode()
    fs={f.name:f for f in functions('user/sh.c',src)};lines=src.splitlines(keepends=True)
    names=('restore_fd','job_mark_done','job_of','job_finish','job_wait','job_free_slot','jobs_reap','job_wait_all','sh_exit_status','run')
    missing=[n for n in names if n not in fs]
    if missing:print('FAIL missing lifecycle implementation:',missing);return 1
    body='\n'.join(''.join(lines[fs[n].line-1:fs[n].end]) for n in names)
    original_body=body
    if a.mutate=='early-close':body=body.replace('        *job = cur;', '        close_quiet(cur.save_fd); cur.save_pending = 0;\n        *job = cur;',1)
    elif a.mutate=='ignore-sync':body=body.replace('if (synced != 0 || closed != 0)', 'if (0 && (synced != 0 || closed != 0))')
    elif a.mutate=='borrow-path':body=body.replace('cur.save_path[k] = keep_path[k]', 'cur.save_path[k] = 0')
    elif a.mutate=='lose-reaped-status':body=body.replace('if (j->status) async_status = j->status;', 'if (j->status) async_status = 0;')
    if a.mutate!='none' and body==original_body:
        print('FAIL mutation did not match source');return 2
    out=Path(tempfile.mkdtemp(prefix='welyos-shell-jobs-'))
    (out/'functions.c').write_text(body)
    types=re.findall(r'^#define\s+(?:MAX_ARGS|MAX_STAGES|MAX_JOBS|SAVE_IN|SAVE_OUT|SAVE_SYNC)\b[^\n]*',src,re.M)
    for name in ('stage','job'):
        start=src.index('struct '+name+' {');types.append(src[start:src.index('};',start)+2])
    (out/'types.h').write_text('\n'.join(types))
    for name in ('fs','proc','errno'):(out/(name+'.h')).write_bytes((root/'include/uapi'/ (name+'.h')).read_bytes())
    (out/'probe.c').write_bytes((ROOT/'tests/host/shell/jobs_probe.c').read_bytes())
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for case in ('background','reap-error','close-error','two-jobs','stop-resume','wait-error','spawn-error','full-jobs','full-fds','path-lifetime','exit-pending','stopped-wait','pipe-fd-collision'):
        p=subprocess.run([str(out/'probe'),case],capture_output=True,text=True,timeout=5)
        rows.append(dict(case=case,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr));print(case,p.returncode,p.stdout.strip(),p.stderr[:500])
    changed=(root/'user/sh.c').read_bytes()!=data
    (out/'results.json').write_text(json.dumps(dict(results=rows,mutation=a.mutate,source_sha256=hashlib.sha256(data).hexdigest(),changed_inputs=changed,
        model='real run/job lifecycle with deterministic fd identity and child completion; no real fork/kernel/console'),indent=2))
    print('artifacts:',out);return 2 if changed else int(any(r['returncode'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
