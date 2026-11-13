#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
/* Linux UAPI uses a four-byte payload despite encoding sizeof(long). */
#define FS_IOC_GETFLAGS _IOR('f',1,long)
#define FS_IOC_SETFLAGS _IOW('f',2,long)
#define FS_IMMUTABLE_FL 0x10U
#define FS_APPEND_FL 0x20U
#define FS_NODUMP_FL 0x40U
#define ATTR_IMMUTABLE 0x10U
#define ATTR_APPEND 0x20U
#define ATTR_NODUMP 0x40U
struct flag_statx {
    uint32_t mask,blksize;uint64_t attributes;uint32_t nlink,uid,gid;uint16_t mode,spare;
    uint64_t ino,size,blocks,attributes_mask;unsigned char rest[192];
};
#define CHECK(x) do { if (!(x)) { dprintf(2,"FILE_FLAGS FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
#define DENIED(x,e) do { errno=0;CHECK((x)==-1 && errno==(e)); } while (0)
static uint32_t getflags(int fd) { uint32_t n=0;CHECK(!ioctl(fd,FS_IOC_GETFLAGS,&n));return n; }
static void flags(int fd,uint32_t n) { CHECK(!ioctl(fd,FS_IOC_SETFLAGS,&n));CHECK(getflags(fd)==n); }
static void content(int fd,const char *s) { char b[80];ssize_t n=pread(fd,b,sizeof(b),0);CHECK(n==(ssize_t)strlen(s) && !memcmp(b,s,(size_t)n)); }
static void statattrs(const char *path,uint32_t flags)
{
    struct flag_statx s;CHECK(!syscall(SYS_statx,AT_FDCWD,path,0,0x7ff,&s));
    CHECK((s.attributes_mask&(ATTR_IMMUTABLE|ATTR_APPEND|ATTR_NODUMP))==(ATTR_IMMUTABLE|ATTR_APPEND|ATTR_NODUMP));
    uint64_t want=(flags&FS_IMMUTABLE_FL?ATTR_IMMUTABLE:0)|(flags&FS_APPEND_FL?ATTR_APPEND:0)|(flags&FS_NODUMP_FL?ATTR_NODUMP:0);
    CHECK((s.attributes&(ATTR_IMMUTABLE|ATTR_APPEND|ATTR_NODUMP))==want);
}
static void joined(pid_t p) { int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status)); }
static void ownership(int fd,uint32_t base)
{
    pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!setresuid(65534,65534,65534));uint32_t v=base|FS_NODUMP_FL;DENIED(ioctl(fd,FS_IOC_SETFLAGS,&v),EPERM);CHECK(getflags(fd)==base);_exit(0);}joined(p);
    CHECK(!fchown(fd,65534,65534));p=fork();CHECK(p>=0);if(!p){CHECK(!setresuid(65534,65534,65534));flags(fd,base|FS_NODUMP_FL);uint32_t v=base|FS_IMMUTABLE_FL;DENIED(ioctl(fd,FS_IOC_SETFLAGS,&v),EPERM);CHECK(getflags(fd)==(base|FS_NODUMP_FL));flags(fd,base);_exit(0);}joined(p);CHECK(!fchown(fd,0,0));
    struct caphdr { uint32_t version;int pid; } h={0x20080522U,0};
    struct capdata { uint32_t effective,permitted,inheritable; } d[2];
    p=fork();CHECK(p>=0);if(!p){CHECK(!syscall(SYS_capget,&h,d));d[0].effective&=~(1U<<9);d[0].permitted&=~(1U<<9);CHECK(!syscall(SYS_capset,&h,d));uint32_t v=base|FS_APPEND_FL;DENIED(ioctl(fd,FS_IOC_SETFLAGS,&v),EPERM);CHECK(getflags(fd)==base);flags(fd,base|FS_NODUMP_FL);flags(fd,base);_exit(0);}joined(p);
}
static void directory(const char *root)
{
    char dir[180],a[200],b[200],c[200];snprintf(dir,sizeof(dir),"%s/dir",root);snprintf(a,sizeof(a),"%s/a",dir);snprintf(b,sizeof(b),"%s/b",dir);snprintf(c,sizeof(c),"%s/c",dir);CHECK(!mkdir(dir,0700));int fd=open(dir,O_RDONLY|O_DIRECTORY);CHECK(fd>=0);uint32_t base=getflags(fd);flags(fd,base|FS_NODUMP_FL);int file=open(a,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(file>=0);uint32_t inherited=getflags(file);CHECK((inherited&(FS_NODUMP_FL|FS_IMMUTABLE_FL|FS_APPEND_FL))==FS_NODUMP_FL);CHECK(write(file,"x",1)==1 && !fsetxattr(file,"user.flags","ok",2,0));CHECK(!close(file));
    flags(fd,base|FS_APPEND_FL);file=open(b,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(file>=0 && !(getflags(file)&(FS_APPEND_FL|FS_IMMUTABLE_FL)));CHECK(!close(file));CHECK(!link(a,c));DENIED(unlink(a),EPERM);DENIED(rename(a,b),EPERM);CHECK(!access(a,F_OK) && !access(b,F_OK));
    flags(fd,base|FS_IMMUTABLE_FL);DENIED(open(c,O_CREAT|O_EXCL|O_WRONLY,0600),EEXIST);DENIED(unlink(a),EPERM);
    char fresh[210];snprintf(fresh,sizeof(fresh),"%s/new",dir);DENIED(open(fresh,O_CREAT|O_EXCL|O_WRONLY,0600),EPERM);DENIED(mkdir(fresh,0700),EPERM);DENIED(link(a,fresh),EPERM);DENIED(rename(a,fresh),EPERM);flags(fd,base);CHECK(!close(fd));CHECK(!unlink(a) && !unlink(b) && !unlink(c) && !rmdir(dir));
}
static void mapping(int fd,uint32_t base,int tmpfs)
{
    long ps=sysconf(_SC_PAGESIZE);CHECK(!ftruncate(fd,ps));char *shared=mmap(0,(size_t)ps,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0),*private=mmap(0,(size_t)ps,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0),*readonly=mmap(0,(size_t)ps,PROT_READ,MAP_SHARED,fd,0);CHECK(shared!=MAP_FAILED && private!=MAP_FAILED && readonly!=MAP_FAILED);shared[0]='S';private[0]='P';CHECK(!msync(shared,(size_t)ps,MS_SYNC));flags(fd,base|FS_IMMUTABLE_FL);private[0]='Q';CHECK(private[0]=='Q' && shared[0]=='S');CHECK(!mprotect(readonly,(size_t)ps,PROT_READ|PROT_WRITE));char *fresh=mmap(0,(size_t)ps,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(fresh!=MAP_FAILED);
    char *views[]={shared,readonly,fresh};for(int i=0;i<3;i++){pid_t p=fork();CHECK(p>=0);if(!p){views[i][0]=(char)('T'+i);_exit(0);}int status;CHECK(waitpid(p,&status,0)==p);CHECK(tmpfs?(WIFEXITED(status) && !WEXITSTATUS(status)):(WIFSIGNALED(status) && WTERMSIG(status)==SIGBUS));CHECK(shared[0]==(tmpfs?'T'+i:'S'));CHECK(private[0]=='Q');}flags(fd,base);CHECK(!munmap(shared,(size_t)ps) && !munmap(private,(size_t)ps) && !munmap(readonly,(size_t)ps) && !munmap(fresh,(size_t)ps));CHECK(!ftruncate(fd,4) && pwrite(fd,"BCDa",4,0)==4);
    flags(fd,base|FS_APPEND_FL);errno=0;CHECK(mmap(0,(size_t)ps,PROT_READ,MAP_SHARED,fd,0)==MAP_FAILED && errno==EACCES);flags(fd,base);
}
static void aliases(const char *dir,const char *path,int fd,uint32_t base)
{
    char hard[200],sym[200],bind[200],target[220];snprintf(hard,sizeof(hard),"%s/hard",dir);snprintf(sym,sizeof(sym),"%s/sym",dir);snprintf(bind,sizeof(bind),"%s/bind",dir);CHECK(!link(path,hard) && !symlink("data",sym));int other=open(hard,O_RDONLY),follow=open(sym,O_RDONLY);CHECK(other>=0 && follow>=0);flags(fd,base|FS_NODUMP_FL);CHECK(getflags(other)==(base|FS_NODUMP_FL) && getflags(follow)==(base|FS_NODUMP_FL));CHECK(!mkdir(bind,0700) && !mount(dir,bind,0,MS_BIND,0));snprintf(target,sizeof(target),"%s/data",bind);int bound=open(target,O_RDONLY);CHECK(bound>=0);CHECK(getflags(bound)==(base|FS_NODUMP_FL));flags(bound,base);CHECK(getflags(other)==base);CHECK(!mount(0,bind,0,MS_REMOUNT|MS_BIND|MS_RDONLY,0));int ro=open(target,O_RDONLY);CHECK(ro>=0);uint32_t wanted=base|FS_NODUMP_FL;DENIED(ioctl(ro,FS_IOC_SETFLAGS,&wanted),EROFS);CHECK(getflags(fd)==base && getflags(ro)==base);CHECK(!close(ro) && !close(bound) && !umount(bind) && !rmdir(bind));
    int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!close(fd));int n;CHECK(write(ready[1],"r",1)==1);CHECK(read(go[0],&n,sizeof(n))==sizeof(n));CHECK(getflags(other)==(base|FS_NODUMP_FL));char b[4];CHECK(pread(other,b,4,0)==4 && !memcmp(b,"seed",4));_exit(0);}char r;CHECK(read(ready[0],&r,1)==1);flags(fd,base|FS_NODUMP_FL);int n=1;CHECK(write(go[1],&n,sizeof(n))==sizeof(n));joined(p);flags(fd,base);CHECK(!close(ready[0]) && !close(ready[1]) && !close(go[0]) && !close(go[1]));CHECK(!close(other) && !close(follow) && !unlink(hard) && !unlink(sym));
}
static void onefs(const char *dir,int tmpfs)
{
    char path[180],alias[180],renamed[180];snprintf(path,sizeof(path),"%s/data",dir);snprintf(alias,sizeof(alias),"%s/link",dir);snprintf(renamed,sizeof(renamed),"%s/renamed",dir);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);CHECK(write(fd,"seed",4)==4);uint32_t base=getflags(fd);CHECK(!(base&(FS_IMMUTABLE_FL|FS_APPEND_FL|FS_NODUMP_FL)));
    long ps=sysconf(_SC_PAGESIZE);char *guard=mmap(0,(size_t)ps*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+ps,(size_t)ps,PROT_NONE));uint32_t *word=(uint32_t *)(guard+ps-4);
    CHECK(!ioctl(fd,FS_IOC_GETFLAGS,word) && *word==base);*word=base|FS_NODUMP_FL;CHECK(!ioctl(fd,FS_IOC_SETFLAGS,word));CHECK(getflags(fd)==*word);flags(fd,base);CHECK(!munmap(guard,(size_t)ps*2));
    aliases(dir,path,fd,base);ownership(fd,base);uint32_t unsupported=base|0x80000000U;DENIED(ioctl(fd,FS_IOC_SETFLAGS,&unsupported),EOPNOTSUPP);CHECK(getflags(fd)==base);
    uint32_t bad=0;DENIED(ioctl(-1,FS_IOC_GETFLAGS,&bad),EBADF);DENIED(ioctl(-1,FS_IOC_SETFLAGS,&bad),EBADF);
    DENIED(ioctl(fd,FS_IOC_GETFLAGS,(void *)1),EFAULT);DENIED(ioctl(fd,FS_IOC_SETFLAGS,(void *)1),EFAULT);CHECK(getflags(fd)==base);
    int ro=open(path,O_RDONLY);CHECK(ro>=0);flags(ro,base|FS_NODUMP_FL);CHECK(getflags(fd)==(base|FS_NODUMP_FL));CHECK(pwrite(fd,"data",4,0)==4);CHECK(!fsetxattr(fd,"user.flags","ok",2,0));content(fd,"data");statattrs(path,base|FS_NODUMP_FL);flags(ro,base);CHECK(!close(ro));
    struct timespec denied_times[2]={{5,0},{6,0}};struct stat before;CHECK(!fstat(fd,&before));
    int opath=open(path,O_PATH);CHECK(opath>=0);uint32_t v=0;DENIED(ioctl(opath,FS_IOC_GETFLAGS,&v),EBADF);DENIED(ioctl(opath,FS_IOC_SETFLAGS,&v),EBADF);CHECK(!close(opath));
    flags(fd,base|FS_APPEND_FL);statattrs(path,base|FS_APPEND_FL);DENIED(open(path,O_WRONLY),EPERM);DENIED(ftruncate(fd,0),EPERM);DENIED(truncate(path,0),EPERM);DENIED(fchmod(fd,0644),EPERM);DENIED(fchown(fd,0,0),EPERM);DENIED(futimens(fd,denied_times),EPERM);DENIED(utimensat(AT_FDCWD,path,denied_times,0),EPERM);DENIED(fsetxattr(fd,"user.flags","bad",3,0),EPERM);DENIED(link(path,alias),EPERM);DENIED(unlink(path),EPERM);DENIED(rename(path,renamed),EPERM);struct stat after;CHECK(!fstat(fd,&after) && before.st_mode==after.st_mode && before.st_uid==after.st_uid && before.st_gid==after.st_gid && before.st_size==after.st_size);content(fd,"data");
    /* Linux does not retrofit O_APPEND onto an already open description. */
    CHECK(lseek(fd,0,SEEK_SET)==0 && write(fd,"A",1)==1);CHECK(pwrite(fd,"B",1,0)==1);struct iovec iov[2]={{.iov_base="C",.iov_len=1},{.iov_base="D",.iov_len=1}};CHECK(writev(fd,iov,2)==2);content(fd,"BCDa");
    int append=open(path,O_WRONLY|O_APPEND);CHECK(append>=0);struct iovec tail={.iov_base="E",.iov_len=1};CHECK(syscall(SYS_pwritev2,fd,&tail,1,0,0,0x10)==1);content(fd,"BCDaE");DENIED(syscall(SYS_pwritev2,append,&tail,1,0,0,0x20),EPERM);CHECK(!close(append));flags(fd,base);CHECK(!ftruncate(fd,4));mapping(fd,base,tmpfs);flags(fd,base|FS_IMMUTABLE_FL);statattrs(path,base|FS_IMMUTABLE_FL);DENIED(open(path,O_WRONLY),EPERM);if(tmpfs){CHECK(!ftruncate(fd,4));}else{DENIED(ftruncate(fd,4),EPERM);}DENIED(truncate(path,4),EPERM);DENIED(fallocate(fd,0,0,4096),EPERM);DENIED(fsetxattr(fd,"user.flags","bad",3,0),EPERM);DENIED(fchmod(fd,0644),EPERM);DENIED(fchown(fd,0,0),EPERM);DENIED(futimens(fd,denied_times),EPERM);DENIED(utimensat(AT_FDCWD,path,denied_times,0),EPERM);DENIED(link(path,alias),EPERM);DENIED(unlink(path),EPERM);DENIED(rename(path,renamed),EPERM);
    /* Linux tmpfs write_iter lacks ext4's immutable check on an old writable FD. */
    if(tmpfs){CHECK(pwrite(fd,"Z",1,0)==1);content(fd,"ZCDa");}else{DENIED(pwrite(fd,"Z",1,0),EPERM);content(fd,"BCDa");}
    flags(fd,base);struct timespec times[2]={{1,0},{2,0}};CHECK(!futimens(fd,times));CHECK(!close(fd));CHECK(!unlink(path));directory(dir);printf("FILE_FLAGS %s file ok\n",tmpfs?"tmpfs":"ext4");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);char ext[128],mem[128];snprintf(ext,sizeof(ext),"/tmp/file-flags-ext-%d",getpid());snprintf(mem,sizeof(mem),"/tmp/file-flags-mem-%d",getpid());CHECK(!mkdir(ext,0700) && !mkdir(mem,0700));
    onefs(ext,0);CHECK(!mount("tmpfs",mem,"tmpfs",0,"mode=0700"));onefs(mem,1);CHECK(!umount(mem));CHECK(!rmdir(mem) && !rmdir(ext));puts("FILE_FLAGS PASS");return 0;
}
