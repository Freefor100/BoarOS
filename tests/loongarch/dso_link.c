#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
int boaros_tls_get(void);
void boaros_tls_set(int);
void *boaros_tls_address(void);
static int (*const relocated_function)(void) __attribute__((section(".data.rel.ro")))=boaros_tls_get;
static pthread_barrier_t gate;
static void *addresses[3];
static void *worker(void *pointer)
{
    uintptr_t index=(uintptr_t)pointer;
    if(boaros_tls_get()!=700)return (void *)1;
    boaros_tls_set(800+index);addresses[index]=boaros_tls_address();
    pthread_barrier_wait(&gate);
    return boaros_tls_get()==800+(int)index ? 0 : (void *)2;
}
int main(void)
{
    if(boaros_tls_get()!=700 || pthread_barrier_init(&gate,0,3))return 1;
    boaros_tls_set(709);addresses[2]=boaros_tls_address();pthread_t threads[2];
    for(uintptr_t i=0;i<2;i++)if(pthread_create(&threads[i],0,worker,(void *)i))return 2;
    pthread_barrier_wait(&gate);
    for(unsigned i=0;i<2;i++){void *result;if(pthread_join(threads[i],&result) || result)return 3;}
    if(addresses[0]==addresses[1] || addresses[0]==addresses[2] || addresses[1]==addresses[2] || boaros_tls_get()!=709 || pthread_barrier_destroy(&gate))return 4;
    if(relocated_function()!=709)return 5;
    pid_t child=fork();if(child<0)return 6;
    if(!child){__asm__ volatile("st.d $zero,%0,0"::"r"(&relocated_function):"memory");_exit(1);}
    int status;if(waitpid(child,&status,0)!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGSEGV)return 7;
    puts("LA DT_NEEDED/RPATH/initial DSO TLS passed");return 0;
}
