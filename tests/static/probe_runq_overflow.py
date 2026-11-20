#!/usr/bin/env python3
"""Deterministic producer/consumer handshakes in the real runq_ring overflow code (I-072)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,default=ROOT)
    ap.add_argument('--mutate',choices=('none','producer-check','claim-unlink'),default='none')
    a=ap.parse_args();root=a.source.resolve()
    paths=['kernel/runq_ring.c','include/ring.h','include/runq.h']
    data={p:(root/p).read_bytes() for p in paths}
    src=data['kernel/runq_ring.c'].decode()
    if a.mutate=='producer-check':
        old='if (!__atomic_load_n(&n->queued, SEQ))'
        assert src.count(old)==1
        src=src.replace(old,'if (0 && !__atomic_load_n(&n->queued, SEQ))')
    if a.mutate=='claim-unlink':
        old='if (__atomic_load_n(&n->where, SEQ) == W_OVF)'
        assert src.count(old)==1
        src=src.replace(old,'if (0 && __atomic_load_n(&n->where, SEQ) == W_OVF)')
    old='    ovf_link(n, p);'
    assert src.count(old)==1
    src=src.replace(old,old+'\n    after_link();')
    old='    if (__atomic_load_n(&n->where, SEQ) == W_OVF)'
    # Include the mutation form as well, without deleting the original branch.
    marker='    if (0 && __atomic_load_n(&n->where, SEQ) == W_OVF)' if a.mutate=='claim-unlink' else old
    assert src.count(marker)==1
    src=src.replace(marker,'    after_claim();\n'+marker)
    out=Path(tempfile.mkdtemp(prefix='welyos-runq-overflow-'))
    (out/'runq.c').write_text(src)
    for p in paths[1:]: (out/Path(p).name).write_bytes(data[p])
    for name in ('cpu.h','spinlock.h'):
        (out/name).write_bytes((ROOT/'tests/host/stub'/name).read_bytes())
    (out/'probe.c').write_bytes((ROOT/'tests/host/runq_overflow_probe.c').read_bytes())
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',str(out/'probe.c'),'-o',str(out/'probe')],check=True)
    rows=[]
    for case in (1,2,3):
        p=subprocess.run([str(out/'probe'),str(case)],capture_output=True,text=True,timeout=10)
        rows.append(dict(case=case,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr))
        print(p.stdout.strip(),p.stderr[:500])
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in data.items()}
    changed=[p for p,h in hashes.items() if hashlib.sha256((root/p).read_bytes()).hexdigest()!=h]
    (out/'results.json').write_text(json.dumps(dict(results=rows,mutation=a.mutate,source_hashes=hashes,changed_inputs=changed,
        model='real ovf_add/claim helpers, explicit pthread barriers in inserted hooks; not a full kernel scheduler'),indent=2))
    print('artifacts:',out)
    return 2 if changed else int(any(r['returncode'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
