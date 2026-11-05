#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sched.h>
#include <unistd.h>
#include <termios.h>
static int control;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"cost contract failed line %d errno %d\n",__LINE__,errno); exit(1); } } while (0)
static void command(const char *text, int error)
{
    errno=0; ssize_t n=write(control,text,strlen(text));
    if (error) CHECK(n==-1 && errno==error); else CHECK(n==(ssize_t)strlen(text));
}
static void snapshot(const char *name, unsigned epoch)
{
    int fd=open("/proc/boaros_cost",O_RDONLY); CHECK(fd>=0);
    printf("COST SNAPSHOT %s %u\n",name,epoch); fflush(stdout);
    char buffer[2048]; ssize_t n;
    while ((n=read(fd,buffer,sizeof(buffer)))>0) CHECK(fwrite(buffer,1,(size_t)n,stdout)==(size_t)n);
    CHECK(n==0); CHECK(close(fd)==0); puts("COST END"); fflush(stdout);
}
static void blocked(pid_t pid)
{
    char path[64], text[512]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    for (int i=0;i<10000;i++) {
        int fd=open(path,O_RDONLY); CHECK(fd>=0); ssize_t n=read(fd,text,sizeof(text)-1); CHECK(n>0); close(fd);
        text[n]=0; char *state=strrchr(text,')'); CHECK(state!=NULL);
        if (state[2]=='S') return;
        sched_yield();
    }
    CHECK(0);
}
static int finish(void)
{
    puts("COST PASS contract"); CHECK(fflush(stdout)==0);
    /* 用户缓冲写完仍可能留在 UART 队列；快照须先排空，再允许关机 raw 输出。 */
    CHECK(tcdrain(STDOUT_FILENO)==0);
    return 0;
}
int main(void)
{
    mkdir("/proc",0755); CHECK(mount("proc","/proc","proc",0,0)==0);
    control=open("/proc/boaros_cost_control",O_WRONLY);
    if(control<0) { CHECK(errno==ENOENT); CHECK(open("/proc/boaros_cost",O_RDONLY)<0 && errno==ENOENT); return finish(); }
    CHECK(control>=0);
    command("end\n",EINVAL); command("begin",EINVAL); command("begin\nextra",EINVAL);
    CHECK(write(control,(void *)1,6)==-1 && errno==EFAULT);
    command("begin\n",0); command("begin\n",EBUSY);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) { command("end\n",EPERM); command("begin\n",EBUSY); _exit(0); }
    int status; CHECK(waitpid(child,&status,0)==child && status==0);
    command("end\n",0); snapshot("contract",1);
    command("end\n",EINVAL);
    command("begin\n",0); command("end\n",0); snapshot("reuse",2);
    int pipefd[2]; CHECK(pipe(pipefd)==0);
    child=fork(); CHECK(child>=0);
    if (!child) { char byte; CHECK(read(pipefd[0],&byte,1)==1); _exit(0); }
    blocked(child); /* This operation predates begin; membership rebases its pin. */
    command("begin\n",0); command("end\n",EBUSY);
    CHECK(write(pipefd[1],"x",1)==1); CHECK(waitpid(child,&status,0)==child && status==0);
    command("end\n",0); snapshot("inflight",3);
    close(pipefd[0]); close(pipefd[1]);
    child=fork(); CHECK(child>=0);
    if (!child) { command("begin\n",0); _exit(0); }
    CHECK(waitpid(child,&status,0)==child && status==0);
    int report=open("/proc/boaros_cost",O_RDONLY); CHECK(report>=0);
    char header[1024]; ssize_t n=read(report,header,sizeof(header)-1); CHECK(n>0); header[n]=0;
    CHECK(strstr(header,"state=incomplete\n")!=NULL); close(report);
    command("begin\n",0); command("end\n",0); snapshot("after-abort",5);
    return finish();
}
