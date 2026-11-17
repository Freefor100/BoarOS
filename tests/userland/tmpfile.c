#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"TMPFILE FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);} }while(0)
#define FAIL(e,x) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void child_ok(pid_t p){int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void run(const char *directory,const char *label)
{
    int d=open(directory,O_RDONLY|O_DIRECTORY);CHECK(d>=0);
#ifdef BOAROS_DIRECT_CAPABILITIES
    if (!strcmp(label,"ext4")) {
        int direct=openat(d,".",O_TMPFILE|O_RDWR|O_DIRECT,0600);CHECK(direct>=0);
        CHECK(!close(direct));
    } else {
    /* tmpfs has no device-backed direct I/O capability. */
    FAIL(EINVAL,openat(d,".",O_TMPFILE|O_RDWR|O_DIRECT,0600));
    struct {unsigned long long flags,mode,resolve;} direct_how={O_TMPFILE|O_RDWR|O_DIRECT,0600,0};
    FAIL(EINVAL,syscall(SYS_openat2,d,".",&direct_how,sizeof(direct_how)));
    int probe=openat(d,".",O_TMPFILE|O_RDWR,0600);CHECK(probe>=0);
    int original=fcntl(probe,F_GETFL);CHECK(original>=0);
    FAIL(EINVAL,fcntl(probe,F_SETFL,original|O_DIRECT));
    CHECK(fcntl(probe,F_GETFL)==original);
    CHECK(write(probe,"kept",4)==4);
    CHECK(!linkat(probe,"",d,"direct-rejected",AT_EMPTY_PATH));
    FAIL(EINVAL,openat(d,"direct-rejected",O_RDWR|O_DIRECT|O_TRUNC));
    struct stat retained;CHECK(!fstat(probe,&retained)&&retained.st_size==4);
    CHECK(!close(probe)&&!unlinkat(d,"direct-rejected",0));
    }
#endif

    FAIL(EINVAL,openat(d,".",O_TMPFILE|O_RDONLY,0600));
    FAIL(EINVAL,openat(d,".",O_TMPFILE|O_CREAT|O_RDWR,0600));
    FAIL(ENOENT,openat(d,"missing",O_TMPFILE|O_RDWR,0600));
    int special=openat(d,".",O_TMPFILE|3,0600);CHECK(special>=0);char scratch;
    FAIL(EBADF,read(special,&scratch,1));FAIL(EBADF,write(special,"x",1));CHECK(!close(special));
    special=openat(d,".",O_TMPFILE|O_WRONLY,0600);CHECK(special>=0);CHECK(write(special,"x",1)==1);FAIL(EBADF,read(special,&scratch,1));CHECK(!close(special));
    struct {unsigned long long flags,mode,resolve;} how={O_TMPFILE|O_RDWR,0600,0};
    special=syscall(SYS_openat2,d,".",&how,sizeof(how));CHECK(special>=0);CHECK(!close(special));
    how.resolve=0x20;FAIL(EAGAIN,syscall(SYS_openat2,d,".",&how,sizeof(how)));
    mode_t old=umask(0027);
    int f=openat(d,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0666);umask(old);CHECK(f>=0);
    char procname[64],target[512];snprintf(procname,sizeof(procname),"/proc/self/fd/%d",f);
    ssize_t length=readlink(procname,target,sizeof(target)-1);CHECK(length>0);target[length]=0;
    CHECK(strstr(target,directory)==target&&strstr(target,"/#")&&strstr(target," (deleted)"));
    struct stat st;CHECK(!fstat(f,&st)&&S_ISREG(st.st_mode)&&!st.st_nlink&&(st.st_mode&0777)==0640);
    CHECK(fcntl(f,F_GETFD)==FD_CLOEXEC);
    CHECK(write(f,"unpublished",11)==11);
    FAIL(ENOENT,fstatat(d,"saved",&st,0));
    int duplicate=dup(f);CHECK(duplicate>=0);CHECK(!close(f));f=duplicate;
    CHECK(!linkat(f,"",d,"saved",AT_EMPTY_PATH));
    CHECK(!fstat(f,&st)&&st.st_nlink==1&&st.st_size==11);
    FAIL(EEXIST,linkat(f,"",d,"saved",AT_EMPTY_PATH));
    int r=openat(d,"saved",O_RDONLY);CHECK(r>=0);char b[12]={0};CHECK(read(r,b,11)==11&&!memcmp(b,"unpublished",11));CHECK(!close(r));
    CHECK(!unlinkat(d,"saved",0));FAIL(ENOENT,linkat(f,"",d,"resurrected",AT_EMPTY_PATH));CHECK(!close(f));
    f=openat(d,".",O_TMPFILE|O_RDWR|O_EXCL,0600);CHECK(f>=0);FAIL(ENOENT,linkat(f,"",d,"exclusive",AT_EMPTY_PATH));CHECK(!close(f));
    f=openat(d,".",O_TMPFILE|O_RDWR,0600);CHECK(f>=0);
    /* Shared anonymous inode and same-name publication race: exactly one link. */
    char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",f);
    int ready[2],go[2];CHECK(!pipe(ready)&&!pipe(go));pid_t p=fork();CHECK(p>=0);
    if(!p){CHECK(!close(ready[0])&&!close(go[1]));CHECK(write(ready[1],"r",1)==1);CHECK(read(go[0],b,1)==1);int x=linkat(AT_FDCWD,proc,d,"race",AT_SYMLINK_FOLLOW);CHECK(!x||errno==EEXIST);CHECK(!close(f)&&!close(d));_exit(0);}
    CHECK(!close(ready[1])&&!close(go[0]));CHECK(read(ready[0],b,1)==1);CHECK(write(go[1],"g",1)==1);
    int x=linkat(AT_FDCWD,proc,d,"race",AT_SYMLINK_FOLLOW);CHECK(!x||errno==EEXIST);child_ok(p);
    CHECK(!close(ready[0])&&!close(go[1]));CHECK(!fstat(f,&st)&&st.st_nlink==1);CHECK(!unlinkat(d,"race",0));CHECK(!close(f));
    /* Last fd can close while a shared mapping retains the anonymous inode. */
    f=openat(d,".",O_TMPFILE|O_RDWR,0600);CHECK(f>=0);long page=sysconf(_SC_PAGESIZE);CHECK(!ftruncate(f,page));
    char *map=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);CHECK(map!=MAP_FAILED);CHECK(!close(f));map[0]='x';CHECK(!msync(map,page,MS_SYNC)&&map[0]=='x');CHECK(!munmap(map,page));
    CHECK(!mkdirat(d,"locked",0555));p=fork();CHECK(p>=0);
    if(!p){if(setgid(65534)||setuid(65534)){ if(getenv("TMPFILE_SKIP_UID")){puts("TMPFILE SKIP uid transition (host namespace)");fflush(stdout);_exit(0);} CHECK(0); }FAIL(EACCES,openat(d,"locked",O_TMPFILE|O_RDWR,0600));_exit(0);}child_ok(p);CHECK(!unlinkat(d,"locked",AT_REMOVEDIR));
    /* Descriptor reservation failure must not allocate or leak an inode. */
    struct rlimit limit;CHECK(!getrlimit(RLIMIT_NOFILE,&limit));struct rlimit low=limit;low.rlim_cur=0;
    CHECK(!setrlimit(RLIMIT_NOFILE,&low));FAIL(EMFILE,openat(d,".",O_TMPFILE|O_RDWR,0600));CHECK(!setrlimit(RLIMIT_NOFILE,&limit));
    CHECK(!close(d));printf("TMPFILE %s PASS\n",label);
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    if(argc==3){run(argv[1],"first");run(argv[2],"second");}
    else {CHECK(!mkdir("/proc",0555));CHECK(!mount("proc","/proc","proc",0,NULL));CHECK(!mkdir("/tmpfile-ext4",0777));CHECK(!chmod("/tmpfile-ext4",0777));CHECK(!mkdir("/tmpfile-tmpfs",0777));CHECK(!mount("tmpfs","/tmpfile-tmpfs","tmpfs",0,"mode=0777"));FAIL(EOPNOTSUPP,open("/proc",O_TMPFILE|O_RDWR,0600));
    int anonymous=open("/tmpfile-ext4",O_TMPFILE|O_RDWR,0600);CHECK(anonymous>=0);
    FAIL(EXDEV,linkat(anonymous,"",AT_FDCWD,"/tmpfile-tmpfs/cross",AT_EMPTY_PATH));CHECK(!close(anonymous));
    CHECK(!mount(NULL,"/tmpfile-tmpfs",NULL,MS_REMOUNT|MS_RDONLY,NULL));
    FAIL(EROFS,open("/tmpfile-tmpfs",O_TMPFILE|O_RDWR,0600));
    CHECK(!mount(NULL,"/tmpfile-tmpfs",NULL,MS_REMOUNT,NULL));
    run("/tmpfile-ext4","ext4");run("/tmpfile-tmpfs","tmpfs");CHECK(!umount("/tmpfile-tmpfs")&&!rmdir("/tmpfile-tmpfs")&&!rmdir("/tmpfile-ext4"));}
    puts("TMPFILE ALL PASS");return 0;
}
