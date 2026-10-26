#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"STORAGE-COPY FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while (0)
static void run(const char *file,const char *one,const char *two)
{
    pid_t child=fork();CHECK(child>=0);
    if (!child) {execl(file,file,one,two,(char *)NULL);_exit(127);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void configuration(const char *path,const char *text)
{
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);CHECK(fd>=0);
    CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text) && !close(fd));
}
static void results(const char *path)
{
    int fd=open(path,O_RDONLY);CHECK(fd>=0);char bytes[4096];ssize_t n;
    while ((n=read(fd,bytes,sizeof(bytes)))>0) CHECK(write(1,bytes,n)==n);
    CHECK(n==0 && !close(fd));
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if (!access("/storage-transfer.sh",R_OK)) {
        execl("/busybox","busybox","sh","/storage-transfer.sh",(char *)NULL);CHECK(0);
    }
    CHECK(!mkdir("/proc",0555) || errno==EEXIST);
    CHECK(!mount("proc","/proc","proc",0,NULL) || errno==EBUSY);
    configuration("/performance","");
    run("/io-proc","--vectors-only",NULL);
    run("/network",NULL,NULL);
    run("/network","--vector-perf",NULL);
    run("/network","--scatter-perf",NULL);
    configuration("/warm-config","ext4 hot-read cache 1 4194304 4096 120\n");
    configuration("/range-config","ext4 range-modify fsync 1 4194304 4096 120\n");
    run("/data-path","/warm-config","/data");
    results("/warm-config.results");
    run("/data-path","/range-config","/data");
    results("/range-config.results");
    sync();puts("STORAGE-COPY PASS vectors=8 network=9 ranges=4096");return 0;
}
