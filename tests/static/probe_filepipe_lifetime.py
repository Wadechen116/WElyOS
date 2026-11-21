#!/usr/bin/env python3
"""Extract unchanged object-accounting/file/pipe constructors and finalizers."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
from probe_pte_ticket import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define PIPE_SIZE 4096u
#define MB_NCLASS 1
#define MB_REFS_MAX 0xFFFFFFF0u
#define E_NOMEM 12
#define E_INVAL 22
#define O_ACCMODE 3
#define O_RDONLY 0
#define O_WRONLY 1
#define I_PIPE 3
#define MX_FILE 0
enum mb_class { MB_KOBJ };
struct mutex {int unused;}; struct wait_queue {int unused;};
struct inode;
struct inode_ops {void (*closed)(struct inode*,int);void (*release)(struct inode*);};
struct inode {const struct inode_ops *ops;unsigned refs,nlink;};
static unsigned long errors; static int mb_lk,inode_lk;
static unsigned long spin_lock_irqsave(int *l){(void)l;return 0;}
static void spin_unlock_irqrestore(int *l,unsigned long f){(void)l;(void)f;}
static unsigned long sched_lock(void){return 0;}
static void sched_unlock(unsigned long f){(void)f;}
static void wq_wake_all(struct wait_queue *q){(void)q;}
static void mutex_init(struct mutex *m,const char *s,int c){(void)m;(void)s;(void)c;}
static void panic(const char *s){fprintf(stderr,"panic %s\n",s);exit(90);}
struct alloc {size_t n;max_align_t align;};
static unsigned calls,fail_at,live;static size_t extra;
static void *kzalloc(size_t n){if(++calls==fail_at)return NULL;struct alloc *a=calloc(1,sizeof(*a)+n+extra);if(!a)abort();a->n=n+extra;live++;return a+1;}
static size_t kmalloc_size(void *p){return ((struct alloc*)p-1)->n;}
static void kfree(void *p){if(p){live--;free((struct alloc*)p-1);}}
static struct mb_domain *current;
static struct mb_domain *thread_domain(void){return current;}
static void inode_init(struct inode *i,const struct inode_ops *o,int t){(void)t;memset(i,0,sizeof(*i));i->ops=o;}
'''
MAIN=r'''
int main(void){unsigned cases=0;
for(unsigned pad=0;pad<2;pad++)for(unsigned limit=0;limit<12;limit++)
for(unsigned fail=0;fail<4;fail++)for(unsigned kernel=0;kernel<2;kernel++)
for(unsigned order=0;order<2;order++)for(unsigned dups=0;dups<4;dups++){
 extra=pad*4096;calls=fail_at=live=errors=0;
 struct mb_domain *d=kzalloc(sizeof(*d));d->refs=1;d->limit=limit;current=kernel?NULL:d;
 calls=0;fail_at=fail;struct file *r=NULL,*w=NULL;int rc=vfs_pipe(&r,&w);
 unsigned long kp=(sizeof(struct pipe)+extra+4095)/4096;
 unsigned long kf=(sizeof(struct file)+extra+4095)/4096;
 int expected=fail==0 && (kernel || limit>=kp+2*kf);
 if((rc==0)!=expected)return 10;
 if(rc){if(d->refs!=1 || d->total || d->used[0] || live!=1 || errors)return 11;mb_domain_put(d);}
 else{
  if(d->refs!=(kernel?1u:4u) || d->total!=(kernel?0:kp+2*kf) || live!=4)return 12;
  for(unsigned i=0;i<dups;i++){vfs_dup(r);vfs_dup(w);}
  if(d->refs!=(kernel?1u:4u) || d->total!=(kernel?0:kp+2*kf))return 13;
  /* Creator exits, then another domain closes inherited descriptors. */
  struct mb_domain closer={.refs=1,.limit=100};current=&closer;mb_domain_put(d);
  for(unsigned i=0;i<dups;i++){vfs_close(r);vfs_close(w);}
  if(order){vfs_close(w);vfs_close(r);}else{vfs_close(r);vfs_close(w);}
  if(closer.refs!=1 || closer.total || closer.used[0])return 14;
 }
 if(live || errors)return 14;
 cases++;
}
/* Saturated domain reference count: charge fails before quota changes. */
extra=0;calls=fail_at=live=errors=0;struct mb_domain *d=kzalloc(sizeof(*d));
d->refs=MB_REFS_MAX;d->limit=100;current=d;struct file *r=NULL,*w=NULL;
if(vfs_pipe(&r,&w)==0 || d->refs!=MB_REFS_MAX || d->total || live!=1)return 15;
d->refs=1;mb_domain_put(d);if(live)return 16;
printf("PASS %u pipe construction/failure/dup/inherited-close cases plus ref saturation\n",cases);return 0;}
'''
def main():
 paths=['kernel/vfs.c','kernel/pipe.c','kernel/membudget.c','include/membudget.h']
 src={p:(ROOT/p).read_text() for p in paths};v,p,m,h=[src[x] for x in paths]
 structs='\n'.join(re.search(r'struct '+name+r' \{.*?\n\};',s,re.S).group() for name,s in [('mb_domain',m),('file',v),('pipe',p)])
 code=PRE+structs+'\n'+function(h,'mb_pages_for_bytes')+'\n'
 code+='\n'.join(function(m,n) for n in ['mb_domain_get','mb_domain_put','fits_locked','mb_quota_add','sub_locked','mb_quota_sub','mb_charge_object','mb_release_object'])+'\n'
 code+='\n'.join(function(v,n) for n in ['iget','iput','vfs_file_new','vfs_dup','vfs_close'])+'\n'
 code+='\n'.join(function(p,n) for n in ['pp','pipe_closed','pipe_free','pipe_release'])+'\n'
 code+='static const struct inode_ops pipe_ops={.closed=pipe_closed,.release=pipe_release};\n'+function(p,'vfs_pipe')+'\n'+MAIN
 variants={'none':code,'file-release-omitted':code.replace('f->dom && mb_release_object(f->dom, f->kobj) != 0','0'), 'pipe-release-omitted':code.replace('p->dom && mb_release_object(p->dom, p->kobj) != 0','0'),'charge-fail-ref-leak':code.replace('mb_domain_put(d);                    /* 剛取得的參考,不可能是最後一份 */','/* mutation */')}
 out=Path(tempfile.mkdtemp(prefix='welyos-filepipe-lifetime-'));rows=[]
 for name,body in variants.items():
  if name!='none' and body==code:raise RuntimeError('mutation missed '+name)
  c=out/(name+'.c');c.write_text(body);exe=c.with_suffix('')
  cp=subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
  if cp.returncode:raise RuntimeError(cp.stderr)
  run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
  ok=run.returncode==0 if name=='none' else run.returncode in range(10,17)
  rows.append(dict(variant=name,passed=ok,exit=run.returncode,stdout=run.stdout,stderr=run.stderr));print(name,ok,run.returncode,run.stdout.strip())
 changed=[p for p,s in src.items() if (ROOT/p).read_text()!=s]
 (out/'results.json').write_text(json.dumps(dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()},changed_inputs=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_inputs:',changed)
 return int(bool(changed) or not all(r['passed'] for r in rows))
if __name__=='__main__':raise SystemExit(main())
