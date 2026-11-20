/* Actual VFS reference/offset and WFS write/flush wrappers, pthread locks;
 * RAM cache/durable media replace WFS block allocation and the device. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"
#include "file_type.h"
static pthread_mutex_t inode_lk=PTHREAD_MUTEX_INITIALIZER;
static struct mutex wfs_lk;
static int fs_failed,closed_calls,freed_calls;
static unsigned char cache[4096],durable[4096];
static unsigned length,durable_len;
static _Thread_local int wfs_owned;
static pthread_mutex_t event_lk=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv=PTHREAD_COND_INITIALIZER;
static int controlled,entered,allow_write,flush_attempted,flush_returned;
static unsigned long spin_lock_irqsave(pthread_mutex_t *m) {assert(!pthread_mutex_lock(m));return 0;}
static void spin_unlock_irqrestore(pthread_mutex_t *m,unsigned long f) {(void)f;assert(!pthread_mutex_unlock(m));}
static void mutex_lock(struct mutex *m) {
    if(m==&wfs_lk && controlled && !wfs_owned) {
        pthread_mutex_lock(&event_lk);
        if(entered) {flush_attempted=1;pthread_cond_broadcast(&event_cv);}
        pthread_mutex_unlock(&event_lk);
    }
    assert(!pthread_mutex_lock(&m->m));if(m==&wfs_lk)wfs_owned=1;
}
static void mutex_unlock(struct mutex *m) {if(m==&wfs_lk)wfs_owned=0;assert(!pthread_mutex_unlock(&m->m));}
static void kfree(void *p) {(void)p;__atomic_add_fetch(&freed_calls,1,__ATOMIC_SEQ_CST);}
static uint64_t timer_epoch(void *p) {(void)p;return 0;}
struct wnode {struct inode in;};
static struct wnode *wn(struct inode *in) {return (struct wnode*)in;}
static long file_write(struct wnode *n,const void *buf,size_t count,uint64_t off) {
    assert(wfs_owned);
    if(controlled) {
        pthread_mutex_lock(&event_lk);entered=1;pthread_cond_broadcast(&event_cv);
        while(!allow_write)pthread_cond_wait(&event_cv,&event_lk);
        pthread_mutex_unlock(&event_lk);
    }
    assert(off+count<=sizeof(cache));memcpy(cache+off,buf,count);
    if(off+count>length)length=(unsigned)(off+count);
    n->in.size=length;return (long)count;
}
static int bsync_opt(int do_durable) {
    assert(wfs_owned);
    if(fs_failed)return -E_IO;
    if(do_durable) {memcpy(durable,cache,length);durable_len=length;}
    return 0;
}
static void inode_closed(struct inode *in,int flags) {(void)in;(void)flags;__atomic_add_fetch(&closed_calls,1,__ATOMIC_SEQ_CST);}
static void inode_release(struct inode *in) {(void)in;abort();}
#include "functions.c"
static struct inode_ops ops={.write=wfs_write,.flush=wfs_flush,.closed=inode_closed,.release=inode_release};
static struct wnode node;
static struct file shared;
static int fail_write;
static void init(void) {
    assert(!pthread_mutex_init(&wfs_lk.m,NULL));
    assert(!pthread_mutex_init(&node.in.lock.m,NULL));
    assert(!pthread_mutex_init(&shared.lock.m,NULL));
    node.in.type=I_FILE;node.in.ops=&ops;node.in.refs=1;node.in.nlink=1;
    shared.in=&node.in;shared.flags=O_RDWR;shared.refs=1;
}
struct writer_args {struct file *f;char c;int n;};
static void *writer(void *ptr) {
    struct writer_args *a=ptr;
    for(int i=0;i<a->n;i++)if(vfs_write(a->f,&a->c,1)!=1)__atomic_store_n(&fail_write,1,__ATOMIC_SEQ_CST);
    vfs_close(a->f);return NULL;
}
static void *flusher(void *ptr) {
    struct file *f=ptr;int rc=vfs_fsync(f);assert(rc==0);
    pthread_mutex_lock(&event_lk);flush_returned=1;pthread_cond_broadcast(&event_cv);pthread_mutex_unlock(&event_lk);
    vfs_close(f);return NULL;
}
int main(int argc,char **argv) {
    if(argc!=2)return 90;
    init();
    if(!strcmp(argv[1],"concurrent")) {
        pthread_t a,b;struct writer_args aa={vfs_dup(&shared),'A',1000},bb={vfs_dup(&shared),'B',1000};
        assert(aa.f==bb.f && shared.refs==3);vfs_close(&shared);
        assert(!pthread_create(&a,NULL,writer,&aa));assert(!pthread_create(&b,NULL,writer,&bb));
        assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
        unsigned ca=0,cb=0;for(unsigned i=0;i<length;i++){ca+=cache[i]=='A';cb+=cache[i]=='B';}
        assert(!fail_write && shared.off==2000 && length==2000 && ca==1000 && cb==1000);
        assert(shared.refs==0 && closed_calls==1 && freed_calls==1 && node.in.refs==0);
    } else if(!strcmp(argv[1],"write-vs-sync")) {
        controlled=1;
        pthread_t a,b;struct writer_args aa={vfs_dup(&shared),'X',1};struct file *sync_ref=vfs_dup(&shared);
        assert(!pthread_create(&a,NULL,writer,&aa));
        pthread_mutex_lock(&event_lk);while(!entered)pthread_cond_wait(&event_cv,&event_lk);pthread_mutex_unlock(&event_lk);
        assert(!pthread_create(&b,NULL,flusher,sync_ref));
        pthread_mutex_lock(&event_lk);while(!flush_attempted)pthread_cond_wait(&event_cv,&event_lk);
        assert(!flush_returned && !durable_len);allow_write=1;pthread_cond_broadcast(&event_cv);pthread_mutex_unlock(&event_lk);
        assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
        assert(flush_returned && durable_len==1 && durable[0]=='X' && shared.off==1);
        assert(shared.refs==1 && !closed_calls && !freed_calls);vfs_close(&shared);
        assert(closed_calls==1 && freed_calls==1);
    } else if(!strcmp(argv[1],"sync-error")) {
        assert(vfs_write(&shared,"X",1)==1);fs_failed=1;
        assert(vfs_fsync(&shared)==-E_IO && vfs_flush(&shared)==-E_IO && !durable_len);
        assert(shared.refs==1);vfs_close(&shared);assert(closed_calls==1 && freed_calls==1);
    } else return 91;
    printf("%s PASS shared_off=%llu closed=%d freed=%d\n",argv[1],(unsigned long long)shared.off,closed_calls,freed_calls);
    return 0;
}
