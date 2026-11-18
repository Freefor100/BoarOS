#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"DIRECT FILE FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define FAIL(e,x) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static unsigned char data[32768] __attribute__((aligned(16384)));
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);const char *dir=argc>1?argv[1]:"/";
    size_t page=sysconf(_SC_PAGESIZE);CHECK(page<=16384);
    int fd=open(dir,O_TMPFILE|O_RDWR|O_DIRECT,0600);CHECK(fd>=0);
    memset(data,0x41,sizeof(data));
    int probe=argc>2&&!strcmp(argv[2],"--probe-alignment");
    if (probe) {
        errno=0;ssize_t result=pwrite(fd,data+1,512,0);
        printf("alignment probe unaligned address: result=%zd errno=%d\n",result,errno);
        errno=0;result=pwrite(fd,data,511,0);
        printf("alignment probe unaligned length: result=%zd errno=%d\n",result,errno);
    } else {
        FAIL(EINVAL,pwrite(fd,data+1,512,0));
        FAIL(EINVAL,pwrite(fd,data,511,0));
    }
    CHECK(pwrite(fd,data,2*page,0)==(ssize_t)(2*page));
    struct iovec bad[2]={{data,256},{data+256,256}};
    if (probe) {
        errno=0;ssize_t result=preadv(fd,bad,2,0);
        printf("alignment probe short vector segments: result=%zd errno=%d\n",result,errno);
    } else FAIL(EINVAL,preadv(fd,bad,2,0));
    unsigned char *map=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(map!=MAP_FAILED);
    map[0]=0x62;CHECK(pread(fd,data,page,0)==(ssize_t)page&&data[0]==0x62);
    map[0]=0x63;CHECK(pwrite(fd,map,page,page)==(ssize_t)page&&map[page]==0x63);
    CHECK(pread(fd,map,page,page)==(ssize_t)page&&map[0]==0x63);
    CHECK(!munmap(map,2*page));
    void *fault=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(fault!=MAP_FAILED);
    FAIL(EFAULT,pread(fd,fault,page,0));FAIL(EFAULT,pwrite(fd,fault,page,0));CHECK(!munmap(fault,page));
    CHECK(!ftruncate(fd,123));CHECK(pread(fd,data,512,0)==123);
    CHECK(pread(fd,data,512,512)==0);
    memset(data,0x71,page);CHECK(pwrite(fd,data,page,4*page)==(ssize_t)page);
    CHECK(pread(fd,data,page,page)==(ssize_t)page);for(size_t i=0;i<page;i++)CHECK(!data[i]);
    int flags=fcntl(fd,F_GETFL);CHECK(flags&O_DIRECT);int copy=dup(fd);CHECK(copy>=0);
    CHECK(!fcntl(copy,F_SETFL,flags&~O_DIRECT));CHECK(!(fcntl(fd,F_GETFL)&O_DIRECT));
    CHECK(pwrite(fd,"z",1,0)==1);CHECK(!fcntl(copy,F_SETFL,flags));
    CHECK(pread(fd,data,512,0)==512&&data[0]=='z');
    int target=open(dir,O_TMPFILE|O_RDWR|O_DIRECT,0600);CHECK(target>=0);
    off_t position=0;CHECK(sendfile(target,fd,&position,page)==(ssize_t)page&&position==(off_t)page);
    CHECK(pread(target,data,page,0)==(ssize_t)page&&data[0]=='z');
    int pipefd[2];CHECK(!pipe(pipefd));position=0;
    CHECK(splice(fd,&position,pipefd[1],NULL,page,0)==(ssize_t)page);
    position=page;CHECK(splice(pipefd[0],NULL,target,&position,page,0)==(ssize_t)page);
    CHECK(pread(target,data,page,page)==(ssize_t)page&&data[0]=='z');
    CHECK(!close(pipefd[0])&&!close(pipefd[1]));
    CHECK(!fsync(target)&&!fsync(fd));CHECK(!close(target)&&!close(copy)&&!close(fd));
    puts("DIRECT FILE ALL PASS");return 0;
}
