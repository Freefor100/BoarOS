#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"pipe geometry failure line=%u errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE),capacity=16*page;
    unsigned char *input=malloc(page),*output=malloc(capacity);CHECK(input && output);
    int fd[2];CHECK(!pipe2(fd,O_NONBLOCK));
    for(unsigned i=0;i<16;i++) {
        memset(input,(int)('A'+i),page);
        CHECK(write(fd[1],input,page)==(ssize_t)page);
    }
    CHECK(write(fd[1],input,1)==-1 && errno==EAGAIN);
    CHECK(read(fd[0],output,15*page)==(ssize_t)(15*page));
    for(unsigned i=0;i<15;i++) {
        memset(input,(int)('a'+i),page);
        CHECK(write(fd[1],input,page)==(ssize_t)page);
    }
    CHECK(read(fd[0],output,capacity)==(ssize_t)capacity);
    for(size_t i=0;i<capacity;i++)CHECK(output[i]==(i<page ? 'P' : 'a'+i/page-1));
    CHECK(!close(fd[0]) && !close(fd[1]));free(input);free(output);
    puts("PIPE GEOMETRY PASS 16 target pages, wrap, content and close");
    return 0;
}
