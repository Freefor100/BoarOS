/* Standalone Linux reference probe; never changes pool configuration or touches
 * an unreserved mapping. The same program can run in a BoarOS guest. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static size_t huge_size;
static int failures;
static void probe(const char *name,int flags,int fd,off_t off,size_t size,void *hint,int expected)
{
    errno=0;
    void *p=mmap(hint,size,PROT_READ|PROT_WRITE,flags,fd,off);
    int result=p==MAP_FAILED?errno:0;
    printf("%s=%d\n",name,result);
    if(expected>=0 && result!=expected)failures++;
    if(p!=MAP_FAILED && munmap(p,(size+huge_size-1)&~(huge_size-1)))failures++;
}
int main(int argc,char **argv)
{
    huge_size=argc>1?strtoull(argv[1],0,0):2097152;
    if(!huge_size || (huge_size&(huge_size-1)))return 2;
    long base=sysconf(_SC_PAGESIZE);
    int flags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB;
    int fd=open("/dev/zero",O_RDWR);
    if(fd<0)return 2;
    probe("reserved",flags,-1,0,base,0,-1); /* Pool-dependent. */
    probe("private-noreserve",flags|MAP_NORESERVE,-1,0,base,0,0);
    probe("shared-noreserve",MAP_SHARED|MAP_ANONYMOUS|MAP_HUGETLB|MAP_NORESERVE,-1,0,base,0,0);
    probe("base-offset",flags|MAP_NORESERVE,-1,base,base,0,EINVAL);
    probe("huge-offset",flags|MAP_NORESERVE,-1,huge_size,base,0,0);
    probe("growsdown",flags|MAP_NORESERVE|MAP_GROWSDOWN,-1,0,base,0,EINVAL);
    probe("unknown-size",flags|MAP_NORESERVE|(17U<<26),-1,0,base,0,EINVAL);
    probe("fixed-base-alignment",flags|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0,base,(void *)(uintptr_t)(2*huge_size+base),EINVAL);
    probe("ordinary-fd",MAP_PRIVATE|MAP_HUGETLB,fd,0,base,0,EINVAL);
    probe("invalid-fd",MAP_PRIVATE|MAP_HUGETLB,-1,0,base,0,EBADF);
    close(fd);
    return failures?1:0;
}
