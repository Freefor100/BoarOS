#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static unsigned case_number;
static void run(const char *name,char *const args[])
{
    unsigned id=++case_number; getpgid(-10000-(int)id);
    printf("PROBE begin %s id=%u\n",name,id);
    pid_t p=fork(); if(!p) { execv(args[0],args); perror("execv"); _exit(127); }
    if(p<0) { printf("PROBE end %s fork_errno=%d\n",name,errno); return; }
    long end=ms()+4000; int status=0,timeout=0;
    while(waitpid(p,&status,WNOHANG)==0) {
        if(ms()>end) { timeout=1; kill(p,SIGKILL); }
        struct timespec t={0,10000000}; nanosleep(&t,NULL);
    }
    getpgid(-20000-(int)id);
    printf("PROBE end %s timeout=%d wait=%d\n",name,timeout,status);
}
static int probe(void)
{
    struct sched_param param={0}; cpu_set_t mask; CPU_ZERO(&mask);
    int policy=syscall(SYS_sched_getscheduler,0),pr=syscall(SYS_sched_getparam,0,&param),ar=sched_getaffinity(0,sizeof(mask),&mask);
    printf("PROBE endpoint pid=%d pgid=%d sid=%d policy=%d param_rc=%d priority=%d affinity_rc=%d cpu0=%d\n",
        getpid(),getpgid(0),getsid(0),policy,pr,param.sched_priority,ar,(int)CPU_ISSET(0,&mask));
    return 0;
}
static void daemon_probe(void)
{
    unsigned id=++case_number; getpgid(-10000-(int)id);
    printf("PROBE begin libc-daemon id=%u\n",id);
    int fd[2]; if(pipe(fd)) return;
    pid_t p=fork();
    if(!p) { close(fd[0]); errno=0; int rc=daemon(1,1); int result[]={rc,errno,getpid(),getsid(0),getpgid(0)};
        write(fd[1],result,sizeof(result)); _exit(0); }
    close(fd[1]); int result[5]={-99,0,0,0,0}; ssize_t n=read(fd[0],result,sizeof(result)); close(fd[0]);
    waitpid(p,NULL,0);
    getpgid(-20000-(int)id);
    printf("PROBE end libc-daemon bytes=%ld rc=%d errno=%d pid=%d sid=%d pgid=%d\n",(long)n,result[0],result[1],result[2],result[3],result[4]);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc>1 && !strcmp(argv[1],"endpoint")) return probe();
    mkdir("/dev",0755); mkdir("/proc",0755); mkdir("/tmp",01777); mkdir("/lib",0755);
    mknod("/dev/null",S_IFCHR|0666,makedev(1,3));
    mkdir("/dev/shm",01777);
    mknod("/dev/urandom",S_IFCHR|0666,makedev(1,9));
    mount("proc","/proc","proc",0,NULL);
    symlink("/musl/lib/libc.so","/lib/ld-musl-riscv64.so.1");
    setenv("LD_LIBRARY_PATH","/musl/lib",1);
    char *sid[]={"/busybox","setsid","/init","endpoint",NULL}; run("busybox-setsid",sid);
    char *fifo[]={"/busybox","chrt","-f","10","/init","endpoint",NULL}; run("busybox-chrt-fifo",fifo);
    char *rr[]={"/busybox","chrt","-r","10","/init","endpoint",NULL}; run("busybox-chrt-rr",rr);
    char *aff[]={"/busybox","taskset","1","/init","endpoint",NULL}; run("busybox-taskset",aff);
    daemon_probe();
    char *ip[]={"/musl/iperf3","-c","127.0.0.1","-A","0","-t","1",NULL}; run("original-iperf-client",ip);
    char *cy[]={"/musl/cyclictest","-q","-t1","-p10","-i1000","-l10",NULL}; run("original-cyclictest",cy);
    puts("PROBE complete"); return 0;
}
