#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
static atomic_int executed;
static _Thread_local int value=17;
static void *worker(void *unused)
{ (void)unused;atomic_fetch_add(&executed,1);return value==17 ? 0 : (void *)1; }
int main(void)
{
    pthread_t thread;void *result;
    if(pthread_create(&thread,0,worker,0)!=EAGAIN || atomic_load(&executed))return 1;
    if(pthread_create(&thread,0,worker,0) || pthread_join(thread,&result) || result || atomic_load(&executed)!=1)return 2;
    puts("LA pthread creation OOM rollback/retry passed");return 0;
}
