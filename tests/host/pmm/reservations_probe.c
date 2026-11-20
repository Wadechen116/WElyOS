/* 真PMM；合成位圖包含正常狀態及應急欠帳。panic攔截只核對停止前狀態，不模擬核心復原。 */
#define _XOPEN_SOURCE 700
#include <setjmp.h>
static jmp_buf panic_env;
static int expect_panic;
#define PMM_TEST_PANIC_HOOK() do { if(expect_panic)longjmp(panic_env,1); } while(0)
#define main c31_main
#include "floor_probe.c"
#undef main
#include "uapi/errno.h"
static void state(size_t total,unsigned mask,size_t floor,size_t reserved) {
    reset(total,mask);check_floor=0;floor_on=floor!=0;floor_pages=floor;nreserved=reserved;
    assert(reserved<=nfree);
}
static size_t first_fit(size_t n) {
    size_t run=0;
    for(size_t i=0;i<npages;i++) {
        run=bit_test(i)?0:run+1;
        if(n&&run==n)return i+1-n;
    }
    return npages;
}
static void map_same(struct snapshot *s,size_t reserved) {unchanged(s);assert(nreserved==reserved);}
static void reservation_exhaustive(void) {
    size_t cases=0,debt=0;
    for(size_t total=1;total<=7;total++)for(unsigned mask=0;mask<(1u<<total);mask++) {
        size_t freep=total-(size_t)__builtin_popcount(mask);
        for(size_t floor=0;floor<=total;floor++)for(size_t r=0;r<=freep&&r+floor<=total;r++)
        for(size_t q=0;q<=total+2;q++)for(int op=0;op<5;op++) {
            state(total,mask,floor,r);
            size_t n=q==total+2?SIZE_MAX:q,avail=freep-r;
            struct snapshot s=snapshot();size_t first=first_fit(n);
            int fits=floor<=avail&&n<=avail-floor;
            if(op==0 || op==1) {
                int allowed=n&&n<=avail&&(op==1||fits)&&first!=npages;
                void *p=op?pmm_alloc_pages_reserve(n):pmm_alloc_pages(n);
                assert(!!p==allowed);
                if(p) {
                    assert(p==arena+first*PAGE_SIZE&&nfree==freep-n&&nreserved==r);
                    for(size_t i=first;i<first+n;i++)assert(refs[i]==1&&bit_test(i));
                    pmm_free_pages(p,n);assert(nfree==freep);
                } else map_same(&s,r);
            } else if(op==2) {
                int ok=n==0||fits;
                assert(pmm_reserve(n)==(ok?0:-E_NOMEM));
                unchanged(&s);assert(nreserved==r+(ok?n:0));
            } else if(op==3) {
                if(!r)continue;
                size_t f=first_fit(1);void *p=pmm_alloc_reserved();
                assert(p==arena+f*PAGE_SIZE&&nfree==freep-1&&nreserved==r-1);
                assert(pmm_refcount(p)==1);pmm_ref(p);pmm_free(p);assert(nfree==freep-1);
                pmm_free(p);assert(nfree==freep&&nreserved==r-1);
            } else {
                if(n>r)continue;
                pmm_unreserve(n);unchanged(&s);assert(nreserved==r-n);
            }
            struct pmm_snapshot view;pmm_snapshot(&view);
            assert(view.free==nfree&&view.reserved==nreserved&&view.floor==floor);
            cases++;debt+=floor>avail;
        }
    }
    printf("reservation exhaustive PASS %zu operations (%zu in emergency debt), totals1..7/all bitmaps\n",cases,debt);
}
static void reservation_boundaries(void) {
    reset(16,0);check_floor=0;
    assert(!pmm_reserve(13));assert(pmm_reserved_count()==13);
    assert(pmm_reserve_enable(4)==-1&&!floor_on);assert(!pmm_reserve_enable(3));
    assert(!pmm_reserve(0));pmm_unreserve(0);
    assert(pmm_reserve(SIZE_MAX)==-E_NOMEM);assert(!pmm_alloc_pages(SIZE_MAX));
    void *e=pmm_alloc_pages_reserve(3);assert(e&&nfree==13&&nreserved==13);
    assert(!pmm_alloc_pages_reserve(1)&&!pmm_alloc());assert(pmm_reserve(1)==-E_NOMEM);
    void *p=pmm_alloc_reserved();assert(p&&nfree==12&&nreserved==12);
    pmm_unreserve(1);assert(nreserved==11&&nfree==12);assert(!pmm_alloc());
    pmm_free_pages(e,3);assert(nfree==15);assert(!pmm_reserve(1));assert(!pmm_alloc());
    pmm_unreserve(12);pmm_free(p);assert(nfree==16&&!nreserved);
    void *normal=pmm_alloc_pages(13);assert(normal);pmm_free_pages(normal,13);
    /* 空閒頁彼此不連續，批次失敗仍可逐頁兌現。 */
    state(16,0xaaaau,2,6);assert(!pmm_alloc_pages_reserve(2));
    void *pages[6];for(int i=0;i<6;i++)pages[i]=pmm_alloc_reserved();
    assert(nfree==2&&!nreserved);
    for(int i=0;i<6;i++)pmm_free(pages[i]);
    assert(nfree==8);
    puts("reservation boundaries PASS initialization/debt/recovery/fragmentation/references/SIZE_MAX");
}
static void reservation_getters(void) {
    reset(16,0);check_floor=1;assert(!pmm_reserve_enable(4));assert(!pmm_reserve(8));
    unsigned before=lock_calls;assert(pmm_reserved_count()==8);assert(lock_calls==before+1);
    struct pmm_snapshot s;before=lock_calls;pmm_snapshot(&s);assert(lock_calls==before+1);
    assert(s.free==16&&s.reserved==8&&s.floor==4&&s.refusals==0);
    puts("reservation getters PASS same-lock snapshot and reserved getter");
}
static void reservation_invalid(void) {
    /* 真panic不會返回；攔截後只在單執行緒驗證未突變，再釋放替身鎖。 */
    for(volatile int kind=0;kind<3;kind++) {
        reset(16,0);check_floor=0;assert(!pmm_reserve_enable(4));
        if(kind)assert(!pmm_reserve(3));
        struct snapshot s=snapshot();size_t prior=nreserved;expect_panic=1;
        if(!setjmp(panic_env)) {
            if(kind==0)(void)pmm_alloc_reserved();
            else pmm_unreserve(kind==1?4:SIZE_MAX);
            assert(!"invalid accounting must panic");
        }
        expect_panic=0;map_same(&s,prior);assert(held==1);held=0;
        assert(!pthread_mutex_unlock(&pmm_lk.mutex));
    }
    puts("reservation invalid PASS empty consume and excessive/SIZE_MAX unreserve stop before mutation");
}
static int reserve_wins[THREADS];
static void *reserve_contender(void *arg) {
    size_t id=(uintptr_t)arg;
    for(int k=0;k<ROUNDS;k++) {
        barrier_wait();reserve_wins[id]=!pmm_reserve(2);barrier_wait();
        if(!id) {
            int count=0;for(int i=0;i<THREADS;i++)count+=reserve_wins[i];
            assert(count==1);struct pmm_snapshot s;pmm_snapshot(&s);
            assert(s.free==34&&s.reserved==2&&s.floor==32);assert(!pmm_alloc());
        }
        barrier_wait();
        if(reserve_wins[id]) {
            if(k&1)pmm_unreserve(2);
            else {void *a=pmm_alloc_reserved(),*b=pmm_alloc_reserved();pmm_free(a);pmm_free(b);}
        }
        barrier_wait();
    }
    return NULL;
}
static int include_emergency;
static void *reservation_churn(void *arg) {
    uint32_t rng=(uint32_t)(uintptr_t)arg+31;size_t mine=0;
    void *owned[16]={0};size_t sizes[16]={0};
    for(int k=0;k<10000;k++) {
        rng=rng*1664525u+1013904223u;size_t slot=(rng>>16)%16,n=1+(rng>>8)%4;
        switch(rng%5) {
        case 0:if(!pmm_reserve(n))mine+=n;break;
        case 1:if(mine&&!owned[slot]){owned[slot]=pmm_alloc_reserved();sizes[slot]=1;mine--;}break;
        case 2:if(mine){size_t drop=mine<n?mine:n;pmm_unreserve(drop);mine-=drop;}break;
        case 3:if(!owned[slot]) {
            owned[slot]=include_emergency&&(rng&128)?pmm_alloc_pages_reserve(n):pmm_alloc_pages(n);sizes[slot]=n;
        }break;
        case 4:if(owned[slot]){pmm_free_pages(owned[slot],sizes[slot]);owned[slot]=NULL;}break;
        }
        struct pmm_snapshot s;pmm_snapshot(&s);assert(s.free>=s.reserved);
        if(!include_emergency)assert(s.free-s.reserved>=s.floor);
        (void)pmm_reserved_count();
    }
    pmm_unreserve(mine);
    for(int i=0;i<16;i++)if(owned[i])pmm_free_pages(owned[i],sizes[i]);
    return NULL;
}
static void reservation_parallel(void) {
    pthread_t threads[THREADS];reset(34,0);check_floor=1;assert(!pmm_reserve_enable(32));
    assert(!pthread_barrier_init(&barrier,NULL,THREADS));
    for(uintptr_t i=0;i<THREADS;i++)assert(!pthread_create(&threads[i],NULL,reserve_contender,(void*)i));
    for(int i=0;i<THREADS;i++)assert(!pthread_join(threads[i],NULL));
    assert(nfree==34&&!nreserved);assert(!pthread_barrier_destroy(&barrier));
    for(int emergency=0;emergency<=1;emergency++) {
        reset(128,0);check_floor=!emergency;include_emergency=emergency;assert(!pmm_reserve_enable(32));
        for(uintptr_t i=0;i<THREADS;i++)assert(!pthread_create(&threads[i],NULL,reservation_churn,(void*)i));
        for(int i=0;i<THREADS;i++)assert(!pthread_join(threads[i],NULL));
        assert(nfree==128&&!nreserved);
    }
    puts("reservation parallel PASS 8 threads, 1000 reserve races, 160000 normal/emergency churn operations");
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"exhaustive"))reservation_exhaustive();
    else if(!strcmp(argv[1],"boundaries"))reservation_boundaries();
    else if(!strcmp(argv[1],"parallel"))reservation_parallel();
    else if(!strcmp(argv[1],"getters"))reservation_getters();
    else if(!strcmp(argv[1],"invalid"))reservation_invalid();
    else return 2;
    return 0;
}
