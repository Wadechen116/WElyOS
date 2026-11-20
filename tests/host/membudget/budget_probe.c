#define _XOPEN_SOURCE 700
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct spinlock;
static _Thread_local struct spinlock *held_locks[4];
static _Thread_local unsigned depth,lock_calls;
static void audit_lock(struct spinlock *s);
static jmp_buf panic_env;
static int expect_panic,fail_heap;
static unsigned heap_live;
static void *kzalloc(size_t n) {
    assert(!depth);if(fail_heap)return NULL;
    void *p=calloc(1,n);if(p)__atomic_fetch_add(&heap_live,1,__ATOMIC_RELAXED);return p;
}
static void kfree(void *p) {assert(!depth);if(p){__atomic_fetch_sub(&heap_live,1,__ATOMIC_RELAXED);free(p);}}
#include "pmm.c"
#include "membudget.c"
char __kernel_start[1],__kernel_end[1];
static uint8_t bits[8];static uint16_t refbuf[64];
static _Alignas(4096) unsigned char arena[64*4096];
static void audit_lock(struct spinlock *s) {
    assert(depth&&held_locks[depth-1]==s);
    if(s==&pmm_lk) {
        size_t count=0;
        for(size_t i=0;i<npages;i++){assert(!!bit_test(i)==!!refs[i]);count+=!bit_test(i);if(i<hint)assert(bit_test(i));}
        assert(count==nfree&&nreserved<=nfree&&hint<=npages);
    }
}
static void reset_pmm(size_t count,size_t floor,size_t r) {
    assert(!depth);npages=nfree=count;hint=0;bitmap=bits;refs=refbuf;mem_start=(uintptr_t)arena;
    memset(bits,0,sizeof(bits));memset(refbuf,0,sizeof(refbuf));floor_on=floor!=0;floor_pages=floor;nreserved=r;floor_refusals=0;
}
static struct mb_stats view(struct mb_domain *d) {
    struct mb_stats s;mb_stats(d,&s);unsigned long sum=0;
    for(int i=0;i<MB_NCLASS;i++){assert(s.used[i]<=ULONG_MAX-sum);sum+=s.used[i];}
    assert(sum==s.total&&s.refs);return s;
}
static void stats_equal(struct mb_stats a,struct mb_stats b) {
    assert(a.total==b.total&&a.limit==b.limit&&a.refs==b.refs);
    for(int i=0;i<MB_NCLASS;i++)assert(a.used[i]==b.used[i]);
}
static void seed(struct mb_domain *d,unsigned long a,unsigned long b,unsigned long limit) {
    memset(d->used,0,sizeof(d->used));d->used[MB_ANON]=a;d->used[MB_PGTABLE]=b;d->total=a+b;d->limit=limit;d->refs=1;
}
static void scalar_exhaustive(void) {
    reset_pmm(64,8,0);struct mb_domain *d=mb_domain_new(0);assert(d);size_t cases=0;
    for(unsigned long a=0;a<=6;a++)for(unsigned long b=0;b<=6;b++)for(unsigned long limit=0;limit<=12;limit++)
    for(int c=0;c<MB_NCLASS;c++)for(unsigned long n=0;n<=16;n++)for(int op=0;op<2;op++) {
        seed(d,a,b,limit);struct mb_stats before=view(d);unsigned long old_errors=mb_accounting_errors();
        if(!op) {
            int ok=!n||(a+b<=limit&&n<=limit-a-b);
            assert(mb_quota_add(d,(enum mb_class)c,n)==(ok?0:-E_NOMEM));
            if(ok){before.total+=n;before.used[c]+=n;}
            assert(mb_accounting_errors()==old_errors);
        } else {
            unsigned long take=n<before.used[c]?n:before.used[c];int over=n>before.used[c];
            assert(mb_quota_sub(d,(enum mb_class)c,n)==(over?-1:0));
            before.total-=take;before.used[c]-=take;assert(mb_accounting_errors()==old_errors+(unsigned)over);
        }
        stats_equal(before,view(d));cases++;
    }
    seed(d,0,0,0);assert(!mb_domain_put(d));printf("scalar PASS %zu add/sub/limit states\n",cases);
}
static void bundle_exhaustive(void) {
    struct mb_domain *d=mb_domain_new(0);assert(d);size_t cases=0;
    for(unsigned long a=0;a<=4;a++)for(unsigned long b=0;b<=4;b++)for(unsigned long limit=0;limit<=10;limit++)
    for(unsigned long x=0;x<=5;x++)for(unsigned long y=0;y<=5;y++)for(unsigned long r=0;r<=14;r++) {
        reset_pmm(16,4,3);seed(d,a,b,limit);
        struct mb_bundle bundle={.quota={[MB_ANON]=x,[MB_PGTABLE]=y},.reserve=r};
        struct mb_stats before=view(d);unsigned long sum=x+y;
        int ok=(!sum||(a+b<=limit&&sum<=limit-a-b))&&r<=9;
        assert(mb_commit_bundle(d,&bundle)==(ok?0:-E_NOMEM));
        struct mb_stats after=before;
        if(ok){after.total+=sum;after.used[MB_ANON]+=x;after.used[MB_PGTABLE]+=y;}
        stats_equal(after,view(d));assert(nreserved==3+(ok?r:0)&&nfree==16);
        if(ok){assert(!mb_release_bundle(d,&bundle));stats_equal(before,view(d));assert(nreserved==3);}
        cases++;
    }
    reset_pmm(64,8,0);seed(d,0,0,0);assert(!mb_domain_put(d));printf("bundle PASS %zu multi-class/PMM-capacity/rollback states\n",cases);
}
static void edges(void) {
    reset_pmm(64,8,0);struct mb_domain *d=mb_domain_new(ULONG_MAX);assert(d);
    assert(!mb_quota_add(d,MB_ANON,ULONG_MAX));assert(mb_quota_add(d,MB_STACK,1)==-E_NOMEM);
    assert(!mb_quota_add(d,MB_STACK,0));mb_set_limit(d,0);assert(mb_limit(d)==0);
    assert(mb_quota_add(d,MB_ANON,1)==-E_NOMEM);assert(!mb_quota_sub(d,MB_ANON,ULONG_MAX));
    mb_set_limit(d,ULONG_MAX);struct mb_stats before=view(d);
    struct mb_bundle bad={.quota={[MB_ANON]=ULONG_MAX,[MB_STACK]=1}};
    assert(mb_commit_bundle(d,&bad)==-E_NOMEM);stats_equal(before,view(d));assert(!nreserved);
    bad=(struct mb_bundle){.quota={[MB_ANON]=1},.reserve=ULONG_MAX};
    assert(mb_commit_bundle(d,&bad)==-E_NOMEM);stats_equal(before,view(d));assert(!nreserved);
    for(int c=-1;c<=MB_NCLASS;c+=MB_NCLASS+1) {
        assert(mb_quota_add(d,(enum mb_class)c,0)==-E_INVAL);
        assert(mb_quota_sub(d,(enum mb_class)c,1)==-E_INVAL);stats_equal(before,view(d));
    }
    d->refs=MB_REFS_MAX-1;assert(!mb_domain_get(d));assert(d->refs==MB_REFS_MAX);
    before=view(d);assert(mb_domain_get(d)==-E_NOMEM);stats_equal(before,view(d));d->refs=1;
    assert(!mb_domain_get(d));assert(!mb_domain_put(d));assert(view(d).refs==1);
    assert(!mb_domain_put(d));assert(!heap_live);
    fail_heap=1;assert(!mb_domain_new(1));fail_heap=0;
    d=mb_domain_new(10);assert(d);assert(!mb_quota_add(d,MB_IMAGE,1));
    unsigned long e=mb_accounting_errors();assert(mb_domain_put(d)==-1&&mb_accounting_errors()==e+1);assert(!heap_live);
    puts("edges PASS ULONG_MAX/class/refcount/allocation-failure/last-reference");
}
static void bad_release(void) {
    reset_pmm(64,8,0);struct mb_domain *d=mb_domain_new(10);assert(d);
    struct mb_bundle own={.quota={[MB_ANON]=2,[MB_PGTABLE]=1},.reserve=3};assert(!mb_commit_bundle(d,&own));
    struct mb_stats before=view(d);unsigned long e=mb_accounting_errors();
    struct mb_bundle bad={.quota={[MB_ANON]=1,[MB_RAMFS]=1},.reserve=1};
    assert(mb_release_bundle(d,&bad)==-1);stats_equal(before,view(d));assert(nreserved==3&&mb_accounting_errors()==e+1);
    assert(!mb_release_bundle(d,&own));assert(!mb_domain_put(d));puts("release-error PASS rejects whole invalid bundle without releasing other credits");
}
static void pmm_release_error(void) {
    reset_pmm(64,8,0);struct mb_domain *d=mb_domain_new(10);assert(d);
    struct mb_bundle own={.quota={[MB_ANON]=2},.reserve=3};assert(!mb_commit_bundle(d,&own));
    struct mb_stats before=view(d);struct mb_bundle bad={.quota={[MB_ANON]=1},.reserve=4};expect_panic=1;
    if(!setjmp(panic_env)){(void)mb_release_bundle(d,&bad);assert(!"excessive global unreserve must stop");}
    expect_panic=0;
    /* Real panic is terminal. Before releasing stub locks, check state directly. */
    assert(nreserved==3&&d->total==before.total&&d->used[MB_ANON]==before.used[MB_ANON]);
    while(depth){struct spinlock *s=held_locks[--depth];assert(!pthread_mutex_unlock(&s->mutex));}
    assert(!mb_release_bundle(d,&own));assert(!mb_domain_put(d));puts("release-pmm-error PASS global underflow stops before quota mutation");
}
#define THREADS 8
static struct mb_domain *shared;
static void *worker(void *arg) {
    struct mb_bundle bundles[8]={0};void *pages[8]={0};int active[8]={0};
    uint32_t rng=(uint32_t)(uintptr_t)arg+1;
    for(int k=0;k<15000;k++) {
        rng=rng*1664525u+1013904223u;unsigned i=(rng>>16)%8;
        assert(!mb_domain_get(shared));
        if(active[i]) {
            if(pages[i]){pmm_free(pages[i]);pages[i]=NULL;}
            assert(!mb_release_bundle(shared,&bundles[i]));active[i]=0;
        } else {
            unsigned n=1+(rng>>8)%4;
            bundles[i]=(struct mb_bundle){.quota={[MB_ANON]=n,[MB_PGTABLE]=1},.reserve=n+1};
            if(!mb_commit_bundle(shared,&bundles[i])) {
                active[i]=1;
                if(rng&64){pages[i]=pmm_alloc_reserved();bundles[i].reserve--;}
            }
        }
        if(!((uintptr_t)arg)&&!(k%17))mb_set_limit(shared,(rng>>16)%97);
        (void)view(shared);struct pmm_snapshot s;pmm_snapshot(&s);assert(s.free-s.reserved>=s.floor);
        assert(!mb_domain_put(shared));
    }
    for(int i=0;i<8;i++)if(active[i]){if(pages[i])pmm_free(pages[i]);assert(!mb_release_bundle(shared,&bundles[i]));}
    return NULL;
}
static void parallel(void) {
    reset_pmm(64,8,0);shared=mb_domain_new(96);assert(shared);unsigned long e=mb_accounting_errors();
    pthread_t threads[THREADS];for(uintptr_t i=0;i<THREADS;i++)assert(!pthread_create(&threads[i],NULL,worker,(void*)i));
    for(int i=0;i<THREADS;i++)assert(!pthread_join(threads[i],NULL));
    struct mb_stats s=view(shared);assert(!s.total&&s.refs==1&&nfree==64&&!nreserved&&mb_accounting_errors()==e);
    assert(!mb_domain_put(shared)&&!heap_live);puts("parallel PASS 8 threads, 120000 get/commit/consume/release/limit/stats/put iterations");
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"scalar"))scalar_exhaustive();else if(!strcmp(argv[1],"bundle"))bundle_exhaustive();
    else if(!strcmp(argv[1],"edges"))edges();else if(!strcmp(argv[1],"release-error"))bad_release();
    else if(!strcmp(argv[1],"release-pmm-error"))pmm_release_error();else if(!strcmp(argv[1],"parallel"))parallel();else return 2;
    assert(!heap_live&&!depth);return 0;
}
