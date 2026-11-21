#!/usr/bin/env python3
"""True fixed RAMFS plus membudget: metadata, write-failure and release checks."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#define PAGE_SIZE 4096UL
#define E_NOMEM 12
#define E_INVAL 22
#define E_NOSPC 28
#define E_NOENT 2
#define E_FBIG 27
#define FS_NAME_MAX 63
#define CAP_ADMIN 1
#define I_FILE 1
#define I_DIR 2
enum mb_class {MB_ANON,MB_STACK,MB_IMAGE,MB_PGTABLE,MB_KOBJ,MB_RAMFS,MB_NCLASS};
struct mb_domain;
struct mb_exec_txn {struct mb_domain *d;unsigned long pending;int active;const void *bound;};
struct mb_bundle {unsigned long quota[MB_NCLASS],reserve;};
struct mb_stats {unsigned long used[MB_NCLASS],total,limit;unsigned refs;};
struct spinlock {int x;};
#define SPINLOCK_INIT(n,l) {0}
static unsigned long spin_lock_irqsave(struct spinlock *l){(void)l;return 0;}
static void spin_unlock_irqrestore(struct spinlock *l,unsigned long f){(void)l;(void)f;}
static unsigned live,heap_calls,fail_heap_at,pmm_calls,fail_pmm_at,physical,extra_size;
static void *kzalloc(size_t n){if(++heap_calls==fail_heap_at)return NULL;size_t *p=calloc(1,sizeof(*p)+n);if(!p)abort();*p=sizeof(*p)+n+extra_size;live++;return p+1;}
static size_t kmalloc_size(void *p){return ((size_t*)p)[-1];}
static void kfree(void *p){if(p){live--;free((size_t*)p-1);}}
static void *pmm_alloc(void){if(++pmm_calls==fail_pmm_at)return NULL;void *p=malloc(PAGE_SIZE);if(p)physical++;return p;}
static void pmm_free(void *p){if(!p||!physical)abort();physical--;free(p);}
static unsigned long pmm_total_pages(void){return 10000;}
static unsigned long pmm_free_count(void){return 9000-physical;}
static int pmm_reserve(unsigned long n){(void)n;return 0;}
static void pmm_unreserve(unsigned long n){if(n)abort();}
static unsigned long mb_pages_for_bytes(unsigned long n){return (n+PAGE_SIZE-1)/PAGE_SIZE;}
static void panic(const char *s,...){fprintf(stderr,"panic %s\n",s);abort();}
struct cred {unsigned caps;};static struct cred current;static int no_cred;
static const struct cred *cred_current(void){return no_cred?NULL:&current;}
int mb_quota_add_txn(struct mb_domain*,enum mb_class,unsigned long,const struct mb_exec_txn*);
int mb_commit_bundle_txn(struct mb_domain*,const struct mb_bundle*,const struct mb_exec_txn*);
MB
struct inode;struct dirent {char name[FS_NAME_MAX+1];uint32_t type;};
struct fsinfo {unsigned long block_size,blocks,free_blocks;char type[16];};
OPS
struct inode {const struct inode_ops *ops;int type;uint64_t size;};
static void inode_init(struct inode *in,const struct inode_ops *ops,int type){in->ops=ops;in->type=type;}
RAMFS
#define CHECK(x,n) do{if(!(x)){printf("FAIL line %d code %d\n",__LINE__,n);return n;}}while(0)
static void counters(void){heap_calls=fail_heap_at=pmm_calls=fail_pmm_at=current.caps=no_cred=0;}
static int trial(unsigned round){extra_size=round*PAGE_SIZE;counters();struct mb_domain *d=mb_domain_new(10000);ramfs_set_domain(d);struct inode *root=ramfs_create_root();CHECK(root,1);unsigned long rbase=d->total;unsigned hbase=live;struct inode *out=NULL;
/* Every create metadata rejection preserves the directory and total. */
fail_heap_at=heap_calls+1;CHECK(ramfs_create(root,"f",I_FILE,&out)==-E_NOMEM&&!out&&!root->size&&d->total==rbase&&live==hbase,2);fail_heap_at=0;
mb_set_limit(d,rbase);CHECK(ramfs_create(root,"f",I_FILE,&out)==-E_NOSPC&&!out&&!root->size&&d->total==rbase&&live==hbase,3);
mb_set_limit(d,10000);fail_heap_at=heap_calls+2;CHECK(ramfs_create(root,"f",I_FILE,&out)==-E_NOSPC&&!out&&!root->size&&d->total==rbase&&live==hbase,4);fail_heap_at=0;
mb_set_limit(d,rbase+round+1);CHECK(ramfs_create(root,"f",I_FILE,&out)==-E_NOSPC&&!out&&!root->size&&d->total==rbase&&live==hbase,5);
mb_set_limit(d,10000);CHECK(!ramfs_create(root,"f",I_FILE,&out)&&out,6);struct inode *file=out;struct rnode *f=rn(file);unsigned long nodebase=d->total;
static unsigned char buf[3*PAGE_SIZE],readback[3*PAGE_SIZE];memset(buf,0x6b,sizeof(buf));
/* Link rejection and final-reference lifetime: unlink releases only dirents. */
mb_set_limit(d,d->total);CHECK(ramfs_link(root,"g",file)==-E_NOSPC&&root->size==1&&d->total==nodebase,7);
mb_set_limit(d,10000);CHECK(!ramfs_link(root,"g",file)&&root->size==2,8);struct inode *victim=NULL;CHECK(!ramfs_unlink(root,"g",&victim)&&victim==file&&root->size==1&&d->total==nodebase,9);
/* A failed first physical page may retain a newly successful metadata table, but no data charge or EOF. */
fail_pmm_at=pmm_calls+1;CHECK(ramfs_write(file,buf,17,2*PAGE_SIZE)==-E_NOMEM&&file->size==0&&!physical&&!ramfs_pages()&&d->total==nodebase+obj_pages(f->blocks),10);fail_pmm_at=0;
unsigned long tablebase=d->total;unsigned oldlive=live;void **old=f->blocks;size_t oldcap=f->nblocks;
fail_heap_at=heap_calls+1;CHECK(grow_table(f,600)==-E_NOMEM&&f->blocks==old&&f->nblocks==oldcap&&d->total==tablebase&&live==oldlive,11);fail_heap_at=0;
mb_set_limit(d,tablebase);CHECK(grow_table(f,600)==-E_NOSPC&&f->blocks==old&&f->nblocks==oldcap&&d->total==tablebase&&live==oldlive,12);
/* Partial quota write: exactly one data page can be charged. */
mb_set_limit(d,tablebase+1);CHECK(ramfs_write(file,buf,sizeof(buf),0)==PAGE_SIZE&&file->size==PAGE_SIZE&&physical==1&&ramfs_pages()==1&&d->total==tablebase+1,13);
CHECK(ramfs_write(file,buf,1,PAGE_SIZE)==-E_NOSPC&&file->size==PAGE_SIZE&&physical==1,14);
/* Existing-page overwrites need no positive quota, even after lowering limit. */
mb_set_limit(d,0);CHECK(ramfs_write(file,buf,1,0)==1&&d->total==tablebase+1,15);
/* Administrator and no-credential kernel callers bypass limit but remain charged. */
current.caps=CAP_ADMIN;CHECK(ramfs_write(file,buf,1,PAGE_SIZE)==1&&physical==2&&d->total==tablebase+2,16);current.caps=0;no_cred=1;CHECK(ramfs_write(file,buf,1,2*PAGE_SIZE)==1&&physical==3&&d->total==tablebase+3,17);no_cred=0;
struct fsinfo fi;CHECK(!ramfs_statfs(file,&fi)&&!fi.free_blocks,18);
/* Growth preserves all page pointers and charges only the final new table after replacement. */
mb_set_limit(d,10000);unsigned long oldcost=obj_pages(f->blocks),before=d->total;void *pg=f->blocks[0];CHECK(!grow_table(f,600)&&f->blocks[0]==pg&&d->total==before-oldcost+obj_pages(f->blocks),19);
/* Shrink zeroes the retained page tail; metadata remains until release. */
CHECK(!ramfs_truncate(file,9)&&physical==1&&ramfs_pages()==1&&file->size==9,20);for(unsigned i=9;i<PAGE_SIZE;i++)CHECK(((unsigned char*)f->blocks[0])[i]==0,21);
CHECK(!ramfs_truncate(file,0)&&!physical&&!ramfs_pages()&&d->total==nodebase+obj_pages(f->blocks),22);
/* Sparse hole readback and partial PMM failure. */
memset(readback,0x3c,sizeof(readback));CHECK(ramfs_write(file,buf,17,2*PAGE_SIZE)==17&&ramfs_read(file,readback,sizeof(readback),0)==2*PAGE_SIZE+17,23);for(unsigned i=0;i<2*PAGE_SIZE;i++)CHECK(readback[i]==0,24);
ramfs_truncate(file,0);fail_pmm_at=pmm_calls+2;CHECK(ramfs_write(file,buf,sizeof(buf),0)==PAGE_SIZE&&physical==1&&file->size==PAGE_SIZE,25);fail_pmm_at=0;
CHECK(!ramfs_unlink(root,"f",&victim)&&victim==file&&root->size==0&&physical==1,26); /* caller still holds inode */
ramfs_release(file);CHECK(!physical&&!ramfs_pages()&&d->total==rbase,27);ramfs_release(root);CHECK(d->total==0&&live==1&&!errors,28);ramfs_set_domain(NULL);CHECK(!mb_domain_put(d)&&!live,29);return 0;}
int main(void){for(unsigned round=0;round<2;round++){int rc=trial(round);if(rc)return rc;}puts("PASS 2 object-rounding costs: metadata alloc/quota rollback, grow failure/success, link/unlink lifetime, partial quota/PMM writes, EOF, sparse holes, truncate/release, admin/kernel bypass and charging");return 0;}
'''

def main():
    bundle=ROOT/'build/milestones/c32-d-ramfs';patch=(bundle/'source.patch').read_bytes();out=Path(tempfile.mkdtemp(prefix='welyos-d-ramfs-host-'))
    for p in re.findall(r'^diff --git a/(\S+) b/\S+$',patch.decode(),re.M):
        r=subprocess.run(['git','show','fbdfbe4:'+p],cwd=ROOT,capture_output=True,check=True);f=out/p;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(r.stdout)
    subprocess.run(['patch','-p1','--batch','--fuzz=0','-i',str(bundle/'source.patch')],cwd=out,capture_output=True,check=True)
    paths=['kernel/ramfs.c','kernel/membudget.c','include/fs.h'];src={p:(out/p).read_text() for p in paths}
    expected=dict((p,h) for h,p in re.findall(r'^  ([a-f0-9]{64})  (\S+)$',(bundle/'manifest.txt').read_text(),re.M));hashes={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()}
    if any(hashes[p]!=expected[p] for p in paths):raise RuntimeError('frozen source mismatch')
    ops=re.search(r'struct inode_ops \{.*?\n\};',src['include/fs.h'],re.S).group()
    mb='\n'.join(x for x in src['kernel/membudget.c'].splitlines() if not x.startswith('#include'))
    ramfs='\n'.join(x for x in src['kernel/ramfs.c'].splitlines() if not x.startswith('#include'))
    body=PRE.replace('MB\n',mb+'\n').replace('OPS\n',ops+'\n').replace('RAMFS\n',ramfs+'\n')
    variants={'baseline':(body,0),'omit-data-failure-uncharge':(body.replace('if (!page) {\n                fs_uncharge(1);','if (!page) {'),10),'omit-table-old-uncharge':(body.replace('fs_uncharge(obj_pages(f->blocks));','/* mutation */'),19),'omit-dirent-uncharge':(body.replace('fs_uncharge(obj_pages(e));\n        kfree(e);\n        d->in.size--;','/* mutation */\n        kfree(e);\n        d->in.size--;'),9)}
    rows=[]
    for name,(code,expect) in variants.items():
        if name!='baseline' and code==body:raise RuntimeError('mutation missed')
        c=out/(name+'.c');c.write_text(code);exe=c.with_suffix('');cp=subprocess.run(['cc','-std=c11','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30) if cp.returncode==0 else cp
        row=dict(variant=name,compile_exit=cp.returncode,exit=r.returncode,expected_exit=expect,passed=cp.returncode==0 and r.returncode==expect,stdout=r.stdout,stderr=r.stderr);rows.append(row);print(row)
    (out/'results.json').write_text(json.dumps(dict(source_hashes=hashes,patch_sha256=hashlib.sha256(patch).hexdigest(),cases=rows),indent=2)+'\n');print('artifacts:',out);return not all(r['passed'] for r in rows)

if __name__=='__main__':raise SystemExit(main())
