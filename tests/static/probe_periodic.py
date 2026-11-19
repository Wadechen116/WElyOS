#!/usr/bin/env python3
"""抽取真實 RT 與 tick 函式，驗證算術／記帳並展示同級時間片的期限反例。"""
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.source
    source = (root / 'kernel/sched.c').read_text()
    selected = {f.name: f for f in functions('kernel/sched.c', source)}
    lines = source.splitlines(keepends=True)
    out = Path(tempfile.mkdtemp(prefix='welyos-periodic-'))
    parts = []
    names = ['update_prio']
    if 'rt_assign_levels' in selected:
        names.append('rt_assign_levels')
    names += ['rt_begin_period', 'rt_check_deadlines', 'rt_remove', 'thread_set_periodic', 'sched_tick']
    for name in names:
        fn = selected[name]
        parts.append(''.join(lines[fn.line - 1:fn.end]))
    (out / 'periodic.c').write_text('\n'.join(parts))
    constants = re.findall(r'^#define\s+(?:RT_MAX|RT_UTIL_MAX|TIME_SLICE_MS)\b[^\n]*', source, re.M)
    constants += re.findall(r'^#define\s+PRIO_\w+\b[^\n]*', (root / 'include/sched.h').read_text(), re.M)
    (out / 'constants.h').write_text('\n'.join(constants)+'\n')
    (out / 'errno.h').write_bytes((root / 'include/uapi/errno.h').read_bytes())
    (out / 'uapi_proc.h').write_bytes((root / 'include/uapi/proc.h').read_bytes())
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/sched/periodic_probe.c').read_bytes())
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=3)
    report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'returncode': p.returncode,
              'stdout': p.stdout, 'stderr': p.stderr, 'model': 'single CPU deterministic scheduler environment'}
    (out / 'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Artifacts: {out}')
    print(json.dumps(report, indent=2))
    return int(p.returncode != 0)


if __name__ == '__main__':
    raise SystemExit(main())
