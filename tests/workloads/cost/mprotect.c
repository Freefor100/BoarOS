#include "common.h"
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
static volatile unsigned checksum;
int main(void)
{
    cost_init();
    int file=open("/resident-data",O_RDONLY); CHECK(file>=0);
    for (unsigned resident=0;resident<=64;resident=resident==0?16:resident*4) {
        size_t bytes=(size_t)resident*1048576;
        unsigned char *pages=0;
        if (bytes) { pages=mmap(0,bytes,PROT_READ,MAP_PRIVATE,file,0); CHECK(pages!=MAP_FAILED); for(size_t i=0;i<bytes;i+=4096) checksum+=pages[i]; }
        for (unsigned count=16;count<=256;count*=4) {
            void *unrelated[256];
            for(unsigned i=0;i<count;i++) { unrelated[i]=mmap((void *)(0x10000000UL+i*8192UL),4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0); CHECK(unrelated[i]!=MAP_FAILED); }
            unsigned char *target=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(target!=MAP_FAILED); target[4096]=0x5a;
            char name[80]; snprintf(name,sizeof(name),"mprotect-%u-%u",count,resident);
            cost_begin(); CHECK(mprotect(target+4096,4096,PROT_READ)==0); CHECK(target[4096]==0x5a); CHECK(mprotect(target+4096,4096,PROT_READ|PROT_WRITE)==0); target[4096]=0xa5;
            cost_end(name,0,0,0,0);
            printf("COST METRIC %s mprotect_resident_visits %u\n",name,resident*256*2);
            printf("COST METRIC %s mprotect_calls 2\n",name);
            CHECK(target[4096]==0xa5); CHECK(munmap(target,12288)==0);
            for(unsigned i=0;i<count;i++) CHECK(munmap(unrelated[i],4096)==0);
        }
        if(bytes) CHECK(munmap(pages,bytes)==0);
        if(resident==64) break;
    }
    int shmid=shmget(IPC_PRIVATE,4096,IPC_CREAT|0600); CHECK(shmid>=0); char *ro=shmat(shmid,0,SHM_RDONLY); CHECK(ro!=(void *)-1);
    unsigned char *hole=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(hole!=MAP_FAILED); hole[0]=11; CHECK(munmap(hole+4096,4096)==0);
    cost_begin(); errno=0; CHECK(mprotect(ro,4096,PROT_READ|PROT_WRITE)==-1 && errno==EACCES); errno=0; CHECK(mprotect(hole,12288,PROT_READ)==-1 && errno==ENOMEM); hole[0]=12;
    cost_end("mprotect-failure",0,0,0,0); printf("COST METRIC mprotect-failure mprotect_pte_visits 0\n");
    CHECK(shmdt(ro)==0); CHECK(shmctl(shmid,IPC_RMID,0)==0); CHECK(munmap(hole,4096)==0); CHECK(munmap(hole+8192,4096)==0); CHECK(close(file)==0);
    puts("COST PASS mprotect"); return 0;
}
