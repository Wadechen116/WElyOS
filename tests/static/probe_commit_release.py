#!/usr/bin/env python3
"""記憶體承諾的歸還永遠成功:摘取真的 commit_change_locked,窮舉 delta <= 0 的情況(UBSan)。"""
import argparse
from pathlib import Path
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
    a = ap.parse_args()
    src = (a.source / 'kernel/proc_mem.c').read_text()
    fn = {f.name: f for f in functions('kernel/proc_mem.c', src)}['commit_change_locked']
    lines = src.splitlines(keepends=True)
    decl = 'static unsigned long commit_total;\nstatic unsigned long commit_cap;\n'
    out = Path(tempfile.mkdtemp(prefix='welyos-commit-'))
    (out / 'function.c').write_text(decl + ''.join(lines[fn.line - 1:fn.end]))
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/proc/commit_release_probe.c').read_bytes())
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined,address',
                    '-fno-sanitize-recover=all', '-I', str(out), str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=10)
    print(p.stdout.strip(), p.stderr.strip()[:300])
    print('PASS' if p.returncode == 0 else 'FAIL', f'artifacts: {out}')
    return p.returncode


if __name__ == '__main__':
    raise SystemExit(main())
