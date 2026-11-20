#!/usr/bin/env python3
"""Actual membudget.c plus actual PMM: quota, reservations, refs, rollback and concurrency."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]

RELEASE='''int mb_release_bundle(struct mb_domain *d, const struct mb_bundle *b)
{
    unsigned long f = spin_lock_irqsave(&mb_lk);
    /* 整批先檢查:不能先退前幾類,才在後面的類別發現超量。 */
    for (unsigned i = 0; i < MB_NCLASS; i++) {
        if (b->quota[i] > d->used[i]) {
            errors++;
            spin_unlock_irqrestore(&mb_lk, f);
            return -1;
        }
    }
    /* 先交由 PMM 檢查與退票;若帳務錯誤而停止,配額仍未改變。
       呼叫者必須持有並驗證這筆 bundle 自己的未兌現票,不可借用其他 owner 的票。 */
    pmm_unreserve(b->reserve);
    for (unsigned i = 0; i < MB_NCLASS; i++) {
        d->used[i] -= b->quota[i];
        d->total -= b->quota[i];
    }
    spin_unlock_irqrestore(&mb_lk, f);
    return 0;
}
'''

def replace_function(src,name,change):
    pattern=r'(?m)^(?:int|void) '+re.escape(name)+r'\([^\n]*\)\n\{.*?\n\}'
    m=re.search(pattern,src,re.S)
    if not m:raise ValueError('function not found: '+name)
    body=change(m.group())
    if body==m.group():raise ValueError('replacement did not match: '+name)
    return src[:m.start()]+body+src[m.end():]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,default=ROOT)
    ap.add_argument('--fix-release',action='store_true')
    ap.add_argument('--sanitizer',choices=('address','thread'),default='address')
    ap.add_argument('--case',choices=('all','scalar','bundle','edges','release-error','release-pmm-error','parallel'),default='all')
    ap.add_argument('--mutate',choices=('none','no-used-guard','bundle-overflow','quota-before-reserve','get-overflow','stats-no-lock'),default='none')
    a=ap.parse_args();root=a.source.resolve()
    paths=('kernel/membudget.c','kernel/pmm.c','include/membudget.h','include/pmm.h','include/boot_info.h','include/uapi/errno.h')
    data={p:(root/p).read_bytes() for p in paths};src=data['kernel/membudget.c'].decode()
    if a.fix_release:src=replace_function(src,'mb_release_bundle',lambda _:RELEASE.rstrip())
    fixed=src
    if a.mutate=='no-used-guard':
        old='d->total <= d->limit && ';assert src.count(old)==1;src=src.replace(old,'')
    elif a.mutate=='bundle-overflow':
        old='if (b->quota[i] > (unsigned long)-1 - s)';assert src.count(old)==1;src=src.replace(old,'if (0 && b->quota[i] > (unsigned long)-1 - s)')
    elif a.mutate=='quota-before-reserve':
        src=replace_function(src,'mb_commit_bundle',lambda b:b.replace('fits_locked(d, n) && pmm_reserve(b->reserve) == 0','fits_locked(d, n)').replace('rc = 0;', 'rc = pmm_reserve(b->reserve);'))
    elif a.mutate=='get-overflow':
        old='d->refs < MB_REFS_MAX ? (d->refs++, 0) : -E_NOMEM';assert src.count(old)==1;src=src.replace(old,'(d->refs++, 0)')
    elif a.mutate=='stats-no-lock':
        src=replace_function(src,'mb_stats',lambda b:b.replace('    unsigned long f = spin_lock_irqsave(&mb_lk);\n','').replace('    spin_unlock_irqrestore(&mb_lk, f);\n',''))
    out=Path(tempfile.mkdtemp(prefix='welyos-membudget-'))
    for p,b in data.items():
        dest=out/('uapi/errno.h' if p.endswith('uapi/errno.h') else Path(p).name)
        dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(b)
    (out/'membudget.c').write_text(src)
    (out/'probe.c').write_bytes((ROOT/'tests/host/membudget/budget_probe.c').read_bytes())
    (out/'spinlock.h').write_text('''
#ifndef TEST_SPINLOCK_H
#define TEST_SPINLOCK_H
struct spinlock { pthread_mutex_t mutex;unsigned rank; };
#define LK_MB 75
#define LK_PMM 90
#define SPINLOCK_INIT(name,rank) { PTHREAD_MUTEX_INITIALIZER,rank }
static unsigned long spin_lock_irqsave(struct spinlock *s) {
    assert(depth<4);if(depth)assert(held_locks[depth-1]->rank<s->rank);
    assert(!pthread_mutex_lock(&s->mutex));held_locks[depth++]=s;lock_calls++;return 0;
}
static void spin_unlock_irqrestore(struct spinlock *s,unsigned long flags) {
    (void)flags;audit_lock(s);depth--;assert(!pthread_mutex_unlock(&s->mutex));
}
#endif
''')
    (out/'arch.h').write_text('');(out/'kmalloc.h').write_text('')
    (out/'kstring.h').write_text('#include <string.h>\n')
    (out/'kprintf.h').write_text('static inline int kprintf(const char *fmt,...) { (void)fmt;return 0; }\n')
    (out/'panic.h').write_text('''
_Noreturn static inline void panic(const char *fmt,...) {
    if(expect_panic)longjmp(panic_env,1);
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();
}
''')
    if a.fix_release:(out/'release.patch').write_text(''.join(difflib.unified_diff(data['kernel/membudget.c'].decode().splitlines(True),fixed.splitlines(True),fromfile='a/kernel/membudget.c',tofile='b/kernel/membudget.c')))
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fno-pie','-no-pie',
                    '-fsanitize='+('address,undefined' if a.sanitizer=='address' else 'thread'),'-fno-sanitize-recover=all',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    cases=('scalar','bundle','edges','release-error','release-pmm-error','parallel') if a.case=='all' else (a.case,)
    for case in cases:
        p=subprocess.run([str(out/'probe'),case],capture_output=True,text=True,timeout=60)
        rows.append(dict(case=case,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr))
        print(case,'PASS' if not p.returncode else 'FAIL',p.stdout.strip(),p.stderr[:800],flush=True)
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed=[p for p,h in hashes.items() if hashlib.sha256((root/p).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps(dict(results=rows,source_hashes=hashes,changed_inputs=changed,fix_release=a.fix_release,mutation=a.mutate,sanitizer=a.sanitizer,
        model='full actual membudget.c and pmm.c; synthetic preinitialized bitmap; rank-checking pthread lock; host calloc/free instead of kernel heap; not VM or hardware'),indent=2))
    print('artifacts:',out,'changed_inputs:',changed)
    return 2 if changed else int(any(r['returncode'] for r in rows))

if __name__=='__main__':raise SystemExit(main())
