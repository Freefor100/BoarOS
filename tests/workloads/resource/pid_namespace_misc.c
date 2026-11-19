#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Keep ABI definitions local so the same source builds with the guest musl. */
struct nl_address {uint16_t family,pad;uint32_t port,groups;};
struct filter_insn {uint16_t code;uint8_t jt,jf;uint32_t k;};
struct filter_program {uint16_t len;struct filter_insn *filter;};
static long allow_filter(unsigned flags)
{
    struct filter_insn allow={.code=6,.k=0x7fff0000};
    struct filter_program program={1,&allow};
    return syscall(SYS_seccomp,1,flags,&program);
}
static pthread_barrier_t installed,release_peer;
static pid_t peer_tid;
static void barrier(pthread_barrier_t *b)
{int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *diverge(void *unused)
{
    (void)unused;peer_tid=(pid_t)syscall(SYS_gettid);
    assert(!allow_filter(0));barrier(&installed);barrier(&release_peer);return NULL;
}
static void seccomp_case(void)
{
    assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!allow_filter(0));
    assert(!pthread_barrier_init(&installed,NULL,2));
    assert(!pthread_barrier_init(&release_peer,NULL,2));
    pthread_t peer;assert(!pthread_create(&peer,NULL,diverge,NULL));barrier(&installed);
    /* The sibling diverged from our shared filter leaf, so TSYNC reports its
     * namespace-local TID rather than an errno or the caller's TID. */
    assert(peer_tid>0&&peer_tid!=(pid_t)syscall(SYS_gettid));
    assert(allow_filter(1)==peer_tid);barrier(&release_peer);
    assert(!pthread_join(peer,NULL));assert(!allow_filter(1));
    assert(!pthread_barrier_destroy(&installed));assert(!pthread_barrier_destroy(&release_peer));
}
static uint32_t port(int fd)
{
    struct nl_address a={0};socklen_t n=sizeof(a);
    assert(!getsockname(fd,(struct sockaddr *)&a,&n));
    assert(n==sizeof(a)&&a.family==AF_NETLINK&&a.port);return a.port;
}
static int bound(void)
{
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,0);if(fd<0)perror("NETLINK_ROUTE socket");assert(fd>=0);
    struct nl_address a={.family=AF_NETLINK};
    assert(!bind(fd,(struct sockaddr *)&a,sizeof(a)));return fd;
}
static void wait_ok(pid_t pid)
{int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void ordinary(void)
{
    int fd=bound();uint32_t original=port(fd);assert(original==(uint32_t)getpid());
    pid_t child=fork();assert(child>=0);
    if(!child){
        assert(port(fd)==original&&original!=(uint32_t)getpid());
        int local=bound();assert(port(local)==(uint32_t)getpid());
        assert(!close(local)&&!close(fd));seccomp_case();_exit(0);
    }
    wait_ok(child);assert(port(fd)==original);assert(!close(fd));seccomp_case();
}
static int reply[2][2],command[2][2];
static void transfer(int fd,void *data,size_t n,int writing)
{ssize_t r=writing?write(fd,data,n):read(fd,data,n);assert(r==(ssize_t)n);}
static int nested_peer(void *argument)
{
    unsigned which=(unsigned)(uintptr_t)argument;assert(getpid()==1);
    int fd=bound();uint32_t first=port(fd);transfer(reply[which][1],&first,sizeof(first),1);
    unsigned token=0;transfer(command[which][0],&token,sizeof(token),0);
    if(which){
        /* Peer zero has closed and exited. The old bound address does not
         * change, but a fresh autobind can now acquire this local TGID. */
        assert(port(fd)==first);int fresh=bound();assert(port(fresh)==1);
        assert(!close(fresh));seccomp_case();
    }
    assert(!close(fd));return 0;
}
static void nested(void)
{
    /* Do initial checks outside PID1, leaving the shared net namespace's
     * preferred port 1 free for the two new namespace init processes. */
    pid_t plain=fork();assert(plain>=0);if(!plain){ordinary();_exit(0);}wait_ok(plain);
    void *stacks[2];pid_t peers[2];uint32_t ports[2];
    for(unsigned i=0;i<2;i++){
        assert(!pipe(reply[i])&&!pipe(command[i]));stacks[i]=malloc(65536);assert(stacks[i]);
        peers[i]=clone(nested_peer,(char *)stacks[i]+65536,CLONE_NEWPID|SIGCHLD,(void *)(uintptr_t)i);
        assert(peers[i]>0);transfer(reply[i][0],&ports[i],sizeof(ports[i]),0);
    }
    assert(ports[0]==1&&ports[1]&&ports[1]!=ports[0]);
    unsigned token=1;transfer(command[0][1],&token,sizeof(token),1);wait_ok(peers[0]);
    transfer(command[1][1],&token,sizeof(token),1);wait_ok(peers[1]);
    for(unsigned i=0;i<2;i++){
        for(unsigned j=0;j<2;j++)assert(!close(reply[i][j])&&!close(command[i][j]));
        free(stacks[i]);
    }
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--seccomp-only")){
        seccomp_case();printf("PIDNS MISC PASS TSYNC only pid=%d failed_tid=%d\n",getpid(),peer_tid);
    } else if(argc==2&&!strcmp(argv[1],"--nested")){
        nested();puts("PIDNS MISC PASS nested sibling netlink collision reuse and TSYNC local TID");
    } else {
        assert(argc==1);ordinary();puts("PIDNS MISC PASS netlink inherited port and TSYNC failed TID");
    }
    return 0;
}
