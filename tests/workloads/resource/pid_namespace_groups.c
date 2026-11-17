#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){dprintf(2,"PIDNS GROUP FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
static int nested,which,master,slave,async_pipe[2],ready[2],command[2];
static pid_t outer;
static void send_byte(int fd){CHECK(write(fd,"x",1)==1);}
static void receive_byte(int fd){char c;CHECK(read(fd,&c,1)==1);}
static void continued(int sig){(void)sig;_exit(0);}
static void background_child(void)
{
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid){
        CHECK(!setpgid(0,0));CHECK(signal(SIGCONT,continued)!=SIG_ERR);char byte;
        (void)read(slave,&byte,1);CHECK(!"background read must stop, not report an orphan EIO");
    }
    int status;CHECK(waitpid(pid,&status,WUNTRACED)==pid&&WIFSTOPPED(status)&&WSTOPSIG(status)==SIGTTIN);
    CHECK(!kill(pid,SIGCONT));CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
static int child(void *unused)
{
    (void)unused;pid_t self=getpid();if(nested)CHECK(self==1);
    CHECK(getpgid(0)==(nested?0:outer)&&getsid(0)==(nested?0:outer));
    pid_t session=-1;CHECK(!ioctl(slave,TIOCGSID,&session)&&session==(nested?0:outer));
    if(!which){
        CHECK(tcgetpgrp(slave)==(nested?0:outer));
        CHECK(!setpgid(0,0)&&getpgrp()==self&&getsid(0)==(nested?0:outer));
        CHECK(!tcsetpgrp(slave,self)&&tcgetpgrp(slave)==self);
        background_child();
        CHECK(!fcntl(async_pipe[0],F_SETOWN,self));
        CHECK(fcntl(async_pipe[0],F_GETOWN)==self);
        struct f_owner_ex owner={0};CHECK(!fcntl(async_pipe[0],F_GETOWN_EX,&owner)&&owner.type==F_OWNER_PID&&owner.pid==self);
        send_byte(ready[1]);receive_byte(command[0]);
    }else{
        /* The sibling's owner and foreground remain real objects, invisible
         * here even though both nested children have local PID 1. */
        if(nested){CHECK(!fcntl(async_pipe[0],F_GETOWN));CHECK(!tcgetpgrp(slave));}
        CHECK(!setpgid(0,0)&&getpgrp()==self);
        CHECK(!tcsetpgrp(slave,self)&&tcgetpgrp(slave)==self);
        struct f_owner_ex owner={.type=F_OWNER_PGRP,.pid=self};
        CHECK(!fcntl(async_pipe[0],F_SETOWN_EX,&owner));
        owner.pid=-1;CHECK(!fcntl(async_pipe[0],F_GETOWN_EX,&owner)&&owner.pid==self&&owner.type==F_OWNER_PGRP);
        send_byte(ready[1]);receive_byte(command[0]);
    }
    return 0;
}
static void reap(pid_t pid){int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
static int thread_sync[2],thread_ready[2];
static void *thread_owner(void *unused)
{
    (void)unused;pid_t tid=(pid_t)syscall(SYS_gettid);
    CHECK(write(thread_ready[1],&tid,sizeof(tid))==sizeof(tid));
    receive_byte(thread_sync[0]);sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);siginfo_t info;
    CHECK(sigwaitinfo(&mask,&info)==SIGUSR1&&info.si_fd==async_pipe[0]);
    send_byte(thread_ready[1]);return NULL;
}
static void thread_case(void)
{
    sigset_t mask,old;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);CHECK(!pthread_sigmask(SIG_BLOCK,&mask,&old));
    CHECK(!pipe(thread_sync)&&!pipe(thread_ready));pthread_t thread;CHECK(!pthread_create(&thread,NULL,thread_owner,NULL));
    pid_t tid;CHECK(read(thread_ready[0],&tid,sizeof(tid))==sizeof(tid)&&tid!=getpid());
    CHECK(!fcntl(async_pipe[0],F_SETOWN,tid)&&!fcntl(async_pipe[0],F_GETOWN));
    CHECK(!fcntl(async_pipe[0],F_SETSIG,SIGUSR1));CHECK(!fcntl(async_pipe[0],F_SETFL,O_ASYNC|O_NONBLOCK));
    /* Linux GETOWN reports no TGID role, yet send_sigio selects this TID
     * and delivers process-wide. Verify delivery, not just the zero getter. */
    send_byte(async_pipe[1]);siginfo_t process_info;
    CHECK(sigwaitinfo(&mask,&process_info)==SIGUSR1&&process_info.si_fd==async_pipe[0]);receive_byte(async_pipe[0]);
    struct f_owner_ex owner={.type=F_OWNER_TID,.pid=tid};CHECK(!fcntl(async_pipe[0],F_SETOWN_EX,&owner));
    CHECK(fcntl(async_pipe[0],F_GETOWN)==tid);
    CHECK(!fcntl(async_pipe[0],F_SETSIG,SIGUSR1));CHECK(!fcntl(async_pipe[0],F_SETFL,O_ASYNC|O_NONBLOCK));
    send_byte(thread_sync[1]);send_byte(async_pipe[1]);receive_byte(thread_ready[0]);CHECK(!pthread_join(thread,NULL));
    /* pthread_join synchronizes libc/clear_child_tid, not kernel task reap.
     * Process wait below tests the final-role removal boundary. */
    CHECK(!fcntl(async_pipe[0],F_SETOWN,0));CHECK(!fcntl(async_pipe[0],F_SETFL,0));receive_byte(async_pipe[0]);
    CHECK(!pthread_sigmask(SIG_SETMASK,&old,NULL));
    for(unsigned i=0;i<2;i++){close(thread_sync[i]);close(thread_ready[i]);}
}
static void run(void)
{
    outer=getpid();CHECK(setsid()==outer);signal(SIGTTOU,SIG_IGN);signal(SIGHUP,SIG_IGN);
    int mounted=0;master=posix_openpt(O_RDWR|O_NOCTTY);
    if(master<0&&errno==ENOENT){
        CHECK(!mkdir("/dev/pts",0755)||errno==EEXIST);
        CHECK(!mount("devpts","/dev/pts","devpts",0,"newinstance,mode=620,ptmxmode=666"));mounted=1;
        master=open("/dev/pts/ptmx",O_RDWR|O_NOCTTY);
    }
    CHECK(master>=0&&!grantpt(master)&&!unlockpt(master));
    char path[64];unsigned number;CHECK(!ioctl(master,TIOCGPTN,&number));
    CHECK(snprintf(path,sizeof(path),"/dev/pts/%u",number)>0);slave=open(path,O_RDWR);CHECK(slave>=0);
    CHECK(!ioctl(slave,TIOCSCTTY,0)&&tcgetpgrp(slave)==outer);
    CHECK(!pipe(async_pipe)&&!pipe(ready)&&!pipe(command));
    thread_case();
    char *stacks[2]={malloc(65536),malloc(65536)};CHECK(stacks[0]&&stacks[1]);pid_t children[2];
    for(which=0;which<2;which++){
        children[which]=clone(child,stacks[which]+65536,SIGCHLD|(nested?CLONE_NEWPID:0),NULL);CHECK(children[which]>0);
        receive_byte(ready[0]);CHECK(tcgetpgrp(master)==children[which]);
        struct f_owner_ex owner={0};CHECK(!fcntl(async_pipe[0],F_GETOWN_EX,&owner));
        CHECK(owner.pid==children[which]&&owner.type==(which?F_OWNER_PGRP:F_OWNER_PID));
        if(which){send_byte(command[1]);send_byte(command[1]);}
    }
    reap(children[0]);reap(children[1]);CHECK(!fcntl(async_pipe[0],F_GETOWN));
    /* A retained TTY foreground may outlive its last process, unlike GETOWN. */
    CHECK(tcgetpgrp(master)==children[1]);
    for(unsigned i=0;i<2;i++){free(stacks[i]);close(async_pipe[i]);close(ready[i]);close(command[i]);}
    close(slave);close(master);if(mounted)CHECK(!umount("/dev/pts"));
}
int main(int argc,char **argv)
{
    nested=argc==2&&!strcmp(argv[1],"--nested");pid_t supervisor=fork();CHECK(supervisor>=0);
    if(!supervisor){run();_exit(0);}reap(supervisor);
    puts(nested?"PIDNS GROUP PASS nested inherited session sibling same-number tty and async owners":"PIDNS GROUP PASS inherited session tty and async owners");return 0;
}
