#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"PROCESS_MEMORY FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static size_t page;
static void byte(int fd){CHECK(write(fd,"x",1)==1);}
static void await(int fd){char x;CHECK(read(fd,&x,1)==1&&x=='x');}
static void joined(pid_t c){int s;CHECK(waitpid(c,&s,0)==c&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static void permissions(void)
{
    pid_t helper=fork();CHECK(helper>=0);
    if(!helper){
        struct {uint32_t version;int32_t pid;} header={0x20080522,0};
        struct {uint32_t effective,permitted,inheritable;} caps[2]={{0}};
        CHECK(!syscall(SYS_capset,&header,caps));
        int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t child=fork();CHECK(child>=0);
        if(!child){CHECK(!prctl(PR_SET_DUMPABLE,0));byte(from[1]);await(to[0]);_exit(0);}
        await(from[0]);unsigned char value=1;struct iovec iov={&value,1};
        ERROR(process_vm_readv(child,&iov,1,&iov,1,0),EPERM);
        ERROR(process_vm_writev(child,&iov,1,&iov,1,0),EPERM);
        byte(to[1]);joined(child);_exit(0);
    }
    joined(helper);puts("PROCESS_MEMORY CASE realcreds-dumpable-denial PASS");
}
static void replaced_mm(const char *program)
{
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t c=fork();CHECK(c>=0);
    if(!c){
        char input[24],output[24];snprintf(input,sizeof(input),"%d",to[0]);snprintf(output,sizeof(output),"%d",from[1]);
        execl(program,program,"--exec-child",input,output,(char *)0);_exit(1);
    }
    uintptr_t pointer;CHECK(read(from[0],&pointer,sizeof(pointer))==sizeof(pointer));
    unsigned char value=0;struct iovec local={&value,1},remote={(void *)pointer,1};
    CHECK(process_vm_readv(c,&local,1,&remote,1,0)==1&&value==119);
    value=123;CHECK(process_vm_writev(c,&local,1,&remote,1,0)==1);
    byte(to[1]);joined(c);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));
    puts("PROCESS_MEMORY CASE exec-new-mm PASS");
}
int main(int argc,char **argv)
{
    page=sysconf(_SC_PAGESIZE);CHECK(page==4096||page==16384);
    if(argc==4&&!strcmp(argv[1],"--exec-child")){
        int input=atoi(argv[2]),output=atoi(argv[3]);
        unsigned char *p=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
        p[0]=119;uintptr_t pointer=(uintptr_t)p;CHECK(write(output,&pointer,sizeof(pointer))==sizeof(pointer));
        await(input);CHECK(p[0]==123);_exit(0);
    }
    char input[16]="remote-memory",output[16]={0};
    struct iovec local={output,sizeof(output)},remote={input,sizeof(input)};
    CHECK(process_vm_readv(getpid(),&local,1,&remote,1,0)==16&&!memcmp(input,output,16));
    puts("PROCESS_MEMORY CASE self-read PASS");
    ERROR(process_vm_readv(getpid(),0,0,0,0,1),EINVAL);
    CHECK(process_vm_readv(-1,0,0,(void *)-1,1025,0)==0);
    CHECK(process_vm_readv(-1,&local,1,0,0,0)==0);
    ERROR(process_vm_readv(-1,&local,1,&remote,1,0),ESRCH);
    ERROR(process_vm_readv(getpid(),(void *)-1,1,&remote,1,0),EFAULT);
    ERROR(process_vm_readv(getpid(),&local,1025,&remote,1,0),EINVAL);
    remote.iov_len=SIZE_MAX;ERROR(process_vm_readv(getpid(),&local,1,&remote,1,0),EINVAL);
    remote=(struct iovec){(void *)-1,1};ERROR(process_vm_readv(getpid(),&local,1,&remote,1,0),EFAULT);
    puts("PROCESS_MEMORY CASE validation-order PASS");
    unsigned char *p=mmap(0,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
    memset(p,41,3*page);CHECK(!mprotect(p+page,page,PROT_NONE));
    local=(struct iovec){output,8};remote=(struct iovec){p+page-4,8};
    CHECK(process_vm_readv(getpid(),&local,1,&remote,1,0)==4);
    CHECK(process_vm_writev(getpid(),&local,1,&remote,1,0)==4);
    CHECK(!mprotect(p+page,page,PROT_READ));remote=(struct iovec){p+page,8};
    ERROR(process_vm_writev(getpid(),&local,1,&remote,1,0),EFAULT);
    struct iovec many[9];for(unsigned i=0;i<9;i++)many[i]=(struct iovec){p+2*page+i,1};
    local=(struct iovec){output,9};CHECK(process_vm_readv(getpid(),&local,1,many,9,0)==9);
    puts("PROCESS_MEMORY CASE partial-readonly-heap-iov PASS");
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t c=fork();CHECK(c>=0);
    if(!c){byte(from[1]);await(to[0]);CHECK(p[0]==73&&p[2*page]==41);byte(from[1]);_exit(0);}
    await(from[0]);unsigned char value=73;local=(struct iovec){&value,1};remote=(struct iovec){p,1};
    CHECK(process_vm_writev(c,&local,1,&remote,1,0)==1&&p[0]==41);
    byte(to[1]);await(from[0]);joined(c);
    ERROR(process_vm_readv(c,&local,1,&remote,1,0),ESRCH);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1])&&!munmap(p,3*page));
    puts("PROCESS_MEMORY CASE child-cow-exit PASS");
    permissions();replaced_mm(argv[0]);
    puts("PROCESS_MEMORY PASS");return 0;
}
