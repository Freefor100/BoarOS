#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ready[2],go[2],member;
static uint32_t command;
static void byte(int fd,int send){char x='x';assert((send?write(fd,&x,1):read(fd,&x,1))==1);}
static long boot(uint32_t magic,uint32_t cmd){return syscall(SYS_reboot,magic,672274793,cmd,(void *)1);}
static void denied(void)
{
    struct {uint32_t version;int pid;} h={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} d[2]={{0},{0}};
    assert(!syscall(SYS_capset,&h,d));errno=0;assert(boot(0,0)==-1&&errno==EPERM);
}
static int init(void *unused)
{
    (void)unused;assert(getpid()==1);byte(ready[1],1);byte(go[0],0);
    if(member){pid_t p=fork();assert(p>=0);if(p)for(;;)pause();}
    boot(0xfee1dead,command);_exit(99);
}
static int peer(void *unused)
{(void)unused;assert(getpid()==1);byte(ready[1],1);byte(go[0],0);return 17;}
static void nested_cases(const char *proc)
{
    char path[256];snprintf(path,sizeof(path),"%s/self/ns/pid",proc);int own=open(path,O_RDONLY|O_CLOEXEC);assert(own>=0);
    assert(!pipe(ready)&&!pipe(go));void *stack=malloc(65536);assert(stack);
    pid_t sibling=clone(peer,(char *)stack+65536,CLONE_NEWPID|SIGCHLD,NULL);assert(sibling>0);byte(ready[0],0);
    /* The peer keeps copies of its original pipe while each victim receives
     * a separate pair. No timing or sleeps are needed for liveness checks. */
    int peer_go=go[1];assert(!close(go[0])&&!close(ready[0])&&!close(ready[1]));
    const uint32_t commands[]={0x01234567,0xa1b2c3d4,0xcdef0123,0x4321fedc};
    for(unsigned i=0;i<4;i++)for(member=0;member<2;member++){
        command=commands[i];assert(!pipe(ready)&&!pipe(go));void *s=malloc(65536);assert(s);
        pid_t p=clone(init,(char *)s+65536,CLONE_NEWPID|SIGCHLD,NULL);assert(p>0);byte(ready[0],0);
        snprintf(path,sizeof(path),"%s/%d/ns/pid",proc,p);int fd=open(path,O_RDONLY|O_CLOEXEC);assert(fd>=0);
        byte(go[1],1);int status;assert(waitpid(p,&status,0)==p&&WIFSIGNALED(status)&&WTERMSIG(status)==(i<2?SIGHUP:SIGINT));
        assert(!kill(sibling,0));assert(!setns(fd,CLONE_NEWPID));
        errno=0;assert(fork()==-1&&errno==ENOMEM);assert(!setns(own,CLONE_NEWPID));assert(!close(fd));
        for(unsigned j=0;j<2;j++){assert(!close(ready[j])&&!close(go[j]));}free(s);
    }
    byte(peer_go,1);int status;assert(waitpid(sibling,&status,0)==sibling&&WIFEXITED(status)&&WEXITSTATUS(status)==17);
    assert(!close(peer_go)&&!close(own));free(stack);
}
int main(int argc,char **argv)
{
    errno=0;assert(boot(0,0)==-1&&errno==EINVAL);
    errno=0;assert(boot(0xfee1dead,0)==-1&&errno==EINVAL);
    pid_t p=fork();assert(p>=0);if(!p){denied();_exit(0);}int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
    if(argc==3&&!strcmp(argv[1],"--nested")){nested_cases(argv[2]);puts("PIDNS REBOOT PASS nested init/member four actions wait signal dead namespace sibling survival");}
    else {assert(argc==1);puts("PIDNS REBOOT PASS invalid inputs and permission ordering");}
    return 0;
}
