#!/usr/bin/env python3
"""Compile proposed changes in an isolated source copy; never edit the product.

Uses existing CMake-generated compiler flags. Translation units only, no link,
boot, or claim of integration success.
"""
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    out = Path(tempfile.mkdtemp(prefix='welyos-proposal-build-'))
    snapshot = out / 'source'
    for folder in ('kernel', 'include', 'arch'):
        shutil.copytree(ROOT / folder, snapshot / folder)
    patches = ['tempobj-kobj-20261121.patch', 'ramfs-zero-write-eof-20261121.patch']
    inputs = {str(p.relative_to(ROOT)): sha(p)
              for folder in ('kernel', 'include', 'arch')
              for p in (ROOT / folder).rglob('*') if p.is_file()}
    patch_hashes = {}
    for name in patches:
        patch = ROOT / 'tests/static/proposals' / name
        patch_hashes[name] = sha(patch)
        r = subprocess.run(['git', 'apply', str(patch)], cwd=snapshot,
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)
    compilers = {'i386': 'gcc', 'riscv64': 'riscv64-unknown-elf-gcc',
                 'aarch64': 'aarch64-linux-gnu-gcc'}
    rows = []
    for arch, compiler in compilers.items():
        flags_file = ROOT / 'build' / arch / 'CMakeFiles/welyos.elf.dir/flags.make'
        text = flags_file.read_text()
        values = dict(line.split(' = ', 1) for line in text.splitlines()
                      if ' = ' in line)
        flags = shlex.split(' '.join(values.get(k, '') for k in
                                     ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'))
                            .replace(str(ROOT), str(snapshot)))
        for unit in ('proc', 'ramfs'):
            argv = [compiler, *flags, '-Werror', '-c',
                    str(snapshot / 'kernel' / (unit + '.c')), '-o',
                    str(out / (arch + '-' + unit + '.o'))]
            r = subprocess.run(argv, capture_output=True, text=True, timeout=45)
            row = dict(arch=arch, unit=unit, argv=argv, rc=r.returncode,
                       stdout=r.stdout, stderr=r.stderr)
            rows.append(row)
            print(arch, unit, 'PASS' if r.returncode == 0 else 'FAIL', r.stderr)
    changed = [p for p, h in inputs.items() if sha(ROOT / p) != h]
    result = dict(source_sha256=inputs, patch_sha256=patch_hashes,
                  changed_inputs=changed, cases=rows,
                  limitation='Compile-only isolated proposal, no linking or QEMU')
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('artifacts:', out, 'changed_inputs:', changed)
    return int(bool(changed) or any(r['rc'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
