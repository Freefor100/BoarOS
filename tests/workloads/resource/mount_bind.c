#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <termios.h>
#ifndef TIOCGPTPEER
#define TIOCGPTPEER 0x5441
#endif
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"BIND FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1); } } while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));} while(0)
static unsigned cases;
static size_t page;
static char base[1024];
static void pass(const char *name){printf("BIND CASE %s PASS\n",name);cases++;}
static void directory(const char *name){CHECK(!mkdir(name,0755));}
static void same(const char *a,const char *b)
{struct stat x,y;CHECK(!stat(a,&x)&&!stat(b,&y)&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino);}
static void content(int fd,const char *text)
{char bytes[16]={0};CHECK(strlen(text)<sizeof(bytes)&&pread(fd,bytes,strlen(text),0)==(ssize_t)strlen(text)&&!strcmp(bytes,text));}
static void create(const char *name,const char *text)
{int fd=open(name,O_CREAT|O_RDWR|O_EXCL,0755);CHECK(fd>=0&&write(fd,text,strlen(text))==(ssize_t)strlen(text)&&!ftruncate(fd,(off_t)page)&&!close(fd));}
static void flags(const char *name,unsigned expected)
{struct statfs s;CHECK(!statfs(name,&s)&&((unsigned long)s.f_flags&15U)==expected);}
static void identity(void)
{
    CHECK(!mount("bs/sub","bv",0,MS_BIND,0));same("bs/sub/data","bv/data");
    int fd=open("bv/data",O_RDWR);CHECK(fd>=0&&pwrite(fd,"VIEW",4,0)==4&&!close(fd));
    fd=open("bs/sub/data",O_RDONLY);CHECK(fd>=0);content(fd,"VIEWle");CHECK(!close(fd));
    CHECK(!mount("bs/sub/data","bf",0,MS_BIND,0));same("bs/sub/data","bf");
    fd=open("bf",O_RDONLY);CHECK(fd>=0);ERROR(umount("bf"),EBUSY);
    CHECK(!rename("bs/sub/data","bs/sub/renamed")&&!unlink("bs/sub/renamed"));
    content(fd,"VIEWle");CHECK(!close(fd)&&!umount("bf"));
    create("bs/sub/data","stable");pass("directory-file-bind-and-unlinked-owner");
}
static void renamed_directory(void)
{
    directory("bs/sub/dir");int dir=open("bv/dir",O_RDONLY|O_DIRECTORY),cwd=open(".",O_RDONLY|O_DIRECTORY);CHECK(dir>=0&&cwd>=0);
    CHECK(!rename("bs/sub/dir","bs/sub/moved")&&!fchdir(dir));
    char actual[2048],expected[2048];CHECK(snprintf(expected,sizeof(expected),"%s%sbv/moved",base,strcmp(base,"/")?"/":"")>0);
    CHECK(getcwd(actual,sizeof(actual))&&!strcmp(actual,expected));
    int parent=open("..",O_RDONLY|O_DIRECTORY);struct stat p,s;CHECK(parent>=0&&!fstat(parent,&p)&&!fstatat(dir,"..",&s,0)&&p.st_dev==s.st_dev&&p.st_ino==s.st_ino);
    CHECK(!close(parent)&&!fchdir(cwd)&&!close(cwd));
    CHECK(!rename("bv/moved","bv/again"));same("bs/sub/again","bv/again");
    CHECK(!close(dir)&&!rmdir("bv/again"));pass("alias-rename-held-dirfd-cwd-and-parent");
}
static void attributes(void)
{
    int original=open("bs/sub/data",O_RDWR),view=open("bv/data",O_RDWR);CHECK(original>=0&&view>=0);
    ERROR(mount(0,"bv",0,MS_BIND|MS_REMOUNT|MS_RDONLY,0),EBUSY);CHECK(!close(view));
    CHECK(!mount(0,"bv",0,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC,0));
    flags("bv",15);flags("bs",0);ERROR(open("bv/data",O_RDWR),EROFS);
    CHECK(pwrite(original,"stable",6,0)==6);ERROR(access("bv/data",X_OK),EACCES);CHECK(!access("bs/sub/data",X_OK));
    CHECK(!mount(0,"bv",0,MS_BIND|MS_REMOUNT,0));flags("bv",0);CHECK(!close(original));
    pass("independent-readonly-nodev-noexec-nosuid");
}
static void recursive(void)
{
    directory("bs/child");CHECK(!mount("tmpfs","bs/child","tmpfs",0,0));create("bs/child/token","child");
    CHECK(!mount("bs","bp",0,MS_BIND,0)&&!mount("bs","br",0,MS_BIND|MS_REC,0));
    struct stat st;ERROR(stat("bp/child/token",&st),ENOENT);same("bs/child/token","br/child/token");
    ERROR(rmdir("bp/child"),EBUSY);ERROR(rename("bp/child","bp/elsewhere"),EBUSY);
    int child=open("br/child/token",O_RDONLY);CHECK(child>=0&&!umount("bs/child"));content(child,"child");
    CHECK(!umount2("br",MNT_DETACH));ERROR(stat("br/child/token",&st),ENOENT);content(child,"child");
    CHECK(!close(child)&&!umount("bp"));pass("recursive-clone-and-lazy-child-owner");
}
static void errors(void)
{
    ERROR(mount("absent-bind-source","bp",0,MS_BIND,0),ENOENT);
    ERROR(mount("bs/sub/data","bp",0,MS_BIND,0),ENOTDIR);
    ERROR(mount("bs/sub","bf",0,MS_BIND,0),ENOTDIR);
    ERROR(umount2("bs/sub",MNT_DETACH),EINVAL);
    ERROR(link("bs/sub/data","bv/cross-link"),EXDEV);
    ERROR(rename("bs/sub/data","bv/cross-rename"),EXDEV);
    CHECK(!mount("bs/sub","bp",0,MS_BIND,0));
    CHECK(!syscall(SYS_umount2,"bp",UINT64_C(0x100000002)));
    pass("source-target-type-and-nonmount-errors");
}
static void detach(void)
{
    CHECK(!umount("bs"));int fd=open("bv/data",O_RDWR),dir=open("bv",O_RDONLY|O_DIRECTORY),cwd=open(".",O_RDONLY|O_DIRECTORY);CHECK(fd>=0&&dir>=0&&cwd>=0);
    content(fd,"stable");unsigned char *shared=mmap(0,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0),*private=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(shared!=MAP_FAILED&&private!=MAP_FAILED);private[0]='P';shared[1]='H';CHECK(!close(fd));
    pass("original-unmount-keeps-bind-instance");
    CHECK(!umount2("bv",MNT_DETACH));struct stat st;ERROR(stat("bv/data",&st),ENOENT);
    fd=openat(dir,"data",O_RDWR);char byte;CHECK(fd>=0&&pread(fd,&byte,1,0)==1&&byte=='s');CHECK(!close(fd));
    CHECK(!fchdir(dir));char path[2048];errno=0;CHECK(!getcwd(path,sizeof(path))&&errno==ENOENT);CHECK(!fchdir(cwd)&&!close(cwd));
    CHECK(shared[0]=='s'&&shared[1]=='H'&&private[0]=='P');
    CHECK(!madvise(shared,page,MADV_REMOVE));CHECK(!shared[0]&&!shared[1]&&private[0]=='P');
    CHECK(!close(dir)&&!munmap(private,page)&&!munmap(shared,page));
    pass("lazy-final-dirfd-mmap-remove-and-cow");
}
static void pty_exchange(int master,int slave)
{
    struct termios mode;CHECK(!tcgetattr(slave,&mode));cfmakeraw(&mode);CHECK(!tcsetattr(slave,TCSANOW,&mode));
    char c;CHECK(write(master,"x",1)==1&&read(slave,&c,1)==1&&c=='x');
    CHECK(write(slave,"y",1)==1&&read(master,&c,1)==1&&c=='y');
}
static void pty_bind(void)
{
    directory("bd");CHECK(!mount("devpts","bd","devpts",0,"newinstance,ptmxmode=0666"));
    int master=open("bd/ptmx",O_RDWR|O_NOCTTY),unlock=0;CHECK(master>=0&&!ioctl(master,TIOCSPTLCK,&unlock));
    int slave=ioctl(master,TIOCGPTPEER,O_RDWR|O_NOCTTY);CHECK(slave>=0);pty_exchange(master,slave);
    CHECK(!close(slave)&&!close(master)&&!umount2("bd",MNT_DETACH));
    CHECK(!mount("devpts","bd","devpts",0,"newinstance,ptmxmode=0666"));struct stat source,peer;CHECK(!stat("bd",&source));
    create("bt","");CHECK(!mount("bd/ptmx","bt",0,MS_BIND,0)&&!umount("bd"));
    master=open("bt",O_RDWR|O_NOCTTY);CHECK(master>=0&&!ioctl(master,TIOCSPTLCK,&unlock));
    ERROR(ioctl(master,TIOCGPTPEER,O_RDWR|O_NOCTTY),ENOENT);
    CHECK(!umount2("bt",MNT_DETACH));
    slave=ioctl(master,TIOCGPTPEER,O_RDWR|O_NOCTTY);CHECK(slave>=0&&!fstat(slave,&peer)&&source.st_dev==peer.st_dev);
    pty_exchange(master,slave);
    CHECK(!close(slave)&&!close(master)&&!unlink("bt")&&!rmdir("bd"));
    pass("devpts-ptmx-file-bind-peer-and-last-owner");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);cpu_set_t allowed,selected;CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));
    int cpu=0;for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&allowed))cpu=i;
    CPU_ZERO(&selected);CPU_SET(cpu,&selected);CHECK(!sched_setaffinity(0,sizeof(selected),&selected));
    printf("bind caller cpu=%d\n",cpu);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page&&getcwd(base,sizeof(base)));
    for(unsigned i=0;i<4;i++)directory((const char *[]){"bs","bv","bp","br"}[i]);
    create("bf","");CHECK(!mount("tmpfs","bs","tmpfs",0,0));directory("bs/sub");create("bs/sub/data","stable");
    identity();renamed_directory();attributes();recursive();errors();detach();pty_bind();
    CHECK(!unlink("bf"));for(unsigned i=0;i<4;i++)CHECK(!rmdir((const char *[]){"bs","bv","bp","br"}[i]));
    printf("BIND END %u\n",cases);return 0;
}
