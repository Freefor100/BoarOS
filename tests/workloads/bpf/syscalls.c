#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    int ready=open("/tmp/bpf-ready",O_WRONLY|O_CLOEXEC);
    if(ready<0 || write(ready,"R\n",2)!=2)return 2;
    close(ready);
    int start=open("/tmp/bpf-start",O_RDONLY|O_CLOEXEC);
    char byte;
    if(start<0 || read(start,&byte,1)!=1)return 3;
    close(start);
    for (unsigned i=0;i<64;i++) if(syscall(SYS_getpgid,0)<0)return 1;
    if(argc>1 && !strcmp(argv[1],"interrupt")) {
        ready=open("/tmp/bpf-ready",O_WRONLY|O_CLOEXEC);
        if(ready<0 || write(ready,"R\n",2)!=2)return 2;
        close(ready);
        int control=open("/tmp/bpf-control",O_RDONLY|O_CLOEXEC);
        if(control<0 || read(control,&byte,1)!=1)return 3;
        close(control);
    }
    return 0;
}
