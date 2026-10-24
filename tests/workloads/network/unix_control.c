#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"UNIX-CONTROL FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
/* The SEQPACKET consumer runs the same ancillary ABI with its record type. */
#ifndef UNIX_CONTROL_MESSAGE_TYPE
#define UNIX_CONTROL_MESSAGE_TYPE SOCK_DGRAM
#define UNIX_CONTROL_FIRST_TYPE SOCK_STREAM
#define UNIX_CONTROL_LAST_TYPE SOCK_DGRAM
#define UNIX_CONTROL_LISTENER_TYPE SOCK_STREAM
#endif
#define MAX_RIGHTS 253
#define CONTROL_SIZE (CMSG_SPACE(MAX_RIGHTS*sizeof(int))+CMSG_SPACE(sizeof(struct ucred)))
union control { struct cmsghdr alignment; unsigned char bytes[CONTROL_SIZE]; };
struct received { unsigned char data[256]; int fds[MAX_RIGHTS]; unsigned nfds,ncreds,cred_bytes; struct ucred cred; int flags; size_t control_len; ssize_t size; };
static unsigned cpu_ids[4],cpus,case_count;
static char workdir[80];

static void passed(const char *name) { ++case_count;printf("UNIX-CONTROL CASE %s PASS\n",name); }
static void pair_at(int type,int p[2]) { if(type==SOCK_DGRAM)type=UNIX_CONTROL_MESSAGE_TYPE;CHECK(!socketpair(AF_UNIX,type,0,p)); }
static void close_pair(int p[2]) { CHECK(!close(p[0]) && !close(p[1])); }
static void wait_child(pid_t pid) { int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status)); }
static void barrier(pthread_barrier_t *b) { int r=pthread_barrier_wait(b);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static uint64_t clock_ns(clockid_t clock) { struct timespec t;CHECK(!clock_gettime(clock,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static void pin(unsigned index)
{
    cpu_set_t mask,actual;CPU_ZERO(&mask);CPU_SET(cpu_ids[index%cpus],&mask);
    CHECK(!sched_setaffinity(0,sizeof(mask),&mask));CPU_ZERO(&actual);
    CHECK(!sched_getaffinity(0,sizeof(actual),&actual) && !memcmp(&mask,&actual,sizeof(mask)));
}
static void affinity(void)
{
    cpu_set_t mask;CPU_ZERO(&mask);CHECK(!sched_getaffinity(0,sizeof(mask),&mask));
    for(unsigned i=0;i<CPU_SETSIZE && cpus<4;i++)if(CPU_ISSET(i,&mask))cpu_ids[cpus++]=i;
    CHECK(cpus);pin(0);
}
static unsigned fd_count(void)
{
    DIR *directory=opendir("/proc/self/fd");CHECK(directory);unsigned count=0;struct dirent *entry;
    while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    CHECK(!closedir(directory));return count-1;
}
static int data_file(void)
{
    char name[]="data-XXXXXX";int fd=mkstemp(name);CHECK(fd>=0 && !unlink(name));
    CHECK(write(fd,"0123456789abcdef",16)==16 && lseek(fd,0,SEEK_SET)==0);return fd;
}
static size_t add_rights(union control *control,size_t offset,const int *fds,unsigned count)
{
    struct cmsghdr *c=(void *)(control->bytes+offset);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
    memcpy(CMSG_DATA(c),fds,count*sizeof(int));return offset+CMSG_SPACE(count*sizeof(int));
}
static size_t add_cred(union control *control,size_t offset,const struct ucred *cred)
{
    struct cmsghdr *c=(void *)(control->bytes+offset);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_CREDENTIALS;c->cmsg_len=CMSG_LEN(sizeof(*cred));
    memcpy(CMSG_DATA(c),cred,sizeof(*cred));return offset+CMSG_SPACE(sizeof(*cred));
}
static ssize_t send_control(int fd,const void *data,size_t length,const int *fds,unsigned count,const struct ucred *cred,int flags)
{
    union control control={0};size_t used=0;if(count)used=add_rights(&control,used,fds,count);if(cred)used=add_cred(&control,used,cred);
    struct iovec iov={(void *)data,length};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=used?control.bytes:0,.msg_controllen=used};
    return sendmsg(fd,&m,flags|MSG_NOSIGNAL);
}
static struct received receive_control(int fd,size_t length,size_t capacity,int flags)
{
    struct received result={0};union control control;memset(&control,0xa5,sizeof(control));CHECK(length<=sizeof(result.data) && capacity<=sizeof(control));
    struct iovec iov={result.data,length};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=capacity?control.bytes:0,.msg_controllen=capacity};
    result.size=recvmsg(fd,&m,flags);result.flags=m.msg_flags;result.control_len=m.msg_controllen;
    if(result.size<0)return result;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)) {
        CHECK(c->cmsg_level==SOL_SOCKET && c->cmsg_len>=CMSG_LEN(0));
        if(c->cmsg_type==SCM_RIGHTS) { size_t bytes=c->cmsg_len-CMSG_LEN(0);CHECK(!(bytes%sizeof(int)) && result.nfds+bytes/sizeof(int)<=MAX_RIGHTS);memcpy(result.fds+result.nfds,CMSG_DATA(c),bytes);result.nfds+=(unsigned)(bytes/sizeof(int)); }
        else { CHECK(c->cmsg_type==SCM_CREDENTIALS && c->cmsg_len<=CMSG_LEN(sizeof(struct ucred)));result.cred_bytes=(unsigned)(c->cmsg_len-CMSG_LEN(0));memcpy(&result.cred,CMSG_DATA(c),result.cred_bytes);++result.ncreds; }
    }
    return result;
}
static void release_received(struct received *r) { for(unsigned i=0;i<r->nfds;i++)CHECK(!close(r->fds[i]));r->nfds=0; }
static void cred_is(const struct received *r,pid_t pid,uid_t uid,gid_t gid) { CHECK(r->ncreds==1 && r->cred_bytes==sizeof(struct ucred) && r->cred.pid==pid && r->cred.uid==uid && r->cred.gid==gid); }
static void passcred(int fd,int enabled) { CHECK(!setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled))); }
static void peer_is(int fd,pid_t pid,uid_t uid,gid_t gid)
{ struct ucred c;socklen_t n=sizeof(c);CHECK(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&c,&n) && n==sizeof(c) && c.pid==pid && c.uid==uid && c.gid==gid); }
static void empty_queue(int fd) { char c;CHECK(recv(fd,&c,1,MSG_DONTWAIT)==-1 && errno==EAGAIN); }

static void rights_objects(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2],pipefd[2],other[2];pair_at(type,p);CHECK(!pipe(pipefd));pair_at(SOCK_STREAM,other);
        int file=data_file(),event=eventfd(7,EFD_CLOEXEC),path=open(".",O_PATH|O_DIRECTORY|O_CLOEXEC);CHECK(event>=0 && path>=0);
        int fds[]={file,pipefd[0],other[0],event,path};CHECK(!fcntl(file,F_SETFD,FD_CLOEXEC));
        CHECK(send_control(p[0],"K",1,fds,5,0,0)==1);struct received r=receive_control(p[1],1,CONTROL_SIZE,0);
        CHECK(r.size==1 && r.data[0]=='K' && r.nfds==5 && !r.ncreds && !(r.flags&MSG_CTRUNC));
        for(unsigned i=0;i<5;i++)CHECK(!(fcntl(r.fds[i],F_GETFD)&FD_CLOEXEC));
        char c;CHECK(read(r.fds[0],&c,1)==1 && c=='0' && lseek(file,0,SEEK_CUR)==1);
        CHECK(!fcntl(r.fds[0],F_SETFL,fcntl(r.fds[0],F_GETFL)|O_NONBLOCK) && (fcntl(file,F_GETFL)&O_NONBLOCK));
        CHECK(write(pipefd[1],"P",1)==1 && read(r.fds[1],&c,1)==1 && c=='P');
        CHECK(send(other[1],"S",1,0)==1 && recv(r.fds[2],&c,1,0)==1 && c=='S');
        uint64_t value;CHECK(read(r.fds[3],&value,sizeof(value))==sizeof(value) && value==7);
        struct stat a,b;CHECK(!fstat(path,&a) && !fstat(r.fds[4],&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino && S_ISDIR(b.st_mode));
        CHECK(read(r.fds[4],&c,1)==-1 && errno==EBADF && (fcntl(r.fds[4],F_GETFL)&O_PATH));release_received(&r);
        CHECK(send_control(p[0],"C",1,fds,5,0,0)==1);r=receive_control(p[1],1,CONTROL_SIZE,MSG_CMSG_CLOEXEC);CHECK(r.size==1 && r.nfds==5);
        for(unsigned i=0;i<5;i++)CHECK(fcntl(r.fds[i],F_GETFD)&FD_CLOEXEC);
        release_received(&r);
        CHECK(!close(file) && !close(event) && !close(path));close_pair(pipefd);close_pair(other);close_pair(p);
    }
    passed("rights-file-pipe-socket-eventfd-opath-shared-state-cloexec");
}
static void source_lifetime(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);int source=data_file(),number=source;
        CHECK(send_control(p[0],"L",1,&source,1,0,0)==1 && !close(source));int replacement=open("/dev/null",O_RDONLY);CHECK(replacement==number);
        struct received r=receive_control(p[1],1,CONTROL_SIZE,0);char c;CHECK(r.size==1 && r.nfds==1 && read(r.fds[0],&c,1)==1 && c=='0');
        release_received(&r);CHECK(!close(replacement));close_pair(p);
    }
    passed("send-retains-file-after-close-and-fd-reuse");
}
static void control_capacity(void)
{
    const size_t capacities[]={0,sizeof(struct cmsghdr)-1,CMSG_LEN(0),CMSG_LEN(sizeof(int)),CMSG_SPACE(sizeof(int)),CMSG_SPACE(3*sizeof(int))};
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++)for(unsigned j=0;j<sizeof(capacities)/sizeof(*capacities);j++) {
        int p[2];pair_at(type,p);int file=data_file(),fds[]={file,file,file};CHECK(send_control(p[0],"T",1,fds,3,0,0)==1);
        struct received r=receive_control(p[1],1,capacities[j],0);unsigned expected=capacities[j]<CMSG_LEN(0)?0:(unsigned)((capacities[j]-CMSG_LEN(0))/sizeof(int));if(expected>3)expected=3;
        CHECK(r.size==1 && r.data[0]=='T' && r.nfds==expected && !!(r.flags&MSG_CTRUNC)==(expected<3));
        release_received(&r);CHECK(!close(file));close_pair(p);
    }
    int p[2];pair_at(SOCK_DGRAM,p);int file=data_file();union control c={0};size_t used=add_rights(&c,0,&file,1);used=add_rights(&c,used,&file,1);
    struct ucred cred={getpid(),getuid(),getgid()};used=add_cred(&c,used,&cred);passcred(p[1],1);
    char byte='M';struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=c.bytes,.msg_controllen=used};CHECK(sendmsg(p[0],&m,0)==1);
    struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.nfds==2 && !(r.flags&MSG_CTRUNC));cred_is(&r,getpid(),getuid(),getgid());release_received(&r);
    CHECK(!close(file));close_pair(p);passed("control-none-small-multiple-records-truncation");
}
static void rights_limits(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);int file=data_file(),fds[254];for(unsigned i=0;i<254;i++)fds[i]=file;
        int invalid=-1;CHECK(send_control(p[0],"B",1,&invalid,1,0,0)==-1 && errno==EBADF);empty_queue(p[1]);
        CHECK(send_control(p[0],"B",1,fds,254,0,0)==-1 && errno==EINVAL);empty_queue(p[1]);
        CHECK(send_control(p[0],"G",1,fds,253,0,0)==1);struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.nfds==253 && !(r.flags&MSG_CTRUNC));
        char c;for(unsigned i=0;i<r.nfds;i++)CHECK(pread(r.fds[i],&c,1,i%16)==1 && c=="0123456789abcdef"[i%16]);release_received(&r);CHECK(!close(file));close_pair(p);
    }
    passed("bad-fd-and-253-254-rights-limit");
}
static void credentials_capacity(void)
{
    const size_t capacities[]={0,sizeof(struct cmsghdr)-1,CMSG_LEN(0),CMSG_LEN(sizeof(pid_t)),CMSG_LEN(2*sizeof(int)),CMSG_LEN(sizeof(struct ucred)),CMSG_SPACE(sizeof(struct ucred)),CMSG_SPACE(sizeof(struct ucred))+CMSG_LEN(sizeof(int)),CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(3*sizeof(int))};
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++)for(unsigned j=0;j<sizeof(capacities)/sizeof(*capacities);j++) {
        int p[2];pair_at(type,p);passcred(p[1],1);int file=data_file(),fds[]={file,file,file};CHECK(send_control(p[0],"C",1,fds,3,0,0)==1);
        size_t capacity=capacities[j];struct received r=receive_control(p[1],1,capacity,0);CHECK(r.size==1 && r.data[0]=='C');
        if(capacity<CMSG_LEN(0))CHECK(!r.ncreds);
        else {
            unsigned bytes=(unsigned)(capacity-CMSG_LEN(0));if(bytes>sizeof(struct ucred))bytes=sizeof(struct ucred);
            CHECK(r.ncreds==1 && r.cred_bytes==bytes);struct ucred expected={getpid(),getuid(),getgid()};CHECK(!memcmp(&r.cred,&expected,bytes));
        }
        unsigned rights=0;if(capacity>=CMSG_SPACE(sizeof(struct ucred))+CMSG_LEN(0))rights=(unsigned)((capacity-CMSG_SPACE(sizeof(struct ucred))-CMSG_LEN(0))/sizeof(int));if(rights>3)rights=3;
        CHECK(r.nfds==rights && !!(r.flags&MSG_CTRUNC)==(rights<3));release_received(&r);CHECK(!close(file));close_pair(p);
    }
    passed("credentials-partial-control-and-rights-truncation");
}
static void table_full_child(unsigned type,unsigned slots)
{
    int p[2];pair_at(type,p);int file=data_file(),fds[]={file,file,file};CHECK(send_control(p[0],"F",1,fds,3,0,0)==1);
    struct rlimit limit;CHECK(!getrlimit(RLIMIT_NOFILE,&limit));limit.rlim_cur=64;CHECK(!setrlimit(RLIMIT_NOFILE,&limit));
    int fill[64];unsigned count=0;for(;;) { int fd=dup(file);if(fd<0) { CHECK(errno==EMFILE);break; }CHECK(count<64);fill[count++]=fd; }
    CHECK(count>slots);for(unsigned i=0;i<slots;i++)CHECK(!close(fill[--count]));
    struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='F' && r.nfds==slots && (r.flags&MSG_CTRUNC));release_received(&r);
    while(count)CHECK(!close(fill[--count]));
    CHECK(!close(file));close_pair(p);_exit(0);
}
static void full_table(void)
{ for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++)for(unsigned slots=0;slots<=1;slots++) { pid_t child=fork();CHECK(child>=0);if(!child)table_full_child(type,slots);wait_child(child); }passed("receive-full-fd-table-payload-and-partial-rights"); }
static void token_write(int fd,char token) { CHECK(write(fd,&token,1)==1); }
static void token_read(int fd,char expected) { char token;CHECK(read(fd,&token,1)==1 && token==expected); }
static void clear_capabilities(void)
{
    struct { uint32_t version;int32_t pid; } header={0x20080522,0};
    struct { uint32_t effective,permitted,inheritable; } data[2]={{0}};
    CHECK(!syscall(SYS_capset,&header,data));memset(data,0xa5,sizeof(data));CHECK(!syscall(SYS_capget,&header,data));
    for(unsigned i=0;i<2;i++)CHECK(!data[i].effective && !data[i].permitted && !data[i].inheritable);
}
static void quota_receive(int fd,char expected)
{
    struct received r=receive_control(fd,1,CONTROL_SIZE,0);char byte;
    CHECK(r.size==1 && r.data[0]==(unsigned char)expected && r.nfds==1 && !(r.flags&MSG_CTRUNC) &&
        pread(r.fds[0],&byte,1,0)==1 && byte=='0');release_received(&r);
}
static void quota_rejected(int fd,int file)
{ CHECK(send_control(fd,"X",1,&file,1,0,0)==-1 && errno==ETOOMANYREFS); }
static void inflight_quota(void)
{
    int p[2],command[2],events[2];pair_at(SOCK_DGRAM,p);int file=data_file();CHECK(!pipe(command) && !pipe(events));int numeric_ids=!geteuid();
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(p[1]) && !close(command[1]) && !close(events[0]));
        /* Keep all seven live descriptors below the new soft NOFILE limit. */
        CHECK(dup2(events[1],p[1])==p[1] && !close(events[1]));int event=p[1];
        if(numeric_ids)CHECK(!setresgid(65001,65001,65001) && !setresuid(65001,65002,65002));
        clear_capabilities();struct rlimit limit;CHECK(!getrlimit(RLIMIT_NOFILE,&limit));limit.rlim_cur=8;CHECK(!setrlimit(RLIMIT_NOFILE,&limit));
        for(unsigned i=0;i<9;i++)CHECK(send_control(p[0],"A",1,&file,1,0,0)==1);
        quota_rejected(p[0],file);token_write(event,'F');token_read(command[0],'1');
        CHECK(send_control(p[0],"R",1,&file,1,0,0)==1);quota_rejected(p[0],file);
        /* Both ids are already real/effective/saved candidates. Changing the
         * real uid needs no capability and selects a different UID account. */
        if(numeric_ids) {
            CHECK(!setresuid(65002,(uid_t)-1,(uid_t)-1) && getuid()==65002);clear_capabilities();
            CHECK(send_control(p[0],"B",1,&file,1,0,0)==1);
        }
        token_write(event,'B');token_read(command[0],'D');
        for(unsigned i=numeric_ids?1:0;i<9;i++)CHECK(send_control(p[0],"B",1,&file,1,0,0)==1);
        quota_rejected(p[0],file);token_write(event,'Q');token_read(command[0],'D');
        CHECK(send_control(p[0],"Z",1,&file,1,0,0)==1);token_write(event,'Z');
        CHECK(!close(p[0]) && !close(file) && !close(command[0]) && !close(event));_exit(0);
    }
    CHECK(!close(p[0]) && !close(file) && !close(command[0]) && !close(events[1]));
    token_read(events[0],'F');quota_receive(p[1],'A');token_write(command[1],'1');token_read(events[0],'B');
    for(unsigned i=0;i<8;i++)quota_receive(p[1],'A');
    quota_receive(p[1],'R');
    token_write(command[1],'D');token_read(events[0],'Q');for(unsigned i=0;i<9;i++)quota_receive(p[1],'B');
    token_write(command[1],'D');token_read(events[0],'Z');quota_receive(p[1],'Z');wait_child(child);
    CHECK(!close(p[1]) && !close(command[1]) && !close(events[0]));
    printf("UNIX-CONTROL ACTION inflight-quota-numeric-uid-rotation executed=%d\n",numeric_ids);
    passed("inflight-nofile-eight-ninth-allowed-tenth-rejected-drain-uid-account");
}
struct blocked_control_sender { int fd,right;uint64_t tid; };
static void *blocked_control_send(void *argument)
{
    struct blocked_control_sender *sender=argument;pin(1);
    __atomic_store_n(&sender->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
    (void)send_control(sender->fd,"W",1,&sender->right,1,0,0);CHECK(0);return NULL;
}
static void wait_control_sender_sleep(struct blocked_control_sender *sender)
{
    uint64_t deadline=clock_ns(CLOCK_MONOTONIC)+5000000000ULL;
    do {
        uint64_t tid=__atomic_load_n(&sender->tid,__ATOMIC_ACQUIRE);
        if(tid) {
            char filename[96],text[512];snprintf(filename,sizeof(filename),"/proc/self/task/%llu/stat",(unsigned long long)tid);
            int fd=open(filename,O_RDONLY);CHECK(fd>=0);ssize_t size=read(fd,text,sizeof(text)-1);CHECK(size>0 && !close(fd));text[size]=0;
            char *end=strrchr(text,')');CHECK(end);if(end[2]=='S')return;
        }
        sched_yield();
    }while(clock_ns(CLOCK_MONOTONIC)<deadline);
    CHECK(0);
}
static void exit_group_control_owner(void)
{
    int pipefd[2],command[2],events[2];CHECK(!pipe2(pipefd,O_NONBLOCK) && !pipe(command) && !pipe(events));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(pipefd[0]) && !close(command[1]) && !close(events[0]));
        int p[2];pair_at(SOCK_DGRAM,p);int size=4096;CHECK(!setsockopt(p[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)));
        /* Warm the libc control path and discard its received duplicate before
         * filling the queue; the blocked send is the only remaining pipe owner. */
        CHECK(send_control(p[0],"P",1,&pipefd[1],1,0,0)==1);struct received warm=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(warm.size==1 && warm.nfds==1);release_received(&warm);
        unsigned queued=0;while(send(p[0],"F",1,MSG_DONTWAIT)==1)CHECK(++queued<65536);CHECK(errno==EAGAIN && queued);
        struct blocked_control_sender sender={.fd=p[0],.right=pipefd[1]};pthread_t thread;CHECK(!pthread_create(&thread,0,blocked_control_send,&sender));
        wait_control_sender_sleep(&sender);CHECK(!close(pipefd[1]));token_write(events[1],'S');token_read(command[0],'X');
        syscall(SYS_exit_group,0);CHECK(0);
    }
    CHECK(!close(pipefd[1]) && !close(command[0]) && !close(events[1]));token_read(events[0],'S');
    char byte;CHECK(read(pipefd[0],&byte,1)==-1 && errno==EAGAIN);token_write(command[1],'X');wait_child(child);
    CHECK(read(pipefd[0],&byte,1)==0 && !close(pipefd[0]) && !close(command[1]) && !close(events[0]));
    puts("UNIX-CONTROL ACTION exit-group-pending-control sleeping_sender=1 original_pipe_writer_closed=1 eof_after_wait=1");
    passed("exit-group-blocked-sendmsg-releases-unpublished-rights");
}
static void peek_and_read(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);int file=data_file();CHECK(send_control(p[0],"P",1,&file,1,0,0)==1);
        struct received a=receive_control(p[1],1,CONTROL_SIZE,MSG_PEEK),b=receive_control(p[1],1,CONTROL_SIZE,MSG_PEEK),c=receive_control(p[1],1,CONTROL_SIZE,0);
        CHECK(a.size==1 && b.size==1 && c.size==1 && a.nfds==1 && b.nfds==1 && c.nfds==1 && a.fds[0]!=b.fds[0] && b.fds[0]!=c.fds[0] && a.fds[0]!=c.fds[0]);
        char byte;CHECK(read(a.fds[0],&byte,1)==1 && byte=='0' && read(b.fds[0],&byte,1)==1 && byte=='1' && read(c.fds[0],&byte,1)==1 && byte=='2');
        release_received(&a);release_received(&b);release_received(&c);CHECK(!close(file));
        int pipes[2];CHECK(!pipe2(pipes,O_NONBLOCK));CHECK(send_control(p[0],"D",1,&pipes[1],1,0,0)==1 && !close(pipes[1]));
        CHECK(read(p[1],&byte,1)==1 && byte=='D' && read(pipes[0],&byte,1)==0 && !close(pipes[0]));close_pair(p);
    }
    passed("peek-duplicates-and-plain-read-discards-rights");
}
static void stream_boundaries(void)
{
    int p[2];pair_at(SOCK_STREAM,p);int file=data_file();CHECK(send_control(p[0],"A",1,&file,1,0,0)==1 && send(p[0],"bc",2,0)==2 && send_control(p[0],"D",1,&file,1,0,0)==1 && send(p[0],"ef",2,0)==2);
    struct received a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==1 && a.data[0]=='A' && a.nfds==1);release_received(&a);
    a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==3 && !memcmp(a.data,"bcD",3) && a.nfds==1);release_received(&a);
    a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==2 && !memcmp(a.data,"ef",2) && !a.nfds);CHECK(!close(file));close_pair(p);
    pair_at(SOCK_STREAM,p);passcred(p[1],1);CHECK(send(p[0],"A",1,0)==1);pid_t child=fork();CHECK(child>=0);if(!child) { CHECK(send(p[0],"B",1,0)==1);_exit(0); }wait_child(child);CHECK(send(p[0],"C",1,0)==1);
    a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==1 && a.data[0]=='A');cred_is(&a,getpid(),getuid(),getgid());
    a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==1 && a.data[0]=='B');cred_is(&a,child,getuid(),getgid());
    a=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(a.size==1 && a.data[0]=='C');cred_is(&a,getpid(),getuid(),getgid());close_pair(p);
    passed("stream-rights-and-credential-boundaries");
}
static struct ucred control_cookie(const union control *control)
{
    struct cmsghdr header;struct ucred cred;memcpy(&header,control->bytes,sizeof(header));
    CHECK(header.cmsg_level==SOL_SOCKET && header.cmsg_type==SCM_CREDENTIALS && header.cmsg_len==CMSG_LEN(sizeof(cred)));
    memcpy(&cred,control->bytes+CMSG_LEN(0),sizeof(cred));return cred;
}
static void stream_control_boundaries(void)
{
    int p[2],file=data_file();char byte;
    pair_at(SOCK_STREAM,p);passcred(p[1],1);CHECK(send_control(p[0],"AB",2,&file,1,0,0)==2);
    struct received r=receive_control(p[1],0,CONTROL_SIZE,0);
    CHECK(r.size==0 && r.nfds==1 && !(r.flags&MSG_CTRUNC));cred_is(&r,getpid(),getuid(),getgid());
    CHECK(pread(r.fds[0],&byte,1,0)==1 && byte=='0');release_received(&r);
    r=receive_control(p[1],2,CONTROL_SIZE,0);CHECK(r.size==2 && !memcmp(r.data,"AB",2) && !r.nfds);cred_is(&r,getpid(),getuid(),getgid());close_pair(p);
    pair_at(SOCK_STREAM,p);passcred(p[1],1);CHECK(send_control(p[0],"AB",2,&file,1,0,0)==2);
    struct received a=receive_control(p[1],0,CONTROL_SIZE,MSG_PEEK),b=receive_control(p[1],0,CONTROL_SIZE,MSG_PEEK),c=receive_control(p[1],0,CONTROL_SIZE,0);
    CHECK(a.size==0 && b.size==0 && c.size==0 && a.nfds==1 && b.nfds==1 && c.nfds==1 && a.fds[0]!=b.fds[0] && a.fds[0]!=c.fds[0] && b.fds[0]!=c.fds[0]);
    cred_is(&a,getpid(),getuid(),getgid());cred_is(&b,getpid(),getuid(),getgid());cred_is(&c,getpid(),getuid(),getgid());
    CHECK(read(a.fds[0],&byte,1)==1 && byte=='0' && read(b.fds[0],&byte,1)==1 && byte=='1' && read(c.fds[0],&byte,1)==1 && byte=='2');
    release_received(&a);release_received(&b);release_received(&c);
    r=receive_control(p[1],2,CONTROL_SIZE,0);CHECK(r.size==2 && !memcmp(r.data,"AB",2) && !r.nfds);cred_is(&r,getpid(),getuid(),getgid());close_pair(p);
    pair_at(SOCK_STREAM,p);passcred(p[1],1);CHECK(send_control(p[0],"AB",2,&file,1,0,0)==2 && read(p[1],NULL,0)==0);
    r=receive_control(p[1],2,CONTROL_SIZE,0);CHECK(r.size==2 && !memcmp(r.data,"AB",2) && r.nfds==1);cred_is(&r,getpid(),getuid(),getgid());release_received(&r);close_pair(p);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    pair_at(SOCK_STREAM,p);passcred(p[1],1);CHECK(send_control(p[0],"F",1,&file,1,0,0)==1);
    union control control;memset(&control,0xa5,sizeof(control));struct iovec iov={bad,1};
    struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control),.msg_namelen=77,.msg_flags=1234},before=message;
    unsigned descriptors=fd_count();
    CHECK(syscall(SYS_recvmsg,p[1],&message,0)==-1 && errno==EFAULT && !memcmp(&message,&before,sizeof(message)) && fd_count()==descriptors);
    struct ucred cookie=control_cookie(&control);CHECK(cookie.pid==getpid() && cookie.uid==getuid() && cookie.gid==getgid());
    for(unsigned i=CMSG_SPACE(sizeof(cookie));i<sizeof(control);i++)CHECK(control.bytes[i]==0xa5);
    r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='F' && r.nfds==1);cred_is(&r,getpid(),getuid(),getgid());release_received(&r);close_pair(p);CHECK(!munmap(bad,page));
    pair_at(SOCK_STREAM,p);passcred(p[1],1);memset(&control,0xa5,sizeof(control));iov=(struct iovec){&byte,1};
    message=(struct msghdr){.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control),.msg_namelen=77,.msg_flags=1234};before=message;
    CHECK(syscall(SYS_recvmsg,p[1],&message,MSG_DONTWAIT)==-1 && errno==EAGAIN && !memcmp(&message,&before,sizeof(message)));
    cookie=control_cookie(&control);CHECK(!cookie.pid);
    /* The initial user namespace exports an all-zero cookie as 0/0. An
     * unmapped global root id becomes overflow 65534 in a nested host userns. */
    CHECK((!cookie.uid && !cookie.gid) || (geteuid()!=0 && cookie.uid==65534 && cookie.gid==65534));
    CHECK(!shutdown(p[0],SHUT_WR));r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==0 && !r.nfds);cred_is(&r,0,cookie.uid,cookie.gid);close_pair(p);
    printf("UNIX-CONTROL ACTION stream-null-cookie pid=0 uid=%u gid=%u\n",cookie.uid,cookie.gid);
    pair_at(SOCK_STREAM,p);CHECK(send(p[0],"O",1,0)==1);passcred(p[1],1);
    r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='O' && !r.nfds);cred_is(&r,0,65534,65534);close_pair(p);
    CHECK(!close(file));passed("stream-zero-peek-read-fault-null-cookie-old-packet");
}
static void empty_dgram(void)
{
    int p[2];pair_at(SOCK_DGRAM,p);passcred(p[1],1);int file=data_file();CHECK(send_control(p[0],"",0,&file,1,0,0)==0);
    struct received r=receive_control(p[1],256,CONTROL_SIZE,0);CHECK(r.size==0 && r.nfds==1);cred_is(&r,getpid(),getuid(),getgid());release_received(&r);empty_queue(p[1]);CHECK(!close(file));close_pair(p);
    passed("empty-datagram-delivers-rights-and-credentials");
}
static void credentials_identity(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);passcred(p[1],1);pid_t original=getpid();uid_t uid=getuid();gid_t gid=getgid();
        pid_t child=fork();CHECK(child>=0);if(!child) {
            int numeric_ids=!geteuid();
            if(numeric_ids)CHECK(!setresgid(61001,61002,61002) && !setresuid(62001,62002,62002));
            peer_is(p[0],original,uid,gid);CHECK(send_control(p[0],"I",1,0,0,0,0)==1);
            struct ucred cred={getpid(),getuid(),getgid()};CHECK(send_control(p[0],"R",1,0,0,&cred,0)==1);
            cred.uid=geteuid();cred.gid=getegid();CHECK(send_control(p[0],"E",1,0,0,&cred,0)==1);
            cred.pid=original;CHECK(send_control(p[0],"X",1,0,0,&cred,0)==-1 && errno==EPERM);
            /* An unmapped id is EINVAL, not a permission check. Root guests
             * drop to concrete distinct ids; unprivileged hosts check -1. */
            cred.pid=getpid();cred.uid=numeric_ids?63001:(uid_t)-1;CHECK(send_control(p[0],"X",1,0,0,&cred,0)==-1 && errno==(numeric_ids?EPERM:EINVAL));
            cred.uid=getuid();cred.gid=numeric_ids?63001:(gid_t)-1;CHECK(send_control(p[0],"X",1,0,0,&cred,0)==-1 && errno==(numeric_ids?EPERM:EINVAL));_exit(0);
        }
        uid_t real_uid=geteuid()?getuid():62001,effective_uid=geteuid()?geteuid():62002;gid_t real_gid=geteuid()?getgid():61001,effective_gid=geteuid()?getegid():61002;
        struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='I');cred_is(&r,child,real_uid,real_gid);peer_is(p[1],original,uid,gid);
        r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='R');cred_is(&r,child,real_uid,real_gid);
        r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='E');cred_is(&r,child,effective_uid,effective_gid);wait_child(child);empty_queue(p[1]);close_pair(p);
    }
    printf("UNIX-CONTROL ACTION credentials-distinct-real-effective-ids executed=%d\n",geteuid()==0);
    passed("credentials-real-ids-peer-snapshot-explicit-permissions");
}
static struct sockaddr_un address(const char *name,socklen_t *length)
{
    struct sockaddr_un a={.sun_family=AF_UNIX};size_t n=strlen(name);CHECK(n+1<sizeof(a.sun_path));memcpy(a.sun_path+1,name,n);*length=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+n);return a;
}
static void credentials_options(void)
{
    int p[2];pair_at(SOCK_DGRAM,p);int enabled=0;socklen_t n=sizeof(enabled);CHECK(!getsockopt(p[1],SOL_SOCKET,SO_PASSCRED,&enabled,&n) && !enabled && n==sizeof(enabled));
    passcred(p[1],1);n=sizeof(enabled);CHECK(!getsockopt(p[1],SOL_SOCKET,SO_PASSCRED,&enabled,&n) && enabled==1);passcred(p[1],0);
    CHECK(send(p[0],"N",1,0)==1);struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && !r.ncreds);passcred(p[1],1);
    CHECK(send(p[0],"Y",1,0)==1);r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1);cred_is(&r,getpid(),getuid(),getgid());close_pair(p);
    socklen_t length;struct sockaddr_un a=address("control-autobind",&length),from,local;int receiver=socket(AF_UNIX,SOCK_DGRAM,0),sender=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(receiver>=0 && sender>=0 && !bind(receiver,(void *)&a,length));
    passcred(sender,1);CHECK(sendto(sender,"U",1,0,(void *)&a,length)==1);n=sizeof(local);CHECK(!getsockname(sender,(void *)&local,&n) && n==offsetof(struct sockaddr_un,sun_path)+6 && !local.sun_path[0]);
    char byte;socklen_t m=sizeof(from);CHECK(recvfrom(receiver,&byte,1,0,(void *)&from,&m)==1 && byte=='U' && m==n && !memcmp(&from,&local,n));CHECK(!close(receiver) && !close(sender));
    a=address("control-listener",&length);int listener=socket(AF_UNIX,SOCK_STREAM,0);CHECK(listener>=0);passcred(listener,1);CHECK(!bind(listener,(void *)&a,length) && !listen(listener,1));int client=socket(AF_UNIX,SOCK_STREAM,0);CHECK(client>=0 && !connect(client,(void *)&a,length));int accepted=accept(listener,0,0);CHECK(accepted>=0);enabled=0;n=sizeof(enabled);CHECK(!getsockopt(accepted,SOL_SOCKET,SO_PASSCRED,&enabled,&n) && enabled==1);
    CHECK(send(client,"L",1,0)==1);r=receive_control(accepted,1,CONTROL_SIZE,0);CHECK(r.size==1);cred_is(&r,getpid(),getuid(),getgid());CHECK(!close(accepted) && !close(client) && !close(listener));
    passed("passcred-option-autobind-listener-inheritance");
}

/* Copy-fault oracles deliberately stand alone: a failed control copy can still
 * return payload and discard rights, whereas a failed payload copy is EFAULT. */
static void fault_send_control(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);char byte='F';struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=bad,.msg_controllen=CMSG_SPACE(sizeof(int))};
        /* musl sendmsg copies control in userspace before entering the kernel. */
        CHECK(syscall(SYS_sendmsg,p[0],&m,0)==-1 && errno==EFAULT);empty_queue(p[1]);close_pair(p);
    }
    CHECK(!munmap(bad,page));passed("fault-send-control-no-enqueue");
}
static void libc_control_fault(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    int p[2];pair_at(SOCK_DGRAM,p);pid_t child=fork();CHECK(child>=0);
    if(!child) {
        char byte='L';struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=bad,.msg_controllen=CMSG_SPACE(sizeof(int))};
        errno=0;ssize_t result=sendmsg(p[0],&m,0);_exit(result==-1 && errno==EFAULT?0:2);
    }
    int status;CHECK(waitpid(child,&status,0)==child);
    if(WIFSIGNALED(status)) { CHECK(WTERMSIG(status)==SIGSEGV);puts("UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=sigsegv"); }
    else { CHECK(WIFEXITED(status) && !WEXITSTATUS(status));puts("UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=efault"); }
    empty_queue(p[1]);close_pair(p);CHECK(!munmap(bad,page));passed("libc-sendmsg-control-fault-observation");
}
static void null_control(void)
{
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2],pipefd[2];pair_at(type,p);CHECK(!pipe2(pipefd,O_NONBLOCK));
        CHECK(send_control(p[0],"N",1,&pipefd[1],1,0,0)==1 && !close(pipefd[1]));
        char byte=0;struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=NULL,.msg_controllen=CONTROL_SIZE};
        CHECK(syscall(SYS_recvmsg,p[1],&m,0)==1 && byte=='N' && (m.msg_flags&MSG_CTRUNC) && !m.msg_controllen);
        CHECK(read(pipefd[0],&byte,1)==0 && !close(pipefd[0]));close_pair(p);
    }
    passed("null-control-nonzero-capacity-discards-rights");
}
static void null_name_readonly(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);unsigned char *memory=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED);
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);CHECK(send(p[0],"N",1,0)==1);char byte=0;struct iovec iov={&byte,1};
        CHECK(!mprotect(memory,page,PROT_READ|PROT_WRITE));
        struct msghdr *message=(void *)(memory+page-16);*message=(struct msghdr){.msg_name=NULL,.msg_namelen=(socklen_t)-1,.msg_iov=&iov,.msg_iovlen=1};
        /* Only msg_name/namelen are read-only. A NULL name suppresses the
         * namelen copyout; flags/controllen remain writable on the next page. */
        CHECK(!mprotect(memory,page,PROT_READ));
        CHECK(syscall(SYS_recvmsg,p[1],message,0)==1 && byte=='N' && message->msg_namelen==(socklen_t)-1 && !message->msg_flags && !message->msg_controllen);
        close_pair(p);
    }
    CHECK(!munmap(memory,page*2));passed("null-name-skips-readonly-namelen-output");
}
static void fault_receive_control(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2],pipefd[2];pair_at(type,p);CHECK(!pipe2(pipefd,O_NONBLOCK));CHECK(send_control(p[0],"F",1,&pipefd[1],1,0,0)==1 && !close(pipefd[1]));
        char byte=0;struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=bad,.msg_controllen=CMSG_SPACE(sizeof(int))};
        CHECK(recvmsg(p[1],&m,0)==1 && byte=='F' && (m.msg_flags&MSG_CTRUNC) && m.msg_controllen==0);
        CHECK(read(pipefd[0],&byte,1)==0 && !close(pipefd[0]));close_pair(p);
    }
    CHECK(!munmap(bad,page));passed("fault-receive-control-returns-payload-ctrunc");
}
static void fault_receive_partial(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);unsigned char *memory=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED && !mprotect(memory+page,page,PROT_NONE));
    for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++) {
        int p[2];pair_at(type,p);int file=data_file(),fds[]={file,file};CHECK(send_control(p[0],"P",1,fds,2,0,0)==1);
        unsigned char *control=memory+page-CMSG_LEN(sizeof(int));char byte=0;struct iovec iov={&byte,1};
        struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=CMSG_SPACE(2*sizeof(int))};
        CHECK(recvmsg(p[1],&m,0)==1 && byte=='P' && (m.msg_flags&MSG_CTRUNC));
        struct cmsghdr header;memcpy(&header,control,sizeof(header));CHECK(header.cmsg_level==SOL_SOCKET && header.cmsg_type==SCM_RIGHTS && header.cmsg_len==CMSG_LEN(sizeof(int)));
        int received;memcpy(&received,control+CMSG_LEN(0),sizeof(received));CHECK(read(received,&byte,1)==1 && byte=='0' && !close(received) && !close(file));close_pair(p);
    }
    CHECK(!munmap(memory,page*2));passed("fault-partial-control-installs-first-fd");
}
static void fault_payload(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    int p[2],pipefd[2];pair_at(SOCK_DGRAM,p);CHECK(!pipe2(pipefd,O_NONBLOCK));CHECK(send_control(p[0],"D",1,&pipefd[1],1,0,0)==1 && !close(pipefd[1]));
    union control control={0};struct iovec iov={bad,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(p[1],&m,0)==-1 && errno==EFAULT);char byte;CHECK(read(pipefd[0],&byte,1)==0);empty_queue(p[1]);CHECK(!close(pipefd[0]));close_pair(p);
    CHECK(!munmap(bad,page));passed("fault-dgram-payload-consumes-and-drops-rights");
}
static void malformed_control(void)
{
    int p[2];pair_at(SOCK_DGRAM,p);int file=data_file();union control c={0};size_t used=add_rights(&c,0,&file,1);struct cmsghdr *header=(void *)c.bytes;
    char byte='M';struct iovec iov={&byte,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=c.bytes,.msg_controllen=used};
    header->cmsg_len=CMSG_LEN(0)-1;CHECK(sendmsg(p[0],&m,0)==-1 && errno==EINVAL);empty_queue(p[1]);
    header->cmsg_len=used+1;CHECK(sendmsg(p[0],&m,0)==-1 && errno==EINVAL);empty_queue(p[1]);
    header->cmsg_len=CMSG_LEN(0);CHECK(sendmsg(p[0],&m,0)==1);struct received r=receive_control(p[1],1,CONTROL_SIZE,0);CHECK(r.size==1 && !r.nfds);
    CHECK(!close(file));close_pair(p);passed("malformed-and-empty-rights-control");
}
static void self_cycle(void)
{
    int p[2];pair_at(SOCK_DGRAM,p);CHECK(send_control(p[0],"S",1,&p[1],1,0,0)==1);close_pair(p);
}
static void gc_actions(void)
{
    for(unsigned i=0;i<32;i++)self_cycle();
    printf("UNIX-CONTROL ACTION gc-self-cycle queues=32 closed_user_fds=64\n");
    for(unsigned i=0;i<32;i++) {
        int a[2],b[2];pair_at(SOCK_DGRAM,a);pair_at(SOCK_DGRAM,b);
        CHECK(send_control(a[0],"A",1,&b[1],1,0,0)==1 && send_control(b[0],"B",1,&a[1],1,0,0)==1);close_pair(a);close_pair(b);
    }
    printf("UNIX-CONTROL ACTION gc-mutual-cycle queues=64 closed_user_fds=128\n");
    int p[2];pair_at(SOCK_DGRAM,p);int root=dup(p[1]);CHECK(root>=0 && send_control(p[0],"R",1,&p[1],1,0,0)==1);close_pair(p);
    struct received r=receive_control(root,1,CONTROL_SIZE,0);CHECK(r.size==1 && r.data[0]=='R' && r.nfds==1);struct stat a,b;CHECK(!fstat(root,&a) && !fstat(r.fds[0],&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino);release_received(&r);CHECK(!close(root));
    printf("UNIX-CONTROL ACTION gc-external-root retained_message_read=1 closed_user_fds=4\n");
    for(unsigned i=0;i<16;i++) {
        socklen_t length;struct sockaddr_un name=address("control-unaccepted",&length);int listener=socket(AF_UNIX,UNIX_CONTROL_LISTENER_TYPE,0),client=socket(AF_UNIX,UNIX_CONTROL_LISTENER_TYPE,0);CHECK(listener>=0 && client>=0);
        CHECK(!bind(listener,(void *)&name,length) && !listen(listener,1) && !connect(client,(void *)&name,length));
        CHECK(send_control(client,"U",1,&client,1,0,0)==1 && !close(client) && !close(listener));
    }
    printf("UNIX-CONTROL ACTION gc-unaccepted-child queued_children=16 closed_user_fds=32\n");
}
struct transfer { int fd;pthread_barrier_t *gate;unsigned rounds; };
static void *transfer_receiver(void *argument)
{
    struct transfer *t=argument;pin(1);
    for(unsigned i=0;i<t->rounds;i++) {
        barrier(t->gate);struct received r=receive_control(t->fd,1,CONTROL_SIZE,0);char byte;
        CHECK(r.size==1 && r.data[0]==(unsigned char)i && r.nfds==1 && read(r.fds[0],&byte,1)==1 && byte=='0');release_received(&r);
        CHECK(send(t->fd,&r.data[0],1,0)==1);barrier(t->gate);
    }
    return 0;
}
static void concurrent_transfer(void)
{
    int p[2];pair_at(SOCK_DGRAM,p);pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,2));
    struct transfer t={p[1],&gate,32};pthread_t thread;CHECK(!pthread_create(&thread,0,transfer_receiver,&t));
    for(unsigned i=0;i<t.rounds;i++) {
        int file=data_file();unsigned char byte=(unsigned char)i;CHECK(send_control(p[0],&byte,1,&file,1,0,0)==1 && !close(file));
        /* The receiver starts only after send completed and the source closed;
         * cycle destruction then overlaps its receive and descriptor close. */
        barrier(&gate);self_cycle();CHECK(recv(p[0],&byte,1,0)==1 && byte==(unsigned char)i);barrier(&gate);
    }
    CHECK(!pthread_join(thread,0) && !pthread_barrier_destroy(&gate));close_pair(p);
    passed("concurrent-transfer-source-close-with-cycle-churn");
    printf("UNIX-CONTROL ACTION gc-concurrent-churn queues=32 closed_user_fds=64\n");
}

#define PERF_PAIRS 4
#define PERF_ROUNDS 1024
struct packet { uint64_t nonce,sequence;uint32_t channel;unsigned char pattern[44]; };
_Static_assert(sizeof(struct packet)==64,"performance packets are 64 bytes");
struct perf_channel {
    int fd[2],file;unsigned id,type,mode;uint64_t nonce;dev_t device;ino_t inode;pthread_barrier_t *gate;
    uint64_t start[2],end[2],cpu[2],samples[PERF_ROUNDS];
};
struct perf_actor { struct perf_channel *channel;unsigned side; };
static void packet_is(const struct packet *packet,const struct perf_channel *c,unsigned sequence)
{
    CHECK(packet->nonce==c->nonce && packet->sequence==sequence && packet->channel==c->id);
    for(unsigned i=0;i<sizeof(packet->pattern);i++)CHECK(packet->pattern[i]==(unsigned char)(c->id*17+i));
}
static void perf_send(const struct perf_channel *c,unsigned side,const struct packet *packet)
{
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control={0};
    struct iovec iov={(void *)packet,sizeof(*packet)};struct msghdr message={.msg_iov=&iov,.msg_iovlen=1};
    if(c->mode==2) {
        struct cmsghdr *header=(void *)control.bytes;header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header),&c->file,sizeof(c->file));message.msg_control=control.bytes;message.msg_controllen=sizeof(control);
    }
    CHECK(sendmsg(c->fd[side],&message,MSG_NOSIGNAL)==sizeof(*packet));
}
static void perf_receive(const struct perf_channel *c,unsigned side,struct packet *packet,unsigned sequence)
{
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(sizeof(int))]; } control;
    struct iovec iov={packet,sizeof(*packet)};struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(c->fd[side],&message,0)==sizeof(*packet) && !(message.msg_flags&MSG_CTRUNC));packet_is(packet,c,sequence);
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);
    if(!c->mode) { CHECK(!header);return; }
    CHECK(header && header->cmsg_level==SOL_SOCKET && !CMSG_NXTHDR(&message,header));
    if(c->mode==1) {
        CHECK(header->cmsg_type==SCM_CREDENTIALS && header->cmsg_len==CMSG_LEN(sizeof(struct ucred)));
        struct ucred cred;memcpy(&cred,CMSG_DATA(header),sizeof(cred));CHECK(cred.pid==getpid() && cred.uid==getuid() && cred.gid==getgid());
    } else {
        CHECK(header->cmsg_type==SCM_RIGHTS && header->cmsg_len==CMSG_LEN(sizeof(int)));int fd;memcpy(&fd,CMSG_DATA(header),sizeof(fd));struct stat st;uint64_t nonce;
        CHECK(!fstat(fd,&st) && st.st_dev==c->device && st.st_ino==c->inode && pread(fd,&nonce,sizeof(nonce),0)==sizeof(nonce) && nonce==c->nonce && !close(fd));
    }
}
static void *perf_actor(void *argument)
{
    struct perf_actor *a=argument;struct perf_channel *c=a->channel;unsigned side=a->side;pin(c->id*2+side);
    struct packet packet={.nonce=c->nonce,.channel=c->id};for(unsigned i=0;i<sizeof(packet.pattern);i++)packet.pattern[i]=(unsigned char)(c->id*17+i);
    barrier(c->gate);c->cpu[side]=clock_ns(CLOCK_THREAD_CPUTIME_ID);c->start[side]=clock_ns(CLOCK_MONOTONIC);
    for(unsigned round=0;round<PERF_ROUNDS;round++) {
        if(!side) { packet.sequence=round;uint64_t start=clock_ns(CLOCK_MONOTONIC);perf_send(c,side,&packet);perf_receive(c,side,&packet,round);c->samples[round]=clock_ns(CLOCK_MONOTONIC)-start; }
        else { perf_receive(c,side,&packet,round);perf_send(c,side,&packet); }
    }
    c->end[side]=clock_ns(CLOCK_MONOTONIC);c->cpu[side]=clock_ns(CLOCK_THREAD_CPUTIME_ID)-c->cpu[side];return 0;
}
static int compare_u64(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static void performance(unsigned type,unsigned mode)
{
    struct perf_channel channels[PERF_PAIRS]={0};struct perf_actor actors[PERF_PAIRS*2];pthread_t threads[PERF_PAIRS*2];pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,PERF_PAIRS*2));
    for(unsigned i=0;i<PERF_PAIRS;i++) {
        struct perf_channel *c=&channels[i];c->id=i;c->type=type;c->mode=mode;c->gate=&gate;c->nonce=0x554e49584354524cULL+i;
        pair_at(type,c->fd);c->file=data_file();CHECK(pwrite(c->file,&c->nonce,sizeof(c->nonce),0)==sizeof(c->nonce));struct stat st;CHECK(!fstat(c->file,&st));c->device=st.st_dev;c->inode=st.st_ino;
        if(mode==1) { passcred(c->fd[0],1);passcred(c->fd[1],1); }
    }
    for(unsigned i=0;i<PERF_PAIRS*2;i++) { actors[i]=(struct perf_actor){&channels[i/2],i%2};CHECK(!pthread_create(&threads[i],0,perf_actor,&actors[i])); }
    for(unsigned i=0;i<PERF_PAIRS*2;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t start=UINT64_MAX,end=0,cpu=0,samples[PERF_PAIRS*PERF_ROUNDS];unsigned count=0;
    for(unsigned i=0;i<PERF_PAIRS;i++) {
        struct perf_channel *c=&channels[i];for(unsigned side=0;side<2;side++) { if(c->start[side]<start)start=c->start[side];if(c->end[side]>end)end=c->end[side];cpu+=c->cpu[side]; }
        memcpy(samples+count,c->samples,sizeof(c->samples));count+=PERF_ROUNDS;close_pair(c->fd);CHECK(!close(c->file));
    }
    CHECK(!pthread_barrier_destroy(&gate));qsort(samples,count,sizeof(*samples),compare_u64);const char *modes[]={"none","passcred","rights"};uint64_t elapsed=end-start;
    printf("UNIX-CONTROL PERF name=%s-%s type=%s mode=%s pairs=4 rounds_per_pair=1024 count=%u bytes=%llu cpus=%u wall_ns=%llu thread_cpu_ns=%llu p50_ns=%llu p95_ns=%llu max_ns=%llu ops_per_sec=%llu\n",type==SOCK_STREAM?"stream":"dgram",modes[mode],type==SOCK_STREAM?"stream":"dgram",modes[mode],count,(unsigned long long)count*64*2,cpus,(unsigned long long)elapsed,(unsigned long long)cpu,(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],(unsigned long long)((uint64_t)count*1000000000ULL/elapsed));
}
struct test_case { const char *name;void (*run)(void); };
static const struct test_case cases[]={
    {"rights-objects",rights_objects},{"source-lifetime",source_lifetime},{"control-capacity",control_capacity},
    {"credentials-capacity",credentials_capacity},
    {"rights-limits",rights_limits},{"full-table",full_table},{"inflight-quota",inflight_quota},
    {"exit-group-control-owner",exit_group_control_owner},{"peek-read",peek_and_read},
    {"stream-boundaries",stream_boundaries},{"stream-control-boundaries",stream_control_boundaries},
    {"empty-dgram",empty_dgram},{"credentials-identity",credentials_identity},
    {"credentials-options",credentials_options},{"fault-send-control",fault_send_control},
    {"libc-control-fault",libc_control_fault},{"null-control",null_control},{"null-name-readonly",null_name_readonly},
    {"fault-receive-control",fault_receive_control},{"fault-receive-partial",fault_receive_partial},
    {"fault-payload",fault_payload},{"malformed-control",malformed_control},{"concurrent-transfer",concurrent_transfer},
    {"gc-actions",gc_actions}
};
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);signal(SIGPIPE,SIG_IGN);alarm(120);affinity();
    if(getpid()==1) { CHECK(!mkdir("/proc",0555) || errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY); }
    CHECK(!mkdir("/tmp",0777) || errno==EEXIST);strcpy(workdir,"/tmp/unix-control-XXXXXX");CHECK(mkdtemp(workdir) && !chdir(workdir));
    unsigned before=fd_count();
    if((argc==2 && !strcmp(argv[1],"--perf")) || (argc==1 && access("/performance",F_OK)==0)) { for(unsigned type=UNIX_CONTROL_FIRST_TYPE;type<=UNIX_CONTROL_LAST_TYPE;type++)for(unsigned mode=0;mode<3;mode++)performance(type,mode);puts("UNIX-CONTROL PERF END 6"); }
    else {
        const char *only=0;if(argc==3 && !strcmp(argv[1],"--case"))only=argv[2];else CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--correctness")));
        int matched=0;for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++)if(!only || !strcmp(only,cases[i].name)) { cases[i].run();matched=1; }CHECK(matched);
        printf("UNIX-CONTROL END %u\n",case_count);
    }
    unsigned after=fd_count();printf("UNIX-CONTROL RESOURCE fd_before=%u fd_after=%u\n",before,after);CHECK(before==after);
    CHECK(!chdir("/") && !rmdir(workdir));alarm(0);if(getpid()==1) { sync();(void)reboot(RB_POWER_OFF); }return 0;
}
