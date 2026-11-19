/* 抽取真實 RT 函式與 sched_tick；不模擬 context switch 或真硬體中斷。 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "errno.h"
#include "uapi_proc.h"
#include "constants.h"
enum {T_READY,T_RUNNING,T_SLEEPING};
struct rq_node {int unused;};
struct thread {
    unsigned rt_period,rt_budget,rt_used,rt_util,rt_releases,rt_misses,rt_overruns,rt_max_used;
    int rt_level,rt_waiting,rt_throttled,killed,interrupted,prio,inherit_prio,base_prio,starve_boost,state,cpu;
    unsigned slice;
    unsigned long cpu_ticks;
    uint64_t rt_release,wake_tick;
    struct rq_node rq;
    struct thread *next;
};
struct cpu { int id,need_resched; struct thread *current,*idle; uint64_t ticks_idle,ticks_busy; };
static struct cpu cpus[1];
static struct thread idle,*ready,*sleepers,*rt_tasks[RT_MAX];
static unsigned rt_util_total,starve_scan_ticks=10,time_slice=TIME_SLICE_MS;
static uint64_t now;
static int sched_lk;
static struct cpu *this_cpu(void) { return cpus; }
static unsigned timer_hz(void) { return 1000; }
static uint64_t timer_ticks(void) { return now; }
static struct thread *cur(void) { return cpus[0].current; }
static unsigned long sched_lock(void) { return 0; }
static void sched_unlock(unsigned long flags) { (void)flags; }
static void spin_lock(int *lk) { (void)lk; }
static void spin_unlock(int *lk) { (void)lk; }
static int rq_highest(void) { return ready ? ready->prio:-1; }
static void cpu_resched(struct cpu *cpu) { cpu->need_resched=1; }
static void kick(int prio) { if (prio>cur()->prio) cpus[0].need_resched=1; }
static void runq_reprio(struct rq_node *n,int prio) { (void)n;(void)prio; }
static void starve_scan(uint64_t tick) { (void)tick; }
static void make_ready(struct thread *t) { t->state=T_READY;ready=t;kick(t->prio); }
static void sched_preempt_point(void) { }
#include "periodic.c"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"check %d: %s\n",__LINE__,#c); return 2; } } while(0)
static struct thread make_thread(void) {
    struct thread t={0};t.prio=t.base_prio=PRIO_NORMAL;t.inherit_prio=-1;t.state=T_RUNNING;t.slice=TIME_SLICE_MS;return t;
}
int main(void) {
    struct thread pool[RT_MAX];
    for (unsigned i=0;i<RT_MAX;i++) pool[i]=make_thread();
    cpus[0].idle=&idle;
    const unsigned bounds[]={0,1,2,3,1000,60000,60001,UINT_MAX};
    for (unsigned i=0;i<sizeof(bounds)/sizeof(bounds[0]);i++) for (unsigned j=0;j<sizeof(bounds)/sizeof(bounds[0]);j++) {
        unsigned p=bounds[i],b=bounds[j]; cpus[0].current=&pool[0];
        uint64_t util=p ? ((uint64_t)b*1000+p-1)/p:0;
        int valid=(!p && !b) || (p>=2 && p<=60000 && b && b<p && util<=RT_UTIL_MAX);
        int rc=thread_set_periodic(p,b); CHECK((rc==0)==valid);rt_remove(&pool[0]);CHECK(rt_util_total==0);
    }
    uint32_t rng=17;
    for (int i=0;i<20000;i++) {
        rng=rng*1664525u+1013904223u; unsigned slot=(rng>>16)%RT_MAX;
        cpus[0].current=&pool[slot];
        if (rng&1) { unsigned p=2+(rng%59999),b=1+((rng>>3)%p); thread_set_periodic(p,b); }
        else rt_remove(&pool[slot]);
        unsigned sum=0;
        for (unsigned j=0;j<RT_MAX;j++) {
            unsigned occurrences=0;
            for (unsigned k=0;k<RT_MAX;k++) occurrences+=rt_tasks[k]==&pool[j];
            CHECK(occurrences==(unsigned)!!pool[j].rt_period);
            if (pool[j].rt_period) sum+=pool[j].rt_util;
        }
        CHECK(sum==rt_util_total && sum<=RT_UTIL_MAX);
        for (unsigned a=0;a<RT_MAX;a++) for (unsigned b=0;b<RT_MAX;b++)
            if (pool[a].rt_period && pool[b].rt_period && pool[a].rt_period<pool[b].rt_period) {
                /* 舊版會在這個排序斷言失敗；期限反例則在下方獨立觀察。 */
                if (pool[a].prio<pool[b].prio) return 2;
            }
    }
    for (unsigned i=0;i<RT_MAX;i++) { rt_remove(&pool[i]);rt_remove(&pool[i]); }
    CHECK(rt_util_total==0);
    struct thread long_task=make_thread(),short_task=make_thread();
    cpus[0].current=&long_task;CHECK(thread_set_periodic(1000,100)==0);
    cpus[0].current=&short_task;CHECK(thread_set_periodic(2,1)==0);
    /* short_task 已完成第一輪並睡到 t=2，long_task 此時正在計算且未用完自己的預算。 */
    short_task.rt_waiting=1;short_task.state=T_SLEEPING;short_task.wake_tick=2;sleepers=&short_task;
    cpus[0].current=&long_task;cpus[0].need_resched=0;
    for (now=1;now<=4;now++) {
        sched_tick();
        /* 主機環境尊重 sched_tick 的搶佔要求；短工作使用一個 tick 完成後阻塞到下個 release。 */
        if (cur()==&short_task && short_task.rt_used>=short_task.rt_budget) {
            short_task.rt_waiting=1;short_task.wake_tick=short_task.rt_release;
            short_task.state=T_SLEEPING;short_task.next=sleepers;sleepers=&short_task;
            cpus[0].current=ready;ready=NULL;cur()->state=T_RUNNING;
        }
        if (cpus[0].need_resched && ready && ready->prio>cur()->prio) {
            struct thread *next=ready;ready=cur();ready->state=T_READY;
            cpus[0].current=next;next->state=T_RUNNING;cpus[0].need_resched=0;
        }
    }
    printf("{\"admission_boundary_checks\":64,\"bookkeeping_operations\":20000,\"util\":%u,"
           "\"long_prio\":%d,\"short_prio\":%d,\"short_misses\":%u,\"need_resched\":%d,"
           "\"long_cpu_ticks\":%lu,\"short_cpu_ticks\":%lu}\n",
           rt_util_total,long_task.prio,short_task.prio,short_task.rt_misses,cpus[0].need_resched,
           long_task.cpu_ticks,short_task.cpu_ticks);
    return short_task.rt_misses ? 1:0;
}
