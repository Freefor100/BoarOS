#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/sysmacros.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"fifo:%d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static void metadata(void)
{
    int p[2]; CHECK(pipe2(p,O_NONBLOCK)==0);
    struct stat a,b;
    CHECK(fstat(p[0],&a)==0 && S_ISFIFO(a.st_mode));
    CHECK(fchmod(p[0],0640)==0);
    CHECK(fstat(p[0],&a)==0 && fstat(p[1],&b)==0);
    CHECK((a.st_mode&07777)==0640 && a.st_mode==b.st_mode);
    CHECK(a.st_ino!=0 && a.st_ino==b.st_ino && a.st_dev==b.st_dev);
    CHECK(a.st_ctim.tv_sec>0 && a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec);
    int copy=dup(p[1]); CHECK(copy>=0 && fchmod(copy,S_IFREG|0600)==0);
    CHECK(fstat(p[0],&a)==0 && S_ISFIFO(a.st_mode) && (a.st_mode&07777)==0600);
    char path[64]; snprintf(path,sizeof(path),"/proc/self/fd/%d",p[0]);
    int reopened=open(path,O_RDONLY|O_NONBLOCK); CHECK(reopened>=0);
    CHECK(fstat(reopened,&b)==0 && b.st_ino==a.st_ino && b.st_mode==a.st_mode);
    CHECK(stat(path,&b)==0 && b.st_ino==a.st_ino && b.st_mode==a.st_mode);
    pid_t child=fork();CHECK(child>=0);
    if(!child) { CHECK(fchmod(p[1],0000)==0);_exit(0); }
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(fstat(copy,&b)==0 && (b.st_mode&07777)==0 && S_ISFIFO(b.st_mode));
    CHECK(write(p[1],"x",1)==1);char byte;CHECK(read(reopened,&byte,1)==1 && byte=='x');
    CHECK(syscall(SYS_fstat,p[0],(void *)1)==-1 && errno==EFAULT);
    CHECK(close(copy)==0 && close(reopened)==0 && close(p[0])==0 && close(p[1])==0);
    CHECK(fchmod(-1,0600)==-1 && errno==EBADF);
    puts("FIFO PASS metadata");
}
static void named(const char *dir)
{
    char path[128], alias[128];
    CHECK(mkdir(dir,0755)==0);
    snprintf(path,sizeof(path),"%s/fifo",dir);
    snprintf(alias,sizeof(alias),"%s/alias",dir);
    mode_t old=umask(0027); CHECK(mkfifo(path,0666)==0); umask(old);
    struct stat st; CHECK(stat(path,&st)==0 && S_ISFIFO(st.st_mode) && (st.st_mode&0777)==0640 && st.st_size==0);
    DIR *entries=opendir(dir);CHECK(entries!=NULL);struct dirent *entry;int found=0;
    while((entry=readdir(entries))) if(!strcmp(entry->d_name,"fifo")) {CHECK(entry->d_type==DT_FIFO);found=1;}
    CHECK(closedir(entries)==0 && found);
    CHECK(mkfifo(path,0600)==-1 && errno==EEXIST);
    CHECK(open(path,O_WRONLY|O_NONBLOCK)==-1 && errno==ENXIO);
    int r=open(path,O_RDONLY|O_NONBLOCK); CHECK(r>=0);
    char byte; CHECK(read(r,&byte,1)==0);
    struct pollfd p={r,POLLIN|POLLRDHUP,0}; CHECK(poll(&p,1,0)==0);
    int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN|EPOLLRDHUP,.data.fd=r};CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,r,&event)==0 && epoll_wait(ep,&event,1,0)==0);
    fd_set set;FD_ZERO(&set);FD_SET(r,&set);struct timeval zero={0};CHECK(select(r+1,&set,NULL,NULL,&zero)==0);
    struct timespec oldtimes[2]={{1,0},{1,0}};CHECK(utimensat(AT_FDCWD,path,oldtimes,0)==0);
    int w=open(path,O_WRONLY|O_NONBLOCK); CHECK(w>=0);
    CHECK(write(w,"abc",3)==3 && close(w)==0);
    CHECK(poll(&p,1,0)==1 && (p.revents&(POLLIN|POLLHUP))==(POLLIN|POLLHUP));
    CHECK(epoll_wait(ep,&event,1,0)==1 && (event.events&EPOLLHUP) && close(ep)==0);
    char data[8]; CHECK(read(r,data,sizeof(data))==3 && !memcmp(data,"abc",3));
    CHECK(fstat(r,&st)==0 && st.st_atime>1 && st.st_mtime>1);
    CHECK(read(r,data,1)==0 && close(r)==0);
    int both=open(path,O_RDWR|O_NONBLOCK); CHECK(both>=0);
    CHECK(link(path,alias)==0); r=open(alias,O_RDONLY|O_NONBLOCK); CHECK(r>=0);
    CHECK(write(both,"x",1)==1 && read(r,data,1)==1 && data[0]=='x');
    CHECK(unlink(path)==0 && mkfifo(path,0600)==0);
    int fresh=open(path,O_RDWR|O_NONBLOCK); CHECK(fresh>=0);
    CHECK(write(both,"y",1)==1 && read(fresh,data,1)==-1 && errno==EAGAIN);
    CHECK(read(r,data,1)==1 && data[0]=='y');
    CHECK(write(both,"discard",7)==7 && close(r)==0 && close(both)==0);
    both=open(alias,O_RDWR|O_NONBLOCK); CHECK(both>=0 && read(both,data,1)==-1 && errno==EAGAIN);
    CHECK(close(both)==0 && close(fresh)==0);
    CHECK(unlink(alias)==0 && unlink(path)==0 && rmdir(dir)==0);
    printf("FIFO PASS named %s\n",dir);
}
static void wait_blocked(pid_t child)
{
    char path[64], text[512]; snprintf(path,sizeof(path),"/proc/%d/stat",child);
    for(unsigned i=0;i<10000;i++) {
        int fd=open(path,O_RDONLY); CHECK(fd>=0); ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0);close(fd);text[n]=0;
        char *end=strrchr(text,')');CHECK(end!=NULL);
        if(end[2]=='S') return;
        usleep(1000);
    }
    CHECK(0);
}
static void handler(int sig) { (void)sig; }
static void handshake(void)
{
    const char *path="/meeting";CHECK(mkfifo(path,0600)==0);
    for(int write_end=0;write_end<2;write_end++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) { int fd=open(path,write_end?O_WRONLY:O_RDONLY);CHECK(fd>=0);CHECK(close(fd)==0);_exit(0); }
        wait_blocked(child);
        int fd=open(path,(write_end?O_RDONLY:O_WRONLY)|O_NONBLOCK);CHECK(fd>=0);CHECK(close(fd)==0);
        int status;CHECK(waitpid(child,&status,0)==child && status==0);
    }
    pid_t kids[2];
    for(int i=0;i<2;i++) { kids[i]=fork();CHECK(kids[i]>=0);if(!kids[i]) { int fd=open(path,O_RDONLY);CHECK(fd>=0);close(fd);_exit(0); } wait_blocked(kids[i]); }
    int fd=open(path,O_WRONLY|O_NONBLOCK);CHECK(fd>=0);close(fd);
    for(int i=0;i<2;i++) {int status;CHECK(waitpid(kids[i],&status,0)==kids[i] && status==0);}
    for(int restart=0;restart<3;restart++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            struct sigaction act={.sa_handler=handler,.sa_flags=restart==1?SA_RESTART:0};sigemptyset(&act.sa_mask);CHECK(sigaction(SIGUSR1,&act,NULL)==0);
            int f=open(path,O_RDONLY);
            if(restart==0) CHECK(f==-1 && errno==EINTR); else CHECK(f>=0 && close(f)==0);
            _exit(0);
        }
        wait_blocked(child);CHECK(kill(child,restart==2?SIGKILL:SIGUSR1)==0);
        if(restart==1) {usleep(10000);wait_blocked(child);fd=open(path,O_WRONLY|O_NONBLOCK);CHECK(fd>=0);close(fd);}
        int status;CHECK(waitpid(child,&status,0)==child);
        CHECK(restart==2 ? WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL : status==0);
        CHECK(open(path,O_WRONLY|O_NONBLOCK)==-1 && errno==ENXIO);
    }
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        struct rlimit limit={16,16};CHECK(setrlimit(RLIMIT_NOFILE,&limit)==0);
        int fds[16],count=0,f;
        while((f=open("/dev/null",O_RDONLY))>=0) fds[count++]=f;
        CHECK(errno==EMFILE && open(path,O_RDWR)==-1 && errno==EMFILE);
        while(count) close(fds[--count]);
        _exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(open(path,O_WRONLY|O_NONBLOCK)==-1 && errno==ENXIO);
    CHECK(unlink(path)==0);puts("FIFO PASS handshake and cancellation");
}
static void readonly(void)
{
    puts("FIFO readonly discovery");unsigned major=252,minor=16;FILE *devices=fopen("/proc/devices","r");
    if(devices) {minor=0;char line[128],name[64];unsigned number;int found=0;while(fgets(line,sizeof(line),devices))if(sscanf(line,"%u %63s",&number,name)==2&&!strcmp(name,"virtblk")){major=number;found=1;}CHECK(fclose(devices)==0 && found);}
    printf("FIFO readonly major=%u\n",major);CHECK(mknod("/disk-readonly",S_IFBLK|0600,makedev(major,minor))==0);
    CHECK(mkdir("/readonly",0755)==0 && mount("/disk-readonly","/readonly","ext4",MS_RDONLY,NULL)==0);
    CHECK(mkfifo("/readonly/new",0600)==-1&&errno==EROFS);
    int both=open("/readonly/fifo",O_RDWR|O_NONBLOCK);CHECK(both>=0);struct stat before,after;CHECK(fstat(both,&before)==0);
    CHECK(write(both,"x",1)==1);char c;CHECK(read(both,&c,1)==1&&c=='x');CHECK(fstat(both,&after)==0 && before.st_mtime==after.st_mtime && before.st_atime==after.st_atime);
    CHECK(fchmod(both,0644)==-1&&errno==EROFS);CHECK(close(both)==0&&umount("/readonly")==0);
    puts("FIFO PASS readonly");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(60);
    CHECK(mkdir("/proc",0755)==0 || errno==EEXIST);
    CHECK(mount("proc","/proc","proc",0,NULL)==0);
    if(access("/persist-fifo",F_OK)==0) {
        struct stat st;CHECK(stat("/persist-fifo",&st)==0&&S_ISFIFO(st.st_mode)&&(st.st_mode&0777)==0640);
        int fd=open("/persist-fifo",O_RDWR|O_NONBLOCK);CHECK(fd>=0);char byte;CHECK(read(fd,&byte,1)==-1&&errno==EAGAIN);CHECK(close(fd)==0);puts("FIFO PASS reboot");puts("FIFO PASS all");return 0;
    }
    readonly();metadata();handshake();named("/fifo-ext4");
    CHECK(mkdir("/ram",0755)==0 && mount("tmpfs","/ram","tmpfs",0,NULL)==0);
    named("/ram/fifo");CHECK(umount("/ram")==0);
    CHECK(mkfifo("/persist-fifo",0640)==0);int fd=open("/persist-fifo",O_RDWR|O_NONBLOCK);CHECK(fd>=0&&write(fd,"volatile",8)==8&&close(fd)==0);sync();puts("FIFO PASS all");return 0;
}
