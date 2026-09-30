#include "common.h"
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
static const char *second_path="/lock-b";
static void blocked(pid_t pid)
{
    char path[64],text[512]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    for(unsigned i=0;i<100000;i++) {
        int fd=open(path,O_RDONLY); CHECK(fd>=0); ssize_t n=read(fd,text,sizeof(text)-1); CHECK(n>0); close(fd);
        text[n]=0; char *state=strrchr(text,')'); CHECK(state);
        if(state[2]=='S') return;
        sched_yield();
    }
    CHECK(0);
}
static void phase(unsigned waiters, unsigned relationship, unsigned operation)
{
    unsigned jobs=waiters+1; pid_t children[33]; int statuses[33];
    int flags=O_RDWR|(operation==1?O_APPEND:operation==2?O_SYNC:0);
    int base=open("/lock-a",flags); CHECK(base>=0);
    struct stat original; CHECK(fstat(base,&original)==0);
    int input=open(operation==4?"/lock-input":"/lock-a",O_RDONLY); CHECK(input>=0);
    unsigned char *mapping=mmap(0,8192,PROT_READ,MAP_PRIVATE,input,0); CHECK(mapping!=MAP_FAILED);
    int ready[2],gate[2]; CHECK(pipe(ready)==0 && pipe(gate)==0);
    for(unsigned i=0;i<jobs;i++) {
        children[i]=fork(); CHECK(children[i]>=0);
        if(!children[i]) {
            int target=base;
            if(relationship) { target=open(relationship==1||i%2==0?"/lock-a":second_path,flags); CHECK(target>=0); }
            CHECK(write(ready[1],"r",1)==1); char token; CHECK(read(gate[0],&token,1)==1);
            if(operation==4 && i==0) CHECK(ftruncate(target,0)==0);
            else if(operation==1) {
                struct iovec vectors[]={{mapping,4096},{mapping+4096,4096}};
                CHECK(writev(target,vectors,2)==8192);
            } else if(operation==0) CHECK(write(target,mapping,8192)==8192);
            else CHECK(pwrite(target,mapping,8192,0)==8192);
            if(relationship) close(target);
            _exit(0);
        }
    }
    for(unsigned i=0;i<jobs;i++) { char token; CHECK(read(ready[0],&token,1)==1); blocked(children[i]); }
    char name[80]; snprintf(name,sizeof(name),"locking-%u-%u-%u",relationship,operation,waiters);
    cost_begin();
    if(operation==3) CHECK(kill(children[0],SIGTERM)==0);
    char tokens[33]; memset(tokens,'g',jobs); CHECK(write(gate[1],tokens,jobs)==(ssize_t)jobs);
    for(unsigned i=0;i<jobs;i++) { CHECK(waitpid(children[i],&statuses[i],0)==children[i]);
        if(operation==3 && i==0) CHECK(WIFSIGNALED(statuses[i]) && WTERMSIG(statuses[i])==SIGTERM);
        else CHECK(statuses[i]==0);
    }
    unsigned calls=jobs-(operation==3||operation==4?1:0);
    cost_end(name,"file",calls,calls*8192ULL,calls*8192ULL);
    for(unsigned i=0;i<jobs;i++) printf("COST STATUS %s %u %d\n",name,i,statuses[i]);
    CHECK(lseek(base,0,SEEK_CUR)==(relationship==0 && operation==0?(off_t)jobs*8192:0));
    struct stat after; CHECK(fstat(base,&after)==0);
    if(operation==1) CHECK(after.st_size==original.st_size+(off_t)(relationship==2?(jobs+1)/2:jobs)*8192);
    else if(operation==4) CHECK(after.st_size==0 || after.st_size==8192);
    if(after.st_size>=8192) {
        unsigned char data[8192]; CHECK(pread(base,data,sizeof(data),0)==sizeof(data));
        for(unsigned i=0;i<sizeof(data);i++) CHECK(data[i]==0x5a);
    }
    CHECK(munmap(mapping,8192)==0); close(input); close(base);
    close(ready[0]); close(ready[1]); close(gate[0]); close(gate[1]);
}
int main(void)
{
    cost_init();
    if(access("/cost-dual",F_OK)==0) {
        mkdir("/second",0755); mkdir("/dev",0755);
        CHECK(mknod("/dev/vdb",S_IFBLK|0600,makedev(252,16))==0);
        CHECK(mount("/dev/vdb","/second","ext4",0,0)==0);
        second_path="/second/lock-b";
    }
    unsigned counts[]={1,8,32};
    for(unsigned relationship=0;relationship<3;relationship++)
        for(unsigned n=0;n<3;n++) phase(counts[n],relationship,0);
    for(unsigned operation=1;operation<=4;operation++) phase(8,1,operation);
    if(strcmp(second_path,"/lock-b")) CHECK(umount("/second")==0);
    puts("COST PASS locking"); return 0;
}
