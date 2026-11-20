#!/usr/bin/env python3
"""摘取 shell 管線控制流程，逐一注入描述符操作失敗。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts/static'))
from rules import functions


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT)
    args = ap.parse_args()
    root = args.source.resolve()
    out = Path(tempfile.mkdtemp(prefix='welyos-shell-matrix-'))
    paths = ('user/sh.c', 'include/uapi/fs.h', 'include/uapi/errno.h', 'include/uapi/proc.h')
    data = {p: (root / p).read_bytes() for p in paths}
    source = data['user/sh.c'].decode()
    selected = {f.name: f for f in functions('user/sh.c', source)}
    lines = source.splitlines(keepends=True)
    names = tuple(n for n in ('restore_fd', 'job_mark_done', 'job_finish', 'job_wait', 'run') if n in selected)
    (out / 'shell_functions.c').write_text('\n'.join(''.join(lines[selected[n].line-1:selected[n].end]) for n in names))
    types = re.findall(r'^#define\s+(?:MAX_ARGS|MAX_STAGES|MAX_JOBS|SAVE_IN|SAVE_OUT|SAVE_SYNC)\b[^\n]*', source, re.M)
    for name in ('stage', 'job'):
        start = source.index('struct '+name+' {')
        types.append(source[start:source.index('};', start)+2])
    types.append('#define SH_DYNAMIC_OUTPUT '+str(int('save_pending' in source)))
    (out / 'shell_types.h').write_text('\n'.join(types))
    for p, content in data.items():
        if p != 'user/sh.c':
            (out / Path(p).name).write_bytes(content)
    fixture = (ROOT / 'tests/host/shell/matrix_probe.c').read_bytes()
    (out / 'probe.c').write_bytes(fixture)
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    results = []

    def run(ns, redir, op='none', nth=0, forced_open=0):
        argv = [str(out / 'probe'), str(ns), str(redir), op, str(nth), str(forced_open)]
        p = subprocess.run(argv, capture_output=True, text=True, timeout=4)
        row = dict(stages=ns, redirects=redir, operation=op, occurrence=nth, forced_open=forced_open,
                   returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)
        results.append(row)
        if p.returncode:
            print('FAIL', ns, redir, op, nth, forced_open, p.stdout.strip(), p.stderr[:300])
        return json.loads(p.stdout) if p.stdout else {}

    for ns in (1, 2, 3):
        for redir in (0, 1, 2, 3):
            base = run(ns, redir)
            for op, count in base.get('counts', {}).items():
                for nth in range(1, count+1):
                    run(ns, redir, op, nth)
            # 輸出 open 失敗才會執行的提早 restore，需雙重注入才能覆蓋。
            if redir & 2:
                open_index = 2 if redir & 1 else 1
                failed = run(ns, redir, 'none', 0, open_index)
                for nth in range(1, failed.get('counts', {}).get('dup2', 0)+1):
                    run(ns, redir, 'dup2', nth, open_index)
    hashes = {p: hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed = [p for p,h in hashes.items() if hashlib.sha256((root/p).read_bytes()).hexdigest() != h]
    failures = sum(r['returncode'] != 0 for r in results)
    (out / 'results.json').write_text(json.dumps(dict(source_hashes=hashes, changed_inputs=changed,
        fixture_sha256=hashlib.sha256(fixture).hexdigest(), results=results,
        model='deterministic descriptor identity and process/wait bookkeeping; no real child execution'), indent=2)+'\n')
    print(f'{len(results)-failures}/{len(results)} PASS; changed_inputs={changed}; artifacts: {out}')
    return 2 if changed else int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
