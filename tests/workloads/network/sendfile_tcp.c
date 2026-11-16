#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"sendfile-tcp:%d: %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while (0)
#define SOURCE_BYTES (2U * 1024U * 1024U)
static unsigned char data[16384];
static volatile sig_atomic_t signals;
static void notified(int number) { (void)number; signals++; }
static unsigned char pattern(size_t position) { return (unsigned char)(position % 251U); }
static void receive(int fd, size_t size, size_t offset)
{
    while (size) {
        size_t want=size<sizeof(data)?size:sizeof(data);
        ssize_t n=read(fd,data,want); CHECK(n>0);
        for (ssize_t i=0;i<n;i++) CHECK(data[i]==pattern(offset+(size_t)i));
        offset+=(size_t)n;size-=(size_t)n;
    }
}
static void wait_ok(pid_t child)
{ int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0); }
static void wait_blocked(pid_t child)
{
    char path[64],line[512];snprintf(path,sizeof(path),"/proc/%ld/stat",(long)child);
    for (unsigned i=0;i<2000;i++) {
        FILE *file=fopen(path,"r");CHECK(file);
        CHECK(fgets(line,sizeof(line),file) && fclose(file)==0);
        char *end=strrchr(line,')');CHECK(end && end[1]==' ' && end[2]!='Z');
        if (end[2]=='S') return;
        struct timespec delay={.tv_nsec=1000000};CHECK(nanosleep(&delay,NULL)==0);
    }
    CHECK(!"sendfile caller did not block");
}
static void readiness(int fd, short events)
{ struct pollfd p={.fd=fd,.events=events};CHECK(poll(&p,1,5000)==1 && (p.revents&events)); }
static void connect_pair(int fds[2],int bounded,uint16_t port)
{
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    /* A bounded receive window makes the paused-consumer tests reach real
     * TCP backpressure, instead of waiting for a large host autotuned buffer. */
    int size=4096;if (bounded) CHECK(setsockopt(listener,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size))==0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(bind(listener,(void *)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t length=sizeof(address);CHECK(getsockname(listener,(void *)&address,&length)==0);
    fds[0]=socket(AF_INET,SOCK_STREAM,0);CHECK(fds[0]>=0);
    if (bounded) CHECK(setsockopt(fds[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0);
    if (port) {
        struct sockaddr_in local=address;local.sin_port=htons((uint16_t)(port+100));
        CHECK(bind(fds[0],(void *)&local,sizeof(local))==0);
    }
    CHECK(connect(fds[0],(void *)&address,length)==0);
    fds[1]=accept(listener,NULL,NULL);CHECK(fds[1]>=0 && close(listener)==0);
}
static void pair(int fds[2]) { connect_pair(fds,1,0); }
static int source(const char *path)
{
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    for (size_t offset=0;offset<SOURCE_BYTES;offset+=sizeof(data)) {
        for (size_t i=0;i<sizeof(data);i++) data[i]=pattern(offset+i);
        CHECK(write(fd,data,sizeof(data))==(ssize_t)sizeof(data));
    }
    CHECK(lseek(fd,0,SEEK_SET)==0);return fd;
}
static void send_all(int socket,int input,off_t *offset,size_t count,size_t chunk)
{
    while (count) {
        size_t want=count<chunk?count:chunk;
        ssize_t n=sendfile(socket,input,offset,want);
        if (n<0 && errno==EAGAIN) { readiness(socket,POLLOUT);continue; }
        CHECK(n>0 && (size_t)n<=want);count-=(size_t)n;
    }
}
static void offsets(int in)
{
    int fds[2];pair(fds);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[0])==0);receive(fds[1],70001,17);receive(fds[1],70003,37);
        CHECK(read(fds[1],data,1)==0 && close(fds[1])==0);_exit(0);
    }
    CHECK(close(fds[1])==0 && lseek(in,37,SEEK_SET)==37);
    off_t offset=17;send_all(fds[0],in,&offset,70001,70001);
    CHECK(offset==70018 && lseek(in,0,SEEK_CUR)==37);
    send_all(fds[0],in,NULL,70003,70003);CHECK(lseek(in,0,SEEK_CUR)==70040);
    offset=SOURCE_BYTES;CHECK(sendfile(fds[0],in,&offset,4097)==0 && offset==SOURCE_BYTES);
    offset=3;CHECK(sendfile(fds[0],in,&offset,0)==0 && offset==3);
    CHECK(shutdown(fds[0],SHUT_WR)==0 && close(fds[0])==0);wait_ok(child);
    puts("SENDFILE-TCP CASE content-offset-eof PASS");
}
static size_t fill(int out,int in)
{
    CHECK(fcntl(out,F_SETFL,fcntl(out,F_GETFL)|O_NONBLOCK)==0);
    size_t total=0;unsigned short_send=0;
    for (;;) {
        off_t offset=(off_t)total;
        size_t wanted=total?4096:SOURCE_BYTES;
        ssize_t n=sendfile(out,in,&offset,wanted);
        if (n<0) {
            CHECK(errno==EAGAIN && offset==(off_t)total);
            struct pollfd p={.fd=out,.events=POLLOUT};
            int ready=poll(&p,1,200);CHECK(ready>=0);
            if (ready) { CHECK(p.revents&POLLOUT);continue; }
            /* POLLOUT uses a low-water threshold, not exact free capacity.
             * A delayed ACK can free a small suffix without making it ready. */
            n=sendfile(out,in,&offset,wanted);
            if (n<0) { CHECK(errno==EAGAIN && offset==(off_t)total);break; }
        }
        CHECK(n>0 && offset==(off_t)(total+(size_t)n));
        if ((size_t)n<wanted) short_send++;
        total+=(size_t)n;CHECK(total<SOURCE_BYTES);
    }
    CHECK(short_send && total);return total;
}
static size_t nonblocking_to_again(int out,int in,int explicit_offset,size_t queued)
{
    for (;;) {
        off_t offset=(off_t)queued;
        ssize_t n=sendfile(out,in,explicit_offset?&offset:NULL,4096);int error=errno;
        off_t after=explicit_offset?offset:lseek(in,0,SEEK_CUR);
        if (n<0) { CHECK(error==EAGAIN && after==(off_t)queued);return queued; }
        CHECK(n>0 && n<=4096 && after==(off_t)(queued+(size_t)n));
        queued+=(size_t)n;CHECK(queued<SOURCE_BYTES);
    }
}
static void nonblocking(int in)
{
    int fds[2];pair(fds);CHECK(lseek(in,123,SEEK_SET)==123);
    size_t queued=fill(fds[0],in);
    /* An earlier EAGAIN is not a reservation against later ACK/credit.
     * Account for legal prefixes until each mode observes its own EAGAIN. */
    queued=nonblocking_to_again(fds[0],in,1,queued);CHECK(lseek(in,0,SEEK_CUR)==123);
    CHECK(lseek(in,(off_t)queued,SEEK_SET)==(off_t)queued);
    queued=nonblocking_to_again(fds[0],in,0,queued);CHECK(lseek(in,123,SEEK_SET)==123);
    off_t offset=(off_t)queued;
    struct timeval timeout={.tv_usec=50000};
    CHECK(setsockopt(fds[0],SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))==0);
    CHECK(fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)&~O_NONBLOCK)==0);
    for (;;) {
        ssize_t n=sendfile(fds[0],in,&offset,4096);
        if (n<0) { CHECK(errno==EAGAIN && offset==(off_t)queued);break; }
        /* Delayed TCP credit may arrive while a timed call waits. Preserve
         * that real prefix rather than assuming POLLOUT meant zero space. */
        CHECK(n>0 && offset==(off_t)(queued+(size_t)n));queued+=(size_t)n;
        CHECK(queued<SOURCE_BYTES);
    }
    CHECK(fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)|O_NONBLOCK)==0);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[0])==0);receive(fds[1],SOURCE_BYTES,0);
        CHECK(read(fds[1],data,1)==0 && close(fds[1])==0);_exit(0);
    }
    CHECK(close(fds[1])==0);send_all(fds[0],in,&offset,SOURCE_BYTES-queued,65537);
    CHECK(offset==SOURCE_BYTES && lseek(in,0,SEEK_CUR)==123);
    CHECK(shutdown(fds[0],SHUT_WR)==0 && close(fds[0])==0);wait_ok(child);
    puts("SENDFILE-TCP CASE nonblocking-short-resume PASS");
}
struct outcome { ssize_t sent; off_t offset, before; int error, signals; };
static void signal_before_progress(int in)
{
    int fds[2],ready[2],report[2];pair(fds);size_t queued=fill(fds[0],in);
    CHECK(fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)&~O_NONBLOCK)==0);
    CHECK(pipe(ready)==0 && pipe(report)==0);pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[1])==0 && close(ready[0])==0 && close(report[0])==0);
        struct outcome result={.offset=(off_t)queued};
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        for (;;) {
            result.before=result.offset;signals=0;
            CHECK(result.before<(off_t)SOURCE_BYTES);
            result.sent=sendfile(fds[0],in,&result.offset,SOURCE_BYTES-(size_t)result.before);
            result.error=errno;result.signals=signals;
            if (result.sent>0) {
                CHECK(result.offset==result.before+result.sent);
                if (!result.signals) continue;
            } else CHECK(result.sent==-1 && result.error==EINTR && result.offset==result.before);
            CHECK(result.signals==1 && write(report[1],&result,sizeof(result))==sizeof(result));
            if (result.sent<0) break;
            /* No control wait between sends: after this small report the
             * only sleeping state the parent observes is TCP backpressure. */
        }
        CHECK(close(report[1])==0 && close(fds[0])==0);_exit(0);
    }
    CHECK(close(ready[1])==0 && close(report[1])==0);
    CHECK(read(ready[0],data,1)==1 && close(ready[0])==0);
    struct outcome result;unsigned attempt=0;
    do {
        CHECK(attempt++<64);wait_blocked(child);CHECK(kill(child,SIGUSR1)==0);
        CHECK(read(report[0],&result,sizeof(result))==sizeof(result) && result.signals==1);
        CHECK(result.before>=(off_t)queued && result.offset<(off_t)SOURCE_BYTES);
        if (result.sent>0) CHECK(result.offset==result.before+result.sent);
    } while (result.sent>0);
    CHECK(result.sent==-1 && result.error==EINTR && result.offset==result.before);
    CHECK(close(report[0])==0);wait_ok(child);CHECK(close(fds[0])==0);
    receive(fds[1],(size_t)result.offset,0);
    CHECK(read(fds[1],data,1)==0 && close(fds[1])==0);
    puts("SENDFILE-TCP CASE signal-before-progress PASS");
}
static void interrupted(int in,int reset)
{
    int fds[2],ready[2],report[2];pair(fds);
    CHECK(fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)&~O_NONBLOCK)==0);
    CHECK(pipe(ready)==0 && pipe(report)==0);pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[1])==0 && close(ready[0])==0 && close(report[0])==0);
        struct outcome result={0};signals=0;
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        result.sent=sendfile(fds[0],in,&result.offset,SOURCE_BYTES);
        result.error=errno;result.signals=signals;
        CHECK(write(report[1],&result,sizeof(result))==sizeof(result));
        CHECK(close(report[1])==0 && close(fds[0])==0);_exit(0);
    }
    CHECK(close(ready[1])==0 && close(report[1])==0);
    CHECK(read(ready[0],data,1)==1 && close(ready[0])==0);
    readiness(fds[1],POLLIN);
    wait_blocked(child);
    if (reset) {
        /* POLLIN above proves unread TCP data. Closing that receiver sends
         * a reset without depending on optional SO_LINGER support. */
        CHECK(close(fds[1])==0);
    } else CHECK(kill(child,SIGUSR1)==0);
    struct outcome result;CHECK(read(report[0],&result,sizeof(result))==sizeof(result));
    CHECK(close(report[0])==0);wait_ok(child);
    CHECK(result.sent>0 && result.sent<SOURCE_BYTES && result.offset==result.sent);
    CHECK(result.signals==(reset?0:1));
    if (reset) {
        /* Linux sendfile returns the accepted prefix but consumes the reset
         * from its later splice-to-socket write. The next write sees EPIPE. */
        off_t offset=17;signals=0;
        CHECK(sendfile(fds[0],in,&offset,1)==-1 && errno==EPIPE && offset==17 && signals==1);
        CHECK(close(fds[0])==0);
    } else {
        CHECK(close(fds[0])==0);
        receive(fds[1],(size_t)result.sent,0);
        CHECK(read(fds[1],data,1)==0 && close(fds[1])==0);
    }
    puts(reset?"SENDFILE-TCP CASE reset-after-prefix PASS":"SENDFILE-TCP CASE signal-after-prefix PASS");
}
static void cancelled(int in)
{
    int fds[2],ready[2];pair(fds);CHECK(pipe(ready)==0);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[1])==0 && close(ready[0])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        off_t offset=0;(void)sendfile(fds[0],in,&offset,SOURCE_BYTES);_exit(99);
    }
    CHECK(close(ready[1])==0 && read(ready[0],data,1)==1 && close(ready[0])==0);
    readiness(fds[1],POLLIN);wait_blocked(child);
    CHECK(close(fds[0])==0 && kill(child,SIGKILL)==0);
    int status;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    /* The child held the last sending descriptor. Its canceled request must
     * return ownership so the receiver can reach EOF, not keep a hidden pin. */
    size_t received=0;ssize_t n;
    while ((n=read(fds[1],data,sizeof(data)))>0) {
        for (ssize_t i=0;i<n;i++) CHECK(data[i]==pattern(received+(size_t)i));
        received+=(size_t)n;
    }
    CHECK(n==0 && received && received<SOURCE_BYTES && close(fds[1])==0);
    puts("SENDFILE-TCP CASE cancel-last-sender PASS");
}
static void shutdown_error(int in)
{
    int fds[2];pair(fds);CHECK(shutdown(fds[0],SHUT_WR)==0);
    signals=0;off_t offset=0;
    CHECK(sendfile(fds[0],in,&offset,0)==0 && offset==0 && signals==0);
    CHECK(sendfile(fds[0],in,&offset,4097)==-1 && errno==EPIPE && offset==0 && signals==1);
    CHECK(read(fds[1],data,1)==0 && close(fds[0])==0 && close(fds[1])==0);
    puts("SENDFILE-TCP CASE shutdown-no-progress PASS");
}
static void sparse_memory(void)
{
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    size_t end=(size_t)page*3U+37U;
    int in=memfd_create("sendfile-sparse",MFD_CLOEXEC);CHECK(in>=0 && ftruncate(in,(off_t)end)==0);
    for (size_t i=0;i<31;i++) data[i]=pattern(7+i);
    CHECK(pwrite(in,data,31,7)==31);
    for (size_t i=0;i<31;i++) data[i]=pattern((size_t)page*3U+3+i);
    CHECK(pwrite(in,data,31,page*3+3)==31);
    int fds[2];pair(fds);pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(in)==0 && close(fds[0])==0);size_t total=0;ssize_t n;
        while ((n=read(fds[1],data,sizeof(data)))>0) {
            CHECK(total+(size_t)n<=end);
            for (ssize_t i=0;i<n;i++) {
                size_t position=total+(size_t)i;
                int stored=(position>=7 && position<38) ||
                    (position>=(size_t)page*3U+3 && position<(size_t)page*3U+34);
                CHECK(data[i]==(stored?pattern(position):0));
            }
            total+=(size_t)n;
        }
        CHECK(n==0 && total==end && close(fds[1])==0);_exit(0);
    }
    CHECK(close(fds[1])==0);send_all(fds[0],in,NULL,end,end);
    CHECK(lseek(in,0,SEEK_CUR)==(off_t)end && close(in)==0);
    CHECK(shutdown(fds[0],SHUT_WR)==0 && close(fds[0])==0);wait_ok(child);
    puts("SENDFILE-TCP CASE sparse-memory-source PASS");
}
static void ordinary_writes(int fd)
{
    for (unsigned mode=0;mode<2;mode++) {
        size_t left=65537U+mode;memset(data,251+(int)mode,sizeof(data));
        while (left) {
            size_t wanted=left<sizeof(data)?left:sizeof(data);
            ssize_t n=mode?write(fd,data,wanted):send(fd,data,wanted,0);
            CHECK(n>0 && (size_t)n<=wanted);left-=(size_t)n;
        }
    }
}
static size_t receive_mixed(int fd,int cancelled)
{
    size_t file=0,ordinary[2]={0};unsigned reads=0;
    for (;;) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        CHECK(poll(&p,1,5000)==1 && (p.revents&(POLLIN|POLLHUP)));
        ssize_t n=reads++%2?read(fd,data,sizeof(data)):recv(fd,data,sizeof(data),0);
        CHECK(n>=0);if (!n) break;
        for (ssize_t i=0;i<n;i++) {
            if (data[i]<251) {
                CHECK(!cancelled || (!ordinary[0] && !ordinary[1]));
                CHECK(data[i]==pattern(file));file++;CHECK(file<=SOURCE_BYTES);
            } else {
                unsigned mode=data[i]-251;CHECK(mode<2);
                CHECK(mode || !ordinary[1]);ordinary[mode]++;
                CHECK(ordinary[mode]<=65537U+mode);
            }
        }
    }
    CHECK(ordinary[0]==65537U && ordinary[1]==65538U);
    CHECK(cancelled?(file>0 && file<SOURCE_BYTES):file==SOURCE_BYTES);
    return file;
}
static void concurrent_writers(int in)
{
    int fds[2],ready[2];pair(fds);CHECK(pipe(ready)==0);
    pid_t file=fork();CHECK(file>=0);
    if (!file) {
        CHECK(close(fds[1])==0 && close(ready[0])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        off_t offset=0;send_all(fds[0],in,&offset,SOURCE_BYTES,SOURCE_BYTES);
        CHECK(offset==SOURCE_BYTES && close(fds[0])==0);_exit(0);
    }
    CHECK(close(ready[1])==0 && read(ready[0],data,1)==1 && close(ready[0])==0);
    readiness(fds[1],POLLIN);wait_blocked(file);
    CHECK(pipe(ready)==0);pid_t ordinary=fork();CHECK(ordinary>=0);
    if (!ordinary) {
        CHECK(close(fds[1])==0 && close(ready[0])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        ordinary_writes(fds[0]);CHECK(close(fds[0])==0);_exit(0);
    }
    CHECK(close(ready[1])==0 && read(ready[0],data,1)==1 && close(ready[0])==0);
    /* Neither child waits on a control pipe: both sleeping callers are in
     * real TCP backpressure before the receiver allows either to advance. */
    wait_blocked(ordinary);CHECK(close(fds[0])==0);
    (void)receive_mixed(fds[1],0);CHECK(close(fds[1])==0);
    wait_ok(file);wait_ok(ordinary);
    puts("SENDFILE-TCP CASE concurrent-send-write PASS");
}
static void cancel_shared_sender(int in)
{
    int fds[2],ready[2];pair(fds);int retained=dup(fds[0]);CHECK(retained>=0 && pipe(ready)==0);
    pid_t file=fork();CHECK(file>=0);
    if (!file) {
        CHECK(close(retained)==0 && close(fds[1])==0 && close(ready[0])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        off_t offset=0;(void)sendfile(fds[0],in,&offset,SOURCE_BYTES);_exit(99);
    }
    CHECK(close(ready[1])==0 && read(ready[0],data,1)==1 && close(ready[0])==0);
    readiness(fds[1],POLLIN);wait_blocked(file);
    /* Closing this alias must not wait for or terminate the active write.
     * After cancellation, the retained OFD must still be usable by a writer. */
    CHECK(close(fds[0])==0 && kill(file,SIGKILL)==0);
    int status;CHECK(waitpid(file,&status,0)==file && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    pid_t ordinary=fork();CHECK(ordinary>=0);
    if (!ordinary) {
        CHECK(close(fds[1])==0);ordinary_writes(retained);
        CHECK(close(retained)==0);_exit(0);
    }
    CHECK(close(retained)==0);(void)receive_mixed(fds[1],1);
    CHECK(close(fds[1])==0);wait_ok(ordinary);
    puts("SENDFILE-TCP CASE cancel-shared-sender PASS");
}
static void pipe_tcp_splice(int in)
{
    int fds[2],p[2];pair(fds);CHECK(pipe(p)==0);
    size_t block=4096;
    for (size_t i=0;i<block;i++) data[i]=pattern(i);
    CHECK(write(p[1],data,block)==(ssize_t)block);
    size_t first=0;
    while (first<block) {
        ssize_t n=splice(p[0],NULL,fds[0],NULL,block+1,SPLICE_F_NONBLOCK);
        CHECK(n>0 && (size_t)n<=block-first);first+=(size_t)n;
    }
    CHECK(splice(p[0],NULL,fds[0],NULL,1,SPLICE_F_NONBLOCK)==-1 && errno==EAGAIN);
    CHECK(fcntl(p[0],F_SETFL,fcntl(p[0],F_GETFL)|O_NONBLOCK)==0);
    size_t queued=fill(fds[0],in),pending=0;
    for (;;) {
        if (!pending) {
            CHECK(queued+block<SOURCE_BYTES);
            for (size_t i=0;i<block;i++) data[i]=pattern(queued+i);
            CHECK(write(p[1],data,block)==(ssize_t)block);pending=block;
        }
        ssize_t n=splice(p[0],NULL,fds[0],NULL,pending+1,SPLICE_F_NONBLOCK);int error=errno;
        if (n<0) { CHECK(error==EAGAIN);break; }
        CHECK(n>0 && (size_t)n<=pending);
        queued+=(size_t)n;pending-=(size_t)n;
    }
    /* Check the exact unconsumed pipe suffix through ordinary reads, then
     * restore it for the blocking continuation; no optional pipe ioctl. */
    CHECK(read(p[0],data,sizeof(data))==(ssize_t)pending);
    for (size_t i=0;i<pending;i++) CHECK(data[i]==pattern(queued+i));
    CHECK(read(p[0],data+pending,1)==-1 && errno==EAGAIN);
    CHECK(write(p[1],data,pending)==(ssize_t)pending);
    int ready[2];CHECK(pipe(ready)==0);
    size_t expected=queued+pending;pid_t receiver=fork();CHECK(receiver>=0);
    if (!receiver) {
        CHECK(close(ready[0])==0 && close(fds[0])==0 && close(p[0])==0 && close(p[1])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        receive(fds[1],block,0);receive(fds[1],expected,0);
        CHECK(read(fds[1],data,1)==0 && close(fds[1])==0);_exit(0);
    }
    CHECK(close(ready[1])==0 && close(fds[1])==0 && close(p[1])==0);
    /* Pipe close can take the pipe mutex. Finish inherited closes before
     * blocking splice holds that mutex while waiting for the consumer. */
    CHECK(read(ready[0],data,1)==1 && close(ready[0])==0);
    CHECK(fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)&~O_NONBLOCK)==0);
    while (pending) {
        ssize_t n=splice(p[0],NULL,fds[0],NULL,pending+1,0);
        CHECK(n>0 && (size_t)n<=pending);pending-=(size_t)n;
    }
    CHECK(splice(p[0],NULL,fds[0],NULL,1,0)==0);
    CHECK(close(p[0])==0 && close(fds[0])==0);wait_ok(receiver);
    puts("SENDFILE-TCP CASE pipe-to-tcp-splice PASS");
}
static unsigned fd_count(void)
{
    DIR *directory=opendir("/proc/self/fd");CHECK(directory);unsigned count=0;
    struct dirent *entry;while ((entry=readdir(directory))) if (entry->d_name[0]!='.') count++;
    CHECK(closedir(directory)==0);return count;
}
static uint64_t ns(clockid_t clock)
{ struct timespec time;CHECK(clock_gettime(clock,&time)==0);return (uint64_t)time.tv_sec*1000000000U+(uint64_t)time.tv_nsec; }
static void pin_cpu(int cpu)
{ cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);CHECK(sched_setaffinity(0,sizeof(set),&set)==0 && sched_getcpu()==cpu); }
static void performance(size_t chunk)
{
    cpu_set_t allowed;CHECK(sched_getaffinity(0,sizeof(allowed),&allowed)==0);
    int cpus[3],nr=0;
    for (int cpu=0;cpu<CPU_SETSIZE && nr<3;cpu++) if (CPU_ISSET(cpu,&allowed)) cpus[nr++]=cpu;
    CHECK(nr);int sender=cpus[nr>1?1:0],receiver=cpus[nr>2?2:0];pin_cpu(sender);
    int in=open("/sendfile-tcp-data",O_RDONLY);CHECK(in>=0);struct stat st;
    CHECK(fstat(in,&st)==0 && st.st_size==8*1024*1024);
    /* Warm the fixed source outside timing. This compares TCP request work,
     * not first-touch disk I/O, and never generates the payload in the sender. */
    receive(in,(size_t)st.st_size,0);CHECK(lseek(in,0,SEEK_SET)==0);
    /* Fixed tuples also keep the protocol shard stable between boots. */
    int fds[2],ready[2];connect_pair(fds,0,chunk==4096?18382:18383);CHECK(pipe(ready)==0);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        pin_cpu(receiver);CHECK(close(ready[0])==0 && close(in)==0 && close(fds[0])==0);
        CHECK(write(ready[1],"R",1)==1 && close(ready[1])==0);
        receive(fds[1],(size_t)st.st_size,0);
        CHECK(read(fds[1],data,1)==0 && write(fds[1],"A",1)==1 && close(fds[1])==0);_exit(0);
    }
    CHECK(close(fds[1])==0 && close(ready[1])==0);
    CHECK(read(ready[0],data,1)==1 && close(ready[0])==0);
    uint64_t start=ns(CLOCK_MONOTONIC),cpu=ns(CLOCK_PROCESS_CPUTIME_ID);
    send_all(fds[0],in,NULL,(size_t)st.st_size,chunk);
    uint64_t accepted=ns(CLOCK_MONOTONIC)-start;
    CHECK(shutdown(fds[0],SHUT_WR)==0 && read(fds[0],data,1)==1 && data[0]=='A');
    uint64_t complete=ns(CLOCK_MONOTONIC)-start;cpu=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    CHECK(close(fds[0])==0 && close(in)==0);wait_ok(child);
    printf("SENDFILE-TCP PERF chunk=%zu bytes=%lld accepted_ns=%llu complete_ns=%llu sender_cpu_ns=%llu sender_cpu=%d receiver_cpu=%d\n",
        chunk,(long long)st.st_size,(unsigned long long)accepted,(unsigned long long)complete,(unsigned long long)cpu,sender,receiver);
    CHECK(sched_setaffinity(0,sizeof(allowed),&allowed)==0);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    struct sigaction action={.sa_handler=notified};sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1,&action,NULL)==0 && sigaction(SIGPIPE,&action,NULL)==0);
    if (access("/proc/self/stat",F_OK)<0) {
        CHECK(errno==ENOENT);if (mkdir("/proc",0555)<0) CHECK(errno==EEXIST);
        CHECK(mount("proc","/proc","proc",0,NULL)==0);
    }
    int control=socket(AF_INET,SOCK_DGRAM,0);CHECK(control>=0);
    struct ifreq interface={.ifr_name="lo"};CHECK(ioctl(control,SIOCGIFFLAGS,&interface)==0);
    if (!(interface.ifr_flags&IFF_UP)) { interface.ifr_flags|=IFF_UP;CHECK(ioctl(control,SIOCSIFFLAGS,&interface)==0); }
    CHECK(close(control)==0);unsigned before=fd_count();
    if (access("/performance",F_OK)==0) { performance(4096);performance(262144); }
    else {
        if (mkdir("/tmp",0755)<0) CHECK(errno==EEXIST);
        char directory[]="/tmp/sendfile-tcp-XXXXXX";CHECK(mkdtemp(directory) && chdir(directory)==0);
        int in=source("input");offsets(in);nonblocking(in);
        signal_before_progress(in);interrupted(in,0);interrupted(in,1);cancelled(in);shutdown_error(in);sparse_memory();
        concurrent_writers(in);cancel_shared_sender(in);pipe_tcp_splice(in);
        CHECK(close(in)==0 && unlink("input")==0 && chdir("/")==0 && rmdir(directory)==0);
    }
    unsigned after=fd_count();CHECK(before==after);
    printf("SENDFILE-TCP RESOURCE fd_before=%u fd_after=%u\n",before,after);
    puts("NETWORK PASS sendfile_tcp");return 0;
}
