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
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"budget:%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)

/* BoarOS以接收端字节预算约束UNIX队列；Linux的SO_RCVBUF不是此预算协议。 */
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
    int pair[2], gate[2], done[2], value=2048;
    CHECK(socketpair(AF_UNIX,type,0,pair)==0 && pipe(gate)==0 && pipe(done)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
    char initial[4096], next[1000]; memset(initial,'a',sizeof(initial)); memset(next,'b',sizeof(next));
    CHECK(write(pair[0],initial,sizeof(initial))==(ssize_t)sizeof(initial));
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        signal(SIGPIPE,SIG_IGN); close(pair[1]); close(gate[0]); close(done[0]);
        CHECK(write(gate[1],"r",1)==1);
        ssize_t result=write(pair[0],next,sizeof(next));
        if (action==1) CHECK(result==-1 && errno==EPIPE);
        else CHECK(result==(ssize_t)sizeof(next));
        CHECK(write(done[1],"d",1)==1); close(pair[0]); close(gate[1]); close(done[1]); _exit(0);
    }
    CHECK(close(pair[0])==0 && close(gate[1])==0 && close(done[1])==0);
    char byte; CHECK(read(gate[0],&byte,1)==1); blocked(child);
    struct pollfd ready={.fd=done[0],.events=POLLIN}; CHECK(poll(&ready,1,0)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
    blocked(child); CHECK(poll(&ready,1,0)==0);
    value=0; CHECK(setsockopt(pair[1],SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
    blocked(child); CHECK(poll(&ready,1,0)==0);
    if (action==0) {
        value=4096; CHECK(setsockopt(pair[1],SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
        CHECK(poll(&ready,1,2000)==1 && read(done[0],&byte,1)==1);
        char content[5096]; size_t total=0;
        while(total<sizeof(content)) { ssize_t n=read(pair[1],content+total,sizeof(content)-total); CHECK(n>0); total+=(size_t)n; }
        CHECK(!memcmp(content,initial,sizeof(initial)) && !memcmp(content+sizeof(initial),next,sizeof(next)));
        CHECK(close(pair[1])==0);
    } else {
        if (action==2) CHECK(kill(child,SIGKILL)==0);
        CHECK(close(pair[1])==0);
        if(action==1) CHECK(poll(&ready,1,2000)==1 && read(done[0],&byte,1)==1);
    }
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(action==2 ? WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL : WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(close(gate[0])==0 && close(done[0])==0);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    CHECK(mkdir("/proc",0755)==0 || errno==EEXIST);
    CHECK(mount("proc","/proc","proc",0,NULL)==0);
    for(unsigned type=SOCK_STREAM;type<=SOCK_DGRAM;type++)
        for(unsigned action=0;action<3;action++) transfer((int)type,action);
    puts("NETWORK PASS budget"); return 0;
}
