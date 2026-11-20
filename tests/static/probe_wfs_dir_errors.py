#!/usr/bin/env python3
"""WFS:讀不到不可以被當成沒有(第 50 步 D4)。真實 bcache + 摘取的 WFS 函式,RAM 磁碟注入讀取失敗。

用法:probe_wfs_dir_errors.py [--source 目錄]
  --source 指向修正前的程式碼(例如 git worktree)時,修正過的情境應該 FAIL(負對照)。
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts/static'))
from rules import functions

CASES = ('balloc-read', 'balloc-ok', 'ialloc-read', 'ialloc-ok', 'empty-read', 'empty-ok',
         'readdir-inode', 'readdir-ok', 'check-inode', 'check-ok')


def struct_text(src, head):
    i = src.index(head)
    return src[i:src.index('};', i) + 2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT)
    a = ap.parse_args()
    root = a.source.resolve()
    out = Path(tempfile.mkdtemp(prefix='welyos-wfs-dir-'))
    for name in ('kernel/bcache.c', 'include/bcache.h', 'include/blk.h', 'include/kstring.h', 'include/uapi/errno.h'):
        dest = out / (name.removeprefix('include/') if name.startswith('include/') else Path(name).name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((root / name).read_bytes())
    src = (root / 'kernel/wfs.c').read_text()
    types = [src[src.index('#define WFS_MAGIC1'):src.index('struct wdirent {')],
             src[src.index('struct wdirent {'):src.index('#define DPB')], re.search(r'#define DPB[^\n]*\n', src).group(0),
             struct_text(src, 'struct wnode {')]
    (out / 'wfs_types.h').write_text('\n'.join(types))
    fns = {f.name: f for f in functions('kernel/wfs.c', src)}
    lines = src.splitlines(keepends=True)
    body = lambda n: ''.join(lines[fns[n].line - 1:fns[n].end])
    parts = [body('wn'), re.search(r'typedef int \(\*dir_fn\)[^\n]*\n', src).group(0)]
    parts += [body(n) for n in ('bitmap_op', 'balloc', 'bfree', 'dinode_rw', 'nget', 'ialloc', 'bmap', 'dir_scan', 'used_fn')]
    parts += [struct_text(src, 'struct list_arg {'), body('list_fn'), body('wfs_readdir'), body('wfs_is_empty'),
              struct_text(src, 'struct check {'), body('check_entry')]
    code = '\n'.join(parts).replace('bcache_forget(', 'bcache_forget_stub(')
    (out / 'wfs_functions.c').write_text(code)
    (out / 'mmu.h').write_text('#define PAGE_SIZE 4096u\n')
    (out / 'pmm.h').write_text('#include <stddef.h>\nvoid *pmm_alloc_pages(size_t);\nvoid pmm_free_pages(void *, size_t);\n')
    (out / 'panic.h').write_text('_Noreturn void panic(const char *, ...);\n')
    defs = []
    if re.search(r'static uint32_t balloc\(void\)', src): defs.append('-DOLD_BALLOC')
    if re.search(r'static struct wnode \*ialloc\(int type, uint32_t parent\)', src): defs.append('-DOLD_IALLOC')
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/wfs/dir_error_probe.c').read_bytes())
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', *defs, '-I', str(out),
                    str(out / 'probe.c'), str(out / 'bcache.c'), '-o', str(out / 'probe')], check=True)
    bad = 0
    for kind in CASES:
        p = subprocess.run([str(out / 'probe'), kind], capture_output=True, text=True, timeout=5)
        bad += p.returncode != 0
        print(f"{kind}: {'PASS' if p.returncode == 0 else 'FAIL'} {p.stdout.strip()} {p.stderr.strip()[:200]}")
    print(f'{len(CASES) - bad}/{len(CASES)} PASS; artifacts: {out}; defines={defs}')
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
