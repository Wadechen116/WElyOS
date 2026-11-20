/* Exercise the actual overflow link/claim handshake, including the interval
 * between linking and the queued recheck. Hooks only schedule the two threads. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "runq.h"
__thread struct cpu host_cpu;
int cpu_count=2;
static pthread_mutex_t event_lk=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv=PTHREAD_COND_INITIALIZER;
static int scenario,linked,claimed,producer_done;
static void after_claim(void);
static void after_link(void);
#include "runq.c"
static void after_link(void) {
    if(scenario!=2)return;
    pthread_mutex_lock(&event_lk);linked=1;pthread_cond_broadcast(&event_cv);
    while(!claimed)pthread_cond_wait(&event_cv,&event_lk);
    pthread_mutex_unlock(&event_lk);
}
static void after_claim(void) {
    if(scenario!=2)return;
    pthread_mutex_lock(&event_lk);claimed=1;pthread_cond_broadcast(&event_cv);
    while(!producer_done)pthread_cond_wait(&event_cv,&event_lk);
    pthread_mutex_unlock(&event_lk);
}
static struct rq_node node;
static int claim_ok;
static void *claimer(void *unused) {
    (void)unused;host_cpu.id=1;
    pthread_mutex_lock(&event_lk);
    while(!linked)pthread_cond_wait(&event_cv,&event_lk);
    pthread_mutex_unlock(&event_lk);
    claim_ok=claim(&node);return NULL;
}
int main(int argc,char **argv) {
    if(argc!=2)return 90;
    scenario=argv[1][0]-'0';
    node.queued=1;host_cpu.id=0;
    if(scenario==1) {
        /* Consumer validated an old ring ticket before producer published the
         * overflow ticket, and wins queued before the producer can link. */
        claim_ok=claim(&node);ovf_add(&node,3);
    } else if(scenario==2) {
        pthread_t t;
        assert(pthread_create(&t,NULL,claimer,NULL)==0);
        ovf_add(&node,3);
        pthread_mutex_lock(&event_lk);producer_done=1;pthread_cond_broadcast(&event_cv);pthread_mutex_unlock(&event_lk);
        assert(pthread_join(t,NULL)==0);
    } else if(scenario==3) {
        ovf_add(&node,3);claim_ok=claim(&node);
    } else return 91;
    int safe=claim_ok && node.queued==0 && node.where==W_NONE && !node.next && !node.prev &&
             ovf_head[3]==NULL && ovf_tail[3]==NULL && ovf_count[3]==0 && !(ovf_mask&(1u<<3));
    printf("{\"interleaving\":%d,\"safe\":%d,\"claim\":%d,\"queued\":%u,\"where\":%u,\"count\":%u}\n",
           scenario,safe,claim_ok,node.queued,node.where,ovf_count[3]);
    return !safe;
}
