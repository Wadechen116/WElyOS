#!/usr/bin/env python3
"""Host checks of unchanged current sys_fork and split pmem fork helpers.

Reuses the older probe's scheduler/PMM/heap/PTE fixtures, not its old extraction
or its counterexample oracle. Does not execute architecture MMUs or real SMP.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
import probe_tempobj_fork_late_failure as fixture
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
PATHS = ['kernel/proc.c', 'kernel/proc_mem.c', 'kernel/vm.c', 'kernel/membudget.c']


def build(src):
    p, pm, v, m = [src[x] for x in PATHS]
    body = fixture.PRE + '\n'
    body += '\n'.join(function(m, n) for n in [
        'mb_domain_new', 'mb_domain_get', 'mb_domain_put', 'fits_locked',
        'mb_quota_add', 'sub_locked', 'mb_quota_sub', 'bundle_sum',
        'mb_commit_bundle', 'mb_release_bundle']) + '\n'
    body += '''static struct mb_domain *pmem_domain_new(const struct proc *p)
{(void)p;return mb_domain_new(1000);}
static void thread_abandon(struct thread *t){kfree(t);}
'''
    body += '\n'.join(function(v, n) for n in [
        'vm_space_create', 'free_table', 'vm_space_destroy', 'in_anon',
        'fork_table', 'vm_space_fork']) + '\n'
    body += '\n'.join(function(pm, n) for n in [
        'proc_kobj_pages', 'pmem_domain', 'pmem_release',
        'pmem_fork_domain', 'pmem_fork_space']) + '\n'
    body += '\n'.join(function(p, n) for n in ['fork_child_main', 'sys_fork']) + '\n'
    assert 'trial(n,1,negative)' in fixture.MAIN and 'early<=4' in fixture.MAIN
    harness = fixture.MAIN.replace('trial(n,1,negative)', 'trial(n,1,1)')
    harness = harness.replace('early<=4', 'early<=7')
    harness = harness.replace('late failure, 3 early allocation failures, child-entry/dying/killed controls checked',
                              '28 failure cases + 12 child-entry/dying/killed lifecycle cases')
    return body + harness


def main():
    src = {p: (ROOT / p).read_text() for p in PATHS}
    fixture_paths = ['tests/static/probe_tempobj_fork_late_failure.py',
                     'tests/static/probe_pte_ticket.py']
    fixture_src = {p: (ROOT / p).read_text() for p in fixture_paths}
    body = build(src)
    assert body.count('thread_abandon(ct);') == 1
    variants = {'none': (body, 0), 'omit-thread-abandon': (
        body.replace('thread_abandon(ct);', '/* omitted abandon */'), 10)}
    out = Path(tempfile.mkdtemp(prefix='welyos-fork-current-'))
    rows = []
    for name, (code, expected) in variants.items():
        c = out / (name + '.c')
        c.write_text(code)
        exe = c.with_suffix('')
        cc = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall',
                             '-Wno-unused-variable', '-Wno-unused-function',
                             '-Wno-misleading-indentation', '-Wno-unused-parameter',
                             '-fsanitize=undefined', '-fno-sanitize-recover=all',
                             str(c), '-o', str(exe)], capture_output=True, text=True)
        row = dict(variant=name, compile_exit=cc.returncode, compiler_stderr=cc.stderr,
                   expected_exit=expected, passed=False)
        if cc.returncode == 0:
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            row.update(exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
            row['passed'] = run.returncode == expected and not run.stderr
        rows.append(row)
        print(name, 'PASS' if row['passed'] else 'FAIL', row)
    inputs = src | fixture_src
    changed = [p for p, s in inputs.items() if (ROOT / p).read_text() != s]
    result = dict(source_sha256={p: hashlib.sha256(s.encode()).hexdigest()
                                 for p, s in src.items()},
                  fixture_sha256={p: hashlib.sha256(s.encode()).hexdigest()
                                  for p, s in fixture_src.items()},
                  changed_inputs=changed, failure_cases=28, lifecycle_cases=12, cases=rows)
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('artifacts:', out, 'changed_inputs:', changed)
    return int(bool(changed) or not all(r['passed'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
