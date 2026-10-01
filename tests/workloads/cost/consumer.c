#include "common.h"
#include <signal.h>
#include <sched.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <dirent.h>
static unsigned command_selection=65535;
static unsigned long long nanoseconds(void)
{ struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (unsigned long long)t.tv_sec*1000000000ULL+t.tv_nsec; }
static void drain_directory(void)
{
    DIR *directory=opendir(".");CHECK(directory!=NULL);
    struct dirent *entry;
    while((entry=readdir(directory))!=NULL){
        struct stat st;CHECK(fstatat(dirfd(directory),entry->d_name,&st,AT_SYMLINK_NOFOLLOW)==0);
        if(!S_ISREG(st.st_mode))continue;
        int fd=open(entry->d_name,O_RDONLY);CHECK(fd>=0);
        CHECK(fsync(fd)==0 && close(fd)==0);
    }
    CHECK(fsync(dirfd(directory))==0 && closedir(directory)==0);
}
static long milliseconds(void)
{ struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return t.tv_sec*1000+t.tv_nsec/1000000; }
static long command_budget_ms=180000;
static void command(unsigned libc, unsigned index)
{
    const char *directories[]={"/musl","/glibc"};
    static const char groups[][2][3]={{"0","1"},{"0","2"},{"0","3"},{"0","5"},{"6","7"},{"9","10"},{"11","12"}};
    char name[48];snprintf(name,sizeof(name),"consumer-%s-%u",libc?"glibc":"musl",index);
    CHECK(chdir(directories[libc])==0);int gate[2],ready[2];CHECK(pipe(gate)==0 && pipe(ready)==0);
    fflush(NULL);pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(setpgid(0,0)==0);CHECK(write(ready[1],"r",1)==1);char c;CHECK(read(gate[0],&c,1)==1);close(gate[0]);close(gate[1]);close(ready[0]);close(ready[1]);
        char *automatic[]={"./iozone","-a","-r","1k","-s","4m",0};
        char *threads[]={"./iozone","-t","4","-i",(char *)groups[index?index-1:0][0],"-i",(char *)groups[index?index-1:0][1],"-r","1k","-s","1m",0};
        setenv("LD_LIBRARY_PATH",libc?"/glibc/lib":"/musl/lib",1);
        execv("./iozone",index?threads:automatic);dprintf(2,"COST EXEC ERROR %s %d\n",name,errno);_exit(127);
    }
    char c;CHECK(read(ready[0],&c,1)==1);printf("COST COMMAND BEGIN %s\n",name);fflush(stdout);cost_begin();CHECK(write(gate[1],"g",1)==1);
    unsigned long long start=(unsigned long long)window_start.tv_sec*1000000000ULL+window_start.tv_nsec;
    long deadline=milliseconds()+command_budget_ms;int status=0,timeout=0;
    for(;;){pid_t r=waitpid(child,&status,WNOHANG);CHECK(r>=0);if(r==child)break;if(milliseconds()>=deadline && !timeout){timeout=1;CHECK(kill(-child,SIGKILL)==0);}struct timespec delay={0,20000000};CHECK(nanosleep(&delay,0)==0);}
    /* PID 1 also reaps orphaned workers after a cancelled process group. */
    if(timeout){for(unsigned retry=0;retry<500;retry++){int st;pid_t r=waitpid(-1,&st,WNOHANG);if(r<0 && errno==ECHILD)break;CHECK(r>=0);if(!r){struct timespec delay={0,20000000};CHECK(nanosleep(&delay,0)==0);}}}
    unsigned long long stopped=nanoseconds();
    /* Flush remaining regular files in the original working directory, then
     * its namespace. Unlinked owners were drained by the kernel's close path. */
    drain_directory();unsigned long long drained=nanoseconds();
    cost_end(name,0,0,0,0);
    printf("COST COMMAND TIMING %s %llu %llu\n",name,stopped-start,drained-stopped);
    printf("COST COMMAND RESULT %s %d %d\n",name,timeout,status);fflush(stdout);
    close(gate[0]);close(gate[1]);close(ready[0]);close(ready[1]);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);cost_init();mkdir("/dev",0755);mkdir("/tmp",01777);mkdir("/lib",0755);
    FILE *policy=fopen("/cost-consumer-budget","r");
    if(policy){char extra;CHECK(fscanf(policy,"%ld %c",&command_budget_ms,&extra)==1);CHECK(fclose(policy)==0);}
    else CHECK(errno==ENOENT);
    CHECK(command_budget_ms>=1000 && command_budget_ms<=3600000);
    printf("COST CONSUMER BUDGET %ld\n",command_budget_ms);
    policy=fopen("/cost-consumer-selection","r");
    if(policy){char extra;CHECK(fscanf(policy,"%u %c",&command_selection,&extra)==1);CHECK(fclose(policy)==0);}
    else CHECK(errno==ENOENT);
    CHECK(command_selection>0 && command_selection<=65535);
    printf("COST CONSUMER SELECTION %u\n",command_selection);
    struct utsname platform;CHECK(uname(&platform)==0);
    printf("COST PLATFORM %s %s %s\n",platform.sysname,platform.release,platform.machine);
    CHECK(mknod("/dev/null",S_IFCHR|0666,makedev(1,3))==0 || errno==EEXIST);
    CHECK(mknod("/dev/zero",S_IFCHR|0666,makedev(1,5))==0 || errno==EEXIST);
    CHECK(symlink("/musl/lib/libc.so","/lib/ld-musl-riscv64-sf.so.1")==0 || errno==EEXIST);
    CHECK(symlink("/glibc/lib/ld-linux-riscv64-lp64d.so.1","/lib/ld-linux-riscv64-lp64d.so.1")==0 || errno==EEXIST);
    setenv("LD_LIBRARY_PATH","/glibc/lib",1);
    for(unsigned libc=0;libc<2;libc++)for(unsigned group=0;group<8;group++)
        if(command_selection&(1U<<(libc*8+group)))command(libc,group);
    puts("COST PASS consumer");fflush(NULL);if(access("/cost-linux",F_OK)==0)reboot(RB_POWER_OFF);return 0;
}
