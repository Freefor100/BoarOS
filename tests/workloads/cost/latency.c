#include "common.h"
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
static void blocked(pid_t pid)
{
    char path[64],s[512];snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    for(unsigned i=0;i<100000;i++){int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,s,sizeof(s)-1);CHECK(n>0);close(fd);s[n]=0;char *state=strrchr(s,')');CHECK(state);if(state[2]=='S')return;sched_yield();}CHECK(0);
}
static unsigned char payload[1048576],verify[1048576];
static void phase(size_t bytes, int protect)
{
    int fd=open("/hot/data",O_RDWR);CHECK(fd>=0);CHECK(pwrite(fd,payload,1048576,0)==1048576);
    char *mapping=mmap(0,1048576,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);memcpy(mapping,payload,1048576);
    int gate[2],ready[2];CHECK(pipe(gate)==0 && pipe(ready)==0);pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(write(ready[1],"r",1)==1);char c;CHECK(read(gate[0],&c,1)==1);_exit(0);}
    char c;CHECK(read(ready[0],&c,1)==1);blocked(child);char name[64];snprintf(name,sizeof(name),"latency-%s-%zu",protect?"protect":"copy",bytes);
    cost_begin();CHECK(write(gate[1],"g",1)==1);
    if(protect){CHECK(mprotect(mapping,bytes,PROT_READ)==0);CHECK(mprotect(mapping,bytes,PROT_READ|PROT_WRITE)==0);}
    else for(unsigned i=0;i<8;i++)CHECK(pwrite(fd,payload,bytes,0)==(ssize_t)bytes);
    int status;CHECK(waitpid(child,&status,0)==child && status==0);cost_end(name,protect?0:"file",protect?0:8,bytes*8,bytes*8);
    if(!protect){CHECK(pread(fd,verify,bytes,0)==(ssize_t)bytes);CHECK(memcmp(payload,verify,bytes)==0);}
    CHECK(munmap(mapping,1048576)==0);close(fd);close(gate[0]);close(gate[1]);close(ready[0]);close(ready[1]);
}
int main(void)
{
    cost_init();mkdir("/hot",0755);CHECK(mount("tmpfs","/hot","tmpfs",0,0)==0);memset(payload,0xa5,sizeof(payload));int fd=open("/hot/data",O_RDWR|O_CREAT,0600);CHECK(fd>=0);close(fd);
    phase(4096,0);phase(65536,0);phase(1048576,0);phase(4096,1);phase(1048576,1);CHECK(unlink("/hot/data")==0);CHECK(umount("/hot")==0);puts("COST PASS latency");
    /* stdout尚有TTY排队字节；真实排空后才能交给PID1关机raw输出。 */
    CHECK(fflush(stdout)==0);CHECK(tcdrain(STDOUT_FILENO)==0);return 0;
}
