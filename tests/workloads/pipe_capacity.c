#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { dprintf(2,"PIPE-CAPACITY FAIL line=%u errno=%d\n",__LINE__,errno);exit(1); } } while(0)
static size_t page;
static long capacity(int fd,int command,uint64_t argument)
{ return syscall(SYS_fcntl,fd,command,argument); }
static void basic(void)
{
    int fd[2];CHECK(!pipe2(fd,O_NONBLOCK));
    CHECK(capacity(fd[0],F_GETPIPE_SZ,0)==(long)(16*page));
    int alias=dup(fd[1]);CHECK(alias>=0);
    CHECK(capacity(alias,F_SETPIPE_SZ,0)==(long)page);
    CHECK(capacity(fd[0],F_GETPIPE_SZ,0)==(long)page);
    CHECK(capacity(fd[0],F_SETPIPE_SZ,page+1)==(long)(2*page));
    CHECK(capacity(fd[1],F_SETPIPE_SZ,UINT64_C(0x100000000)+1)==(long)page);
    CHECK(capacity(fd[0],F_SETPIPE_SZ,UINT32_MAX)==-1 && errno==EINVAL);
    CHECK(capacity(-1,F_GETPIPE_SZ,0)==-1 && errno==EBADF);
    int file=open("/dev/null",O_RDONLY);CHECK(file>=0);
    CHECK(capacity(file,F_GETPIPE_SZ,0)==-1 && errno==EBADF);
    CHECK(capacity(file,F_SETPIPE_SZ,0)==-1 && errno==EBADF);
    CHECK(!close(file) && !close(alias) && !close(fd[0]) && !close(fd[1]));
    puts("PIPE-CAPACITY PASS shared-endpoints-alias uint32-rounding invalid-fd nonpipe");
}
static void wrapped(void)
{
    int fd[2];CHECK(!pipe2(fd,O_NONBLOCK));
    unsigned char *input=malloc(page),*output=malloc(page);CHECK(input && output);
    CHECK(capacity(fd[1],F_SETPIPE_SZ,32*page)==(long)(32*page));
    for(unsigned i=0;i<24;i++) { memset(input,0x20+i,page);CHECK(write(fd[1],input,page)==(ssize_t)page); }
    for(unsigned i=0;i<20;i++)CHECK(read(fd[0],output,page)==(ssize_t)page);
    for(unsigned i=24;i<40;i++) { memset(input,0x20+i,page);CHECK(write(fd[1],input,page)==(ssize_t)page); }
    CHECK(capacity(fd[0],F_SETPIPE_SZ,16*page)==-1 && errno==EBUSY);
    CHECK(capacity(fd[1],F_SETPIPE_SZ,64*page)==(long)(64*page));
    for(unsigned i=20;i<40;i++) {
        CHECK(read(fd[0],output,page)==(ssize_t)page);
        for(size_t j=0;j<page;j++)CHECK(output[j]==0x20+i);
    }
    CHECK(capacity(fd[0],F_SETPIPE_SZ,page)==(long)page);
    CHECK(!close(fd[0]) && !close(fd[1]));free(input);free(output);
    puts("PIPE-CAPACITY PASS wrapped-growth occupied-slot-shrink exact-content");
}
static void fifo(void)
{
    const char *path="/tmp/pipe-capacity-fifo";unlink(path);
    CHECK(!mkfifo(path,0600));int fd=open(path,O_RDWR|O_NONBLOCK);CHECK(fd>=0);
    CHECK(capacity(fd,F_GETPIPE_SZ,0)==(long)(16*page));
    CHECK(capacity(fd,F_SETPIPE_SZ,2*page)==(long)(2*page));
    CHECK(write(fd,"fifo",4)==4);char bytes[4];CHECK(read(fd,bytes,4)==4 && !memcmp(bytes,"fifo",4));
    CHECK(!close(fd) && !unlink(path));
    puts("PIPE-CAPACITY PASS fifo-resize content-close");
}
static void quota_child(void)
{
    CHECK(!setresuid(1007,1007,1007));
    unsigned per=1048576/page,count=16384/per;
    int pipes[257][2];
    for(unsigned i=0;i<count;i++) {
        CHECK(!pipe2(pipes[i],O_NONBLOCK));
        CHECK(capacity(pipes[i][0],F_SETPIPE_SZ,1048576)==1048576);
    }
    CHECK(!pipe2(pipes[count],O_NONBLOCK));
    CHECK(capacity(pipes[count][0],F_GETPIPE_SZ,0)==(long)(2*page));
    CHECK(capacity(pipes[count][0],F_SETPIPE_SZ,4*page)==-1 && errno==EPERM);
    CHECK(capacity(pipes[0][0],F_SETPIPE_SZ,2097152)==-1 && errno==EPERM);
    CHECK(capacity(pipes[0][0],F_SETPIPE_SZ,page)==(long)page);
    CHECK(capacity(pipes[count][0],F_SETPIPE_SZ,4*page)==(long)(4*page));
    for(unsigned i=0;i<=count;i++)CHECK(!close(pipes[i][0]) && !close(pipes[i][1]));
    /* Recreate at the boundary after all old capacities have been returned. */
    int check[2];CHECK(!pipe2(check,O_NONBLOCK));
    CHECK(capacity(check[0],F_GETPIPE_SZ,0)==(long)(16*page));
    CHECK(!close(check[0]) && !close(check[1]));
    puts("PIPE-CAPACITY PASS real-uid-soft-quota two-page-default permitted-shrink returned-charge");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(90);page=(size_t)sysconf(_SC_PAGESIZE);
    CHECK(page==4096 || page==16384);CHECK(!mkdir("/tmp",0777)||errno==EEXIST);basic();wrapped();fifo();
    pid_t child=fork();CHECK(child>=0);
    if(!child) { quota_child();_exit(0); }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PIPE-CAPACITY PASS all");if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
