#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern const unsigned char code_start[],code_end[];
__asm__(".text\n.globl code_start,code_end\ncode_start:\n jirl $zero,$ra,0\ncode_end:\n");
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"LA permissions line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
static int run(unsigned mode)
{
    void *page=mmap(0,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(page!=MAP_FAILED);
    if(mode!=0 && mode!=6)memcpy(page,code_start,code_end-code_start);
    CHECK(!mprotect(page,16384,mode==2 ? PROT_NONE : PROT_EXEC));
    if(mode==3) {
        int fd=open("/exec-page",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0 && write(fd,code_start,code_end-code_start)==code_end-code_start);
        CHECK(!munmap(page,16384));page=mmap(0,16384,PROT_EXEC,MAP_PRIVATE,fd,0);CHECK(page!=MAP_FAILED && !close(fd));
        ((void (*)(void))page)();
    }
    if(mode==4) {
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(*(volatile unsigned char *)page==code_start[0] ? 0 : 2);
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    if(mode==5 || mode==6) {
        int channel[2];CHECK(!pipe(channel));errno=0;ssize_t result=write(channel[1],page,1);
        if(mode==6)CHECK(result==-1 && errno==EFAULT);
        else {unsigned char value;CHECK(result==1 && read(channel[0],&value,1)==1 && value==code_start[0]);}
        CHECK(!close(channel[0]) && !close(channel[1]));
    } else {
        unsigned char value=*(volatile unsigned char *)page;CHECK(value==code_start[0]);
    }
    if(mode==7) {CHECK(!mprotect(page,16384,PROT_READ));((void (*)(void))page)();return 3;}
    CHECK(!munmap(page,16384));return 0;
}
int main(void)
{
    for(unsigned mode=0;mode<8;mode++) {
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(run(mode));
        int status;CHECK(waitpid(child,&status,0)==child);
        int faults=mode==0 || mode==2 || mode==7;
        if(faults)CHECK(WIFSIGNALED(status) && WTERMSIG(status)==11);
        else if(!WIFEXITED(status) || WEXITSTATUS(status)) {fprintf(stderr,"LA permissions mode=%u status=%x\n",mode,status);return 1;}
    }
    unlink("/exec-page");puts("LA permissions cold/resident/exec/fork/uaccess/revoke passed");return 0;
}
