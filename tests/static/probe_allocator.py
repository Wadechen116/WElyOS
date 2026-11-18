#!/usr/bin/env python3
"""以原始 libc.c、名稱隔離與 sbrk 替身驗證 allocator 的失敗契約。"""
import argparse
import hashlib
import json
from pathlib import Path
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
    out = Path(tempfile.mkdtemp(prefix='welyos-allocator-'))
    source = (args.source / 'user/libc.c').read_bytes()
    (out / 'libc.c').write_bytes(source)
    # 名稱隔離的 format/printf 巨集不應改寫 GCC attribute 的名稱。
    header = (args.source / 'user/libc.h').read_text()
    (out / 'libc.h').write_text(header.replace('format(printf,', '__format__(__printf__,'))
    names = {fn.name for fn in functions('user/libc.c', source.decode())}
    (out / 'prefix.h').write_text('\n'.join(f'#define {name} welyos_alloc_{name}'
                                           for name in sorted(names)) + '\n')
    (out / 'usys.h').write_text('''#include <stddef.h>
typedef int umutex_t;
static inline void umutex_lock(umutex_t *p) { (void)p; }
static inline void umutex_unlock(umutex_t *p) { (void)p; }
long probe_sbrk(long);
long probe_write(long,const void *,size_t);
_Noreturn void probe_exit(int);
#define sys_sbrk probe_sbrk
#define sys_write probe_write
#define sys_exit probe_exit
''')
    common = ['gcc', '-std=c11', '-O1', '-g', '-fno-builtin', '-fno-pie',
              '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(common + ['-include', str(out / 'prefix.h'), '-c', str(out / 'libc.c'),
                             '-o', str(out / 'libc.o')], check=True)
    subprocess.run(common + ['-no-pie', str(ROOT / 'tests/host/libc/allocator_probe.c'),
                             str(out / 'libc.o'), '-o', str(out / 'probe')], check=True)
    p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=10)
    report = {'source_sha256': hashlib.sha256(source).hexdigest(), 'returncode': p.returncode,
              'stdout': p.stdout, 'stderr': p.stderr, 'model': 'host 64-bit, single-threaded'}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Artifacts: {out}')
    print(json.dumps(report, indent=2))
    return int(p.returncode != 0)


if __name__ == '__main__':
    raise SystemExit(main())
