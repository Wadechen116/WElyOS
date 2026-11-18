#!/usr/bin/env python3
"""以原始 libc 的名稱隔離版本重現寬度溢位;不執行任何 WElyOS syscall。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/static'))
sys.dont_write_bytecode = True
from rules import functions

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT,
                    help='source snapshot root; defaults to current project')
args = parser.parse_args()
out = Path(tempfile.mkdtemp(prefix='welyos-format-probe-'))
source = (args.source/'user/libc.c').read_text()
names = {f.name for f in functions('user/libc.c', source)}
(out/'prefix.h').write_text('#define __i386__ 1\n' + '\n'.join(
    f'#define {name} welyos_probe_{name}' for name in sorted(names)) + '\n')
(out/'main.c').write_text('''
#include <stddef.h>
#include <limits.h>
int welyos_probe_snprintf(char *, size_t, const char *, ...);
int main(int argc, char **argv) {
    char out[32];
    if(argc>1 && argv[1][0]=='s')
        return welyos_probe_snprintf(out,sizeof(out),"%*d",INT_MIN,1)<0;
    return welyos_probe_snprintf(out,sizeof(out),"%2147483648d",1)<0;
}
''')
cmd = ['gcc', '-std=c11', '-O1', '-g', '-fno-builtin', '-fno-pie', '-no-pie',
       '-fsanitize=undefined', '-fno-sanitize-recover=undefined', '-include', str(out/'prefix.h'),
       str(args.source/'user/libc.c'), str(out/'main.c'), '-o', str(out/'probe')]
proc = subprocess.run(cmd, text=True, capture_output=True)
if proc.returncode:
    print(proc.stderr)
    sys.exit(2)
results = []
for mode in ('star', 'digits'):
    try:
        p = subprocess.run([str(out/'probe'), mode], capture_output=True, text=True, timeout=3)
        results.append({'mode':mode, 'returncode':p.returncode, 'stdout':p.stdout, 'stderr':p.stderr})
    except subprocess.TimeoutExpired:
        results.append({'mode':mode, 'returncode':None, 'stderr':'timeout (large format width still unbounded)'})
(out/'results.json').write_text(json.dumps(results, indent=2))
print(f'Artifacts: {out}')
print(json.dumps(results, indent=2))
# 這是缺陷重現工具,成功重現故意回傳非零;修復後仍須檢查大寬度是否被有界處理。
sys.exit(1 if any(r['returncode'] != 0 for r in results) else 0)
