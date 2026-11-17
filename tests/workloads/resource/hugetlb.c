/* Run only in a disposable guest: deliberately changes its native huge pool. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"HUGETLB FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));}while(0)
static size_t huge,page;
static unsigned cases;
static void pass(const char *s){printf("HUGETLB CASE %s PASS\n",s);cases++;}
static unsigned long counter(const char *name)
{
    FILE *f=fopen("/proc/meminfo","r");CHECK(f);char line[256];unsigned long value=0;int found=0;
    while(fgets(line,sizeof line,f))if(!strncmp(line,name,strlen(name))){CHECK(sscanf(line+strlen(name),"%lu",&value)==1);found=1;break;}
    CHECK(!fclose(f)&&found);return value;
}
static void pool(unsigned n)
{
    int fd=open("/proc/sys/vm/nr_hugepages",O_WRONLY);CHECK(fd>=0);char b[32];int len=snprintf(b,sizeof b,"%u\n",n);
    CHECK(write(fd,b,len)==len);CHECK(!close(fd));
    fd=open("/proc/sys/vm/nr_hugepages",O_RDONLY);CHECK(fd>=0);ssize_t z=read(fd,b,sizeof b-1);CHECK(z>0);b[z]=0;CHECK(strtoul(b,0,10)==n);CHECK(!close(fd));
}
static unsigned char *map(size_t n,int flags,off_t off)
{void *p=mmap(0,n,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_HUGETLB|flags,-1,off);CHECK(p!=MAP_FAILED);CHECK(!((uintptr_t)p&(huge-1)));return p;}
static void joined(pid_t child,int signal)
{int s;CHECK(waitpid(child,&s,0)==child);if(signal)CHECK(WIFSIGNALED(s)&&WTERMSIG(s)==signal);else CHECK(WIFEXITED(s)&&!WEXITSTATUS(s));}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=sysconf(_SC_PAGESIZE);CHECK(page==4096||page==16384);
    CHECK(!mkdir("/proc",0555)||errno==EEXIST);
    if(access("/proc/meminfo",R_OK))CHECK(!mount("proc","/proc","proc",0,0));
    huge=counter("Hugepagesize:")*1024;CHECK(huge>page&&!(huge&(huge-1)));
    printf("HUGETLB BEGIN page=%zu huge=%zu\n",page,huge);
    pool(0);CHECK(!counter("HugePages_Total:"));
    errno=0;CHECK(mmap(0,huge,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,-1,0)==MAP_FAILED&&errno==ENOMEM);
    errno=0;CHECK(mmap(0,huge,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_GROWSDOWN|MAP_NORESERVE,-1,0)==MAP_FAILED&&errno==EINVAL);
    unsigned char *stack=mmap(0,4*page,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN,-1,0);
    CHECK(stack!=MAP_FAILED);stack[0]=17;stack[4*page-1]=29;
    CHECK(stack[0]==17&&stack[4*page-1]==29);CHECK(!munmap(stack,4*page));
    pass("growsdown-flag-isolation");
    pool(4);CHECK(counter("HugePages_Total:")==4&&counter("HugePages_Free:")==4);pass("real-pool");
    unsigned char *p=map(huge,MAP_PRIVATE,0);CHECK(counter("HugePages_Rsvd:")==1);
    unsigned char *vec=calloc(huge/page,1);CHECK(vec);CHECK(!mincore(p,huge,vec));
    for(size_t i=0;i<huge/page;i++)CHECK(!(vec[i]&1));
    CHECK(!p[0]&&!p[huge-1]);p[0]=19;p[page+7]=37;p[huge/2+11]=53;p[huge-1]=71;
    CHECK(!mincore(p,huge,vec));for(size_t i=0;i<huge/page;i++)CHECK(vec[i]&1);
    CHECK(counter("HugePages_Free:")==3&&!counter("HugePages_Rsvd:"));pass("fault-offset-mincore");
    pid_t c=fork();CHECK(c>=0);if(!c){CHECK(p[0]==19&&p[page+7]==37&&p[huge/2+11]==53&&p[huge-1]==71);p[page+7]=91;p[huge-1]=92;_exit(0);}joined(c,0);
    CHECK(p[page+7]==37&&p[huge-1]==71);CHECK(counter("HugePages_Free:")==3);pass("private-fork-cow");
    ERROR(munmap(p+page,page),EINVAL);ERROR(mprotect(p+page,page,PROT_READ),EINVAL);
    CHECK(!mprotect(p,huge,PROT_READ));c=fork();CHECK(c>=0);if(!c){*(volatile unsigned char *)p=1;_exit(4);}joined(c,SIGSEGV);
    CHECK(!mprotect(p,huge,PROT_READ|PROT_WRITE));p[0]=23;CHECK(!munmap(p,huge));CHECK(counter("HugePages_Free:")==4);pass("protect-unmap-boundaries");
    p=map(huge,MAP_SHARED,huge);p[huge/2+9]=31;c=fork();CHECK(c>=0);if(!c){CHECK(p[huge/2+9]==31);p[huge/2+9]=63;_exit(0);}joined(c,0);CHECK(p[huge/2+9]==63);
    CHECK(!munmap(p,huge));CHECK(counter("HugePages_Free:")==4);pass("shared-fork-offset");
    free(vec);pool(0);CHECK(!counter("HugePages_Total:")&&!counter("HugePages_Free:"));
    p=map(huge,MAP_PRIVATE|MAP_NORESERVE,0);c=fork();CHECK(c>=0);if(!c){*(volatile unsigned char *)p=1;_exit(4);}joined(c,SIGBUS);CHECK(!munmap(p,huge));pass("noreserve-sigbus");
    printf("HUGETLB END %u\n",cases);return 0;
}
