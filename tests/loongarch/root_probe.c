#define _GNU_SOURCE
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned char bss[32791];
static const unsigned value=0x5a765432;
#define CHECK(condition) do { if(!(condition)) { fprintf(stderr,"LA root check line=%d errno=%d\n",__LINE__,errno); return 1; } } while(0)
static int applet(char *const argv[])
{
    pid_t child=fork();if(child<0)return -1;
    if(!child) {execv("/busybox",argv);_exit(125);}
    int status=-1;
    return waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : -1;
}
static int child_mode(void)
{
    char buffer[32];int fd=3;
    return read(fd,buffer,11)==11 && !memcmp(buffer,"musl token\n",11) ? 0 : 3;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"child"))return child_mode();
    CHECK(argc==1 && !strcmp(argv[0],"/init"));
    CHECK(getauxval(AT_PAGESZ)==16384 && value==0x5a765432);
    CHECK(getpid()>0 && syscall(SYS_gettid)==getpid());
    for(unsigned i=0;i<sizeof(bss);i++)CHECK(!bss[i]);
    int fd=open("/original",O_RDONLY);CHECK(fd>=0);
    struct stat st;CHECK(fstat(fd,&st)==0 && st.st_size==49169 && S_ISREG(st.st_mode));
    for(unsigned i=0;i<49169;) {
        unsigned char data[509];ssize_t n=read(fd,data,sizeof(data));CHECK(n>0);
        for(ssize_t j=0;j<n;j++)CHECK(data[j]==(i+j)%251);
        i+=n;
    }
    CHECK(lseek(fd,16381,SEEK_SET)==16381);
    int duplicate=dup(fd);CHECK(duplicate>=0);
    unsigned char data[9];CHECK(read(duplicate,data,9)==9 && lseek(fd,0,SEEK_CUR)==16390);
    for(unsigned i=0;i<9;i++)CHECK(data[i]==(16381+i)%251);
    CHECK(close(duplicate)==0);
    unsigned char *mapped=mmap(0,49169,PROT_READ,MAP_PRIVATE,fd,0);CHECK(mapped!=MAP_FAILED);
    CHECK(mapped[16383]==16383%251 && mapped[16384]==16384%251 && mapped[49168]==49168%251);
    CHECK(munmap(mapped,49169)==0 && close(fd)==0);
    mapped=mmap(0,32768,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapped!=MAP_FAILED);
    mapped[16383]=42;mapped[16384]=43;CHECK(mprotect(mapped,16384,PROT_READ)==0 && mapped[16383]==42);
    CHECK(munmap(mapped,32768)==0);
    puts("LA root storage ready");
    struct timespec before,after,delay={0,20000000};CHECK(clock_gettime(CLOCK_MONOTONIC,&before)==0);
    CHECK(nanosleep(&delay,0)==0 && sched_yield()==0 && clock_gettime(CLOCK_MONOTONIC,&after)==0);
    CHECK(after.tv_sec>before.tv_sec || (after.tv_sec==before.tv_sec && after.tv_nsec>=before.tv_nsec+20000000));
    fd=open("/persisted",O_CREAT|O_TRUNC|O_RDWR,0644);
    if(fd<0) {
        CHECK(errno==EROFS);
        char *const uname[]={"busybox","uname","-m",0};CHECK(applet(uname)==0);
        puts("LA musl readonly contracts passed");return 0;
    }
    for(unsigned i=0;i<sizeof(bss);i++)bss[i]=(i*7+3)%256;
    CHECK(write(fd,bss,sizeof(bss))==(ssize_t)sizeof(bss) && fsync(fd)==0);
    mapped=mmap(0,sizeof(bss),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(mapped!=MAP_FAILED);
    mapped[16383]^=0x5a;mapped[16384]^=0xa5;
    CHECK(msync(mapped,sizeof(bss),MS_SYNC)==0);
    mapped[16383]=bss[16383];mapped[16384]=bss[16384];CHECK(msync(mapped,sizeof(bss),MS_SYNC)==0);
    CHECK(munmap(mapped,sizeof(bss))==0 && close(fd)==0);
    fd=open("/message",O_CREAT|O_RDWR|O_TRUNC,0644);CHECK(fd>=0 && write(fd,"musl token\n",11)==11);
    CHECK(lseek(fd,0,SEEK_SET)==0);
    int inherited=dup2(fd,3);CHECK(inherited==3);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {char *const args[]={"/init","child",0};execv("/init",args);_exit(124);}
    int status=-1;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(lseek(fd,0,SEEK_CUR)==11);
    if(fd!=3) { CHECK(close(3)==0); }
    CHECK(close(fd)==0);
    char *const cp[]={"busybox","cp","/original","/copied",0};CHECK(applet(cp)==0);
    char *const cmp[]={"busybox","cmp","/original","/copied",0};CHECK(applet(cmp)==0);
    char *const grep[]={"busybox","grep","token","/message",0};CHECK(applet(grep)==0);
    char *const cat[]={"busybox","cat","/message",0};CHECK(applet(cat)==0);
    char *const echo[]={"busybox","echo","LA BusyBox originals passed",0};CHECK(applet(echo)==0);
    char *const uname[]={"busybox","uname","-m",0};CHECK(applet(uname)==0);
    char *const dd[]={"busybox","dd","if=/original","of=/copied","bs=16384","count=3",0};CHECK(applet(dd)==0);
    CHECK(stat("/copied",&st)==0 && st.st_size==49152);
    puts("LA musl root contracts passed");return 0;
}
