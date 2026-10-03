#define _GNU_SOURCE
/* Shared RV64/Linux UART job-control probe. Fixed contract sources:
 * references/linux/drivers/tty/{tty_jobctrl,tty_io}.c
 * commit f4cdf7ca9a1fdcca413157df19753f388a5a224e.
 * Launcher: setsid child, inherited console stdio, /probe-no-ctty marker.
 * All synchronization uses pipes/wait4; no serial input or sleeps required. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
struct tty_termios {
    uint32_t iflag,oflag,cflag,lflag;
    uint8_t line,cc[19];
};
_Static_assert(sizeof(struct tty_termios)==36,"RV64 TCGETS ABI");
static int journal=-1, serial=-1;
static struct tty_termios original;
static const char *active="startup";
static void write_all(int fd,const void *data,size_t size) {
    const unsigned char *p=data;
    while(size) {
        ssize_t n=write(fd,p,size);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)_exit(120);
        p+=(size_t)n;size-=(size_t)n;
    }
}
static void emit(const char *text) {
    size_t n=strlen(text);
    if(journal>=0)write_all(journal,text,n);
    /* A new console OFD avoids hiding records on a previously hung-up OFD. */
    int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
    if(fd>=0) {
        const char *p=text;size_t left=n;
        while(left) {
            ssize_t got=write(fd,p,left);
            if(got<0&&errno==EINTR)continue;
            if(got<=0)break;
            p+=got;left-=(size_t)got;
        }
        close(fd);
    }
}
static void action(const char *name) {
    char line[192];active=name;
    snprintf(line,sizeof(line),"TTY_ACTION %s\n",name);emit(line);
}
static void record(const char *label,long ret,int error,int boolean) {
    char line[256];
    snprintf(line,sizeof(line),"TTY_RECORD %s ret=%ld errno=%d bool=%d\n",label,ret,error,boolean);
    emit(line);
}
static void fail(const char *label,long ret,long expected,int error) {
    char line[320];
    snprintf(line,sizeof(line),"TTY_RECORD FAIL %s action=%s ret=%ld expected=%ld errno=%d\n",
             label,active,ret,expected,error);emit(line);
    if(journal>=0)(void)fsync(journal);
    _exit(1);
}
static void exact(const char *label,long ret,long expected) {
    int e=ret<0?errno:0;record(label,ret,e,ret==expected);
    if(ret!=expected)fail(label,ret,expected,e);
}
static void error_is(const char *label,long ret,int want) {
    int e=ret<0?errno:0;record(label,ret,e,ret==-1&&e==want);
    if(ret!=-1||e!=want)fail(label,e,want,e);
}
static void truth(const char *label,int value) {
    record(label,0,0,!!value);if(!value)fail(label,value,1,0);
}
static void read_all(int fd,void *data,size_t size) {
    unsigned char *p=data;
    while(size) {
        ssize_t n=read(fd,p,size);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)fail("pipe-read",n,1,n<0?errno:0);
        p+=(size_t)n;size-=(size_t)n;
    }
}
static void make_pipe(int p[2]) {if(pipe(p))fail("pipe",-1,0,errno);}
static pid_t spawn(void) {pid_t p=fork();if(p<0)fail("fork",p,0,errno);return p;}
static int exited(pid_t child,const char *label) {
    int status=-1;pid_t result;
    do {result=wait4(child,&status,0,NULL);}while(result<0&&errno==EINTR);
    truth(label,result==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);return status;
}
static int proc_tty_present(void) {
    char buf[1024];int fd=open("/proc/self/stat",O_RDONLY);
    if(fd<0)fail("proc-stat-open",fd,0,errno);
    ssize_t n=read(fd,buf,sizeof(buf)-1);close(fd);
    if(n<=0)fail("proc-stat-read",n,1,errno);
    buf[n]=0;char *tail=strrchr(buf,')');
    long parent,group,session,tty_nr;char state;
    if(!tail||sscanf(tail+2,"%c %ld %ld %ld %ld",&state,&parent,&group,&session,&tty_nr)!=5)
        fail("proc-stat-format",0,1,0);
    return tty_nr!=0;
}
static int tty_alias(void) {
    int fd=open("/dev/tty",O_RDWR|O_NOCTTY);
    if(fd>=0)close(fd);
    return fd;
}
static void session_queries(int fd,const char *pgrp_label,const char *sid_label) {
    errno=0;pid_t fg=tcgetpgrp(fd);int e=fg<0?errno:0;
    record(pgrp_label,fg<0?-1:0,e,fg==getpgrp());
    if(fg!=getpgrp())fail(pgrp_label,fg<0?-1:0,0,e);
    pid_t sid=-1;errno=0;int ret=ioctl(fd,TIOCGSID,&sid);
    record(sid_label,ret,ret<0?errno:0,ret==0&&sid==getsid(0));
    if(ret||sid!=getsid(0))fail(sid_label,ret,0,errno);
}
static void set_tostop(int enabled) {
    struct tty_termios s=original;s.iflag=0;s.lflag=enabled?0x100U:0;
    s.cc[6]=1;s.cc[5]=0;
    action(enabled?"set-TOSTOP":"clear-TOSTOP");exact("set-TOSTOP-result",ioctl(serial,TCSETS,&s),0);
    exact("flush-input",ioctl(serial,TCFLSH,0),0);
}
struct child_result {long ret;int error;int a,b,c;};
static void child_signal(int number,int behavior) {
    struct sigaction sa={.sa_handler=behavior==1?SIG_IGN:SIG_DFL};
    sigemptyset(&sa.sa_mask);if(sigaction(number,&sa,NULL))_exit(121);
    sigset_t mask;sigemptyset(&mask);if(behavior==2)sigaddset(&mask,number);
    if(sigprocmask(SIG_SETMASK,&mask,NULL))_exit(121);
}
static void bg_case(const char *label,int writing,int behavior) {
    action(label);
    int ready[2],go[2],result[2];make_pipe(ready);make_pipe(go);make_pipe(result);
    pid_t child=spawn();
    if(!child) {
        close(ready[0]);close(go[1]);close(result[0]);
        if(setpgid(0,0))_exit(121);
        child_signal(writing?SIGTTOU:SIGTTIN,behavior);
        unsigned char token=1;write_all(ready[1],&token,1);read_all(go[0],&token,1);
        unsigned char byte='\n';errno=0;
        ssize_t ret=writing?write(serial,&byte,1):read(serial,&byte,1);
        struct child_result r={ret,ret<0?errno:0,0,0,0};write_all(result[1],&r,sizeof(r));_exit(0);
    }
    close(ready[1]);close(go[0]);close(result[1]);unsigned char token;
    read_all(ready[0],&token,1);write_all(go[1],&token,1);
    if(!behavior) {
        int status=-1;pid_t waited;
        do {waited=wait4(child,&status,WUNTRACED,NULL);}while(waited<0&&errno==EINTR);
        int stop=WIFSTOPPED(status)?WSTOPSIG(status):0;
        record(label,waited==child?0:-1,waited<0?errno:0,stop==(writing?SIGTTOU:SIGTTIN));
        if(waited!=child||stop!=(writing?SIGTTOU:SIGTTIN))fail(label,stop,writing?SIGTTOU:SIGTTIN,0);
        if(kill(child,SIGKILL))fail("kill-stopped-child",-1,0,errno);
        do {waited=wait4(child,&status,0,NULL);}while(waited<0&&errno==EINTR);
        truth("stopped-child-reaped",waited==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    } else {
        struct child_result r;read_all(result[0],&r,sizeof(r));
        record(label,r.ret,r.error,writing?r.ret==1:r.ret==-1&&r.error==EIO);
        if(writing?r.ret!=1:r.ret!=-1||r.error!=EIO)fail(label,r.ret,writing?1:-1,r.error);
        exited(child,"background-child-exited");
    }
    close(ready[0]);close(go[1]);close(result[0]);
}
static void fork_metadata(void) {
    action("fork-and-dup-metadata");
    int duplicate=dup(serial);if(duplicate<0)fail("dup",-1,0,errno);
    session_queries(duplicate,"dup-pgrp","dup-sid");
    struct stat a,b;truth("dup-rdev",!fstat(serial,&a)&&!fstat(duplicate,&b)&&a.st_rdev==b.st_rdev);
    int saved=fcntl(serial,F_GETFL);if(saved<0)fail("getflags",-1,0,errno);
    exact("dup-setflags",fcntl(duplicate,F_SETFL,saved|O_NONBLOCK),0);
    truth("dup-shared-status-flags",(fcntl(serial,F_GETFL)&O_NONBLOCK)!=0);
    exact("restore-flags",fcntl(serial,F_SETFL,saved),0);
    int p[2];make_pipe(p);pid_t child=spawn();
    if(!child) {
        close(p[0]);pid_t sid=-1;
        struct child_result r={0,0,tty_alias()>=0,tcgetpgrp(duplicate)==getpgrp(),
                              ioctl(duplicate,TIOCGSID,&sid)==0&&sid==getsid(0)};
        write_all(p[1],&r,sizeof(r));_exit(0);
    }
    close(p[1]);struct child_result r;read_all(p[0],&r,sizeof(r));
    truth("fork-ctty-and-dup-metadata",r.a&&r.b&&r.c);exited(child,"fork-metadata-exited");close(p[0]);close(duplicate);
}
static void nonleader_acquire(void) {
    action("nonleader-TIOCSCTTY-after-detach");
    int p[2];make_pipe(p);pid_t child=spawn();
    if(!child) {
        close(p[0]);if(ioctl(serial,TIOCNOTTY,0))_exit(121);
        errno=0;long ret=ioctl(serial,TIOCSCTTY,0);int error=ret<0?errno:0;
        errno=0;int alias=tty_alias();
        struct child_result r={ret,error,alias==-1&&errno==ENXIO,proc_tty_present()==0,0};
        write_all(p[1],&r,sizeof(r));_exit(0);
    }
    close(p[1]);struct child_result r;read_all(p[0],&r,sizeof(r));
    record("nonleader-TIOCSCTTY",r.ret,r.error,r.ret==-1&&r.error==EPERM&&r.a&&r.b);
    if(r.ret!=-1||r.error!=EPERM||!r.a||!r.b)fail("nonleader-TIOCSCTTY",r.ret,-1,r.error);
    exited(child,"nonleader-child-exited");close(p[0]);
    session_queries(serial,"parent-still-pgrp","parent-still-sid");
}
static void foreign_session(void) {
    action("foreign-session-setsid-and-NOCTTY");
    int p[2],go[2];make_pipe(p);make_pipe(go);pid_t child=spawn();
    if(!child) {
        close(p[0]);close(go[1]);if(setsid()!=getpid())_exit(121);
        errno=0;int alias=tty_alias();int cleared=alias<0&&errno==ENXIO;
        int fd=open("/dev/ttyS0",O_RDWR|O_NOCTTY);if(fd<0)_exit(121);
        errno=0;int still=tty_alias();int absent=still<0&&errno==ENXIO;
        errno=0;pid_t sid=-1;long ret=ioctl(fd,TIOCGSID,&sid);int error=ret<0?errno:0;
        struct child_result r={ret,error,cleared&&absent,proc_tty_present()==0,ioctl(fd,TCGETS,&(struct tty_termios){0})==0};
        write_all(p[1],&r,sizeof(r));unsigned char token;read_all(go[0],&token,1);close(fd);_exit(0);
    }
    close(p[1]);close(go[0]);struct child_result r;read_all(p[0],&r,sizeof(r));
    truth("setsid-clears-inherited-ctty",r.a&&r.b&&r.c);
    record("foreign-TIOCGSID",r.ret,r.error,r.ret==-1&&r.error==ENOTTY);
    if(r.ret!=-1||r.error!=ENOTTY)fail("foreign-TIOCGSID",r.ret,-1,r.error);
    pid_t foreign=child;errno=0;error_is("foreign-session-pgrp",ioctl(serial,TIOCSPGRP,&foreign),EPERM);
    unsigned char token=1;write_all(go[1],&token,1);exited(child,"foreign-child-exited");close(p[0]);close(go[1]);
}
static volatile sig_atomic_t observer_fd=-1;
static void observed_signal(int signal_number) {
    unsigned char token=signal_number==SIGHUP?'H':'C';int saved=errno;
    (void)write((int)observer_fd,&token,1);errno=saved;
}
static void detach_hup(void) {
    action("HUP-observer-ready");
    int events[2],ready[2],go[2];make_pipe(events);make_pipe(ready);make_pipe(go);
    pid_t child=spawn();
    if(!child) {
        close(events[0]);close(ready[0]);close(go[1]);if(setpgid(0,0))_exit(121);
        observer_fd=events[1];struct sigaction sa={.sa_handler=observed_signal};sigemptyset(&sa.sa_mask);
        if(sigaction(SIGHUP,&sa,NULL)||sigaction(SIGCONT,&sa,NULL))_exit(121);
        unsigned char token=1;write_all(ready[1],&token,1);read_all(go[0],&token,1);_exit(0);
    }
    close(events[1]);close(ready[1]);close(go[0]);unsigned char token;read_all(ready[0],&token,1);
    action("foreground-observer-then-TIOCNOTTY");
    exact("foreground-observer",ioctl(serial,TIOCSPGRP,&child),0);
    exact("leader-TIOCNOTTY",ioctl(serial,TIOCNOTTY,0),0);
    int hup=0,cont=0;while(!hup||!cont){read_all(events[0],&token,1);hup|=token=='H';cont|=token=='C';}
    truth("detach-delivers-HUP-CONT",hup&&cont);errno=0;error_is("detach-clears-devtty",tty_alias(),ENXIO);
    write_all(go[1],"G",1);exited(child,"HUP-observer-exited");
    exact("detach-old-fd-still-live",ioctl(serial,TCGETS,&(struct tty_termios){0}),0);
    int fresh=open("/dev/ttyS0",O_RDWR);truth("detach-reacquire-open",fresh>=0);
    session_queries(fresh,"detach-reacquired-pgrp","detach-reacquired-sid");close(fresh);
    close(events[0]);close(ready[0]);close(go[1]);
}
static void stolen_generation(void) {
    action("foreign-session-force-TIOCSCTTY");
    int ready[2],go[2];make_pipe(ready);make_pipe(go);pid_t child=spawn();
    if(!child) {
        close(ready[0]);close(go[1]);if(setsid()!=getpid())_exit(121);
        int fd=open("/dev/ttyS0",O_RDWR|O_NOCTTY);if(fd<0)_exit(121);
        errno=0;long plain=ioctl(fd,TIOCSCTTY,0);int denied=plain<0?errno:0;
        errno=0;long ret=ioctl(fd,TIOCSCTTY,1);int error=ret<0?errno:0;
        pid_t sid=-1;
        struct child_result r={ret,error,plain==-1&&denied==EPERM,tty_alias()>=0,
                              ioctl(fd,TIOCGSID,&sid)==0&&sid==getsid(0)&&proc_tty_present()};
        write_all(ready[1],&r,sizeof(r));unsigned char token;read_all(go[0],&token,1);_exit(0);
    }
    close(ready[1]);close(go[0]);struct child_result r;read_all(ready[0],&r,sizeof(r));
    record("force-TIOCSCTTY",r.ret,r.error,r.ret==0&&r.a&&r.b&&r.c);
    if(r.ret||!r.a||!r.b||!r.c)fail("force-TIOCSCTTY",r.ret,0,r.error);
    errno=0;error_is("steal-clears-old-session",tty_alias(),ENXIO);
    truth("steal-clears-proc-tty",!proc_tty_present());
    exact("steal-old-fd-live-before-exit",ioctl(serial,TCGETS,&(struct tty_termios){0}),0);
    action("stolen-session-leader-exit");write_all(go[1],"X",1);exited(child,"stolen-leader-exited");
    close(ready[0]);close(go[1]);
    errno=0;error_is("old-generation-write",write(serial,"\n",1),EIO);
    errno=0;error_is("old-generation-ioctl",ioctl(serial,TCGETS,&(struct tty_termios){0}),EIO);
    unsigned char byte;exact("old-generation-read-eof",read(serial,&byte,1),0);
    struct pollfd poller={serial,POLLIN|POLLOUT,0};exact("old-generation-poll",poll(&poller,1,0),1);
    truth("old-generation-poll-HUP",(poller.revents&POLLHUP)!=0);
    int fresh=open("/dev/ttyS0",O_RDWR|O_NOCTTY);truth("new-generation-NOCTTY-open",fresh>=0);
    errno=0;error_is("unowned-NOCTTY-devtty",tty_alias(),ENXIO);truth("unowned-NOCTTY-proc",!proc_tty_present());
    exact("new-generation-ioctl",ioctl(fresh,TCGETS,&(struct tty_termios){0}),0);
    exact("new-generation-write",write(fresh,"\n",1),1);
    errno=0;error_is("old-generation-not-revived",write(serial,"\n",1),EIO);
    int acquired=open("/dev/ttyS0",O_RDWR);truth("new-generation-autoacquire",acquired>=0);
    session_queries(acquired,"new-generation-pgrp","new-generation-sid");truth("new-generation-proc-tty",proc_tty_present());
    exact("restore-original-termios",ioctl(acquired,TCSETS,&original),0);
    close(fresh);close(acquired);
}
int main(void) {
    journal=open("/tty-jobctrl-records",O_CREAT|O_WRONLY|O_TRUNC,0600);
    if(journal<0)_exit(119);
    struct sigaction ignored={.sa_handler=SIG_IGN};sigemptyset(&ignored.sa_mask);
    if(sigaction(SIGHUP,&ignored,NULL)||sigaction(SIGTTOU,&ignored,NULL))fail("signal-setup",-1,0,errno);
    action("inherited-console-before-acquisition");
    exact("initial-console-TCGETS",ioctl(0,TCGETS,&original),0);
    int console=open("/dev/console",O_RDWR);truth("console-open",console>=0);
    errno=0;error_is("console-no-autoacquire",tty_alias(),ENXIO);truth("initial-proc-no-tty",!proc_tty_present());close(console);
    serial=open("/dev/ttyS0",O_RDWR);truth("serial-autoacquire",serial>=0);
    session_queries(serial,"autoacquire-pgrp","autoacquire-sid");truth("autoacquire-proc-tty",proc_tty_present());
    exact("save-serial-termios",ioctl(serial,TCGETS,&original),0);set_tostop(0);
    action("ioctl-error-boundaries");
    unsigned commands[]={TCGETS,TCSETS,TIOCGPGRP,TIOCSPGRP,TIOCGSID};
    const char *labels[]={"TCGETS-bad-pointer","TCSETS-bad-pointer","TIOCGPGRP-bad-pointer","TIOCSPGRP-bad-pointer","TIOCGSID-bad-pointer"};
    for(unsigned j=0;j<sizeof(commands)/sizeof(commands[0]);j++){errno=0;error_is(labels[j],ioctl(serial,commands[j],(void *)(uintptr_t)1),EFAULT);}
    pid_t group=-1;errno=0;error_is("negative-pgrp",ioctl(serial,TIOCSPGRP,&group),EINVAL);
    group=INT_MAX;errno=0;error_is("nonexistent-pgrp",ioctl(serial,TIOCSPGRP,&group),ESRCH);
    fork_metadata();nonleader_acquire();foreign_session();
    bg_case("background-TTIN-default-stop",0,0);bg_case("background-TTIN-ignored-EIO",0,1);bg_case("background-TTIN-blocked-EIO",0,2);
    set_tostop(1);bg_case("background-TTOU-default-stop",1,0);bg_case("background-TTOU-ignored-progress",1,1);bg_case("background-TTOU-blocked-progress",1,2);
    set_tostop(0);detach_hup();stolen_generation();
    close(serial);serial=-1;emit("TTY_PROBE_PASS\n");
    if(fsync(journal))fail("journal-fsync",-1,0,errno);
    close(journal);return 0;
}
