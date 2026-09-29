#include <kernel/sched_runqueue.h>
#include <assert.h>
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
    assert(!kernel_sched_pick(&q,1) && !q.last);
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
                               (!head && rank[model[at]]==priority))) at++;
            for(unsigned i=count;i>at;i--) model[i]=model[i-1];
            model[at]=id; count++;
            kernel_sched_enqueue(&q,&nodes[id],&nodes[id],priority,head);
        }
        struct kernel_sched_node *selected=kernel_sched_pick(&q,1);
        assert(selected==(count ? &nodes[model[0]] : 0));
        unsigned at=0; while(at<count && rank[model[at]]) at++;
        assert(kernel_sched_pick(&q,0)==(at<count ? &nodes[model[at]] : 0));
    }
    puts("scheduler priority/FIFO/throttle queue PASS");
}
