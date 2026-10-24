#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/reboot.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KEY ((key_t)0x414e4547)
static pid_t daemon_pid;
static int semid=-1,shmid=-1;
static struct { int32_t size,tail;char data[]; } *shared;
static uint64_t limit;
static uint64_t now(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);
    return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;
}
static void cleanup(void)
{
    if(daemon_pid>0) {kill(daemon_pid,SIGKILL);while(waitpid(daemon_pid,0,0)<0 && errno==EINTR){}daemon_pid=0;}
    if(shared && shared!=(void *)-1) {shmdt(shared);shared=0;}
    if(semid>=0)semctl(semid,0,IPC_RMID);
    if(shmid>=0)shmctl(shmid,IPC_RMID,0);
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"SEMLOG FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void progress(void){CHECK(now()<limit);CHECK(sched_yield()==0);}
static pid_t spawn(const char *applet,const char *a,const char *b,const char *c,int output)
{
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        if(output>=0 && (dup2(output,STDOUT_FILENO)<0 || dup2(output,STDERR_FILENO)<0))_exit(126);
        const char *args[]={"/busybox",applet,a,b,c,0};execv(args[0],(char *const *)args);_exit(127);
    }
    return pid;
}
static int joined(pid_t pid)
{
    int status;CHECK(waitpid(pid,&status,0)==pid);return status;
}
static void logger(const char *text)
{
    int status=joined(spawn("logger","-t","SEMLOG",text,-1));CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void wait_message(const char *message)
{
    struct sembuf down[]={{1,0,0},{0,1,SEM_UNDO}},up={0,-1,SEM_UNDO|IPC_NOWAIT};
    for(;;) {
        CHECK(semop(semid,down,2)==0);
        CHECK(shared->size>0 && shared->size<65536 && shared->tail>=0 && shared->tail<shared->size);
        int found=memmem(shared->data,(size_t)shared->size,message,strlen(message))!=0;
        CHECK(semop(semid,&up,1)==0);if(found)return;progress();
    }
}
static void concurrent_readers(void)
{
    int gate[2],output[2][2];pid_t children[2];CHECK(pipe(gate)==0);
    for(unsigned i=0;i<2;i++) {
        CHECK(pipe(output[i])==0);children[i]=fork();CHECK(children[i]>=0);
        if(!children[i]) {
            char ready;close(gate[1]);close(output[i][0]);
            if(read(gate[0],&ready,1)!=1 || dup2(output[i][1],STDOUT_FILENO)<0)_exit(126);
            execl("/busybox","/busybox","logread",(char *)0);_exit(127);
        }
        close(output[i][1]);
    }
    close(gate[0]);CHECK(write(gate[1],"RR",2)==2);close(gate[1]);
    for(unsigned i=0;i<2;i++) {
        char text[8192];size_t size=0;ssize_t got;
        while((got=read(output[i][0],text+size,sizeof(text)-1-size))>0) {size+=(size_t)got;CHECK(size<sizeof(text)-1);}
        CHECK(!got);text[size]=0;close(output[i][0]);int status=joined(children[i]);CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
        for(unsigned j=0;j<8;j++) {char marker[64];snprintf(marker,sizeof(marker),"boaros-shared-log-%u",j);CHECK(strstr(text,marker));}
    }
    CHECK(semctl(semid,0,GETVAL)==0 && semctl(semid,1,GETVAL)==0);
}
static void reader_exit_undo(void)
{
    int ready[2],hold[2];CHECK(pipe(ready)==0 && pipe(hold)==0);pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(ready[0]);close(hold[1]);struct sembuf down[]={{1,0,0},{0,1,SEM_UNDO}};
        if(semop(semid,down,2) || write(ready[1],"H",1)!=1)_exit(126);
        char byte;(void)read(hold[0],&byte,1);_exit(125);
    }
    close(ready[1]);close(hold[0]);char byte;CHECK(read(ready[0],&byte,1)==1);close(ready[0]);CHECK(semctl(semid,0,GETVAL)==1);
    pid_t writer=spawn("logger","-t","SEMLOG","boaros-after-reader-exit",-1);
    while(semctl(semid,0,GETZCNT)!=1)progress();
    CHECK(kill(child,SIGKILL)==0);int status=joined(child);CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);close(hold[1]);
    status=joined(writer);CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    wait_message("boaros-after-reader-exit");CHECK(semctl(semid,0,GETVAL)==0 && semctl(semid,1,GETVAL)==0);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);CHECK(atexit(cleanup)==0);limit=now()+UINT64_C(20000000000);
    errno=0;CHECK(semget(KEY,0,0)==-1 && errno==ENOENT);errno=0;CHECK(shmget(KEY,0,0)==-1 && errno==ENOENT);
    int log=open("/daemon.log",O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(log>=0);
    daemon_pid=spawn("syslogd","-n","-C64",0,log);close(log);
    for(;;) {
        struct stat st;semid=semget(KEY,2,0);shmid=shmget(KEY,0,0);
        if(semid>=0 && shmid>=0 && !lstat("/dev/log",&st) && S_ISSOCK(st.st_mode))break;
        int status;CHECK(waitpid(daemon_pid,&status,WNOHANG)==0);progress();
    }
    shared=shmat(shmid,0,SHM_RDONLY);CHECK(shared!=(void *)-1);
    puts("SEMLOG CASE daemon-unix-socket-and-ipc-ready PASS");
    for(unsigned i=0;i<8;i++) {char text[64];snprintf(text,sizeof(text),"boaros-shared-log-%u",i);logger(text);}
    wait_message("boaros-shared-log-7");concurrent_readers();puts("SEMLOG CASE real-logger-concurrent-logread-content PASS");
    reader_exit_undo();puts("SEMLOG CASE reader-exit-undo-unblocks-real-daemon PASS");
    CHECK(shmdt(shared)==0);shared=0;CHECK(kill(daemon_pid,SIGTERM)==0);int status=joined(daemon_pid);daemon_pid=0;
    CHECK((WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM) || (WIFEXITED(status) && !WEXITSTATUS(status)));
    errno=0;CHECK(semget(KEY,0,0)==-1 && errno==ENOENT);semid=-1;
    errno=0;CHECK(shmget(KEY,0,0)==-1 && errno==ENOENT);shmid=-1;
    puts("SEMLOG CASE daemon-exit-ipc-removed PASS");puts("SEMLOG END 4");if(getpid()==1){sync();(void)reboot(RB_POWER_OFF);}return 0;
}
