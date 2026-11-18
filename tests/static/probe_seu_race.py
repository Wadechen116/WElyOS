#!/usr/bin/env python3
"""以原始 SEU 消費區塊及確定的 writer 交錯，重現 pending/payload 不一致。"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/proc.c').read_text()
start = source.index('    if (p->seu_pending && __atomic_exchange_n')
end = source.index('    if (p->sig_pending)', start)
block = source[start:end]
out = Path(tempfile.mkdtemp(prefix='welyos-seu-race-'))
(out / 'probe.c').write_text('''#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct proc { volatile int seu_pending; uintptr_t seu_addr; uint8_t seu_mask; };
static struct proc target;
static uint8_t a,b;
static int inject=1;
/* 模擬另一顆 CPU 在 exchange 清除 pending 後，發布完整的第二筆請求。 */
static int exchange(volatile int *ptr,int value,int order) {
    int old=__atomic_exchange_n(ptr,value,order);
    if(inject) { inject=0; target.seu_addr=(uintptr_t)&b; target.seu_mask=2; target.seu_pending=1; }
    return old;
}
#define __atomic_exchange_n exchange
static int copy_from_user(struct proc *p,void *to,uintptr_t from,unsigned n) {
    (void)p; memcpy(to,(void *)from,n); return 0;
}
static int copy_to_user(struct proc *p,uintptr_t to,const void *from,unsigned n) {
    (void)p; memcpy((void *)to,from,n); return 0;
}
static void consume(struct proc *p) {
''' + block + '''}
int main(void) {
    target.seu_pending=1; target.seu_addr=(uintptr_t)&a; target.seu_mask=1;
    consume(&target); unsigned first_a=a,first_b=b;
    consume(&target);
    printf("{\\"after_first\\":[%u,%u],\\"after_second\\":[%u,%u],\\"expected\\":[1,2]}\\n",first_a,first_b,a,b);
    return a==1 && b==2 ? 0 : 1;
}
''')
subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                '-fno-sanitize-recover=all', str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
p = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=3)
report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
          'block_sha256': hashlib.sha256(block.encode()).hexdigest(), 'returncode': p.returncode,
          'stdout': p.stdout, 'stderr': p.stderr, 'model': 'deterministic interleaving, not a full kernel run'}
(out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'Artifacts: {out}')
print(json.dumps(report, indent=2))
raise SystemExit(p.returncode)
