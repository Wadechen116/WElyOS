#!/usr/bin/env python3
"""Isolated counterexamples for the step-47 architecture guard, not a C type proof."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
CASES=(
 ('owner','kernel/proc_signal.c','void f(void){ p->sig.stopped=1; }\n',0),
 ('foreign-common-name','kernel/proc.c','void f(void){ p->sig.stopped=1; }\n',1),
 ('foreign-other-name','kernel/proc.c','void f(void){ target->sig.stopped=1; }\n',1),
 ('foreign-whitespace','kernel/proc.c','void f(void){ p -> sig . stopped=1; }\n',1),
 ('foreign-group-address','kernel/proc.c','void f(void){ consume(&p->mem); }\n',1),
 ('foreign-header','kernel/foreign.h','static inline void f(void){ p->files.fd[0]=0; }\n',1),
 ('relative-include','kernel/syscall.c','#include "./proc_internal.h"\n',1),
 ('angle-include','kernel/syscall.c','#include <proc_internal.h>\n',1),
 ('line-comment','kernel/proc.c','// p->sig.stopped=1;\n',0),
 ('string-literal','kernel/proc.c','const char *s="p->sig.stopped";\n',0),
 ('block-comment','kernel/proc.c','/* p->sig.stopped=1; */\n',0),
)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--checker',type=Path,default=ROOT/'scripts/arch-check.sh');args=ap.parse_args()
    checker=args.checker.read_bytes();out=Path(tempfile.mkdtemp(prefix='welyos-arch-boundaries-'));rows=[]
    for name,path,source,want in CASES:
        root=out/name;(root/'scripts').mkdir(parents=True)
        (root/'scripts/arch-check.sh').write_bytes(checker)
        f=root/path;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(source)
        p=subprocess.run(['bash',str(root/'scripts/arch-check.sh')],capture_output=True,text=True,timeout=5)
        row=dict(case=name,path=path,source=source,expected=want,actual=p.returncode,stdout=p.stdout,stderr=p.stderr)
        rows.append(row);print(name+': '+('PASS' if want==p.returncode else f'FAIL expected={want} actual={p.returncode}'))
    changed=hashlib.sha256(args.checker.read_bytes()).digest()!=hashlib.sha256(checker).digest()
    (out/'results.json').write_text(json.dumps(dict(checker_sha256=hashlib.sha256(checker).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
    print(f'{sum(r["actual"]==r["expected"] for r in rows)}/{len(rows)} PASS; artifacts: {out}')
    return 2 if changed else int(any(r['actual']!=r['expected'] for r in rows))

if __name__=='__main__':raise SystemExit(main())
