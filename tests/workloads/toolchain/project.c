/* Native tools and original build rules run in the guest, on both kernels. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"project:%d %s errno=%d\n",__LINE__,#x,errno);sync();exit(1); } } while(0)
static uint64_t now(void) {struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t hash(const char *p) {int fd=open(p,O_RDONLY);CHECK(fd>=0);char b[8192];ssize_t n;uint64_t h=1469598103934665603ULL;while((n=read(fd,b,sizeof(b)))>0) for(ssize_t i=0;i<n;i++)h=(h^(unsigned char)b[i])*1099511628211ULL;CHECK(n==0&&close(fd)==0);return h;}
static int run(const char *name,const char *command,int success)
{
    char log[160];snprintf(log,sizeof(log),"/evidence/%s.log",name);
    uint64_t start=now();pid_t child=fork();CHECK(child>=0);
    if(!child) {int out=open(log,O_WRONLY|O_CREAT|O_TRUNC,0644);CHECK(out>=0&&dup2(out,1)==1&&dup2(out,2)==2);close(out);execl("/bin/sh","sh","-c",command,(char*)NULL);_exit(127);}
    int status;CHECK(waitpid(child,&status,0)==child);
    printf("PROJECT COMMAND %s cwd=/work/lua argv=sh,-c,%s status=%d elapsed_ns=%llu\n",name,command,status,(unsigned long long)(now()-start));
    if(success) CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);else CHECK(WIFEXITED(status)&&WEXITSTATUS(status)!=0);
    return status;
}
struct artifact { char path[512];struct timespec mtime;uint64_t hash; };
static struct artifact saved[40];static unsigned count;
static void snapshot(void)
{
    count=0;DIR *d=opendir("src");CHECK(d!=NULL);struct dirent *e;
    while((e=readdir(d))) {size_t n=strlen(e->d_name);if(n>2&&!strcmp(e->d_name+n-2,".o")) {CHECK(count<40);struct artifact *a=&saved[count++];snprintf(a->path,sizeof(a->path),"src/%s",e->d_name);struct stat st;CHECK(stat(a->path,&st)==0);a->mtime=st.st_mtim;a->hash=hash(a->path);}}
    CHECK(closedir(d)==0&&count==34);
}
static int same_time(struct timespec a,struct timespec b) {return a.tv_sec==b.tv_sec&&a.tv_nsec==b.tv_nsec;}
static void check_snapshot(int changed)
{
    for(unsigned i=0;i<count;i++) {struct stat st;CHECK(stat(saved[i].path,&st)==0);int affected=!strcmp(saved[i].path,"src/lapi.o");CHECK(same_time(st.st_mtim,saved[i].mtime)==!(changed&&affected));CHECK(hash(saved[i].path)==saved[i].hash);}
}
static void products(void)
{
    run("products","src/lua /inputs/scenario.lua && src/luac -o scenario.luac /inputs/scenario.lua && src/lua scenario.luac && gcc -I src /inputs/embed.c src/liblua.a -lm -ldl -o embed && ./embed && gcc -fPIC -shared -I src /inputs/extension.c -o sample.so && LUA_CPATH='./?.so' src/lua -e 'assert(require(\"sample\").answer()==42)'",1);
    run("lua-error","src/lua -e 'error(\"intentional project error\")'",0);
    printf("PROJECT ARTIFACT lua=%llu luac=%llu liblua=%llu\n",(unsigned long long)hash("src/lua"),(unsigned long long)hash("src/luac"),(unsigned long long)hash("src/liblua.a"));
}
static void interrupt_jobserver(void)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child) {CHECK(setpgid(0,0)==0);execl("/usr/bin/make","make","-j2","-f","/inputs/interrupt.mk",(char*)NULL);_exit(127);}
    uint64_t deadline=now()+5000000000ULL;
    while(access("/evidence/hold-ready",F_OK)) {CHECK(now()<deadline);usleep(1000);}
    CHECK(kill(-child,SIGINT)==0);int status;CHECK(waitpid(child,&status,0)==child);
    int make_status=status;
    CHECK((WIFSIGNALED(status)&&WTERMSIG(status)==SIGINT)||(WIFEXITED(status)&&WEXITSTATUS(status)!=0));
    pid_t reaped;while((reaped=waitpid(-1,&status,0))>0) printf("PROJECT REAP pid=%d status=%d\n",reaped,status);
    CHECK(errno==ECHILD);
    printf("PROJECT INTERRUPT cwd=/work/lua argv=make,-j2,-f,/inputs/interrupt.mk status=%d elapsed_ns=0\n",make_status);
    DIR *d=opendir("/tmp");CHECK(d!=NULL);struct dirent *entry;
    while((entry=readdir(d)))CHECK(strncmp(entry->d_name,"GMfifo",6)!=0);
    CHECK(closedir(d)==0);puts("PROJECT PASS jobserver interrupt and children reaped");
}
static void jobserver(void)
{
    run("jobserver","gcc /inputs/job.c -o /tmp/job && make -j2 -f /inputs/jobserver.mk",1);
    int fd=open("/evidence/job-counts",O_RDONLY);int counts[3];CHECK(fd>=0&&read(fd,counts,sizeof(counts))==sizeof(counts)&&close(fd)==0);
    CHECK(counts[0]==0&&counts[1]==2&&counts[2]==6);interrupt_jobserver();
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);CHECK(setenv("PATH","/usr/bin:/bin",1)==0);CHECK(setenv("LC_ALL","C",1)==0);
    CHECK(mkdir("/proc",0755)==0||errno==EEXIST);CHECK(mount("proc","/proc","proc",0,NULL)==0);
    int temporary=access("/inputs/tmpfs",F_OK)==0;
    if(temporary) {CHECK(mount("tmpfs","/work","tmpfs",0,"size=268435456,nr_inodes=4096")==0);CHECK(chdir("/")==0);run("prepare-tmpfs","cp -a /inputs/lua /work/lua",1);}
    CHECK(chdir("/work/lua")==0);
    int control=-1;if(access("/inputs/observe",F_OK)==0) {control=open("/proc/boaros_cost_control",O_WRONLY);CHECK(control>=0&&write(control,"begin\n",6)==6);}
    if(access("/inputs/jobserver-only",F_OK)==0) {jobserver();sync();puts("PROJECT PASS all");return 42;}
    if(access("/inputs/reboot",F_OK)==0) {run("reboot-products","src/lua scenario.luac && src/luac -p scenario.luac && ./embed && LUA_CPATH='./?.so' src/lua -e 'assert(require(\"sample\").answer()==42)'",1);puts("PROJECT PASS reboot");puts("PROJECT PASS all");return 42;}
    int performance=access("/inputs/performance",F_OK)==0;
    run("clean-j1","make -j1 linux",1);products();
    uint64_t lua=hash("src/lua"),luac=hash("src/luac"),lib=hash("src/liblua.a");
    if(!performance) {
        snapshot();run("noop","make -j1 linux",1);check_snapshot(0);CHECK(lua==hash("src/lua")&&luac==hash("src/luac")&&lib==hash("src/liblua.a"));
        sleep(1);int fd=open("src/lapi.c",O_WRONLY|O_APPEND);CHECK(fd>=0&&write(fd,"\n/* incremental build */\n",25)==25&&close(fd)==0);
        run("incremental","make -j1 linux",1);check_snapshot(1);
        run("clean-rebuild","make clean && make -j1 linux",1);CHECK(lua==hash("src/lua")&&luac==hash("src/luac")&&lib==hash("src/liblua.a"));
        struct stat st;CHECK(stat("src/lapi.c",&st)==0);sleep(1);fd=open("src/lapi.c",O_WRONLY|O_APPEND);CHECK(fd>=0&&write(fd,"\n#error intentional build failure\n",34)==34&&close(fd)==0);
        run("syntax-error","make -j1 linux",0);fd=open("src/lapi.c",O_WRONLY);CHECK(fd>=0&&ftruncate(fd,st.st_size)==0&&close(fd)==0);run("syntax-recovery","make -j1 linux",1);
        jobserver();
    }
    if(control<0) {
        run("clean","make clean",1);run("clean-j2","make -j2 linux",1);products();
        CHECK(lua==hash("src/lua")&&luac==hash("src/luac")&&lib==hash("src/liblua.a"));
    }
    if(control>=0) {
        /* 定点窗口只含一个干净-j1及产物验证；先完成在途同步，再关闭窗口。 */
        sync();uint64_t deadline=now()+5000000000ULL;ssize_t ended;
        while((ended=write(control,"end\n",4))<0&&errno==EBUSY) {CHECK(now()<deadline);usleep(1000);}
        CHECK(ended==4&&close(control)==0);run("cost","cat /proc/boaros_cost",1);
    }
    uint64_t start=now();run("sync","sync",1);printf("PROJECT SYNC elapsed_ns=%llu\n",(unsigned long long)(now()-start));
    puts("PROJECT PASS all");return 42;
}
