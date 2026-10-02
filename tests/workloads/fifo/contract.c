#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"fifo:%d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static void metadata(void)
{
    int p[2]; CHECK(pipe2(p,O_NONBLOCK)==0);
    struct stat a,b;
    CHECK(fstat(p[0],&a)==0 && S_ISFIFO(a.st_mode));
    CHECK(fchmod(p[0],0640)==0);
    CHECK(fstat(p[0],&a)==0 && fstat(p[1],&b)==0);
    CHECK((a.st_mode&07777)==0640 && a.st_mode==b.st_mode);
    CHECK(a.st_ino!=0 && a.st_ino==b.st_ino && a.st_dev==b.st_dev);
    CHECK(a.st_ctim.tv_sec>0 && a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec);
    int copy=dup(p[1]); CHECK(copy>=0 && fchmod(copy,S_IFREG|0600)==0);
    CHECK(fstat(p[0],&a)==0 && S_ISFIFO(a.st_mode) && (a.st_mode&07777)==0600);
    char path[64]; snprintf(path,sizeof(path),"/proc/self/fd/%d",p[0]);
    int reopened=open(path,O_RDONLY|O_NONBLOCK); CHECK(reopened>=0);
    CHECK(fstat(reopened,&b)==0 && b.st_ino==a.st_ino && b.st_mode==a.st_mode);
    CHECK(stat(path,&b)==0 && b.st_ino==a.st_ino && b.st_mode==a.st_mode);
    pid_t child=fork();CHECK(child>=0);
    if(!child) { CHECK(fchmod(p[1],0000)==0);_exit(0); }
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(fstat(copy,&b)==0 && (b.st_mode&07777)==0 && S_ISFIFO(b.st_mode));
    CHECK(write(p[1],"x",1)==1);char byte;CHECK(read(reopened,&byte,1)==1 && byte=='x');
    CHECK(syscall(SYS_fstat,p[0],(void *)1)==-1 && errno==EFAULT);
    CHECK(close(copy)==0 && close(reopened)==0 && close(p[0])==0 && close(p[1])==0);
    CHECK(fchmod(-1,0600)==-1 && errno==EBADF);
    puts("FIFO PASS metadata");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    CHECK(mkdir("/proc",0755)==0 || errno==EEXIST);
    CHECK(mount("proc","/proc","proc",0,NULL)==0);
    metadata();puts("FIFO PASS all");return 0;
}
