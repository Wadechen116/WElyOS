#define _XOPEN_SOURCE 700
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
static _Thread_local unsigned held, lock_calls;
static int check_floor;
static void check_locked(void);
#include "pmm.c"
char __kernel_start[1], __kernel_end[1];
static uint8_t bits[128];
static uint16_t counts[1024];
static _Alignas(4096) unsigned char arena[1024*4096];
static size_t checks;
static void check_locked(void) {
    assert(held==1);
    size_t freep=0;
    for(size_t i=0;i<npages;i++) {
        assert(!!bit_test(i)==!!refs[i]);
        freep+=!bit_test(i);
        if(i<hint)assert(bit_test(i));
    }
    assert(freep==nfree && hint<=npages);
    if(check_floor&&floor_on)assert(nfree>=floor_pages);
}
static void reset(size_t n, unsigned mask) {
    npages=n;bitmap=bits;refs=counts;mem_start=(uintptr_t)arena;
    memset(bits,0,sizeof(bits));memset(counts,0,sizeof(counts));
    nfree=0;hint=n;floor_on=0;floor_pages=0;floor_refusals=0;
    for(size_t i=0;i<n;i++) {
        if(i<32 && (mask&(1u<<i))){bit_set(i);refs[i]=1;}
        else {nfree++;if(hint==n)hint=i;}
    }
}
struct snapshot { uint8_t b[128];uint16_t r[1024];size_t freep,h; };
static struct snapshot snapshot(void) {
    struct snapshot s={.freep=nfree,.h=hint};
    memcpy(s.b,bits,sizeof(bits));memcpy(s.r,counts,sizeof(counts));return s;
}
static void unchanged(struct snapshot *s) {
    assert(s->freep==nfree && s->h==hint);
    assert(!memcmp(s->b,bits,sizeof(bits))&&!memcmp(s->r,counts,sizeof(counts)));
}
static void exhaustive(void) {
    for(size_t total=1;total<=8;total++)for(unsigned mask=0;mask<(1u<<total);mask++) {
        size_t freep=total-(size_t)__builtin_popcount(mask);
        for(size_t floor=0;floor<=freep;floor++)for(size_t q=0;q<=total+3;q++) {
            reset(total,mask);check_floor=1;
            if(floor)assert(!pmm_reserve_enable(floor));
            size_t n=q==total+3?SIZE_MAX:q;
            struct snapshot before=snapshot();
            size_t first=total,run=0;
            if(n && n<=freep && freep-n>=floor)for(size_t i=0;i<total;i++) {
                run=(mask&(1u<<i))?0:run+1;
                if(run==n){first=i+1-n;break;}
            }
            void *p=pmm_alloc_pages(n);
            assert(!!p==(first!=total));
            if(!p)unchanged(&before);
            else {
                assert(p==arena+first*PAGE_SIZE && nfree==freep-n);
                for(size_t i=first;i<first+n;i++)assert(refs[i]==1&&bit_test(i));
                pmm_free_pages(p,n);assert(nfree==freep);
                assert(!memcmp(bits,before.b,sizeof(bits))&&!memcmp(counts,before.r,sizeof(counts)));
            }
            assert(floor_refusals==(n && n<=freep && freep-n<floor?1ul:0ul));checks++;
        }
    }
    printf("exhaustive PASS %zu states/requests (includes fragmented runs and SIZE_MAX)\n",checks);
}
static void boundaries(void) {
    reset(16,0);check_floor=1;
    assert(pmm_reserve_enable(0)==-1&&pmm_reserve_enable(17)==-1);
    assert(!floor_on&&!floor_pages);assert(!pmm_reserve_enable(4));
    assert(pmm_reserve_enable(1)==-1&&pmm_reserve_enable(16)==-1);
    void *p=pmm_alloc_pages(11);assert(p&&nfree==5);
    struct snapshot before=snapshot();assert(!pmm_alloc_pages(2));unchanged(&before);
    void *last=pmm_alloc();assert(last&&nfree==4);assert(!pmm_alloc());
    pmm_ref(last);assert(pmm_refcount(last)==2);pmm_free(last);assert(nfree==4);
    pmm_free(last);assert(nfree==5);pmm_free_pages(p,11);assert(nfree==16);
    check_floor=0;void *all=pmm_alloc_pages_reserve(16);assert(all&&nfree==0);
    assert(!pmm_alloc_pages_reserve(1)&&!pmm_alloc_pages_reserve(SIZE_MAX));
    pmm_free_pages(all,16);check_floor=1;assert(nfree==16);
    reset(8,0);assert(!pmm_reserve_enable(8));assert(!pmm_alloc());
    reset(8,0);void *boot=pmm_alloc_pages(8);assert(boot&&nfree==0);pmm_free_pages(boot,8);
    puts("boundaries PASS enable/exhaustion/references/emergency/pre-enable");
}
#define THREADS 8
#define ROUNDS 1000
static pthread_barrier_t barrier;
static void *wins[THREADS];
static void barrier_wait(void) {int r=pthread_barrier_wait(&barrier);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *contender(void *arg) {
    size_t id=(uintptr_t)arg;
    for(int r=0;r<ROUNDS;r++) {
        barrier_wait();wins[id]=pmm_alloc_pages(2);
        assert(pmm_reserve_refusals()<=(unsigned long)(r+1)*(THREADS-1));
        barrier_wait();
        if(id==0) {
            int success=0;for(int i=0;i<THREADS;i++)success+=wins[i]!=NULL;
            assert(success==1&&pmm_free_count()==32);
        }
        barrier_wait();if(wins[id])pmm_free_pages(wins[id],2);barrier_wait();
    }
    return NULL;
}
static void *churn(void *arg) {
    uint32_t rng=(uint32_t)(uintptr_t)arg+1;
    void *owned[16]={0};size_t sizes[16]={0};unsigned long seen=0;
    for(int k=0;k<20000;k++) {
        rng=rng*1664525u+1013904223u;size_t slot=(rng>>16)%16;
        if(owned[slot]){pmm_free_pages(owned[slot],sizes[slot]);owned[slot]=NULL;}
        else {sizes[slot]=1+(rng%7);owned[slot]=pmm_alloc_pages(sizes[slot]);}
        assert(pmm_free_count()>=32);
        unsigned long now=pmm_reserve_refusals();assert(now>=seen);seen=now;
    }
    for(int i=0;i<16;i++)if(owned[i])pmm_free_pages(owned[i],sizes[i]);
    return NULL;
}
static void parallel(void) {
    pthread_t threads[THREADS];reset(34,0);check_floor=1;assert(!pmm_reserve_enable(32));
    assert(!pthread_barrier_init(&barrier,NULL,THREADS));
    for(uintptr_t i=0;i<THREADS;i++)assert(!pthread_create(&threads[i],NULL,contender,(void*)i));
    for(int i=0;i<THREADS;i++)assert(!pthread_join(threads[i],NULL));
    assert(nfree==34&&floor_refusals==ROUNDS*(THREADS-1));
    assert(!pthread_barrier_destroy(&barrier));
    reset(256,0);assert(!pmm_reserve_enable(32));
    for(uintptr_t i=0;i<THREADS;i++)assert(!pthread_create(&threads[i],NULL,churn,(void*)i));
    for(int i=0;i<THREADS;i++)assert(!pthread_join(threads[i],NULL));
    assert(nfree==256);
    printf("parallel PASS %d threads, %d near-floor races, 160000 churn operations\n",THREADS,ROUNDS);
}
static void getters(void) {
    reset(16,0);check_floor=1;assert(!pmm_reserve_enable(4));
    unsigned before=lock_calls;assert(pmm_free_count()==16);assert(lock_calls==before+1);
    before=lock_calls;assert(!pmm_reserve_refusals());assert(lock_calls==before+1);
    before=lock_calls;assert(pmm_reserve_count()==4);assert(lock_calls==before+1);
    puts("getters PASS synchronized snapshots");
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"exhaustive"))exhaustive();
    else if(!strcmp(argv[1],"boundaries"))boundaries();
    else if(!strcmp(argv[1],"parallel"))parallel();
    else if(!strcmp(argv[1],"getters"))getters();
    else return 2;
    return 0;
}
