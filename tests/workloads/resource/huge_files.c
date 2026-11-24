#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "HUGE FILES FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); \
    _exit(1); \
} } while (0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)

static size_t huge;
static const char point[]="/hugetlb-test";
static const char filename[]="/hugetlb-test/data";

static unsigned long counter(const char *name)
{
    FILE *fp=fopen("/proc/meminfo","r");
    char line[256];
    CHECK(fp);
    while (fgets(line,sizeof(line),fp)) {
        unsigned long value;
        if (strncmp(line,name,strlen(name))) continue;
        CHECK(sscanf(line+strlen(name),"%lu",&value)==1 && !fclose(fp));
        return value;
    }
    CHECK(0);
    return 0;
}

static void pool(unsigned count)
{
    int fd=open("/proc/sys/vm/nr_hugepages",O_WRONLY);
    char text[32];
    int length=snprintf(text,sizeof(text),"%u\n",count);
    CHECK(fd>=0 && write(fd,text,length)==length && !close(fd));
    CHECK(counter("HugePages_Total:")==count);
}

static unsigned char *map(int fd,int shared,int prot,off_t offset)
{
    void *p=mmap(NULL,huge,prot,shared?MAP_SHARED:MAP_PRIVATE,fd,offset);
    CHECK(p!=MAP_FAILED && (size_t)p%huge==0);
    return p;
}

static void shrink_with_live_alias(int fd,const unsigned char *mapping)
{
    int ready[2],go[2];
    CHECK(!pipe(ready) && !pipe(go));
    pid_t child=fork();
    int status;
    unsigned char byte;
    CHECK(child>=0);
    if (!child) {
        CHECK(!close(ready[0]) && !close(go[1]));
        cpu_set_t available,second;
        CPU_ZERO(&available);
        CHECK(!sched_getaffinity(0,sizeof(available),&available));
        if (CPU_ISSET(1,&available)) {
            CPU_ZERO(&second);
            CPU_SET(1,&second);
            CHECK(!sched_setaffinity(0,sizeof(second),&second));
        }
        CHECK(*(const volatile unsigned char *)(mapping+110)==14);
        CHECK(write(ready[1],"r",1)==1 && read(go[0],&byte,1)==1);
        (void)*(const volatile unsigned char *)mapping;
        _exit(3);
    }
    CHECK(!close(ready[1]) && !close(go[0]) && read(ready[0],&byte,1)==1);
    CHECK(!ftruncate(fd,huge) && write(go[1],"g",1)==1);
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGBUS);
    CHECK(!close(ready[0]) && !close(go[1]));
}

static void check_mount(void)
{
    CHECK(!mkdir(point,0755));
    CHECK(!mount("hugetlbfs",point,"hugetlbfs",0,NULL));
    int fd=open(filename,O_CREAT|O_EXCL|O_RDWR,0600);
    struct statfs fs;
    struct stat st;
    unsigned char byte;
    CHECK(fd>=0 && !fstatfs(fd,&fs) && fs.f_type==0x958458f6UL && (size_t)fs.f_bsize==huge);
    ERROR(ftruncate(fd,1),EINVAL);
    CHECK(!ftruncate(fd,2*huge) && counter("HugePages_Rsvd:")==0);
    CHECK(pread(fd,&byte,1,huge+111)==1 && !byte && counter("HugePages_Free:")==4);
    ERROR(pwrite(fd,"x",1,0),EINVAL);
    unsigned char *a=map(fd,1,PROT_READ|PROT_WRITE,huge);
    CHECK(counter("HugePages_Rsvd:")==1);
    CHECK(!fallocate(fd,FALLOC_FL_KEEP_SIZE|FALLOC_FL_PUNCH_HOLE,huge,huge));
    CHECK(counter("HugePages_Rsvd:")==1);
    int reopened=open(filename,O_RDONLY);
    CHECK(reopened>=0);
    unsigned char *b=map(reopened,1,PROT_READ,huge);
    CHECK(counter("HugePages_Rsvd:")==1);
    a[110]=13;
    CHECK(b[110]==13 && pread(fd,&byte,1,huge+110)==1 && byte==13);
    unsigned char *private=map(fd,0,PROT_READ|PROT_WRITE,huge);
    CHECK(private[110]==13);
    a[110]=14;
    CHECK(private[110]==14);
    a[109]=17;
    CHECK(!fallocate(fd,FALLOC_FL_KEEP_SIZE|FALLOC_FL_PUNCH_HOLE,huge+109,1));
    CHECK(a[109]==0 && b[109]==0 && private[109]==0 && a[110]==14);
    long page=sysconf(_SC_PAGESIZE);
    CHECK(page>111 && (size_t)page<huge);
    a[page-1]=29;
    a[page]=73;
    ERROR(madvise(private+page,(size_t)page,MADV_REMOVE),EACCES);
    CHECK(!madvise(a+page,(size_t)page,MADV_REMOVE));
    CHECK(a[page-1]==29 && b[page]==0 && private[page]==0);
    private[110]=15;
    CHECK(a[110]==14 && b[110]==14 && private[110]==15);
    CHECK(!unlink(filename) && !fstat(fd,&st) && st.st_nlink==0 && !close(reopened));
    shrink_with_live_alias(fd,a);
    CHECK(!fstat(fd,&st) && (size_t)st.st_size==huge);
    CHECK(!ftruncate(fd,2*huge));
    CHECK(a[110]==0 && b[110]==0 && private[110]==0);
    a[15]=42;
    CHECK(!close(fd));
    int remaining=open(filename,O_RDWR|O_CREAT|O_EXCL,0600);
    struct timespec saved[2]={{.tv_sec=17},{.tv_sec=18}};
    CHECK(remaining>=0 && !futimens(remaining,saved) && !fallocate(remaining,0,0,huge));
    CHECK(!fstat(remaining,&st) && st.st_mtim.tv_sec==18 && st.st_mtim.tv_nsec==0);
    CHECK(pread(remaining,&byte,1,huge-1)==1 && !byte);
    struct timespec ctime=st.st_ctim;
    CHECK(!fallocate(remaining,FALLOC_FL_KEEP_SIZE|FALLOC_FL_PUNCH_HOLE,0,huge));
    CHECK(!fstat(remaining,&st) && st.st_mtim.tv_sec==18 && st.st_mtim.tv_nsec==0);
    CHECK(st.st_ctim.tv_sec==ctime.tv_sec && st.st_ctim.tv_nsec==ctime.tv_nsec);
    CHECK(!close(remaining));
    CHECK(!umount2(point,MNT_DETACH) && b[15]==42);
    CHECK(!munmap(private,huge) && !munmap(a,huge) && !munmap(b,huge) && !rmdir(point));
    CHECK(counter("HugePages_Free:")==4 && counter("HugePages_Rsvd:")==0);
}

static void check_mount_context(void)
{
    enum { cloexec=1, set_string=1, create=6 };
    int context=syscall(SYS_fsopen,"hugetlbfs",cloexec);
    CHECK(context>=0);
    const char *pagesize=huge==2U*1024*1024?"2M":"32M";
    CHECK(!syscall(SYS_fsconfig,context,set_string,"pagesize",pagesize,0));
    CHECK(!syscall(SYS_fsconfig,context,create,NULL,NULL,0));
    int tree=syscall(SYS_fsmount,context,cloexec,0);
    struct statfs fs;
    CHECK(tree>=0 && !fstatfs(tree,&fs) && fs.f_type==0x958458f6UL && (size_t)fs.f_bsize==huge);
    CHECK(!close(tree) && !close(context));
}

static void check_memfd(void)
{
    struct statfs fs;
    struct stat st;
    int fd=syscall(SYS_memfd_create,"huge-file",MFD_HUGETLB|MFD_ALLOW_SEALING|MFD_CLOEXEC);
    CHECK(fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
    CHECK(!fstatfs(fd,&fs) && fs.f_type==0x958458f6UL);
    CHECK(!fstat(fd,&st) && !st.st_size && !st.st_nlink && !fcntl(fd,F_GET_SEALS));
    CHECK(mmap(NULL,5*huge,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED && errno==ENOMEM);
    CHECK(!fstat(fd,&st) && !st.st_size && counter("HugePages_Rsvd:")==0);
    unsigned char *a=map(fd,1,PROT_READ|PROT_WRITE,0);
    CHECK(!fstat(fd,&st) && (size_t)st.st_size==huge);
    ERROR(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE),EBUSY);
    a[7]=61;
    CHECK(!munmap(a,huge));
    CHECK(!fcntl(fd,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_WRITE));
    ERROR(ftruncate(fd,0),EPERM);
    CHECK(mmap(NULL,huge,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED && errno==EPERM);
    a=map(fd,1,PROT_READ,0);
    CHECK(!close(fd) && a[7]==61 && !munmap(a,huge));
    CHECK(counter("HugePages_Free:")==4 && counter("HugePages_Rsvd:")==0);
}

int main(void)
{
    huge=1024*counter("Hugepagesize:");
    CHECK(huge==2U*1024*1024 || huge==32U*1024*1024);
    pool(4);
    check_memfd();
    check_mount();
    check_mount_context();
    pool(0);
    puts("HUGE FILES PASS");
    return 0;
}
