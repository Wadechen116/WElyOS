#!/usr/bin/env python3
"""真實區塊快取加摘取的 WFS 函式，以 RAM 磁碟注入讀寫失敗。"""
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
    root = args.source.resolve()
    out = Path(tempfile.mkdtemp(prefix='welyos-wfs-failures-'))
    hashes = {}
    for name in ('kernel/bcache.c', 'include/bcache.h', 'include/blk.h',
                 'include/kstring.h', 'include/uapi/errno.h', 'kernel/wfs.c'):
        data = (root / name).read_bytes()
        hashes[name] = hashlib.sha256(data).hexdigest()
        if name != 'kernel/wfs.c':
            dest = out / (name.removeprefix('include/') if name.startswith('include/') else Path(name).name)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        else:
            source = data.decode()
    selected = {f.name: f for f in functions('kernel/wfs.c', source)}
    lines = source.splitlines(keepends=True)
    parts = [source[source.index('#define WFS_MAGIC1'):source.index('struct wdirent {')]]
    start = source.index('struct wnode {')
    parts.append(source[start:source.index('};', start) + 2])
    (out / 'wfs_types.h').write_text('\n'.join(parts))
    names = ('bitmap_op', 'balloc', 'bfree', 'dinode_rw', 'iupdate', 'bmap', 'file_truncate', 'file_read')
    bodies = {n: ''.join(lines[selected[n].line - 1:selected[n].end]) for n in names}
    (out / 'wfs_functions.c').write_text('\n'.join(bodies.values()))
    (out / 'mmu.h').write_text('#define PAGE_SIZE 4096u\n')
    (out / 'pmm.h').write_text('#include <stddef.h>\nvoid *pmm_alloc_pages(size_t);\nvoid pmm_free_pages(void *, size_t);\n')
    (out / 'panic.h').write_text('_Noreturn void panic(const char *, ...);\n')
    fixture = ROOT / 'tests/host/wfs/failure_probe.c'
    fixture_data = fixture.read_bytes()
    (out / 'probe.c').write_bytes(fixture_data)
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(out),
                    str(out / 'probe.c'), str(out / 'bcache.c'), '-o', str(out / 'probe')], check=True)
    results = []
    for kind in ('bitmap-read', 'bitmap-verify', 'free-ok', 'free-invalid',
                 'tail-direct-read', 'tail-indirect-read', 'indirect-read', 'indirect-verify',
                 'truncate-direct-ok', 'truncate-indirect-ok', 'sync-write', 'evict-write',
                 'read-indirect-error', 'read-indirect-verify', 'read-direct-error',
                 'read-direct-hole', 'read-indirect-hole', 'read-indirect-ok'):
        p = subprocess.run([str(out / 'probe'), kind], capture_output=True, text=True, timeout=5)
        row = dict(case=kind, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)
        results.append(row)
        print(f'{kind}: {"PASS" if p.returncode == 0 else "FAIL"} {p.stdout.strip()}')
        if p.stderr:
            print(p.stderr[:1500], file=sys.stderr)
    changed = [n for n, h in hashes.items() if hashlib.sha256((root / n).read_bytes()).hexdigest() != h]
    (out / 'results.json').write_text(json.dumps(dict(source_hashes=hashes, changed_inputs=changed,
        extracted_functions={n: dict(line=selected[n].line, sha256=hashlib.sha256(b.encode()).hexdigest()) for n,b in bodies.items()},
        fixture_sha256=hashlib.sha256(fixture_data).hexdigest(), results=results,
        model='real bcache; RAM disk; WFS functions extracted; minimal inode and checksum callback model'), indent=2) + '\n')
    print(f'Artifacts: {out}; changed_inputs={changed}')
    return 2 if changed else int(any(r['returncode'] for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
