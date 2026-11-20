#!/usr/bin/env python3
"""Actual VFS/WFS wrappers: shared open-file offset/refcounts, overlapping write/fsync, and flush failure (I-323)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/static'))
from rules import functions

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,default=ROOT)
    ap.add_argument('--mutate',choices=('none','offset','refcount','write-lock','flush-lock'),default='none')
    a=ap.parse_args();root=a.source.resolve();out=Path(tempfile.mkdtemp(prefix='welyos-shared-file-'))
    data={p:(root/p).read_bytes() for p in ['kernel/vfs.c','kernel/wfs.c','include/fs.h','include/uapi/fs.h','include/uapi/errno.h']}
    for p in ['include/fs.h','include/uapi/fs.h','include/uapi/errno.h']:
        dest=out/p.removeprefix('include/');dest.parent.mkdir(exist_ok=True);dest.write_bytes(data[p])
    (out/'sync.h').write_text('#include <pthread.h>\nstruct mutex {pthread_mutex_t m;};\n')
    parts=[]
    for path,names in [('kernel/vfs.c',['iput','vfs_dup','vfs_close','vfs_write','flush_file','vfs_fsync','vfs_flush']),('kernel/wfs.c',['wfs_write','wfs_flush'])]:
        src=data[path].decode();fs={f.name:f for f in functions(path,src)};lines=src.splitlines(keepends=True)
        if path=='kernel/vfs.c':
            start=src.index('struct file {');(out/'file_type.h').write_text(src[start:src.index('};',start)+2])
        for name in names:
            f=fs[name];body=''.join(lines[f.line-1:f.end])
            if a.mutate=='offset' and name=='vfs_write':body=body.replace('f->off += (uint64_t)r;', 'f->off += 0;')
            if a.mutate=='refcount' and name=='vfs_dup':body=body.replace('f->refs++;','f->refs += 0;')
            if (a.mutate=='write-lock' and name=='wfs_write') or (a.mutate=='flush-lock' and name=='wfs_flush'):
                body=body.replace('mutex_lock(&wfs_lk);','').replace('mutex_unlock(&wfs_lk);','')
            parts.append(body)
    (out/'functions.c').write_text('\n'.join(parts))
    (out/'probe.c').write_bytes((ROOT/'tests/host/vfs/shared_file_probe.c').read_bytes())
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-fno-pie','-no-pie',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for name in ('concurrent','write-vs-sync','sync-error'):
        p=subprocess.run([str(out/'probe'),name],capture_output=True,text=True,timeout=10)
        rows.append(dict(case=name,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name,p.returncode,p.stdout.strip(),p.stderr[:500])
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed=[p for p,h in hashes.items() if hashlib.sha256((root/p).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps(dict(results=rows,mutation=a.mutate,source_hashes=hashes,changed_inputs=changed,
        model='actual VFS dup/close/write/fsync and WFS serialization wrappers; pthreads and modeled cache/device; not a full fork/syscall/QEMU run'),indent=2))
    print('artifacts:',out);return 2 if changed else int(any(r['returncode'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
