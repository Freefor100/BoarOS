#include <kernel/cpu.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static struct kernel_cpu target;
static pthread_barrier_t gate;
static unsigned published;
static void *publish(void *unused)
{
    (void)unused;int r=pthread_barrier_wait(&gate);
    assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    if(kernel_cpu_publish_events(&target,1))__atomic_fetch_add(&published,1,__ATOMIC_RELAXED);
    return 0;
}
static unsigned consume_first,consumed;
static void *consume(void *unused)
{
    (void)unused;
    if(consume_first)consumed=__atomic_exchange_n(&target.events,0,__ATOMIC_ACQUIRE);
    int r=pthread_barrier_wait(&gate);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    r=pthread_barrier_wait(&gate);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    if(!consume_first)consumed=__atomic_exchange_n(&target.events,0,__ATOMIC_ACQUIRE);
    return 0;
}
static void consume_publication_orders(void)
{
    for(consume_first=0;consume_first<2;consume_first++) {
        target.events=1;consumed=0;
        assert(!pthread_barrier_init(&gate,0,2));pthread_t receiver;
        assert(!pthread_create(&receiver,0,consume,0));
        int r=pthread_barrier_wait(&gate);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
        assert(kernel_cpu_publish_events(&target,1)==(int)consume_first);
        r=pthread_barrier_wait(&gate);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
        assert(!pthread_join(receiver,0));
        assert(consumed==1 && target.events==consume_first);
        pthread_barrier_destroy(&gate);
    }
}
int main(void)
{
    assert(!pthread_barrier_init(&gate,0,4));pthread_t threads[4];
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],0,publish,0));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],0));
    assert(published==1 && target.events==1);
    /* A different event retains both bits even when one interrupt is pending. */
    assert(kernel_cpu_publish_events(&target,2) && target.events==3);
    assert(!kernel_cpu_publish_events(&target,3));
    assert(__atomic_exchange_n(&target.events,0,__ATOMIC_ACQUIRE)==3);
    /* After receiver consumption, the next publication needs a fresh interrupt. */
    assert(kernel_cpu_publish_events(&target,1) && target.events==1);
    assert(!kernel_cpu_publish_events(&target,1));
    assert(__atomic_exchange_n(&target.events,0,__ATOMIC_ACQUIRE)==1);
    assert(kernel_cpu_publish_events(&target,3));
    assert(__atomic_exchange_n(&target.events,0,__ATOMIC_ACQUIRE)==3);
    pthread_barrier_destroy(&gate);
    consume_publication_orders();
    puts("CPU events: concurrent duplicate publication, mixed bits, consume/republication PASS");
}
