/* Run only in a disposable guest: deliberately changes its native huge pool. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
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
static void pipe_owners(void)
{
    int a[2],b[2],d[2];unsigned char out[32],expected[32];
    unsigned char *p=map(huge,MAP_PRIVATE,0),*window=p+huge/2+7;
    for(unsigned i=0;i<32;i++)window[i]=expected[i]=(unsigned char)(i+17);
    CHECK(!pipe(a)&&!pipe(b)&&!pipe(d));
    struct iovec v={window,32};CHECK(vmsplice(a[1],&v,1,0)==32);
    pid_t c=fork();CHECK(c>=0);if(!c){window[0]=99;_exit(0);}joined(c,0);
    CHECK(tee(a[0],b[1],32,0)==32);CHECK(splice(a[0],0,d[1],0,13,0)==13);
    CHECK(!munmap(p,huge));CHECK(counter("HugePages_Free:")==3);
    CHECK(read(d[0],out,13)==13&&!memcmp(out,expected,13));
    CHECK(read(a[0],out,19)==19&&!memcmp(out,expected+13,19));
    CHECK(splice(b[0],0,d[1],0,32,0)==32);
    CHECK(read(d[0],out,32)==32&&!memcmp(out,expected,32));
    for(unsigned i=0;i<2;i++)CHECK(!close(a[i])&&!close(b[i])&&!close(d[i]));
    CHECK(counter("HugePages_Free:")==4);
    p=map(huge,MAP_PRIVATE,0);p[huge-page+19]=71;v=(struct iovec){p+huge-page+19,1};
    CHECK(!pipe(a)&&vmsplice(a[1],&v,1,0)==1);CHECK(!munmap(p,huge));
    CHECK(counter("HugePages_Free:")==3);CHECK(!close(a[0])&&!close(a[1]));
    CHECK(counter("HugePages_Free:")==4);pass("pipe-compound-owners");
}
static void sysv_huge(void)
{
    int id=shmget(IPC_PRIVATE,huge+1,SHM_HUGETLB|0600);CHECK(id>=0);
    CHECK(counter("HugePages_Rsvd:")==2);
    struct shmid_ds ds;CHECK(!shmctl(id,IPC_STAT,&ds)&&ds.shm_segsz==huge+1&&!ds.shm_nattch);
    unsigned char *a=shmat(id,0,0),*b=shmat(id,0,0);CHECK(a!=(void *)-1&&b!=(void *)-1&&a!=b);
    CHECK(!((uintptr_t)a&(huge-1))&&!((uintptr_t)b&(huge-1)));
    CHECK(counter("HugePages_Rsvd:")==2);
    a[page+13]=41;a[2*huge-1]=83;CHECK(b[page+13]==41&&b[2*huge-1]==83);
    CHECK(counter("HugePages_Free:")==2&&!counter("HugePages_Rsvd:"));
    pid_t c=fork();CHECK(c>=0);if(!c){CHECK(!shmctl(id,IPC_STAT,&ds)&&ds.shm_nattch==4);b[page+13]=62;CHECK(!shmdt(a)&&!shmdt(b));_exit(0);}joined(c,0);
    CHECK(a[page+13]==62&&!shmctl(id,IPC_STAT,&ds)&&ds.shm_nattch==2);
    CHECK(!shmctl(id,IPC_RMID,0));
    unsigned char *ro=shmat(id,0,SHM_RDONLY);CHECK(ro!=(void *)-1&&ro[page+13]==62);
    ERROR(mprotect(ro,2*huge,PROT_READ|PROT_WRITE),EACCES);
    ERROR(munmap(a+page,page),EINVAL);
    CHECK(!mprotect(a+huge,huge,PROT_READ));
    CHECK(!shmdt(a)&&!shmdt(b));CHECK(counter("HugePages_Free:")==2);
    CHECK(!shmdt(ro));CHECK(counter("HugePages_Free:")==4&&!counter("HugePages_Rsvd:"));
    ERROR(shmctl(id,IPC_STAT,&ds),EINVAL);
    id=shmget(IPC_PRIVATE,1,SHM_HUGETLB|0600);CHECK(id>=0);CHECK(counter("HugePages_Rsvd:")==1);
    CHECK(!shmctl(id,IPC_RMID,0)&&!counter("HugePages_Rsvd:"));
    pass("sysv-huge-owners");
}
static void sysv_noreserve(void)
{
    int id=shmget(IPC_PRIVATE,1,SHM_HUGETLB|SHM_NORESERVE|0600);CHECK(id>=0);
    unsigned char *p=shmat(id,0,0);CHECK(p!=(void *)-1);
    CHECK(!shmctl(id,IPC_RMID,0));
    pid_t c=fork();CHECK(c>=0);if(!c){*(volatile unsigned char *)p=1;_exit(4);}joined(c,SIGBUS);
    CHECK(!shmdt(p));pass("sysv-noreserve-sigbus");
}
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
    pipe_owners();
    sysv_huge();
    free(vec);pool(0);CHECK(!counter("HugePages_Total:")&&!counter("HugePages_Free:"));
    p=map(huge,MAP_PRIVATE|MAP_NORESERVE,0);c=fork();CHECK(c>=0);if(!c){*(volatile unsigned char *)p=1;_exit(4);}joined(c,SIGBUS);CHECK(!munmap(p,huge));pass("noreserve-sigbus");
    sysv_noreserve();
    printf("HUGETLB END %u\n",cases);return 0;
}
