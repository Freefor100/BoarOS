#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"COPY_RANGE FAIL line=%d expression=%s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
#define BAD(e, x) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)
static unsigned char data[12289], actual[12289];
static volatile sig_atomic_t signals;
static void notified(int number) { (void)number; signals++; }
static ssize_t copy(int in, off_t *pi, int out, off_t *po, size_t n, unsigned flags)
{ return syscall(SYS_copy_file_range,in,pi,out,po,n,flags); }
static int create(const char *name)
{ int fd=open(name,O_CREAT|O_TRUNC|O_RDWR,0600); CHECK(fd>=0); return fd; }
static void contents(int fd, off_t offset, size_t n, size_t source)
{ CHECK(pread(fd,actual,n,offset)==(ssize_t)n && !memcmp(actual,data+source,n)); }
static void wait_ok(pid_t pid)
{ int status; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status)); }

static void ordinary(const char *directory)
{
    int old=open(".",O_RDONLY|O_DIRECTORY); CHECK(old>=0 && !chdir(directory));
    int in=create("source"),out=create("output");
    CHECK(write(in,data,sizeof(data))==(ssize_t)sizeof(data));
    for (unsigned mode=0;mode<4;mode++) {
        off_t pi=17,po=4099;
        CHECK(lseek(in,17,SEEK_SET)==17 && lseek(out,4099,SEEK_SET)==4099);
        CHECK(copy(in,mode&1?&pi:NULL,out,mode&2?&po:NULL,5001,0)==5001);
        CHECK(pi==(mode&1?5018:17) && po==(mode&2?9100:4099));
        CHECK(lseek(in,0,SEEK_CUR)==(mode&1?17:5018));
        CHECK(lseek(out,0,SEEK_CUR)==(mode&2?4099:9100));
        contents(out,4099,5001,17);
    }
    off_t pi=sizeof(data)-5,po=0;
    CHECK(copy(in,&pi,out,&po,100,0)==5 && pi==(off_t)sizeof(data) && po==5);
    CHECK(copy(in,&pi,out,&po,100,0)==0 && po==5);
    pi=0;po=0;CHECK(copy(in,&pi,out,&po,SIZE_MAX,0)==(ssize_t)sizeof(data));
    CHECK(!ftruncate(out,0) && lseek(out,0,SEEK_SET)==0 && lseek(in,0,SEEK_SET)==0);
    CHECK(sendfile(out,in,NULL,5001)==5001);contents(out,0,5001,0);
    int synchronous=open("output",O_RDWR|O_SYNC);CHECK(synchronous>=0);pi=0;po=3;
    CHECK(copy(in,&pi,synchronous,&po,4101,0)==4101);contents(synchronous,3,4101,0);CHECK(!close(synchronous));
    CHECK(!ftruncate(in,0)&&!ftruncate(in,8193));pi=1;po=1;
    CHECK(copy(in,&pi,out,&po,8192,0)==8192);
    CHECK(pread(out,actual,8192,1)==8192);
    for (size_t i=0;i<8192;i++)CHECK(actual[i]==0);
    CHECK(!close(in) && !close(out));
    CHECK(!unlink("source") && !unlink("output") && !fchdir(old) && !close(old));
    printf("COPY_RANGE CASE regular %s PASS\n",directory);
}

static void aliases_and_errors(void)
{
    int in=create("source"),out=create("output");
    CHECK(write(in,data,sizeof(data))==(ssize_t)sizeof(data));
    off_t pi=3,po=7;
    BAD(EINVAL,copy(in,&pi,in,&po,100,0));
    CHECK(!link("source","alias"));int alias=open("alias",O_RDWR);CHECK(alias>=0);
    BAD(EINVAL,copy(in,&pi,alias,&po,100,0));
    pi=0;po=sizeof(data);CHECK(copy(in,&pi,alias,&po,37,0)==37);contents(in,sizeof(data),37,0);
    CHECK(lseek(in,sizeof(data)+37,SEEK_SET)==(off_t)sizeof(data)+37);
    pi=19;CHECK(copy(in,&pi,in,NULL,41,0)==41 && pi==60);
    CHECK(lseek(in,0,SEEK_CUR)==(off_t)sizeof(data)+78);
    CHECK(lseek(in,0,SEEK_SET)==0);po=sizeof(data)+78;
    CHECK(copy(in,NULL,in,&po,43,0)==43 && lseek(in,0,SEEK_CUR)==43);
    BAD(EINVAL,copy(in,NULL,in,NULL,1,0));
    BAD(EBADF,copy(-1,(void *)1,-1,(void *)1,1,1));
    BAD(EBADF,copy(in,(void *)1,-1,(void *)1,1,1));
    BAD(EFAULT,copy(in,(void *)1,out,NULL,1,1));
    BAD(EINVAL,copy(in,NULL,out,NULL,0,1));
    int dir=open(".",O_RDONLY|O_DIRECTORY),p[2];CHECK(dir>=0 && !pipe(p));
    BAD(EISDIR,copy(dir,NULL,out,NULL,1,0));
    BAD(EINVAL,copy(p[0],NULL,out,NULL,1,0));
    int append=open("output",O_WRONLY|O_APPEND);CHECK(append>=0);
    BAD(EBADF,copy(in,NULL,append,NULL,1,0));
    pi=-1;po=0;BAD(EOVERFLOW,copy(in,&pi,out,&po,1,0));
    BAD(EINVAL,copy(in,&pi,out,&po,0,0));
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    off_t *ro=mmap(NULL,(size_t)page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(ro!=MAP_FAILED);
    *ro=0;CHECK(!mprotect(ro,(size_t)page,PROT_READ));
    CHECK(lseek(out,0,SEEK_SET)==0);BAD(EFAULT,copy(in,ro,out,NULL,71,0));
    CHECK(*ro==0 && lseek(out,0,SEEK_CUR)==71);contents(out,0,71,0);
    CHECK(copy(in,ro,out,NULL,0,0)==0);
    CHECK(lseek(in,23,SEEK_SET)==23);BAD(EFAULT,copy(in,NULL,out,ro,17,0));
    CHECK(lseek(in,0,SEEK_CUR)==40 && *ro==0);contents(out,0,17,23);
    CHECK(!munmap(ro,(size_t)page));
    CHECK(!close(in)&&!close(out)&&!close(alias)&&!close(append)&&!close(dir)&&!close(p[0])&&!close(p[1]));
    CHECK(!unlink("alias")&&!unlink("source")&&!unlink("output"));
    puts("COPY_RANGE CASE aliases-errors-copyout PASS");
}

static void fsize(void)
{
    pid_t pid=fork();CHECK(pid>=0);
    if (!pid) {
        int in=create("limit-source"),out=create("limit-output");CHECK(write(in,data,sizeof(data))==(ssize_t)sizeof(data));
        struct sigaction action={.sa_handler=notified};CHECK(!sigemptyset(&action.sa_mask)&&!sigaction(SIGXFSZ,&action,NULL));
        struct rlimit limit={.rlim_cur=123,.rlim_max=123};CHECK(!setrlimit(RLIMIT_FSIZE,&limit));
        off_t pi=0,po=0;CHECK(copy(in,&pi,out,&po,1024,0)==123 && pi==123 && po==123 && !signals);
        BAD(EFBIG,copy(in,&pi,out,&po,1,0));CHECK(signals==1 && pi==123 && po==123);
        CHECK(!close(in)&&!close(out));_exit(0);
    }
    wait_ok(pid);CHECK(!unlink("limit-source")&&!unlink("limit-output"));
    puts("COPY_RANGE CASE fsize PASS");
}

static void opposite_pairs(void)
{
    int a=create("pair-a"),b=create("pair-b"),gate[2];CHECK(!pipe(gate));
    CHECK(write(a,data,sizeof(data))==(ssize_t)sizeof(data));
    CHECK(write(b,data,sizeof(data))==(ssize_t)sizeof(data));
    CHECK(lseek(a,0,SEEK_SET)==0 && lseek(b,0,SEEK_SET)==0);
    pid_t children[2];
    for (unsigned i=0;i<2;i++) {
        children[i]=fork();CHECK(children[i]>=0);
        if (!children[i]) {
            CHECK(!close(gate[1]));char token;CHECK(read(gate[0],&token,1)==1);
            CHECK(copy(i?a:b,NULL,i?b:a,NULL,128,0)==128);_exit(0);
        }
    }
    CHECK(!close(gate[0]) && write(gate[1],"GG",2)==2 && !close(gate[1]));
    wait_ok(children[0]);wait_ok(children[1]);
    off_t pa=lseek(a,0,SEEK_CUR),pb=lseek(b,0,SEEK_CUR);
    CHECK(pa>=128 && pa<=256 && pb>=128 && pb<=256);
    CHECK(!close(a)&&!close(b)&&!unlink("pair-a")&&!unlink("pair-b"));
    puts("COPY_RANGE CASE opposite-OFD-pairs PASS");
}

static void killpriv(void)
{
    if (getuid()) return;
    int in=create("priv-source"),out=create("priv-output");
    CHECK(write(in,data,sizeof(data))==(ssize_t)sizeof(data) && !fchmod(out,06777));
    pid_t pid=fork();CHECK(pid>=0);
    if (!pid) {
        CHECK(!setgid(65534)&&!setuid(65534));off_t pi=0,po=0;
        CHECK(copy(in,&pi,out,&po,31,0)==31);_exit(0);
    }
    wait_ok(pid);struct stat st;CHECK(!fstat(out,&st)&&!(st.st_mode&06000));
    CHECK(!close(in)&&!close(out)&&!unlink("priv-source")&&!unlink("priv-output"));
    puts("COPY_RANGE CASE killpriv PASS");
}

static void mounts(void)
{
    CHECK(!mkdir("memory",0700)&&!mkdir("other",0700)&&!mkdir("view",0700));
    CHECK(!mount("none","memory","tmpfs",0,"size=1048576"));
    CHECK(!mount("none","other","tmpfs",0,"size=1048576"));
    CHECK(!mount("memory","view",NULL,MS_BIND,NULL));ordinary("memory");
    int a=create("memory/a"),b=create("view/b"),c=create("other/c"),disk=create("disk");
    CHECK(write(a,data,sizeof(data))==(ssize_t)sizeof(data));off_t pi=0,po=0;
    CHECK(copy(a,&pi,b,&po,73,0)==73);contents(b,0,73,0);
    BAD(EXDEV,copy(a,&pi,c,&po,1,0));BAD(EXDEV,copy(a,&pi,disk,&po,0,0));
    CHECK(!close(a)&&!close(b)&&!close(c)&&!close(disk));
    CHECK(!unlink("memory/a")&&!unlink("view/b")&&!unlink("other/c")&&!unlink("disk"));
    CHECK(!umount("view")&&!umount("other")&&!umount("memory"));
    CHECK(!rmdir("view")&&!rmdir("other")&&!rmdir("memory"));
    puts("COPY_RANGE CASE instances-bind PASS");
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    size_t rounded=(sizeof(data)+(size_t)page-1)/(size_t)page*(size_t)page;
    char options[64];snprintf(options,sizeof(options),"size=%zu",2*rounded);
    CHECK(!mkdir("small",0700)&&!mount("none","small","tmpfs",0,options));
    a=create("small/a");b=create("small/b");CHECK(write(a,data,sizeof(data))==(ssize_t)sizeof(data));
    pi=po=0;CHECK(copy(a,&pi,b,&po,sizeof(data),0)==(ssize_t)sizeof(data));
    pi=0;CHECK(copy(a,&pi,b,&po,sizeof(data),0)==(ssize_t)(rounded-sizeof(data)));
    CHECK(po==(off_t)rounded && pi==(off_t)(rounded-sizeof(data)));
    BAD(ENOSPC,copy(a,&pi,b,&po,1,0));contents(b,sizeof(data),rounded-sizeof(data),0);
    CHECK(!close(a)&&!close(b)&&!unlink("small/a")&&!unlink("small/b")&&!umount("small")&&!rmdir("small"));
    puts("COPY_RANGE CASE enospc-prefix PASS");
}

int main(int argc,char **argv)
{
    for(size_t i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i%251);
    char directory[]="/tmp/copy-range-XXXXXX";CHECK(mkdtemp(directory)&&!chdir(directory));
    ordinary(".");aliases_and_errors();opposite_pairs();fsize();killpriv();
    if(argc<2 || strcmp(argv[1],"--basic"))mounts();
    const char *vfat=getenv("COPY_RANGE_VFAT");if(vfat)ordinary(vfat);
    CHECK(!chdir("/")&&!rmdir(directory));puts("COPY_RANGE PASS");return 0;
}
