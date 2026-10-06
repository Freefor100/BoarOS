#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../cost/common.h"

/* AF_UNIX charge belongs to the sender; receiver buffer settings do not grant it. */
static void blocked(pid_t pid)
{
    char path[64], text[512]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    for (unsigned i=0; i<100000; i++) {
        int fd=open(path,O_RDONLY); CHECK(fd>=0);
        ssize_t n=read(fd,text,sizeof(text)-1); CHECK(n>0 && close(fd)==0);
        text[n]=0; char *state=strrchr(text,')'); CHECK(state);
        if (state[2]=='S') return;
        sched_yield();
    }
    CHECK(0);
}
static void transfer(int type, unsigned action)
{
    int pair[2], gate[2], done[2], value=4096;
    CHECK(socketpair(AF_UNIX,type,0,pair)==0 && pipe(gate)==0 && pipe(done)==0);
    CHECK(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&value,sizeof(value))==0);
    CHECK(fcntl(pair[0],F_SETFL,fcntl(pair[0],F_GETFL)|O_NONBLOCK)==0);
    char initial[4096], next[1000];memset(initial,'a',sizeof(initial));memset(next,'b',sizeof(next));
    size_t fragment=action==3 ? 3500 : sizeof(initial),initial_size=0;
    for(;;) {
        ssize_t n=write(pair[0],initial,fragment);
        if(n<0){CHECK(errno==EAGAIN);break;}
        CHECK(n>0);initial_size+=(size_t)n;CHECK(initial_size<65536);
    }
    CHECK(initial_size>0 && fcntl(pair[0],F_SETFL,fcntl(pair[0],F_GETFL)&~O_NONBLOCK)==0);
    int observe=type==SOCK_STREAM && (action==0 || action==3);
    if(observe) cost_begin();
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        signal(SIGPIPE,SIG_IGN); close(pair[1]); close(gate[0]); close(done[0]);
        CHECK(write(gate[1],"r",1)==1);
        ssize_t result=write(pair[0],next,sizeof(next));
        if (action==1) CHECK(result==-1 && errno==(type==SOCK_STREAM ? ECONNRESET : ECONNREFUSED));
        else CHECK(result==(ssize_t)sizeof(next));
        CHECK(write(done[1],"d",1)==1); close(pair[0]); close(gate[1]); close(done[1]); _exit(0);
    }
    CHECK(close(gate[1])==0 && close(done[1])==0);
    char byte; CHECK(read(gate[0],&byte,1)==1); blocked(child);
    struct pollfd ready={.fd=done[0],.events=POLLIN}; CHECK(poll(&ready,1,0)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
    blocked(child); CHECK(poll(&ready,1,0)==0);
    value=0; CHECK(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&value,sizeof(value))==0);
    blocked(child); CHECK(poll(&ready,1,0)==0);
    if (action==0 || action==3) {
        value=1<<20; CHECK(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&value,sizeof(value))==0);
        CHECK(poll(&ready,1,2000)==1 && read(done[0],&byte,1)==1);
        char content[65536+1000]; size_t total=0;
        while(total<initial_size+sizeof(next)) { ssize_t n=read(pair[1],content+total,initial_size+sizeof(next)-total); CHECK(n>0); total+=(size_t)n; }
        for(size_t i=0;i<initial_size;i++)CHECK(content[i]=='a');
        CHECK(!memcmp(content+initial_size,next,sizeof(next)));
        CHECK(close(pair[1])==0);
    } else {
        if (action==2) CHECK(kill(child,SIGKILL)==0);
        CHECK(close(pair[1])==0);
        if(action==1) CHECK(poll(&ready,1,2000)==1 && read(done[0],&byte,1)==1);
    }
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(action==2 ? WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL : WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(close(pair[0])==0 && close(gate[0])==0 && close(done[0])==0);
    if(observe) {
        cost_end("budget-staging",NULL,0,0,0);
        if(cost_control>=0) {
            int fd=open("/proc/boaros_cost",O_RDONLY); CHECK(fd>=0);
            char line[256]; size_t used=0; int found=0;
            while(read(fd,&byte,1)==1) {
                if(byte=='\n') {
                    line[used]=0;
                    if(!strncmp(line,"foreground.stream_copy.value=",29)) {
                        CHECK(strtoull(line+29,NULL,10)==sizeof(next)); found++;
                    }
                    used=0;
                } else { CHECK(used+1<sizeof(line)); line[used++]=byte; }
            }
            CHECK(found==1 && close(fd)==0);
        }
    }

}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    cost_init();
    for(unsigned type=SOCK_STREAM;type<=SOCK_DGRAM;type++)
        for(unsigned action=0;action<4;action++) transfer((int)type,action);
    puts("NETWORK PASS budget"); return 0;
}
