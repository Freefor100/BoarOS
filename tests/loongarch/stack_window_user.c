#define _GNU_SOURCE
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
int main(void)
{
    uintptr_t window=UINT64_C(0xffff800000004000);
    int pipefd[2];if(pipe(pipefd))return 1;
    errno=0;if(write(pipefd[1],(void *)window,1)!=-1 || errno!=EFAULT)return 2;
    if(close(pipefd[0]) || close(pipefd[1]))return 3;
    pid_t child=fork();if(child<0)return 4;
    if(!child){(void)*(volatile uint64_t *)window;_exit(5);}
    int status;if(waitpid(child,&status,0)!=child || !WIFSIGNALED(status) ||
       (WTERMSIG(status)!=SIGSEGV && WTERMSIG(status)!=SIGBUS))return 6;
    puts("LA kernel window user access denied");return 0;
}
