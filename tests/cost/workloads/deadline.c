#include "common.h"
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
static void blocked(pid_t pid)
{
    char path[64],text[512]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    for(unsigned i=0;i<100000;i++) { int fd=open(path,O_RDONLY); CHECK(fd>=0); ssize_t n=read(fd,text,sizeof(text)-1); CHECK(n>0); close(fd); text[n]=0; char *s=strrchr(text,')'); CHECK(s); if(s[2]=='S')return; sched_yield(); } CHECK(0);
}
static void handler(int s) { (void)s; }
static void phase(unsigned unrelated, unsigned mode, unsigned reuse)
{
    pid_t tasks[260]; int ready[2],gate[2],forever[2],done[2]; CHECK(pipe(ready)==0 && pipe(gate)==0 && pipe(forever)==0 && pipe(done)==0);
    struct timespec *target=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0); CHECK(target!=MAP_FAILED);
    for(unsigned i=0;i<unrelated+4;i++) { tasks[i]=fork(); CHECK(tasks[i]>=0); if(!tasks[i]) {
        struct sigaction action={.sa_handler=handler}; sigemptyset(&action.sa_mask); CHECK(sigaction(SIGUSR1,&action,0)==0);
        CHECK(write(ready[1],"r",1)==1); char c; CHECK(read((i<4?gate:forever)[0],&c,1)==1);
        if(i<4) { CHECK(write(done[1],"d",1)==1); errno=0; int rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,target,0); CHECK(rc==0 || (mode==1 && rc==EINTR)); }
        _exit(0);
    }}
    for(unsigned i=0;i<unrelated+4;i++){char c;CHECK(read(ready[0],&c,1)==1);blocked(tasks[i]);}
    CHECK(clock_gettime(CLOCK_MONOTONIC,target)==0); target->tv_sec++;
    char name[64];snprintf(name,sizeof(name),"deadline-%u-%u-%u",unrelated,mode,reuse);cost_begin(); CHECK(write(gate[1],"gggg",4)==4);
    for(unsigned i=0;i<4;i++){char c;CHECK(read(done[0],&c,1)==1);blocked(tasks[i]);}
    if(mode) for(unsigned i=0;i<4;i++) CHECK(kill(tasks[i],mode==1?SIGUSR1:mode==2?SIGTERM:SIGKILL)==0);
    for(unsigned i=0;i<4;i++){int st;CHECK(waitpid(tasks[i],&st,0)==tasks[i]);CHECK(mode<2?st==0:WIFSIGNALED(st));}
    if(unrelated){char tokens[256];memset(tokens,'g',unrelated);CHECK(write(forever[1],tokens,unrelated)==(ssize_t)unrelated);}
    for(unsigned i=4;i<unrelated+4;i++){int st;CHECK(waitpid(tasks[i],&st,0)==tasks[i] && st==0);}
    cost_end(name,0,0,0,0);printf("COST METRIC %s deadline_expired %u\n",name,mode?0:4);
    CHECK(munmap(target,4096)==0);
    close(ready[0]);close(ready[1]);close(gate[0]);close(gate[1]);close(forever[0]);close(forever[1]);close(done[0]);close(done[1]);
}
int main(void)
{
    cost_init();for(unsigned i=0;i<4;i++)phase((unsigned[]){0,32,128,256}[i],0,0); for(unsigned mode=1;mode<=3;mode++)phase(32,mode,0);phase(0,0,1);puts("COST PASS deadline");return 0;
}
