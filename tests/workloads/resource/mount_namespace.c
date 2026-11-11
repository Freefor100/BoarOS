#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"MOUNTNS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1); } } while (0)
#define ERROR(x,e) do { errno=0;CHECK((x)==-1 && errno==(e)); } while (0)
static const char data_path[]="/ns-data";
static const char nested_path[]="/ns-data/nested";
static const char marker_path[]="/ns-data/nested/marker";
static const char private_path[]="/ns-data/private";
static const char hidden_path[]="/ns-data/private/hidden";
static const char self_ns[]="/ns-proc/self/ns/mnt";
static char clone_stack[65536] __attribute__((aligned(16)));

static void touch(const char *name)
{
    int fd=open(name,O_CREAT|O_WRONLY|O_EXCL,0644);
    CHECK(fd>=0 && write(fd,"x",1)==1 && !close(fd));
}

static void marker(int dirfd,const char *name)
{
    int fd=openat(dirfd,name,O_RDONLY);
    char data;
    CHECK(fd>=0 && read(fd,&data,1)==1 && data=='x' && !close(fd));
}

static int mount_listing(pid_t pid,const char *mountpoint)
{
    char path[64],text[4096];
    CHECK(snprintf(path,sizeof(path),"/ns-proc/%d/mounts",(int)pid)>0);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);
    ssize_t got=read(fd,text,sizeof(text)-1);
    CHECK(got>=0 && !close(fd));
    text[got]=0;
    return strstr(text,mountpoint)!=NULL;
}

static void joined(pid_t child)
{
    int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

static int clone_isolate(void *unused)
{
    (void)unused;
    marker(AT_FDCWD,marker_path);
    CHECK(!umount(nested_path));
    ERROR(access(marker_path,F_OK),ENOENT);
    return 0;
}

static int clone_share(void *arg)
{
    int fd=*(int *)arg;
    ERROR(setns(fd,CLONE_NEWNS),EINVAL);
    return 0;
}

int main(void)
{
    CHECK(!mkdir("/ns-proc",0755) && !mount("proc","/ns-proc","proc",0,NULL));
    CHECK(!mkdir(data_path,0755) && !mount("tmpfs",data_path,"tmpfs",0,NULL));
    CHECK(!mkdir(nested_path,0755) && !mount("tmpfs",nested_path,"tmpfs",0,NULL));
    CHECK(!mkdir(private_path,0755));touch(marker_path);
    int original=open(self_ns,O_RDONLY|O_CLOEXEC);
    int original_dir=open(nested_path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(original>=0 && original_dir>=0 && (fcntl(original,F_GETFD)&FD_CLOEXEC));
    struct stat oldstat,newstat;
    CHECK(!fstat(original,&oldstat));
    struct statfs ns_statfs;
    CHECK(!fstatfs(original,&ns_statfs) && ns_statfs.f_type==0x6e736673);
    CHECK(ioctl(original,0xb703U)==CLONE_NEWNS);
    ERROR(read(original,&newstat,1),EINVAL);
    char text[64];ssize_t length=readlink(self_ns,text,sizeof(text)-1);
    CHECK(length>6);text[length]=0;CHECK(!strncmp(text,"mnt:[",5));
    CHECK(!chdir(nested_path));

    int ready[2],release[2];
    CHECK(!pipe(ready) && !pipe(release));
    pid_t parent=getpid(), child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!unshare(CLONE_NEWNS));
        CHECK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
        CHECK(getcwd(text,sizeof(text)) && !strcmp(text,nested_path));
        marker(AT_FDCWD,"marker");
        ERROR(umount(nested_path),EBUSY);
        CHECK(!fchdir(original_dir));
        marker(AT_FDCWD,"marker");
        ERROR(mount("tmpfs",".","tmpfs",0,NULL),EINVAL);
        CHECK(!chdir("/"));
        int local=open(self_ns,O_RDONLY);CHECK(local>=0 && !fstat(local,&newstat));
        CHECK(oldstat.st_ino!=newstat.st_ino && !close(local));
        CHECK(!mount("tmpfs",private_path,"tmpfs",0,NULL));touch(hidden_path);
        CHECK(mount_listing(getpid()," /ns-data/private ") && !mount_listing(parent," /ns-data/private "));
        CHECK(write(ready[1],"x",1)==1 && read(release[0],text,1)==1);
        _exit(0);
    }
    CHECK(read(ready[0],text,1)==1);
    ERROR(access(hidden_path,F_OK),ENOENT);
    CHECK(!mount_listing(getpid()," /ns-data/private ") && mount_listing(child," /ns-data/private "));
    CHECK(snprintf(text,sizeof(text),"/ns-proc/%d/ns/mnt",(int)child)>0);
    int copy=open(text,O_RDONLY|O_CLOEXEC);CHECK(copy>=0);
    CHECK(!fstat(copy,&newstat) && newstat.st_ino!=oldstat.st_ino);
    int pidfd=(int)syscall(SYS_pidfd_open,child,0);CHECK(pidfd>=0);
    ERROR(setns(pidfd,0),EINVAL);
    CHECK(!setns(pidfd,CLONE_NEWNS) && getcwd(text,sizeof(text)) && !strcmp(text,"/"));
    marker(AT_FDCWD,hidden_path);
    CHECK(!setns(original,0) && !close(pidfd));
    ERROR(access(hidden_path,F_OK),ENOENT);
    CHECK(write(release[1],"x",1)==1);joined(child);
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(release[0]) && !close(release[1]));
    /* Only the copied namespace FD owns the tree after the child exits. */
    CHECK(!setns(copy,0));marker(AT_FDCWD,hidden_path);
    CHECK(!setns(original,CLONE_NEWNS) && !close(copy));
    ERROR(access(hidden_path,F_OK),ENOENT);
    puts("MOUNTNS CASE unshare-cwd-isolation-proc-setns-last-owner PASS");

    ERROR(clone(clone_isolate,clone_stack+sizeof(clone_stack),CLONE_NEWNS|CLONE_FS|SIGCHLD,NULL),EINVAL);
    child=clone(clone_isolate,clone_stack+sizeof(clone_stack),CLONE_NEWNS|SIGCHLD,NULL);
    CHECK(child>=0);joined(child);
    marker(AT_FDCWD,marker_path);
    child=clone(clone_share,clone_stack+sizeof(clone_stack),CLONE_FS|SIGCHLD,&original);
    CHECK(child>=0);joined(child);
    child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!setuid(65534));
        ERROR(unshare(CLONE_NEWNS),EPERM);
        ERROR(setns(original,CLONE_NEWNS),EPERM);
        _exit(0);
    }
    joined(child);
    puts("MOUNTNS CASE clone-flags-shared-fs-permissions PASS");
    CHECK(!close(original_dir) && !close(original));
    CHECK(!umount(nested_path) && !umount(data_path) && !umount("/ns-proc"));
    CHECK(!rmdir(data_path) && !rmdir("/ns-proc"));
    puts("MOUNTNS END 2");
    return 0;
}
