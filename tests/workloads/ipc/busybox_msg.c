#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int queue = -1;
static void cleanup(void) { if (queue >= 0) msgctl(queue, IPC_RMID, 0); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"MSGTOOLS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static void join(pid_t child)
{
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void applet(const char *name, const char *argument, int output)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        if (output>=0 && dup2(output,STDOUT_FILENO)<0) _exit(126);
        const char *args[]={"/busybox",name,"-q",argument,0};
        execv(args[0],(char *const *)args); _exit(127);
    }
    join(child);
}
static uint64_t now(void)
{
    struct timespec time; CHECK(!clock_gettime(CLOCK_MONOTONIC,&time));
    return (uint64_t)time.tv_sec*1000000000U+time.tv_nsec;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0); CHECK(!atexit(cleanup));
    if(access("/proc/self/stat",F_OK)) {
        if(mkdir("/proc",0755))CHECK(errno==EEXIST);
        CHECK(!mount("proc","/proc","proc",0,0));
    }
    queue=msgget(IPC_PRIVATE,0600); CHECK(queue>=0);
    struct { long type; char text[4]; } message={7,"ipc"};
    CHECK(!msgsnd(queue,&message,sizeof(message.text),0));
    int output=open("/msgtools-output",O_CREAT|O_TRUNC|O_RDWR,0600); CHECK(output>=0);
    applet("ipcs",0,output); CHECK(!lseek(output,0,SEEK_SET));
    FILE *listing=fdopen(output,"r"); CHECK(listing); char line[512]; int found=0;
    while(fgets(line,sizeof(line),listing)) {
        unsigned long key,bytes,count; unsigned mode; int id; char owner[64];
        if(sscanf(line,"%lx %d %63s %o %lu %lu",&key,&id,owner,&mode,&bytes,&count)==6 && id==queue) {
            CHECK(!key && mode==0600 && bytes==4 && count==1); found++;
        }
    }
    CHECK(found==1 && !fclose(listing));
    puts("MSGTOOLS CASE real-ipcs-queue-content PASS");
    CHECK(msgrcv(queue,&message,sizeof(message.text),0,IPC_NOWAIT)==4);
    CHECK(message.type==7 && !memcmp(message.text,"ipc",4));
    int gate[2]; CHECK(!pipe(gate)); pid_t receiver=fork(); CHECK(receiver>=0);
    if(!receiver) {
        close(gate[0]); if(write(gate[1],"R",1)!=1)_exit(125); close(gate[1]);
        errno=0; ssize_t result=msgrcv(queue,&message,sizeof(message.text),9,0);
        _exit(result==-1 && errno==EIDRM ? 0 : 126);
    }
    close(gate[1]); char ready; CHECK(read(gate[0],&ready,1)==1 && ready=='R'); close(gate[0]);
    char path[64]; snprintf(path,sizeof(path),"/proc/%d/stat",receiver);
    uint64_t deadline=now()+UINT64_C(5000000000);
    /* After the pipe handshake the child's only blocking call is msgrcv. */
    for(;;) {
        FILE *state=fopen(path,"r"); CHECK(state);
        CHECK(fgets(line,sizeof(line),state) && !fclose(state));
        char *end=strrchr(line,')'); CHECK(end && end[1]==' ');
        if(end[2]=='S')break;
        CHECK(end[2]!='Z' && now()<deadline && !sched_yield());
    }
    char id[32]; snprintf(id,sizeof(id),"%d",queue); applet("ipcrm",id,-1); join(receiver);
    struct msqid_ds status; errno=0; CHECK(msgctl(queue,IPC_STAT,&status)==-1 && errno==EINVAL);
    queue=-1; CHECK(!unlink("/msgtools-output"));
    puts("MSGTOOLS CASE real-ipcrm-wakes-blocked-receiver PASS");
    puts("MSGTOOLS END 2");
    if(getpid()==1) { sync(); reboot(RB_POWER_OFF); }
    return 0;
}
