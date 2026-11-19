#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"GROWDOWN FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static size_t p;
static void write_child(volatile char *a,int signal_number)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child) {*a=99;_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child);
    CHECK(signal_number ? WIFSIGNALED(status)&&WTERMSIG(status)==signal_number : WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);p=(size_t)sysconf(_SC_PAGESIZE);
    size_t room=2048*p;
    char *base=mmap(NULL,room,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(base!=MAP_FAILED);
    CHECK(!munmap(base,room));
    char *top=base+room;
    char *s=mmap(top-4*p,4*p,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_GROWSDOWN,-1,0);CHECK(s==top-4*p);
    CHECK(mmap(base,p,PROT_READ,MAP_SHARED|MAP_ANONYMOUS|MAP_GROWSDOWN,-1,0)==MAP_FAILED && errno==EINVAL);
    int fd=open("/dev/zero",O_RDONLY);CHECK(fd>=0);
    CHECK(mmap(base,p,PROT_READ,MAP_PRIVATE|MAP_GROWSDOWN,fd,0)==MAP_FAILED && errno==EINVAL);CHECK(!close(fd));
    CHECK(!mprotect(s-p,2*p,PROT_READ|PROT_WRITE|PROT_GROWSDOWN)); /* starts in hole, overlaps stack */
    CHECK(mprotect(s-2*p,p,PROT_READ|PROT_GROWSDOWN)==-1 && errno==ENOMEM);
    s[-1]=17;CHECK(s[-1]==17 && s[-(long)p]==0);
    write_child(s-2*p,0); /* child expands independently */
    unsigned char resident;
    CHECK(mincore(s-2*p,p,&resident)==-1 && errno==ENOMEM);
    CHECK(!mprotect(s+2*p,2*p,PROT_READ)); /* split preserves growth on low fragment */
    s[-2*(long)p]=18;CHECK(s[-2*(long)p]==18);
    CHECK(!mprotect(s+1*p,p,PROT_READ|PROT_GROWSDOWN));
    write_child(s-3*p,SIGSEGV); /* inherited low fragment is read-only */
    CHECK(!mprotect(s-2*p,4*p,PROT_READ|PROT_WRITE));
    CHECK(!munmap(s-2*p,2*p));s[-1]=19;CHECK(s[-1]==19); /* hole can regrow */
    CHECK(mmap(s-258*p,p,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==s-258*p);
    write_child(s-2*p,SIGSEGV); /* below 256-page guard */
    CHECK(!munmap(s-258*p,p));
    struct rlimit before,limit;CHECK(!getrlimit(RLIMIT_STACK,&before));limit=before;limit.rlim_cur=2*p;CHECK(!setrlimit(RLIMIT_STACK,&limit));
    write_child(s-2*p,SIGSEGV);CHECK(!setrlimit(RLIMIT_STACK,&before));
    CHECK(mmap(s-1*p,5*p,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==s-p);
    write_child(s-2*p,SIGSEGV); /* replacement is not grow-down */
    CHECK(mprotect(s,p,PROT_READ|PROT_GROWSDOWN)==-1 && errno==EINVAL);
    CHECK(!munmap(base,room));
    puts("GROWDOWN ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
