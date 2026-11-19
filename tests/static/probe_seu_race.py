#!/usr/bin/env python3
"""摘取真實 SEU reader/writer，以確定交錯驗證兩筆請求及 busy 契約。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "scripts/static"))
from rules import functions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    paths = ('kernel/proc_mem.c', 'kernel/proc_signal.c', 'kernel/syscall.c',
             'include/uapi/errno.h')
    inputs = {name: (args.source / name).read_bytes() for name in paths}
    source = inputs['kernel/proc_mem.c'].decode()
    selected = {fn.name: fn for fn in functions('kernel/proc_mem.c', source)}
    if not {'sys_seu', 'pmem_apply_injection'} <= selected.keys():
        parser.error('SEU entry points missing; update the harness (no correctness result)')
    lines = source.splitlines(keepends=True)
    def extract(name):
        fn = selected[name]
        return ''.join(lines[fn.line-1:fn.end])
    writer = extract('sys_seu')
    block = extract('pmem_apply_injection')
    signal_source = inputs['kernel/proc_signal.c'].decode()
    signal_fns = {fn.name: fn for fn in functions('kernel/proc_signal.c', signal_source)}
    fn = signal_fns['user_return_check']
    if not any(name == 'pmem_apply_injection' for _, name in fn.calls):
        parser.error('user-return injection call missing; no integration result')
    dispatch_source = inputs['kernel/syscall.c'].decode()
    dispatch = next(f for f in functions('kernel/syscall.c', dispatch_source)
                    if f.name == 'syscall_dispatch')
    dispatch_tokens = ' '.join(t.text for t in dispatch.body)
    if 'case SYS_seu : return sys_seu ( p , a0 , ( uintptr_t ) a1 , a2 ) ;' not in dispatch_tokens:
        parser.error('SYS_seu forwarding changed; no integration result')
    out = Path(tempfile.mkdtemp(prefix='welyos-seu-race-'))
    (out / 'errno.h').write_bytes(inputs['include/uapi/errno.h'])
    harness = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "errno.h"
#define CAP_ADMIN 1
struct proc { struct { volatile int seu_pending; uintptr_t seu_addr; uint8_t seu_mask; } mem;
              int zombie; struct { unsigned caps; } cred; };
static struct proc target, caller = { .cred.caps=CAP_ADMIN };
static uint8_t a,b;
static int inject, locked, publication_rc;
long sys_seu(struct proc *, long, uintptr_t, long);
static void publish_b(void) {
    if (inject) {
        inject=0;
        publication_rc=(int)sys_seu(&caller,1,(uintptr_t)&b,1);
    }
}
static unsigned long sched_lock(void) { if (locked) __builtin_trap(); locked=1; return 0; }
static void sched_unlock(unsigned long flags) {
    (void)flags; locked=0;
    /* 新版 reader 在這裡完成 payload 快照；writer 此時才能取得鎖。 */
    publish_b();
}
static struct proc *proc_find(int pid) { return pid==1 ? &target : NULL; }
@WRITER@
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
@READER@
int main(void) {
    target.mem.seu_pending=1; target.mem.seu_addr=(uintptr_t)&a; target.mem.seu_mask=1;
    int busy=(int)sys_seu(&caller,1,(uintptr_t)&b,1);
    int protected_slot=busy==-E_BUSY && target.mem.seu_addr==(uintptr_t)&a && target.mem.seu_mask==1;
    /* 重設A，隔離 busy 契約與 reader interleaving 兩個驗證。 */
    target.mem.seu_pending=1; target.mem.seu_addr=(uintptr_t)&a; target.mem.seu_mask=1; inject=1;
    pmem_apply_injection(&target); unsigned first_a=a,first_b=b;
    pmem_apply_injection(&target);
    printf("{\"busy_rc\":%d,\"slot_preserved\":%d,\"publication_rc\":%d,"
           "\"after_first\":[%u,%u],\"after_second\":[%u,%u],\"pending\":%d}\n",
           busy,protected_slot,publication_rc,first_a,first_b,a,b,target.mem.seu_pending);
    return protected_slot && publication_rc==0 && first_a==1 && first_b==0 &&
           a==1 && b==2 && !target.mem.seu_pending ? 0 : 1;
}
'''
    (out / 'probe.c').write_text(harness.replace('@WRITER@', writer).replace('@READER@', block))
    subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=3)
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in inputs.items()}
    changed = [name for name, sha in hashes.items()
               if hashlib.sha256((args.source / name).read_bytes()).hexdigest() != sha]
    report = {'source_hashes': hashes, 'changed_inputs': changed,
              'reader_sha256': hashlib.sha256(block.encode()).hexdigest(),
              'writer_sha256': hashlib.sha256(writer.encode()).hexdigest(),
              'returncode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr,
              'model': 'deterministic interleaving, not a full kernel run'}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Artifacts: {out}')
    print(json.dumps(report, indent=2))
    return 2 if changed else int(p.returncode != 0)


if __name__ == '__main__':
    raise SystemExit(main())
