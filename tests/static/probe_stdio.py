#!/usr/bin/env python3
"""對原始 libc/ulib/echo 注入輸出故障；短寫須完整重試或回報失敗。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
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
    out = Path(tempfile.mkdtemp(prefix='welyos-stdio-'))
    hashes = {}
    for name in ('user/libc.c', 'user/libc.h', 'user/ulib.h', 'user/echo.c', 'user/cat.c', 'include/uapi/fs.h', 'include/uapi/errno.h'):
        data = (root / name).read_bytes()
        hashes[name] = hashlib.sha256(data).hexdigest()
        if name == 'user/libc.h':
            data = data.replace(b'format(printf,', b'__format__(__printf__,')
        (out / ('uapi_fs.h' if name=='include/uapi/fs.h' else Path(name).name)).write_bytes(data)
    source = (out / 'libc.c').read_text()
    names = {f.name for f in functions('user/libc.c', source)}
    (out / 'prefix.h').write_text('\n'.join(f'#define {n} welyos_stdio_{n}' for n in sorted(names))+'\n')
    (out / 'usys.h').write_text('''#ifndef STDIO_PROBE_USYS_H
#define STDIO_PROBE_USYS_H
#include <stddef.h>
#include "errno.h"
#include "uapi_fs.h"
long probe_read(long,void *,size_t);
long probe_open(const char *,long);
long probe_close(long);
#define sys_read probe_read
#define sys_open probe_open
#define sys_close probe_close
typedef int umutex_t;
static inline void umutex_lock(umutex_t *p) { (void)p; }
static inline void umutex_unlock(umutex_t *p) { (void)p; }
long probe_sbrk(long);
long probe_write(long,const void *,size_t);
_Noreturn void probe_exit(int);
#define sys_sbrk probe_sbrk
#define sys_write probe_write
#define sys_exit probe_exit
#endif
''')
    contracts = []
    selected_helpers = {}
    for candidates, macro, arg in [(('out_str','puts_'), 'PUTS', 's'), (('line_write','line_out'), 'LINE', 'l')]:
        for name in candidates:
            declaration = re.search(r'static\s+inline\s+(void|int|long)\s+'+name+r'\s*\(',
                                    (out / 'ulib.h').read_text())
            if declaration:
                break
        else:
            parser.error(f'{candidates} signature changed; update the harness, no correctness result')
        selected_helpers[macro] = name
        has_status = declaration[1] != 'void'
        contracts += [f'#define {macro}_HAS_STATUS {int(has_status)}',
                      f'#define CALL_{macro}({arg}) '+(f'{name}({arg})' if has_status else f'({name}({arg}),0)')]
    (out / 'contracts.h').write_text('\n'.join(contracts)+'\n')
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/libc/stdio_probe.c').read_bytes())
    common = ['gcc', '-std=c11', '-O1', '-g', '-fno-builtin', '-fno-pie', '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(common+['-include',str(out/'prefix.h'),'-c',str(out/'libc.c'),'-o',str(out/'libc.o')],check=True)
    subprocess.run(common+['-Dmain=welyos_stdio_echo_main','-c',str(out/'echo.c'),'-o',str(out/'echo.o')],check=True)
    subprocess.run(common+['-Dmain=welyos_stdio_cat_main','-c',str(out/'cat.c'),'-o',str(out/'cat.o')],check=True)
    subprocess.run(common+['-no-pie',str(out/'probe.c'),str(out/'libc.o'),str(out/'echo.o'),str(out/'cat.o'),
                          '-o',str(out/'probe')],check=True)
    results=[]
    cases=[]
    for api in ('printf','fprintf','puts','putchar','ulib-output','line-output','echo','cat-stdin','cat-file'):
        faults=['normal','short','zero','eio','epipe','eintr','late-error']
        if api.startswith('cat-'):
            faults.append('read-eio')
        cases += [(api,fault,0) for fault in faults]
    # 三次正常flush的每一個位置，都各自測負值、零進度及可重試的短寫。
    for api in ('printf','fprintf','puts','putchar'):
        for at in range(1, (1 if api=='putchar' else 3)+1):
            cases += [(api,fault,at) for fault in ('epipe','eio','zero','short')]
    for api,fault,at in cases:
        try:
            p=subprocess.run([str(out/'probe'),api,fault,str(at)],capture_output=True,text=True,timeout=3)
            row={'api':api,'fault':fault,'at':at,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
            if p.returncode in (0,1):
                row['observation']=json.loads(p.stdout)
        except subprocess.TimeoutExpired:
            row={'api':api,'fault':fault,'at':at,'returncode':None,'error':'hang'}
        results.append(row)
    changed=[name for name,sha in hashes.items() if hashlib.sha256((root/name).read_bytes()).hexdigest()!=sha]
    report={'source_hashes':hashes,'selected_data_helpers':selected_helpers,'changed_inputs':changed,'results':results,
            'model':'host 64-bit syscall replacement; no real WElyOS syscall'}
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Artifacts: {out}')
    for row in results:
        if row['returncode']!=0 or row.get('stderr'):
            print(f"FAIL {row['api']}/{row['fault']}@{row['at']}: {row.get('observation',row)}")
    passed=sum(r['returncode']==0 and not r.get('stderr') for r in results)
    print(f'{passed}/{len(results)} PASS; changed_inputs={changed}')
    return 2 if changed else int(passed!=len(results))


if __name__=='__main__':
    raise SystemExit(main())
