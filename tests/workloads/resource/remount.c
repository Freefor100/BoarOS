#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"REMOUNT FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); _exit(1); } } while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));} while(0)
static unsigned cases;
static void pass(const char *name){printf("REMOUNT CASE %s PASS\n",name);cases++;}
static void remount(const char *path,unsigned long flags){CHECK(!mount("ignored",path,"ignored",MS_REMOUNT|flags,0));}
static void flags(const char *path,unsigned long expected){struct statfs s;CHECK(!statfs(path,&s)&&((unsigned long)s.f_flags&15)==expected);}
static void directory(const char *path){CHECK(!mkdir(path,0755)||errno==EEXIST);}
static void token(int fd){CHECK(write(fd,"x",1)==1);}
static void wait_token(int fd){char c;CHECK(read(fd,&c,1)==1&&c=='x');}
static void content(int fd,const char *expected){char bytes[8]={0};CHECK(pread(fd,bytes,strlen(expected),0)==(ssize_t)strlen(expected)&&!strcmp(bytes,expected));}
static void ordinary(void)
{
    directory("rm");CHECK(!mount("tmpfs","rm","tmpfs",0,0));directory("rm/sub");
    ERROR(mount(0,"rm/sub",0,MS_REMOUNT,0),EINVAL);
    ERROR(mount(0,"rm/sub",0,MS_REMOUNT,"unsupported"),EINVAL);
    ERROR(mount(0,"/absent-remount",0,MS_REMOUNT,0),ENOENT);
    int fd=open("rm/data",O_CREAT|O_RDWR,0755);CHECK(fd>=0&&write(fd,"stable",6)==6);
    remount("rm",MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SILENT);flags("rm",14);
    ERROR(mount(0,"rm",0,MS_REMOUNT|MS_RDONLY,0),EBUSY);flags("rm",14);CHECK(pwrite(fd,"stable",6,0)==6);
    int copy=dup(fd);CHECK(copy>=0&&!close(fd));ERROR(mount(0,"rm",0,MS_REMOUNT|MS_RDONLY,0),EBUSY);CHECK(!close(copy));
    remount("rm",MS_RDONLY);flags("rm",1);
    ERROR(open("rm/new",O_CREAT|O_RDWR,0600),EROFS);ERROR(chmod("rm/data",0600),EROFS);
    fd=open("rm/data",O_RDONLY);CHECK(fd>=0);content(fd,"stable");
    remount("rm",0);flags("rm",0);CHECK(!close(fd));
    pass("attributes-readonly-busy-ofd-and-restored-write");
    fd=open("rm/data",O_RDWR);CHECK(fd>=0&&!ftruncate(fd,(off_t)sysconf(_SC_PAGESIZE)));
    char *shared=mmap(0,(size_t)sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(shared!=MAP_FAILED&&!close(fd));
    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));pid_t child=fork();CHECK(child>=0);
    if(!child){token(ready[1]);wait_token(release[0]);shared[0]='s';CHECK(!munmap(shared,(size_t)sysconf(_SC_PAGESIZE)));_exit(0);}
    wait_token(ready[0]);CHECK(!munmap(shared,(size_t)sysconf(_SC_PAGESIZE)));
    ERROR(mount(0,"rm",0,MS_REMOUNT|MS_RDONLY,0),EBUSY);token(release[1]);int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    remount("rm",MS_RDONLY);remount("rm",0);
    pass("shared-mapping-close-fork-and-final-owner");
    fd=open("rm/data",O_RDONLY);CHECK(fd>=0&&!unlink("rm/data"));
    ERROR(mount(0,"rm",0,MS_REMOUNT|MS_RDONLY,0),EBUSY);CHECK(!close(fd));
    remount("rm",MS_RDONLY);remount("rm",0);CHECK(!umount("rm")&&!rmdir("rm"));
    directory("rm");CHECK(!mount("tmpfs","rm","tmpfs",MS_RDONLY,0));
    remount("rm",0);fd=open("rm/initial-ro",O_CREAT|O_RDWR,0600);CHECK(fd>=0&&!close(fd));
    CHECK(!umount("rm"));CHECK(!mount("proc","rm","proc",MS_RDONLY,0));
    remount("rm",MS_NOSUID|MS_NODEV|MS_NOEXEC);flags("rm",14);
    remount("rm",MS_RDONLY);flags("rm",1);CHECK(!umount("rm")&&!rmdir("rm"));
    pass("unlinked-read-owner-and-unmount");
}
static void root(void)
{
    int fd=open("/remount-root-data",O_CREAT|O_RDWR,0600);CHECK(fd>=0&&write(fd,"stable",6)==6&&!close(fd));
    remount("/",MS_RDONLY|MS_NOSUID);flags("/",3);
    fd=open("/remount-root-data",O_RDONLY);CHECK(fd>=0);content(fd,"stable");CHECK(!close(fd));
    ERROR(open("/remount-root-new",O_CREAT|O_WRONLY,0600),EROFS);
    remount("/",0);flags("/",0);fd=open("/remount-root-data",O_WRONLY);CHECK(fd>=0&&pwrite(fd,"stable",6,0)==6&&!close(fd));
    CHECK(!unlink("/remount-root-data"));
    remount("/",MS_NOSUID|MS_NODEV);CHECK(!umount("/"));flags("/",7);
    remount("/",0);flags("/",0);pass("root-dirty-sync-readonly-and-readwrite");
}
int main(int argc,char **argv){setvbuf(stdout,0,_IONBF,0);ordinary();if(argc==1||strcmp(argv[1],"--no-root"))root();printf("REMOUNT END %u\n",cases);return 0;}
