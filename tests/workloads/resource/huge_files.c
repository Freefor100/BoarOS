#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
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

static void check_mount_quota(void)
{
    CHECK(!mkdir(point,0755));
    ERROR(mount("hugetlbfs",point,"hugetlbfs",0,"size=25%,min_size=50%"),EINVAL);
    CHECK(counter("HugePages_Rsvd:")==0);
    ERROR(mount("hugetlbfs",point,"hugetlbfs",0,"min_size=125%"),ENOMEM);
    CHECK(counter("HugePages_Rsvd:")==0);
    CHECK(!mount("hugetlbfs",point,"hugetlbfs",0,"size=50%,min_size=25%"));
    struct statfs fs;
    CHECK(!statfs(point,&fs) && fs.f_blocks==2 && fs.f_bfree==2);
    CHECK(counter("HugePages_Rsvd:")==1);
    int fd=open(filename,O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0 && !ftruncate(fd,3*huge));
    unsigned char *a=map(fd,1,PROT_READ|PROT_WRITE,0);
    CHECK(counter("HugePages_Rsvd:")==1);
    unsigned char *alias=map(fd,1,PROT_READ,0);
    unsigned char *b=map(fd,0,PROT_READ|PROT_WRITE,huge);
    CHECK(!fstatfs(fd,&fs) && fs.f_bfree==0);
    CHECK(mmap(NULL,huge,PROT_READ|PROT_WRITE,MAP_SHARED,fd,2*huge)==MAP_FAILED && errno==ENOMEM);
    a[0]=42;
    CHECK(alias[0]==42 && counter("HugePages_Rsvd:")==1);
    b[0]=43;
    CHECK(counter("HugePages_Rsvd:")==0 && b[0]==43);
    CHECK(!munmap(b,huge) && !fstatfs(fd,&fs) && fs.f_bfree==1);
    CHECK(!ftruncate(fd,0) && !fstatfs(fd,&fs) && fs.f_bfree==2);
    CHECK(counter("HugePages_Rsvd:")==1);
    CHECK(!ftruncate(fd,huge));
    a[0]=18;
    CHECK(!unlink(filename) && !close(fd));
    CHECK(!umount2(point,MNT_DETACH) && alias[0]==18);
    CHECK(!munmap(a,huge) && !munmap(alias,huge) && !rmdir(point));
    CHECK(counter("HugePages_Rsvd:")==0 && counter("HugePages_Free:")==4);
}

static void check_huge_remap(void)
{
    unsigned char *window=mmap(NULL,8*huge,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(window!=MAP_FAILED);
    unsigned char *to=(unsigned char *)(((size_t)window+huge-1)&-huge);
    unsigned char *from=to+3*huge;
    CHECK(mmap(from,2*huge,PROT_READ|PROT_WRITE,
        MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,-1,0)==from);
    from[7]=11;
    from[huge+7]=12;
    CHECK(mremap(from,2*huge,3*huge,MREMAP_MAYMOVE)==MAP_FAILED && errno==EINVAL);
    CHECK(mremap(from,huge,huge,MREMAP_FIXED|MREMAP_MAYMOVE,to+4096)==MAP_FAILED && errno==EINVAL);
    CHECK(mremap(from,huge,huge,MREMAP_MAYMOVE|MREMAP_DONTUNMAP)==MAP_FAILED && errno==EINVAL);
    CHECK(!mprotect(from+huge,huge,PROT_NONE));
    CHECK(mremap(from+huge,huge-1,1,MREMAP_FIXED|MREMAP_MAYMOVE,to)==to);
    CHECK(!mprotect(to,huge,PROT_READ|PROT_WRITE));
    CHECK(to[7]==12 && from[7]==11);
    unsigned char present;
    ERROR(mincore(from+huge,4096,&present),ENOMEM);
    CHECK(!munmap(from,huge) && to[7]==12 && !munmap(window,8*huge));
    CHECK(counter("HugePages_Free:")==4 && counter("HugePages_Rsvd:")==0);

    int fd=syscall(SYS_memfd_create,"move",MFD_HUGETLB);
    CHECK(fd>=0 && !ftruncate(fd,2*huge));
    from=mmap(NULL,2*huge,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(from!=MAP_FAILED);
    from[3]=31;
    CHECK(mremap(from,2*huge,1,0)==from && from[3]==31);
    to=map(fd,1,PROT_READ|PROT_WRITE,huge);
    CHECK(mremap(from,huge,huge,MREMAP_FIXED|MREMAP_MAYMOVE,to)==to && to[3]==31);
    CHECK(!ftruncate(fd,0) && !ftruncate(fd,huge) && to[3]==0);
    CHECK(!close(fd) && !munmap(to,huge));
    CHECK(counter("HugePages_Free:")==4 && counter("HugePages_Rsvd:")==0);
}

static uint64_t page_flags(const unsigned char *address,size_t page)
{
    int fd=open("/proc/self/pagemap",O_RDONLY);
    uint64_t entry;
    CHECK(fd>=0 && pread(fd,&entry,8,((uintptr_t)address/page)*8)==8 && !close(fd));
    CHECK(entry>>63);
    uint64_t frame=entry&((UINT64_C(1)<<55)-1);
    CHECK(frame);
    fd=open("/proc/kpageflags",O_RDONLY);
    CHECK(fd>=0 && pread(fd,&entry,8,frame*8)==8 && !close(fd));
    return entry;
}

struct thp_reader {
    const volatile unsigned char *address;
    unsigned stage;
};

static void *check_thp_remote_reader(void *context)
{
    struct thp_reader *reader=context;
    cpu_set_t cpu;
    CPU_ZERO(&cpu);
    CPU_SET(1,&cpu);
    CHECK(!sched_setaffinity(0,sizeof(cpu),&cpu));
    CHECK(*reader->address==88);
    __atomic_store_n(&reader->stage,1,__ATOMIC_RELEASE);
    while (__atomic_load_n(&reader->stage,__ATOMIC_ACQUIRE)==1) (void)*reader->address;
    CHECK(*reader->address==0);
    return NULL;
}

static void check_transparent_hugepages(void)
{
    const uint64_t thp=UINT64_C(1)<<22;
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *window=mmap(NULL,2*huge,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(window!=MAP_FAILED);
    unsigned char *address=(unsigned char *)(((size_t)window+huge-1)&-huge);
    CHECK(mmap(address,huge,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==address);
    CHECK(!madvise(address,huge,MADV_HUGEPAGE));
    address[page]=88;
    CHECK(page_flags(address,page)&thp);
    CHECK(page_flags(address+page,page)&thp);
    CHECK(counter("AnonHugePages:")>=huge/1024);
    CHECK(!counter("HugePages_Rsvd:") && counter("HugePages_Free:")==4);
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(address[page]==88);
        address[page]=77;
        CHECK(address[page]==77);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(address[page]==88);
    CHECK(!mprotect(address+page,page,PROT_READ));
    CHECK(address[page]==88);
    CHECK(!mprotect(address+page,page,PROT_READ|PROT_WRITE));
    CHECK(!madvise(address,huge,25)); /* MADV_COLLAPSE */
    CHECK(address[page]==88 && (page_flags(address+page,page)&thp));
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CHECK(!sched_getaffinity(0,sizeof(cpus),&cpus));
    if (CPU_ISSET(0,&cpus) && CPU_ISSET(1,&cpus)) {
        struct thp_reader reader={.address=address+page};
        pthread_t thread;
        cpu_set_t first;
        CPU_ZERO(&first);
        CPU_SET(0,&first);
        CHECK(!sched_setaffinity(0,sizeof(first),&first));
        CHECK(!pthread_create(&thread,NULL,check_thp_remote_reader,&reader));
        while (!__atomic_load_n(&reader.stage,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(!madvise(address+page,page,MADV_DONTNEED));
        __atomic_store_n(&reader.stage,2,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && !sched_setaffinity(0,sizeof(cpus),&cpus));
    } else CHECK(!madvise(address+page,page,MADV_DONTNEED));
    CHECK(address[page]==0);
    CHECK(!prctl(PR_SET_THP_DISABLE,1,0,0,0));
    CHECK(prctl(PR_GET_THP_DISABLE,0,0,0,0)==1);
    ERROR(madvise(address,huge,25),EINVAL);
    CHECK(!prctl(PR_SET_THP_DISABLE,0,0,0,0));
    CHECK(!madvise(address,huge,MADV_NOHUGEPAGE));
    ERROR(madvise(address,huge,25),EINVAL);
    CHECK(!munmap(window,2*huge));
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
    check_mount_quota();
    check_huge_remap();
    check_transparent_hugepages();
    pool(0);
    puts("HUGE FILES PASS");
    return 0;
}
