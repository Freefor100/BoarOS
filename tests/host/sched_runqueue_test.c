#include <kernel/sched_runqueue.h>
#include <assert.h>
#include <kernel/sched_policy.h>
#include <stdio.h>
int main(void)
{
    struct kernel_sched_runqueue q={0};
    struct kernel_sched_node a={0},b={0},c={0},d={0};
    int owners[4];
    kernel_sched_enqueue(&q,&a,&owners[0],0,0);
    kernel_sched_enqueue(&q,&b,&owners[1],50,0);
    kernel_sched_enqueue(&q,&c,&owners[2],50,0);
    kernel_sched_enqueue(&q,&d,&owners[3],99,0);
    assert(kernel_sched_pick(&q,1)->owner==&owners[3]);
    assert(kernel_sched_pick(&q,0)->owner==&owners[0]);
    kernel_sched_dequeue(&q,&d);
    assert(kernel_sched_pick(&q,1)->owner==&owners[1]);
    kernel_sched_dequeue(&q,&b);
    kernel_sched_enqueue(&q,&b,&owners[1],50,0); /* yield to peer */
    assert(kernel_sched_pick(&q,1)->owner==&owners[2]);
    kernel_sched_dequeue(&q,&b);
    kernel_sched_enqueue(&q,&b,&owners[1],50,1); /* retain place after preempt */
    assert(kernel_sched_pick(&q,1)->owner==&owners[1]);
    kernel_sched_dequeue(&q,&a);
    assert(!kernel_sched_pick(&q,0)); /* throttle means idle, not stolen RT */
    kernel_sched_dequeue(&q,&b);kernel_sched_dequeue(&q,&c);
    assert(!kernel_sched_pick(&q,1) && !kernel_sched_last(&q));
    struct kernel_sched_node nodes[128]={0};
    unsigned rank[128]={0}, active[128]={0}, model[128], count=0;
    unsigned random=0x12345678U;
    for (unsigned step=0;step<20000;step++) {
        random=random*1664525U+1013904223U;
        unsigned id=(random>>16)%128;
        if (active[id]) {
            kernel_sched_dequeue(&q,&nodes[id]); active[id]=0;
            unsigned at=0; while(model[at]!=id) at++;
            for(unsigned i=at;i+1<count;i++) model[i]=model[i+1];
            count--;
        } else {
            unsigned priority=(random>>8)%100, head=(random>>31)&1;
            rank[id]=priority; active[id]=1;
            unsigned at=0;
            while(at<count && (rank[model[at]]>priority ||
                               (rank[model[at]]==priority && (priority ? !head : (uintptr_t)&nodes[model[at]]<(uintptr_t)&nodes[id])))) at++;
            for(unsigned i=count;i>at;i--) model[i]=model[i-1];
            model[at]=id; count++;
            kernel_sched_enqueue(&q,&nodes[id],&nodes[id],priority,head);
        }
        struct kernel_sched_node *selected=kernel_sched_pick(&q,1);
        assert(selected==(count ? &nodes[model[0]] : 0));
        unsigned at=0; while(at<count && rank[model[at]]) at++;
        assert(kernel_sched_pick(&q,0)==(at<count ? &nodes[model[at]] : 0));
    }
    while(q.count)kernel_sched_dequeue(&q,kernel_sched_pick(&q,1));
    a.fair.key=300;b.fair.key=100;c.fair.key=200;
    kernel_sched_enqueue(&q,&a,&a,0,0);kernel_sched_enqueue(&q,&b,&b,0,0);kernel_sched_enqueue(&q,&c,&c,0,0);
    assert(kernel_sched_pick(&q,1)==&b && kernel_sched_next(&q,&b)==&c && kernel_sched_last(&q)==&a);
    kernel_sched_fair_advance(&q,0);assert(q.min_vruntime==100);
    kernel_sched_dequeue(&q,&b);kernel_sched_dequeue(&q,&a);kernel_sched_dequeue(&q,&c);
    struct kernel_sched_runqueue destination={.min_vruntime=500};
    b.fair.key=125;kernel_sched_fair_place(&destination,&b,&q);assert(b.fair.key==525);
    destination.min_vruntime=20000000;b.fair.key=0;
    kernel_sched_fair_place(&destination,&b,&destination);assert(b.fair.key==10000000);
    struct kernel_sched_node fair[2]={0};struct kernel_sched_policy policies[2]={{0},{.nice=5}};
    uint64_t executed[2]={0};q=(struct kernel_sched_runqueue){0};
    for(unsigned i=0;i<2;i++)kernel_sched_enqueue(&q,&fair[i],&policies[i],0,0);
    for(unsigned round=0;round<20000;round++){
        struct kernel_sched_node *n=kernel_sched_pick(&q,1);unsigned i=(unsigned)(n-fair);
        kernel_sched_dequeue(&q,n);executed[i]+=1000;
        n->fair.key+=kernel_sched_fair_delta(&policies[i],1000);
        kernel_sched_enqueue(&q,n,&policies[i],0,0);kernel_sched_fair_advance(&q,0);
    }
    uint64_t difference=executed[0]*335>executed[1]*1024?executed[0]*335-executed[1]*1024:executed[1]*1024-executed[0]*335;
    assert(executed[0]>executed[1] && difference<=UINT64_C(2000)*1024);
    while(q.count)kernel_sched_dequeue(&q,kernel_sched_pick(&q,1));
    puts("scheduler RT FIFO/fair weighted execution/migration/throttle queue PASS");
}
