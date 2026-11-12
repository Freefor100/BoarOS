#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sched.h>
#include "../../common/raw_syscall.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"ADVICE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
static unsigned cases;
static long advice(int fd,int64_t off,int64_t len,int kind)
{ return test_syscall6(223,fd,off,len,kind,0,0); }
static long ahead(int fd,int64_t off,size_t count)
{ return test_syscall6(213,fd,off,count,0,0,0); }
static void pass(const char *name) { printf("ADVICE CASE %s PASS\n",name);cases++; }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);size_t page=(size_t)sysconf(_SC_PAGESIZE),length=8*page+123;
    int fd=open("/file-advice.data",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    char *data=malloc(length);CHECK(data);for(size_t i=0;i<length;i++)data[i]=(char)(i*17+3);
    CHECK(write(fd,data,length)==(ssize_t)length && fsync(fd)==0 && lseek(fd,7,SEEK_SET)==7);
    struct stat before,after;CHECK(fstat(fd,&before)==0);
    for(int kind=0;kind<6;kind++) CHECK(advice(fd,0,0,kind)==0 && posix_fadvise(fd,0,0,kind)==0);
    CHECK(advice(fd,-1,0,0)==-EINVAL && advice(fd,0,-1,0)==-EINVAL && advice(fd,0,0,6)==-EINVAL);
    CHECK(advice(fd,INT64_MAX-2,INT64_MAX,POSIX_FADV_WILLNEED)==0);
    CHECK(advice(-1,-1,-1,7)==-EBADF && ahead(-1,-1,1)==-EBADF);
    int pipes[2];CHECK(pipe(pipes)==0);
    CHECK(advice(pipes[0],-1,-1,7)==-ESPIPE && ahead(pipes[0],0,1)==-EINVAL);
    CHECK(close(pipes[0])==0 && close(pipes[1])==0);
    int path=open("/file-advice.data",O_PATH),wo=open("/file-advice.data",O_WRONLY),dir=open("/",O_RDONLY|O_DIRECTORY);
    CHECK(path>=0 && wo>=0 && dir>=0);
    CHECK(advice(path,0,0,0)==-EBADF && ahead(path,0,1)==-EBADF);
    CHECK(advice(wo,0,0,POSIX_FADV_WILLNEED)==0 && ahead(wo,0,1)==-EBADF && ahead(dir,0,1)==-EINVAL);
    CHECK(ahead(fd,-1,1)==-EINVAL && ahead(fd,0,0)==0 && readahead(fd,page,page)>=0);
    CHECK(fstat(fd,&after)==0 && before.st_size==after.st_size && before.st_atim.tv_sec==after.st_atim.tv_sec &&
        before.st_atim.tv_nsec==after.st_atim.tv_nsec && lseek(fd,0,SEEK_CUR)==7);
    close(path);close(wo);close(dir);pass("abi-types-offset-libc");
    char *map=mmap(NULL,length,PROT_READ,MAP_SHARED,fd,0);CHECK(map!=MAP_FAILED);
    CHECK(pread(fd,data,length,0)==(ssize_t)length);
    unsigned char resident[9];CHECK(mincore(map,length,resident)==0);
    for(unsigned n=0;n<9;n++)CHECK(resident[n]&1);
    CHECK(advice(fd,page+1,2*page-1,POSIX_FADV_DONTNEED)==0 && mincore(map,length,resident)==0);
    CHECK(!(resident[2]&1)); /* Only page 2 is entirely covered. */
    CHECK(advice(fd,8*page,124,POSIX_FADV_DONTNEED)==0 && mincore(map,length,resident)==0 && (resident[8]&1));
    CHECK(advice(fd,8*page,123,POSIX_FADV_DONTNEED)==0 && mincore(map,length,resident)==0 && !(resident[8]&1));
    CHECK(ahead(fd,2*page,page)==0);
    int loaded=0;for(unsigned n=0;n<200000;n++) { CHECK(mincore(map,length,resident)==0);if(resident[2]&1){loaded=1;break;}sched_yield(); }
    CHECK(loaded && map[2*page]==data[2*page]);
    CHECK(advice(fd,2*page,page,POSIX_FADV_DONTNEED)==0 && map[2*page]==data[2*page]);
    CHECK(munmap(map,length)==0);pass("scoped-clean-eviction-and-explicit-load");
    int mem=memfd_create("advice",0);CHECK(mem>=0 && write(mem,data,page)==(ssize_t)page);
    for(int kind=0;kind<6;kind++)CHECK(advice(mem,0,0,kind)==0);
    CHECK(ahead(mem,0,page)==0 && pread(mem,data,page,0)==(ssize_t)page && close(mem)==0);
    int other=open("/file-advice.data",O_RDONLY),shared=dup(fd);CHECK(other>=0 && shared>=0);
    CHECK(advice(fd,0,0,POSIX_FADV_RANDOM)==0 && advice(shared,0,0,POSIX_FADV_NOREUSE)==0 &&
        advice(other,0,0,POSIX_FADV_SEQUENTIAL)==0 && advice(fd,0,0,POSIX_FADV_NORMAL)==0);
    char bytes[123];CHECK(pread(other,bytes,sizeof(bytes),8*page)==sizeof(bytes) && !memcmp(bytes,data+8*page,sizeof(bytes)));
    close(other);close(shared);close(fd);CHECK(unlink("/file-advice.data")==0);free(data);pass("memory-backed-and-shared-ofd");
    printf("ADVICE RESULT cases=%u failures=0\n",cases);return 0;
}
