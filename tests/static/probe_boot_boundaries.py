#!/usr/bin/env python3
"""以真實黑盒子與摘取的 WFS probe，驗證未知磁碟、I/O 與環狀記錄邊界。"""
import argparse
import hashlib
import json
import re
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
    root = args.source.resolve()
    out = Path(tempfile.mkdtemp(prefix='welyos-boot-boundaries-'))
    hashes = {}
    for name in ('kernel/blackbox.c', 'include/blackbox.h', 'include/blk.h',
                 'include/uapi/blackbox.h', 'include/crc32.h', 'lib/crc32.c'):
        data = (root / name).read_bytes()
        hashes[name] = hashlib.sha256(data).hexdigest()
        dest = out / ('uapi/blackbox.h' if name == 'include/uapi/blackbox.h' else Path(name).name)
        dest.parent.mkdir(exist_ok=True)
        dest.write_bytes(data)
    source = (root / 'kernel/wfs.c').read_text()
    hashes['kernel/wfs.c'] = hashlib.sha256(source.encode()).hexdigest()
    selected = {fn.name: fn for fn in functions('kernel/wfs.c', source)}
    lines = source.splitlines(keepends=True)
    parts = [source[source.index('#define WFS_MAGIC1'):source.index('struct wdirent {')]]
    parts += re.findall(r'^#define\s+WFS_MIN_DEV_BLOCKS\b[^\n]*', source, re.M)
    for name in ('sb_crc', 'layout', 'sb_valid', 'wfs_probe'):
        fn = selected[name]
        parts.append(''.join(lines[fn.line - 1:fn.end]))
    (out / 'wfs_probe.c').write_text('\n'.join(parts))
    (out / 'kstring.h').write_text('#include <string.h>\n')
    (out / 'pmm.h').write_text('#include <stddef.h>\n#define PAGE_SIZE 4096u\nvoid *pmm_alloc(void);\nvoid pmm_free(void *);\n')
    (out / 'sync.h').write_text('''struct mutex { int unused; };
#define MUTEX_INIT {0}
static inline void mutex_lock(struct mutex *m) { (void)m; }
static inline void mutex_unlock(struct mutex *m) { (void)m; }
''')
    (out / 'timer.h').write_text('''#include <stdint.h>
static inline uint64_t timer_epoch(void *unused) { (void)unused; return 0; }
''')
    (out / 'kprintf.h').write_text('''#include <stdarg.h>
#include <stdio.h>
static inline int kvsnprintf(char *buf,size_t size,const char *fmt,va_list ap) {
    return vsnprintf(buf,size,fmt,ap);
}
''')
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/boot/probe.c').read_bytes())
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(out / 'probe.c'), str(out / 'crc32.c'), '-o', str(out / 'probe')], check=True)
    results = []
    for kind in ('unknown', 'primary-io', 'backup-io', 'oom', 'tiny63', 'tiny64', 'tiny65', 'tiny66',
                 'valid', 'backup', 'old-full', 'failed-with-test', 'ring', 'bad-crc-boot',
                 'seq-wrap', 'boot-wrap'):
        p = subprocess.run([str(out / 'probe'), kind], capture_output=True, text=True, timeout=3)
        row = {'case': kind, 'returncode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}
        results.append(row)
        print(kind + ': ' + ('PASS' if p.returncode == 0 else p.stdout.strip() or p.stderr[:200]))
    (out / 'results.json').write_text(json.dumps({'source_hashes': hashes, 'results': results}, indent=2) + '\n')
    print(f'Artifacts: {out}')
    return int(any(r['returncode'] for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
