#!/usr/bin/env python3
"""摘取真實 SEU reader/writer，以確定交錯驗證兩筆請求及 busy 契約。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source / 'kernel/proc.c').read_text()
    consumer = source[source.index('void user_return_check('):]
    start = re.search(r'    if \(p->seu_pending\b', consumer)
    if start is None:
        parser.error('SEU consumer changed shape; update the harness (no correctness result)')
    block = consumer[start.start():consumer.index('    if (p->sig_pending)')]
    writer_start = source.index('case SYS_seu: {') + len('case SYS_seu: {')
    writer_end = source.index('    case SYS_rt_set:', writer_start)
    writer = source[writer_start:writer_end].rstrip()
    if not writer.endswith('}'):
        parser.error('SEU writer changed shape; update the harness')
    writer = writer[:-1]
    out = Path(tempfile.mkdtemp(prefix='welyos-seu-race-'))
    (out / 'errno.h').write_bytes((args.source / 'include/uapi/errno.h').read_bytes())
    harness = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "errno.h"
#define CAP_ADMIN 1
struct proc { volatile int seu_pending; uintptr_t seu_addr; uint8_t seu_mask;
              int zombie; struct { unsigned caps; } cred; };
static struct proc target, caller = { .cred.caps=CAP_ADMIN };
static uint8_t a,b;
static int inject, locked, publication_rc;
static long publish(struct proc *, long, long, long);
static void publish_b(void) {
    if (inject) {
        inject=0;
        publication_rc=(int)publish(&caller,1,(long)&b,1);
    }
}
static unsigned long sched_lock(void) { if (locked) __builtin_trap(); locked=1; return 0; }
static void sched_unlock(unsigned long flags) {
    (void)flags; locked=0;
    /* 新版 reader 在這裡完成 payload 快照；writer 此時才能取得鎖。 */
    publish_b();
}
static struct proc *proc_find(int pid) { return pid==1 ? &target : NULL; }
long publish(struct proc *p, long a0, long a1, long a2) {
@WRITER@
}
/* 舊版 reader 在 exchange 後沒有鎖保護 payload；同一個時序會重現舊錯誤。 */
int exchange(volatile int *ptr,int value,int order) {
    int old=__atomic_exchange_n(ptr,value,order); publish_b(); return old;
}
#define __atomic_exchange_n exchange
static int copy_from_user(struct proc *p,void *to,uintptr_t from,unsigned n) {
    (void)p; memcpy(to,(void *)from,n); return 0;
}
static int copy_to_user(struct proc *p,uintptr_t to,const void *from,unsigned n) {
    (void)p; memcpy((void *)to,from,n); return 0;
}
static void consume(struct proc *p) {
@READER@
}
int main(void) {
    target.seu_pending=1; target.seu_addr=(uintptr_t)&a; target.seu_mask=1;
    int busy=(int)publish(&caller,1,(long)&b,1);
    int protected_slot=busy==-E_BUSY && target.seu_addr==(uintptr_t)&a && target.seu_mask==1;
    /* 重設A，隔離 busy 契約與 reader interleaving 兩個驗證。 */
    target.seu_pending=1; target.seu_addr=(uintptr_t)&a; target.seu_mask=1; inject=1;
    consume(&target); unsigned first_a=a,first_b=b;
    consume(&target);
    printf("{\"busy_rc\":%d,\"slot_preserved\":%d,\"publication_rc\":%d,"
           "\"after_first\":[%u,%u],\"after_second\":[%u,%u],\"pending\":%d}\n",
           busy,protected_slot,publication_rc,first_a,first_b,a,b,target.seu_pending);
    return protected_slot && publication_rc==0 && a==1 && b==2 && !target.seu_pending ? 0 : 1;
}
'''
    (out / 'probe.c').write_text(harness.replace('@WRITER@', writer).replace('@READER@', block))
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=3)
    report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'reader_sha256': hashlib.sha256(block.encode()).hexdigest(),
              'writer_sha256': hashlib.sha256(writer.encode()).hexdigest(),
              'returncode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr,
              'model': 'deterministic interleaving, not a full kernel run'}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Artifacts: {out}')
    print(json.dumps(report, indent=2))
    return int(p.returncode != 0)


if __name__ == '__main__':
    raise SystemExit(main())
