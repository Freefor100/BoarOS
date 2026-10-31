#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
#define CHECK(x) do { if (!(x)) { dprintf(2,"MOVE FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1); } } while(0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while(0)
static unsigned cases;
static size_t page;
static char base[1024];
static void pass(const char *name) { printf("MOVE CASE %s PASS\n",name);cases++; }
static void directory(const char *name) { CHECK(!mkdir(name,0755)); }
static void create(const char *name,const char *text)
{ int fd=open(name,O_CREAT|O_RDWR|O_EXCL,0755);CHECK(fd>=0&&write(fd,text,strlen(text))==(ssize_t)strlen(text)&&!close(fd)); }
static void content(int fd,const char *text)
{ char bytes[16]={0};CHECK(strlen(text)<sizeof(bytes)&&pread(fd,bytes,strlen(text),0)==(ssize_t)strlen(text)&&!strcmp(bytes,text)); }
static void same(const char *a,const char *b)
{ struct stat x,y;CHECK(!stat(a,&x)&&!stat(b,&y)&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino); }
static void flags(const char *name,unsigned expected)
{ struct statfs s;CHECK(!statfs(name,&s)&&((unsigned long)s.f_flags&15U)==expected); }
static void token(int fd) { CHECK(write(fd,"x",1)==1); }
static void wait_token(int fd) { char c;CHECK(read(fd,&c,1)==1&&c=='x'); }
static void joined(pid_t child) { int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status)); }
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page&&getcwd(base,sizeof(base)));
    const char *dirs[]={"ma","mb","mc","md","mp","mn"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++)directory(dirs[i]);
    create("mf","");create("ma/under","covered");
    CHECK(!mount("tmpfs","ma","tmpfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,0));
    create("ma/data","stable");directory("ma/sub");directory("ma/child");
    CHECK(!mount("tmpfs","ma/child","tmpfs",0,0));create("ma/child/token","child");
    CHECK(!mount("ma","md",0,MS_BIND|MS_REC,0));
    int fd=open("ma/data",O_RDWR),dir=open("ma/sub",O_RDONLY|O_DIRECTORY),cwd=open(".",O_RDONLY|O_DIRECTORY);
    CHECK(fd>=0&&dir>=0&&cwd>=0&&!ftruncate(fd,(off_t)page));
    char *shared=mmap(0,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0),*private=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(shared!=MAP_FAILED&&private!=MAP_FAILED);private[0]='P';
    CHECK(!mount("ma","mb",0,MS_MOVE,0));
    struct stat st;ERROR(stat("ma/data",&st),ENOENT);int under=open("ma/under",O_RDONLY);CHECK(under>=0);content(under,"covered");CHECK(!close(under));
    same("mb/data","md/data");same("mb/child/token","md/child/token");flags("mb",14);flags("mb/child",0);
    content(fd,"stable");shared[1]='H';CHECK(private[0]=='P'&&private[1]=='t');
    CHECK(!fchdir(dir));char actual[2048],expected[2048];
    CHECK(snprintf(expected,sizeof(expected),"%s%smb/sub",base,strcmp(base,"/")?"/":"")>0);
    CHECK(getcwd(actual,sizeof(actual))&&!strcmp(actual,expected)&&!fchdir(cwd));
    pass("live-subtree-identity-attributes-fd-cwd-and-maps");

    ERROR(mount("mb/sub","mc",0,MS_MOVE,0),EINVAL);
    ERROR(mount("/","mc",0,MS_MOVE,0),ELOOP);
    ERROR(mount("mb","mf",0,MS_MOVE,0),EINVAL);
    ERROR(mount("mb","mb/sub",0,MS_MOVE,0),ELOOP);
    ERROR(mount("mb","mb/child",0,MS_MOVE,0),ELOOP);
    ERROR(mount("mb","mb",0,MS_MOVE,0),ELOOP);
    ERROR(mount("missing-move-source","mc",0,MS_MOVE,0),ENOENT);
    ERROR(mount("mb","missing-move-target",0,MS_MOVE,0),ENOENT);
    same("mb/data","md/data");
    ERROR(syscall(SYS_mount,"/missing-move-source","mc",(void *)(uintptr_t)1,MS_MOVE,0),EFAULT);
    ERROR(syscall(SYS_mount,"/missing-move-source","mc",0,MS_MOVE,(void *)(uintptr_t)1),EFAULT);
    pass("source-root-type-cycles-and-failure-rollback");

    CHECK(!mount("tmpfs","mc","tmpfs",0,0));create("mc/lower","lower");
    CHECK(!mount("mb","mc",0,MS_MOVE|MS_RDONLY|MS_REC,0));flags("mc",14);ERROR(stat("mc/lower",&st),ENOENT);
    CHECK(!mount("mc","mb",0,MS_MOVE,0));under=open("mc/lower",O_RDONLY);CHECK(under>=0);content(under,"lower");CHECK(!close(under)&&!umount("mc"));
    CHECK(!mount("tmpfs","mp","tmpfs",0,0));directory("mp/dest");
    CHECK(!mount("mb","mp/dest",0,MS_MOVE,0));same("mp/dest/data","md/data");
    CHECK(!mount("mp/dest","mb",0,MS_MOVE,0)&&!umount("mp"));
    pass("stack-unstack-cross-parent-and-preserved-flags");

    CHECK(!symlink("mb","move-source-link")&&!symlink("mc","move-target-link"));
    CHECK(!mount("move-source-link","move-target-link",0,MS_MOVE,0));same("mc/data","md/data");
    CHECK(!mount("mc","mb",0,MS_MOVE,0)&&!unlink("move-source-link")&&!unlink("move-target-link"));
    CHECK(!mount("md/data","mf",0,MS_BIND,0));create("mg","");
    CHECK(!mount("mf","mg",0,MS_MOVE,0));same("mg","md/data");
    CHECK(!umount("mg")&&!unlink("mg"));pass("source-target-symlinks-and-file-mount");

    pid_t child=fork();CHECK(child>=0);
    if(!child) { CHECK(!setuid(65534));ERROR(mount("missing-move-source","mc",0,MS_MOVE,0),EPERM);ERROR(umount2("mb",UMOUNT_NOFOLLOW),EPERM);_exit(0); }
    joined(child);same("mb/data","md/data");pass("permission-before-source-validation");

    CHECK(!mount("tmpfs","mn","tmpfs",0,0));CHECK(!symlink("mn","unmount-link"));
    ERROR(umount2("unmount-link",UMOUNT_NOFOLLOW),EINVAL);
    CHECK(!umount2("unmount-link/",UMOUNT_NOFOLLOW));
    CHECK(!mount("tmpfs","mn","tmpfs",0,0));CHECK(!umount("unmount-link"));
    CHECK(!mount("tmpfs","mn","tmpfs",0,0));
    CHECK(!syscall(SYS_umount2,"mn",UINT64_C(0x100000008)));
    CHECK(!mount("tmpfs","mn","tmpfs",0,0));
    CHECK(!symlink(".","unmount-parent-link"));CHECK(!umount2("unmount-parent-link/mn",UMOUNT_NOFOLLOW|MNT_DETACH));
    CHECK(!unlink("unmount-link")&&!unlink("unmount-parent-link"));pass("nofollow-final-trailing-intermediate-and-int32");

    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));child=fork();CHECK(child>=0);
    if(!child) { CHECK(!fchdir(dir));token(ready[1]);wait_token(release[0]);
        CHECK(getcwd(actual,sizeof(actual)));CHECK(strstr(actual,"/mc/sub"));content(fd,"sHable");
        int parent=open("../data",O_RDONLY);CHECK(parent>=0);content(parent,"sHable");CHECK(!close(parent));
        token(ready[1]);wait_token(release[0]);ERROR(getcwd(actual,sizeof(actual))?0:-1,ENOENT);
        CHECK(shared[0]=='s'&&shared[1]=='H'&&private[0]=='P');_exit(0); }
    wait_token(ready[0]);CHECK(!mount("mb","mc",0,MS_MOVE,0));token(release[1]);wait_token(ready[0]);
    ERROR(umount("mc"),EBUSY);CHECK(!umount2("mc",MNT_DETACH));ERROR(stat("mc/data",&st),ENOENT);
    token(release[1]);joined(child);content(fd,"sHable");
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    CHECK(!close(fd)&&!close(dir)&&!close(cwd)&&!munmap(shared,page)&&!munmap(private,page));
    CHECK(!umount2("md",MNT_DETACH));pass("handshaken-move-detach-and-final-owners");
    CHECK(!unlink("mf")&&!unlink("ma/under"));
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++)CHECK(!rmdir(dirs[i]));
    printf("MOVE END %u\n",cases);return 0;
}
