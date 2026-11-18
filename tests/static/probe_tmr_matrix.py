#!/usr/bin/env python3
"""以原始 tmr.h 執行 POSIX 故障矩陣，記錄問題並以 exit 1 表示未滿足契約。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.source.resolve()
    fixtures = Path(__file__).resolve().parents[1] / 'host/tmr'
    out = Path(tempfile.mkdtemp(prefix='welyos-tmr-matrix-'))
    source = (root / 'user/tmr.h').read_bytes()
    (out / 'tmr.h').write_bytes(source)
    (out / 'welyos_errno.h').write_bytes((root / 'include/uapi/errno.h').read_bytes())
    for name in ('libc.h', 'probe.c'):
        (out / name).write_bytes((fixtures / name).read_bytes())
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    cases = [('normal', 0)]
    for kind, count in [('fork', 4), ('pipe', 4), ('malloc', 3), ('dup2', 1),
                        ('child-read', 1), ('parent-read', 2), ('parent-write', 1),
                        ('short-write', 1), ('child-write', 1), ('waitpid', 4)]:
        cases.extend((kind, n) for n in range(1, count + 1))
    cases.extend((kind, 0) for kind in ('overflow', 'early-close', 'timer-drain'))
    results = []
    for kind, n in cases:
        proc = subprocess.Popen([str(out / 'probe'), kind, str(n)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, start_new_session=True)
        row = {'case': kind, 'at': n}
        try:
            stdout, stderr = proc.communicate(timeout=3)
            row.update(returncode=proc.returncode, stderr=stderr.decode(errors='replace'))
            if proc.returncode == 0:
                row.update(json.loads(stdout))
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            row['hang'] = True
        finally:
            # 包含主行程異常結束時留下的所有副本。
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        errors = []
        for key in ('hang', 'fd_delta', 'unreaped', 'running', 'ownership_bad', 'returncode', 'stderr'):
            if row.get(key):
                errors.append(key)
        if kind == 'parent-read' and row.get('invalid', [0])[0] == 0:
            errors.append('read error not marked invalid')
        if kind == 'overflow' and row.get('invalid') != [1, 1, 1]:
            errors.append('overflow not rejected')
        if kind in ('fork', 'pipe', 'malloc', 'parent-write', 'short-write') and row.get('rc') != -1:
            errors.append('setup failure reported success')
        if kind in ('dup2', 'child-read', 'child-write') and (row.get('rc'), row.get('agree')) != (0, 2):
            errors.append('single replica failure did not preserve majority')
        if kind in ('overflow', 'timer-drain') and row.get('rc') != -1:
            errors.append('invalid output won vote')
        if kind in ('early-close', 'timer-drain') and row.get('timed_out') != 1:
            errors.append('timer did not terminate stalled replicas')
        if kind == 'normal' and (row.get('rc'), row.get('agree')) != (0, 3):
            errors.append('normal vote failed')
        row['errors'] = errors
        results.append(row)
    report = {'tmr_sha256': hashlib.sha256(source).hexdigest(), 'results': results}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Artifacts: {out}')
    for row in results:
        print(f"{row['case']}#{row['at']}: " + (', '.join(row['errors']) or 'PASS'))
    return int(any(row['errors'] for row in results))


if __name__ == '__main__':
    raise SystemExit(main())
