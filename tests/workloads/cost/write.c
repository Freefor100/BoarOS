#include "common.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
static unsigned char *payload;
static void verify(int fd, unsigned size, unsigned char value)
{
    unsigned char buffer[4096]; unsigned offset=0;
    struct stat stat; CHECK(fstat(fd,&stat)==0 && stat.st_size==(off_t)size);
    while (offset<size) { size_t n=size-offset; if(n>sizeof(buffer))n=sizeof(buffer);
        CHECK(pread(fd,buffer,n,offset)==(ssize_t)n);
        for(size_t i=0;i<n;i++) CHECK(buffer[i]==value);
        offset+=(unsigned)n;
    }
}
struct reader { int fd; size_t size; unsigned char value; };
static void *receive_stream(void *arg)
{
    struct reader *r=arg; unsigned char buffer[4096]; size_t total=0;
    while(total<r->size) { ssize_t n=read(r->fd,buffer,sizeof(buffer)); CHECK(n>0);
        for(ssize_t i=0;i<n;i++) CHECK(buffer[i]==r->value);
        total+=(size_t)n;
    }
    CHECK(total==r->size); return 0;
}
static void tcp_large(int misaligned)
{
    int listener=socket(AF_INET,SOCK_STREAM,0); CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(25000+misaligned),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener,(void *)&address,sizeof(address))==0 && listen(listener,1)==0);
    int client=socket(AF_INET,SOCK_STREAM,0); CHECK(client>=0);
    CHECK(connect(client,(void *)&address,sizeof(address))==0); int server=accept(listener,0,0); CHECK(server>=0);
    struct reader reader={server,1048576,0x5a}; pthread_t thread;
    CHECK(pthread_create(&thread,0,receive_stream,&reader)==0);
    char name[64]; snprintf(name,sizeof(name),"tcp-1m-%s",misaligned?"misaligned":"aligned");
    size_t sent=0; unsigned calls=0; unsigned long long requested=0;
    cost_begin();
    while(sent<1048576) {
        size_t left=1048576-sent; requested+=left; calls++;
        ssize_t n=write(client,payload+misaligned+sent,left); CHECK(n>0); sent+=(size_t)n;
    }
    CHECK(pthread_join(thread,0)==0); cost_end(name,"stream",calls,requested,sent);
    CHECK(close(server)==0 && close(client)==0 && close(listener)==0);
}
int main(void)
{
    cost_init(); CHECK(posix_memalign((void **)&payload,4096,1048576+4096)==0); memset(payload,0x5a,1048576+4096);
    unsigned sizes[]={0,1,3,63,64,65,4096};
    int fd=open("/write-data",O_CREAT|O_TRUNC|O_RDWR,0600); CHECK(fd>=0);
    CHECK(write(fd,payload,1048576)==1048576 && fsync(fd)==0); verify(fd,1048576,0x5a);
    for(size_t s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++) {
        char path[64],name[64]; snprintf(path,sizeof(path),"/cold-%u",sizes[s]);
        snprintf(name,sizeof(name),"file-cold-%u",sizes[s]);
        int cold=open(path,O_RDWR); CHECK(cold>=0);
        cost_begin(); for(unsigned i=0;i<128;i++) CHECK(write(cold,payload,sizes[s])==(ssize_t)sizes[s]);
        cost_end(name,"file",128,128ULL*sizes[s],128ULL*sizes[s]);
        CHECK(lseek(cold,0,SEEK_CUR)==(off_t)(128ULL*sizes[s])); verify(cold,1048576,0x5a); close(cold);
    }
    for(size_t s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++) {
        char name[64]; snprintf(name,sizeof(name),"file-hot-%u",sizes[s]);
        CHECK(lseek(fd,0,SEEK_SET)==0); cost_begin();
        for(unsigned i=0;i<128;i++) CHECK(write(fd,payload,sizes[s])==(ssize_t)sizes[s]);
        cost_end(name,"file",128,128ULL*sizes[s],128ULL*sizes[s]);
        CHECK(lseek(fd,0,SEEK_CUR)==(off_t)(128ULL*sizes[s])); verify(fd,1048576,0x5a);
    }
    for(int misaligned=0;misaligned<2;misaligned++) {
        char name[64]; snprintf(name,sizeof(name),"file-1m-%s",misaligned?"misaligned":"aligned");
        CHECK(lseek(fd,0,SEEK_SET)==0); cost_begin(); CHECK(write(fd,payload+misaligned,1048576)==1048576);
        cost_end(name,"file",1,1048576,1048576); verify(fd,1048576,0x5a); tcp_large(misaligned);
    }
    int pair[2]; CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,pair)==0);
    cost_begin(); CHECK(write(pair[0],payload,65536)==65536); CHECK(read(pair[1],payload,65536)==65536);
    cost_end("dgram-64k","dgram",1,65536,65536);
    cost_begin(); CHECK(write(pair[0],payload,65537)==-1 && errno==EMSGSIZE);
    cost_end("dgram-oversize","dgram",1,65537,0); close(pair[0]); close(pair[1]);
    for(int mode=0;mode<3;mode++) {
        if(mode) { close(fd); fd=open("/write-data",O_RDWR|(mode==1?O_SYNC:O_DSYNC)); CHECK(fd>=0); }
        if(mode) {
            CHECK(fsync(fd)==0);
            cost_begin(); for(unsigned i=0;i<128;i++) CHECK(pwrite(fd,payload,4096,0)==4096);
            cost_end(mode==1?"file-osync":"file-odsync","file",128,524288,524288);
        } else for(int method=0;method<3;method++) for(int interval=1;interval<=128;interval=interval==1?16:128) {
            char name[64]; snprintf(name,sizeof(name),"file-sync-%d-every-%d",method,interval);
            char *mapping=method==2?mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0):MAP_FAILED;
            if(method==2) { CHECK(mapping!=MAP_FAILED); memcpy(mapping,payload,4096); CHECK(msync(mapping,4096,MS_SYNC)==0); }
            CHECK(fsync(fd)==0);
            cost_begin(); for(unsigned i=0;i<128;i++) {
                if(method==2) memcpy(mapping,payload,4096); else CHECK(pwrite(fd,payload,4096,0)==4096);
                if((i+1)%(unsigned)interval==0 || i==127) CHECK((method==0?fsync(fd):method==1?fdatasync(fd):msync(mapping,4096,MS_SYNC))==0);
            }
            cost_end(name,method==2?NULL:"file",method==2?0:128,method==2?0:524288,method==2?0:524288);
            if(method==2) CHECK(munmap(mapping,4096)==0);
            if(interval==128) break;
        }
    }
    close(fd); fd=open("/write-data",O_RDWR); CHECK(fd>=0);
    cost_begin(); for(unsigned i=0;i<128;i++) CHECK(pwrite(fd,payload,4096,(off_t)((i*73)%256)*4096)==4096);
    cost_end("file-random","file",128,524288,524288);
    int extend=open("/extend-data",O_CREAT|O_TRUNC|O_RDWR,0600); CHECK(extend>=0);
    cost_begin(); for(unsigned i=0;i<128;i++) CHECK(write(extend,payload,4096)==4096);
    cost_end("file-extend-accept","file",128,524288,524288);
    cost_begin(); CHECK(fsync(extend)==0); cost_end("file-extend-sync",NULL,0,0,0); verify(extend,524288,0x5a); close(extend);
    char *fault=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(fault!=MAP_FAILED);
    memset(fault,0x5a,4096); CHECK(munmap(fault+4096,4096)==0);
    cost_begin(); CHECK(pwrite(fd,fault,8192,0)==4096); cost_end("file-fault-prefix","file",1,8192,4096);
    cost_begin(); CHECK(pwrite(fd,(void *)1,4096,0)==-1 && errno==EFAULT); cost_end("file-fault-first","file",1,4096,0);
    CHECK(munmap(fault,4096)==0); verify(fd,1048576,0x5a); close(fd); free(payload);
    puts("COST PASS write"); return 0;
}
