#!/usr/bin/env python3
"""Reproduce zero-progress RAMFS EOF growth using extracted production code.

Exit 0 confirms the reported behavior and guarded comparison; it is not product
acceptance. Heap/PMM are fixtures. No VFS, real PMM, quota, or SMP execution.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096u
#define RAMFS_MAX_BLOCKS 4096u
#define E_NOMEM 12
#define E_FBIG 27
struct inode {uint64_t size;};
struct rnode {struct inode in;struct rnode *parent;void *ents;void **blocks;size_t nblocks;};
static unsigned long data_pages;
static unsigned pmm_calls,fail_page,heap_live,page_live;
static struct rnode *rn(struct inode *in){return (struct rnode*)in;}
static void *kzalloc(size_t n){void *p=calloc(1,n);if(!p)abort();heap_live++;return p;}
static void kfree(void *p){if(p){if(!heap_live)abort();heap_live--;free(p);}}
static void *pmm_alloc(void){if(++pmm_calls==fail_page)return NULL;void *p=malloc(PAGE_SIZE);if(!p)abort();page_live++;return p;}
static void cleanup(struct rnode *f){for(size_t i=0;i<f->nblocks;i++)if(f->blocks[i]){free(f->blocks[i]);page_live--;data_pages--;}kfree(f->blocks);if(heap_live||page_live||data_pages)abort();}
'''
HARNESS = r'''
int main(void){
 unsigned char src[5000],out[5000];memset(src,0xA7,sizeof src);
 struct rnode f={0};long r=ramfs_write(&f.in,src,5000,17);
 if(r!=5000||f.in.size!=5017||data_pages!=2)return 10;
 if(ramfs_read(&f.in,out,5000,17)!=5000||memcmp(out,src,5000))return 11;
 for(unsigned i=0;i<17;i++)if(((unsigned char*)f.blocks[0])[i])return 12;
 printf("NORMAL ret=%ld size=%llu pages=%lu\n",r,(unsigned long long)f.in.size,data_pages);cleanup(&f);
 const uint64_t offsets[]={4096,8192,12288};
 for(unsigned i=0;i<3;i++){
  memset(&f,0,sizeof f);f.in.size=8192;pmm_calls=0;fail_page=1;
  r=ramfs_write(&f.in,src,20,offsets[i]);
  uint64_t expect=i==2&&!GUARDED?offsets[i]:8192;
  if(r!=-E_NOMEM||f.in.size!=expect||data_pages||page_live)return 20+i;
  for(size_t b=0;b<f.nblocks;b++)if(f.blocks[b])return 23;
  printf("ZERO_PROGRESS off=%llu before=8192 ret=%ld after=%llu data_pages=%lu table_slots=%zu\n",(unsigned long long)offsets[i],r,(unsigned long long)f.in.size,data_pages,f.nblocks);cleanup(&f);
 }
 memset(&f,0,sizeof f);pmm_calls=0;fail_page=2;
 r=ramfs_write(&f.in,src,20,4090);
 if(r!=6||f.in.size!=4096||data_pages!=1||f.blocks[1])return 30;
 if(memcmp((unsigned char*)f.blocks[0]+4090,src,6))return 31;
 printf("PARTIAL off=4090 ret=%ld size=%llu pages=%lu\n",r,(unsigned long long)f.in.size,data_pages);cleanup(&f);
 puts(GUARDED?"CONTROL PASS: guarded EOF update keeps zero-progress size unchanged":"REPRODUCED: zero-progress beyond EOF changes size; normal and partial controls pass");return 0;
}
'''


def main():
    path = ROOT / 'kernel/ramfs.c'
    src = path.read_text()
    code = '\n'.join(function(src, n) for n in ['ramfs_read', 'grow_table', 'ramfs_write'])
    needle = 'if (off + done > in->size)'
    assert code.count(needle) == 1
    variants = {'production': code, 'guarded-comparison': code.replace(
        needle, 'if (done && off + done > in->size)')}
    out = Path(tempfile.mkdtemp(prefix='welyos-ramfs-write-current-'))
    rows = []
    for name, body in variants.items():
        guarded = int(name != 'production')
        c = out / (name + '.c')
        c.write_text(f'#define GUARDED {guarded}\n' + PRE + body + HARNESS)
        exe = c.with_suffix('')
        cc = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall',
                             '-fsanitize=undefined', '-fno-sanitize-recover=all',
                             str(c), '-o', str(exe)], capture_output=True, text=True)
        row = dict(variant=name, compile_exit=cc.returncode, compiler_stderr=cc.stderr,
                   passed=False)
        if cc.returncode == 0:
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            row.update(exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
            row['passed'] = run.returncode == 0 and not run.stderr
        rows.append(row)
        print(row)
    changed = path.read_text() != src
    result = dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),
                  changed_input=changed, cases_per_variant=5, cases=rows,
                  exit_zero_means='counterexample and guarded comparison reproduced, not product acceptance')
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('artifacts:', out, 'changed_input:', changed)
    return int(changed or not all(r['passed'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
