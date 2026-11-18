/* 使用真實 allocator，僅替換 sbrk 與單執行緒鎖。 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
void *welyos_alloc_malloc(size_t);
void *welyos_alloc_calloc(size_t,size_t);
void *welyos_alloc_realloc(void *,size_t);
void welyos_alloc_free(void *);
static unsigned char *arena;
static size_t brk_offset;
static int fail_grow, fail_shrink, growth_calls, shrink_calls;
long probe_sbrk(long delta) {
    if (delta>0) {
        growth_calls++;
        if (fail_grow || (size_t)delta > 4*1024*1024-brk_offset) return -12;
        __asan_unpoison_memory_region(arena+brk_offset,(size_t)delta);
    } else if (delta<0) {
        shrink_calls++;
        if (fail_shrink || (size_t)-delta>brk_offset) return -12;
        __asan_poison_memory_region(arena+brk_offset+delta,(size_t)-delta);
    }
    long old=(long)(arena+brk_offset);
    brk_offset+=delta;
    return old;
}
long probe_write(long fd,const void *buf,size_t len) { (void)fd; (void)buf; return (long)len; }
_Noreturn void probe_exit(int status) { _Exit(status); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"check failed at %d: %s\n",__LINE__,#c); return 1; } } while(0)
int main(void) {
    arena=aligned_alloc(4096,4*1024*1024); CHECK(arena);
    __asan_poison_memory_region(arena,4*1024*1024);
    fail_grow=1; CHECK(welyos_alloc_malloc(1)==NULL); CHECK(brk_offset==0);
    fail_grow=0;
    unsigned char *p=welyos_alloc_malloc(32); CHECK(p && !((uintptr_t)p&15)); memset(p,0x71,32);
    fail_grow=1; CHECK(welyos_alloc_realloc(p,1024*1024)==NULL);
    for (int i=0;i<32;i++) CHECK(p[i]==0x71);
    CHECK(welyos_alloc_realloc(p,SIZE_MAX)==NULL);
    for (int i=0;i<32;i++) CHECK(p[i]==0x71);
    fail_grow=0; unsigned char *q=welyos_alloc_realloc(p,1024*1024); CHECK(q);
    for (int i=0;i<32;i++) CHECK(q[i]==0x71);
    fail_shrink=1; size_t previous=brk_offset; welyos_alloc_free(q); CHECK(brk_offset==previous);
    q=welyos_alloc_malloc(1024*1024); CHECK(q); fail_shrink=0; welyos_alloc_free(q); CHECK(brk_offset<previous);
    const size_t extremes[]={SIZE_MAX,SIZE_MAX-1,SIZE_MAX-15,SIZE_MAX-16,(size_t)1<<30,((size_t)1<<30)+1};
    for (size_t i=0;i<sizeof(extremes)/sizeof(extremes[0]);i++) CHECK(welyos_alloc_malloc(extremes[i])==NULL);
    CHECK(welyos_alloc_calloc(SIZE_MAX,2)==NULL); CHECK(welyos_alloc_calloc(2,SIZE_MAX)==NULL);
    void *zero=welyos_alloc_malloc(0); CHECK(zero); welyos_alloc_free(zero);
    zero=welyos_alloc_calloc(0,SIZE_MAX); CHECK(zero); welyos_alloc_free(zero);
    zero=welyos_alloc_realloc(NULL,0); CHECK(zero); CHECK(welyos_alloc_realloc(zero,0)==zero); welyos_alloc_free(zero);
    welyos_alloc_free(NULL);
    void *blocks[128]; size_t lengths[128];
    for (int round=0;round<8;round++) {
        for (int i=0;i<128;i++) {
            lengths[i]=(size_t)((i*53+round*19)%4096+1);
            blocks[i]=welyos_alloc_calloc(lengths[i],1); CHECK(blocks[i]);
            for (size_t j=0;j<lengths[i];j++) CHECK(((unsigned char *)blocks[i])[j]==0);
            memset(blocks[i],i,lengths[i]);
        }
        for (int parity=0;parity<2;parity++) for (int i=parity;i<128;i+=2) {
            unsigned char *grown=welyos_alloc_realloc(blocks[i],lengths[i]+8192); CHECK(grown);
            for (size_t j=0;j<lengths[i];j++) CHECK(grown[j]==(unsigned char)i);
            welyos_alloc_free(grown);
        }
    }
    CHECK(growth_calls>1 && shrink_calls>1);
    __asan_unpoison_memory_region(arena,4*1024*1024); free(arena);
    puts("PASS: sbrk growth/shrink failure, recovery, size extremes, 1024 content-preserving reallocs");
    return 0;
}
