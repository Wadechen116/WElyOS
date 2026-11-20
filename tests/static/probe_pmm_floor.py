#!/usr/bin/env python3
"""Real PMM with synthetic preinitialized page map and pthread lock (C3.1)."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def locked_getters(src):
    for signature, field in (("size_t pmm_reserve_count(void)", "floor_pages"),
                             ("unsigned long pmm_reserve_refusals(void)", "floor_refusals"),
                             ("size_t pmm_free_count(void)", "nfree")):
        import re
        pattern = re.escape(signature) + r"\s*\{ return " + field + r"; \}"
        replacement = (signature + "\n{\n    unsigned long flags = spin_lock_irqsave(&pmm_lk);\n"
                       + ("    unsigned long" if field == "floor_refusals" else "    size_t")
                       + " value = " + field + ";\n    spin_unlock_irqrestore(&pmm_lk, flags);\n    return value;\n}")
        src, count = re.subn(pattern, replacement, src)
        if count != 1:
            raise ValueError("getter source changed: " + signature)
    return src


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT)
    ap.add_argument('--fix-getters', action='store_true', help='isolated proposed product change')
    ap.add_argument('--mutate', choices=('none', 'remove-floor', 'off-by-one', 'ordinary-bypass'), default='none')
    ap.add_argument('--sanitizer', choices=('address', 'thread'), default='address')
    ap.add_argument('--case', choices=('all', 'exhaustive', 'boundaries', 'parallel', 'getters'), default='all')
    a = ap.parse_args()
    root = a.source.resolve()
    paths = ('kernel/pmm.c', 'include/pmm.h', 'include/boot_info.h')
    data = {p: (root / p).read_bytes() for p in paths}
    src = data['kernel/pmm.c'].decode()
    if a.fix_getters:
        src = locked_getters(src)
    if a.mutate != 'none':
        old, new = {
            'remove-floor': ('floor_on && !use_reserve && nfree - n < floor_pages', '0 && floor_on && !use_reserve && nfree - n < floor_pages'),
            'off-by-one': ('nfree - n < floor_pages', 'nfree - n <= floor_pages'),
            'ordinary-bypass': ('return alloc_pages_ref(n, 0);', 'return alloc_pages_ref(n, 1);'),
        }[a.mutate]
        if src.count(old) != 1:
            raise ValueError('mutation source did not match exactly once')
        src = src.replace(old, new)
    out = Path(tempfile.mkdtemp(prefix='welyos-pmm-floor-'))
    (out / 'pmm.c').write_text(src)
    for p in paths[1:]:
        (out / Path(p).name).write_bytes(data[p])
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/pmm/floor_probe.c').read_bytes())
    (out / 'spinlock.h').write_text('''
struct spinlock { pthread_mutex_t mutex; };
#define LK_PMM 1
#define SPINLOCK_INIT(name,rank) { PTHREAD_MUTEX_INITIALIZER }
static unsigned long spin_lock_irqsave(struct spinlock *s) {
    assert(!held);assert(!pthread_mutex_lock(&s->mutex));held=1;lock_calls++;return 0;
}
static void spin_unlock_irqrestore(struct spinlock *s,unsigned long flags) {
    (void)flags;check_locked();held=0;assert(!pthread_mutex_unlock(&s->mutex));
}
''')
    (out / 'arch.h').write_text('')
    (out / 'kstring.h').write_text('#include <string.h>\n')
    (out / 'kprintf.h').write_text('static inline int kprintf(const char *fmt,...) { (void)fmt;return 0; }\n')
    (out / 'panic.h').write_text('''
_Noreturn static inline void panic(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();
}
''')
    if a.fix_getters:
        (out / 'getters.patch').write_text(''.join(difflib.unified_diff(
            data['kernel/pmm.c'].decode().splitlines(True), src.splitlines(True),
            fromfile='a/kernel/pmm.c', tofile='b/kernel/pmm.c')))
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fno-pie', '-no-pie', '-fsanitize=' + ('address,undefined' if a.sanitizer == 'address' else 'thread'),
                    '-fno-sanitize-recover=all', str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    rows = []
    cases = ('exhaustive', 'boundaries', 'parallel', 'getters') if a.case == 'all' else (a.case,)
    for case in cases:
        p = subprocess.run([str(out / 'probe'), case], capture_output=True, text=True, timeout=60)
        rows.append(dict(case=case, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr))
        print(case, 'PASS' if not p.returncode else 'FAIL', p.stdout.strip(), p.stderr[:700])
    hashes = {p: hashlib.sha256(b).hexdigest() for p, b in data.items()}
    changed = [p for p, h in hashes.items() if hashlib.sha256((root / p).read_bytes()).hexdigest() != h]
    (out / 'results.json').write_text(json.dumps(dict(results=rows, source_hashes=hashes,
        changed_inputs=changed, fix_getters=a.fix_getters, mutation=a.mutate, sanitizer=a.sanitizer,
        model='full real pmm.c; synthetic preinitialized bitmap, refs, arena; pthread lock replacing IRQ/spin; no boot/MMU'), indent=2))
    print('artifacts:', out, 'changed_inputs:', changed)
    return 2 if changed else int(any(r['returncode'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
