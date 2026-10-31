#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"ACL FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define USER_OBJ 1
#define USER 2
#define GROUP_OBJ 4
#define MASK 16
#define OTHER 32
struct acl_entry {uint16_t tag,perm;uint32_t id;};
struct acl_five {uint32_t version;struct acl_entry e[5];};
static struct acl_five make_acl(unsigned user,unsigned mask)
{
    return (struct acl_five){2,{{USER_OBJ,6,UINT32_MAX},{USER,user,12345},
        {GROUP_OBJ,4,UINT32_MAX},{MASK,mask,UINT32_MAX},{OTHER,0,UINT32_MAX}}};
}
static void as_user_open(const char *path,int flags,int allowed)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(!setgroups(0,0)&&!setgid(123)&&!setuid(12345));
        errno=0;int fd=open(path,flags);
        if(allowed){CHECK(fd>=0);CHECK(!close(fd));}
        else CHECK(fd==-1&&errno==EACCES);
        int mode=flags==O_WRONLY ? W_OK : R_OK;
        errno=0;int access_result=faccessat(AT_FDCWD,path,mode,AT_EACCESS);
        if(allowed) CHECK(access_result==0);
        else CHECK(access_result==-1&&errno==EACCES);
        int pathfd=open(path,O_PATH);CHECK(pathfd>=0);
        errno=0;access_result=(int)syscall(SYS_faccessat2,pathfd,"",mode,AT_EMPTY_PATH|AT_EACCESS);
        if(allowed) CHECK(access_result==0);
        else CHECK(access_result==-1&&errno==EACCES);
        CHECK(!close(pathfd));
        struct acl_five forged=make_acl(7,7);
        errno=0;CHECK(setxattr(path,"system.posix_acl_access",&forged,sizeof(forged),0)==-1&&errno==EPERM);
        CHECK(!setxattr(path,"system.posix_acl_default",NULL,0,0));
        CHECK(!removexattr(path,"system.posix_acl_default"));
        _exit(0);
    }
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
static void as_user_create(const char *path,int allowed)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(!setgroups(0,0)&&!setgid(123)&&!setuid(12345));
        errno=0;int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);
        if(allowed) {CHECK(fd>=0);CHECK(!close(fd));CHECK(!unlink(path));}
        else CHECK(fd==-1&&errno==EACCES);
        _exit(0);
    }
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
static void as_user_sgid_create(const char *path)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(!setgroups(0,0)&&!setgid(123)&&!setuid(12345));
        int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,02710);CHECK(fd>=0);
        struct stat st;CHECK(!fstat(fd,&st));
        CHECK(st.st_gid==456 && !(st.st_mode & 02000));
        CHECK(!close(fd)&&!unlink(path));_exit(0);
    }
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
struct acl_race {const char *directory,*file;atomic_int ready,start;};
static void race_start(struct acl_race *race,unsigned role)
{
    cpu_set_t available,selected;
    CPU_ZERO(&available);CHECK(!sched_getaffinity(0,sizeof(available),&available));
    unsigned count=(unsigned)CPU_COUNT(&available);CHECK(count);
    unsigned ordinal=role%count;
    CPU_ZERO(&selected);
    for (unsigned cpu=0;cpu<CPU_SETSIZE;cpu++) if (CPU_ISSET(cpu,&available)) {
        if (!ordinal--) {CPU_SET(cpu,&selected);break;}
    }
    CHECK(!sched_setaffinity(0,sizeof(selected),&selected));
    atomic_fetch_add(&race->ready,1);
    while (!atomic_load(&race->start)) sched_yield();
}
static void *race_update(void *argument)
{
    struct acl_race *race=argument;race_start(race,0);
    for (unsigned i=0;i<32;i++) {
        struct acl_five value=make_acl(6,(i&1)?4:6);value.e[0].perm=7;
        CHECK(!setxattr(race->directory,"system.posix_acl_default",&value,sizeof(value),0));
    }
    return NULL;
}
static void *race_create(void *argument)
{
    struct acl_race *race=argument;race_start(race,1);
    for (unsigned i=0;i<32;i++) {
        int fd=open(race->file,O_CREAT|O_EXCL|O_RDWR,0666);CHECK(fd>=0);
        struct stat st;struct acl_five value;
        CHECK(!fstat(fd,&st));
        CHECK(fgetxattr(fd,"system.posix_acl_access",&value,sizeof(value))==(ssize_t)sizeof(value));
        CHECK(value.e[3].tag==MASK && (value.e[3].perm==4 || value.e[3].perm==6));
        CHECK(((st.st_mode>>3)&7)==value.e[3].perm);
        CHECK(!close(fd)&&!unlink(race->file));
    }
    return NULL;
}
static void *race_other_xattr(void *argument)
{
    struct acl_race *race=argument;race_start(race,2);
    for (unsigned i=0;i<32;i++) {
        CHECK(!setxattr(race->directory,"user.acl-race","x",1,0));
        CHECK(!removexattr(race->directory,"user.acl-race"));
    }
    return NULL;
}
static void race_inheritance(const char *directory,const char *file)
{
    struct acl_race race={.directory=directory,.file=file};pthread_t update,create,other;
    CHECK(!pthread_create(&update,NULL,race_update,&race));
    CHECK(!pthread_create(&create,NULL,race_create,&race));
    CHECK(!pthread_create(&other,NULL,race_other_xattr,&race));
    while(atomic_load(&race.ready)!=3) sched_yield();
    atomic_store(&race.start,1);
    CHECK(!pthread_join(update,NULL)&&!pthread_join(create,NULL)&&!pthread_join(other,NULL));
}
static void exercise(const char *base,const char *label)
{
    char dir[160],file[160],alias[160],sub[160],fifo[160];
    snprintf(dir,sizeof(dir),"%s/d",base);snprintf(file,sizeof(file),"%s/d/f",base);
    snprintf(alias,sizeof(alias),"%s/d/link",base);snprintf(sub,sizeof(sub),"%s/d/sub",base);
    snprintf(fifo,sizeof(fifo),"%s/d/fifo",base);
    CHECK(!mkdir(dir,0777));CHECK(!chmod(dir,0777));
    int fd=open(file,O_CREAT|O_EXCL|O_RDWR,0640);CHECK(fd>=0);CHECK(!close(fd));
    CHECK(!chown(file,0,123));struct acl_five access=make_acl(0,4),got;
    struct acl_five invalid=access;invalid.e[1].perm=8;
    errno=0;CHECK(setxattr(file,"system.posix_acl_access",&invalid,sizeof(invalid),0)==-1&&errno==EINVAL);
    errno=0;CHECK(setxattr(file,"system.posix_acl_default",&access,sizeof(access),0)==-1&&errno==EACCES);
    fd=open(file,O_RDONLY);CHECK(fd>=0);struct stat st;
    access.e[3].perm=6;
    CHECK(!setxattr(file,"system.posix_acl_access",&access,sizeof(access),0));
    CHECK(!fstat(fd,&st)&&(st.st_mode&0777)==0660);
    access.e[3].perm=4;
    CHECK(!setxattr(file,"system.posix_acl_access",&access,sizeof(access),0));
    CHECK(!fstat(fd,&st)&&(st.st_mode&0777)==0640);CHECK(!close(fd));
    CHECK(!setxattr(file,"system.posix_acl_access",&access,sizeof(access),XATTR_CREATE));
    CHECK(getxattr(file,"system.posix_acl_access",&got,sizeof(got))==(ssize_t)sizeof(got));
    CHECK(!memcmp(&access,&got,sizeof(got)));as_user_open(file,O_RDONLY,0);
    CHECK(!link(file,alias));CHECK(!removexattr(alias,"system.posix_acl_access"));
    as_user_open(file,O_RDONLY,1);CHECK(!unlink(alias)&&!unlink(file));
    struct acl_five def=make_acl(6,6);def.e[0].perm=7;
    CHECK(!setxattr(dir,"system.posix_acl_default",&def,sizeof(def),0));
    mode_t old=umask(077);fd=open(file,O_CREAT|O_EXCL|O_RDWR,0666);CHECK(fd>=0);CHECK(!close(fd));
    CHECK(!mkdir(sub,0777));CHECK(!mkfifo(fifo,0666));umask(old);
    CHECK(!stat(file,&st)&&(st.st_mode&0777)==0660);
    CHECK(!stat(fifo,&st)&&(st.st_mode&0777)==0660);
    CHECK(!stat(sub,&st)&&(st.st_mode&0777)==0760);
    CHECK(getxattr(sub,"system.posix_acl_default",&got,sizeof(got))==(ssize_t)sizeof(got));
    as_user_open(file,O_WRONLY,1);CHECK(!chmod(file,0640));
    CHECK(getxattr(file,"system.posix_acl_access",&got,sizeof(got))==(ssize_t)sizeof(got));
    CHECK(got.e[3].tag==MASK&&got.e[3].perm==4);as_user_open(file,O_RDONLY,1);as_user_open(file,O_WRONLY,0);
    struct acl_five dir_acl=make_acl(7,7);dir_acl.e[0].perm=7;
    CHECK(!setxattr(dir,"system.posix_acl_access",&dir_acl,sizeof(dir_acl),0));
    as_user_create(alias,1);
    dir_acl.e[1].perm=0;
    CHECK(!setxattr(dir,"system.posix_acl_access",&dir_acl,sizeof(dir_acl),0));
    as_user_create(alias,0);
    race_inheritance(dir,alias);
    CHECK(!chown(dir,0,456)&&!chmod(dir,02777));
    dir_acl=make_acl(7,7);dir_acl.e[0].perm=7;
    CHECK(!setxattr(dir,"system.posix_acl_access",&dir_acl,sizeof(dir_acl),0));
    def=make_acl(6,6);def.e[0].perm=7;
    CHECK(!setxattr(dir,"system.posix_acl_default",&def,sizeof(def),0));
    as_user_sgid_create(alias);
    CHECK(!unlink(file)&&!unlink(fifo)&&!rmdir(sub)&&!rmdir(dir));printf("ACL CASE %s PASS\n",label);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);CHECK(!mkdir("/acl-ext",0777)||errno==EEXIST);
    exercise("/acl-ext","ext4");CHECK(!rmdir("/acl-ext"));
    CHECK(!mkdir("/acl-tmp",0777)||errno==EEXIST);CHECK(!mount("tmpfs","/acl-tmp","tmpfs",0,""));
    exercise("/acl-tmp","tmpfs");CHECK(!umount("/acl-tmp")&&!rmdir("/acl-tmp"));puts("ACL PASS");return 0;
}
