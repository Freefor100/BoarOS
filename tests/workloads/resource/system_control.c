#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
/* Native futex ABI; musl SDK intentionally has no kernel-private headers. */
#define FUTEX_LOCK_PI 6
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_32 2
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"SYSTEM FAIL %d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif
static uint64_t clock_ns(int clock)
{ struct timespec ts;CHECK(!syscall(SYS_clock_gettime,clock,&ts));return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec; }
static struct timespec stamp(uint64_t ns)
{ return (struct timespec){ns/1000000000,ns%1000000000}; }
static uint64_t original_offset;
static unsigned allowed_cpus[8],cpu_count;
static void bind_cpu(unsigned cpu)
{ cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);CHECK(!sched_setaffinity(0,sizeof(set),&set));CHECK(sched_getcpu()==(int)cpu); }
static void wall_set(uint64_t ns)
{ struct timespec ts=stamp(ns);CHECK(!syscall(SYS_clock_settime,CLOCK_REALTIME,&ts)); }
static void restore_wall(void) { wall_set(clock_ns(CLOCK_MONOTONIC)+original_offset); }
static void *vdso_symbol(const char *name)
{
    uintptr_t base=getauxval(AT_SYSINFO_EHDR);CHECK(base);
    const Elf64_Ehdr *eh=(void *)base;const Elf64_Phdr *ph=(void *)(base+eh->e_phoff);
    const Elf64_Dyn *dyn=NULL;const Elf64_Sym *syms=NULL;const char *strings=NULL;const uint32_t *hash=NULL;
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC)dyn=(void *)(base+ph[i].p_vaddr);
    CHECK(dyn);
    for(;dyn->d_tag!=DT_NULL;dyn++)switch(dyn->d_tag){
    case DT_SYMTAB:syms=(void *)(base+dyn->d_un.d_ptr);break;
    case DT_STRTAB:strings=(void *)(base+dyn->d_un.d_ptr);break;
    case DT_HASH:hash=(void *)(base+dyn->d_un.d_ptr);break;
    }
    CHECK(syms&&strings&&hash);
    for(unsigned i=1;i<hash[1];i++)if(!strcmp(strings+syms[i].st_name,name))return (void *)(base+syms[i].st_value);
    CHECK(0);return NULL;
}
static void check_vdso(void)
{
    int (*get)(clockid_t,struct timespec *)=vdso_symbol("__vdso_clock_gettime");
    int (*tod)(struct timeval *,struct timezone *)=vdso_symbol("__vdso_gettimeofday");
    const int clocks[]={0,1,5,6};
    for(unsigned i=0;i<4;i++){
        uint64_t first=clock_ns(clocks[i]);struct timespec ts;CHECK(!get(clocks[i],&ts));
        uint64_t sample=(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec,last=clock_ns(clocks[i]);
        CHECK(first<=sample&&sample<=last);
    }
    struct timezone raw,fast;struct timeval tv;
    CHECK(!syscall(SYS_gettimeofday,NULL,&raw)&&!tod(&tv,&fast));
    CHECK(raw.tz_minuteswest==fast.tz_minuteswest&&raw.tz_dsttime==fast.tz_dsttime);
}
static void clock_abi(void)
{
    uint64_t before=clock_ns(CLOCK_REALTIME),mono=clock_ns(CLOCK_MONOTONIC);
    original_offset=before-mono;
    struct timezone tz={1,7};
    CHECK(!syscall(SYS_settimeofday,NULL,&tz));
    CHECK(clock_ns(CLOCK_REALTIME)>=before+60000000000ULL);check_vdso();restore_wall();
    CHECK(!syscall(SYS_settimeofday,NULL,&tz));
    CHECK(clock_ns(CLOCK_REALTIME)<before+30000000000ULL);
    tz.tz_minuteswest=0;tz.tz_dsttime=0;CHECK(!syscall(SYS_settimeofday,NULL,&tz));
    CHECK(syscall(SYS_clock_settime,CLOCK_MONOTONIC,(void *)1)==-1&&errno==EINVAL);
    CHECK(syscall(SYS_clock_settime,CLOCK_REALTIME,(void *)1)==-1&&errno==EFAULT);
    struct timespec bad={-1,0};CHECK(syscall(SYS_clock_settime,0,&bad)==-1&&errno==EINVAL);
    bad=(struct timespec){0,1000000000};CHECK(syscall(SYS_clock_settime,0,&bad)==-1&&errno==EINVAL);
    struct timeval tv={-1,0};CHECK(syscall(SYS_settimeofday,&tv,(void *)1)==-1&&errno==EFAULT);
    tv.tv_usec=1000000;CHECK(syscall(SYS_settimeofday,&tv,(void *)1)==-1&&errno==EINVAL);
    tv=(struct timeval){0,0};tz=(struct timezone){2,9};
    CHECK(syscall(SYS_settimeofday,&tv,&tz)==-1&&errno==EINVAL);
    struct timezone got;CHECK(!syscall(SYS_gettimeofday,NULL,&got)&&got.tz_minuteswest==2&&got.tz_dsttime==9);check_vdso();
    tz=(struct timezone){0,0};CHECK(!syscall(SYS_settimeofday,NULL,&tz));
    tv.tv_sec=clock_ns(CLOCK_REALTIME)/1000000000;tv.tv_usec=123;
    CHECK(!syscall(SYS_settimeofday,&tv,NULL));check_vdso();restore_wall();
    pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!setuid(123));bad=stamp(clock_ns(0));
        CHECK(syscall(SYS_clock_settime,0,&bad)==-1&&errno==EPERM);
        bad.tv_nsec=-1;CHECK(syscall(SYS_clock_settime,0,&bad)==-1&&errno==EINVAL);
        tz.tz_minuteswest=1000;CHECK(syscall(SYS_settimeofday,NULL,&tz)==-1&&errno==EPERM);
        CHECK(syscall(SYS_settimeofday,NULL,NULL)==-1&&errno==EPERM);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    puts("SYSTEM CASE clock-abi-vdso PASS");
}

struct waiter { int kind; _Atomic int tid,done; long result;int error;struct timespec deadline;uint32_t word,target; };
static mqd_t mq_empty,mq_full;
static void *absolute_wait(void *pointer)
{
    struct waiter *w=pointer;bind_cpu(allowed_cpus[w->kind%cpu_count]);atomic_store(&w->tid,(int)syscall(SYS_gettid));
    if(w->kind==0){w->result=syscall(SYS_clock_nanosleep,CLOCK_REALTIME,TIMER_ABSTIME,&w->deadline,NULL);}
    else if(w->kind==1)w->result=syscall(SYS_futex,&w->word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME,0,&w->deadline,NULL,UINT32_MAX);
    else if(w->kind==2){struct {uint64_t val,uaddr;uint32_t flags,reserved;} wait={0,(uintptr_t)&w->word,FUTEX_32|FUTEX_PRIVATE_FLAG,0};
        w->result=syscall(SYS_futex_waitv,&wait,1,0,&w->deadline,CLOCK_REALTIME);}
    else if(w->kind==3){char byte;w->result=mq_timedreceive(mq_empty,&byte,1,NULL,&w->deadline);}
    else if(w->kind==4)w->result=mq_timedsend(mq_full,"x",1,0,&w->deadline);
    else if(w->kind==5)w->result=syscall(SYS_futex,&w->target,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0,&w->deadline,NULL,0);
    else w->result=syscall(SYS_futex,&w->word,FUTEX_WAIT_REQUEUE_PI|FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME,0,&w->deadline,&w->target,0);
    w->error=errno;atomic_store(&w->done,1);return NULL;
}
static void wait_blocked(struct waiter *w)
{
    uint64_t limit=clock_ns(CLOCK_MONOTONIC)+5000000000ULL;
    for(;;){int tid=atomic_load(&w->tid);CHECK(!atomic_load(&w->done));
        if(tid){char path[80],text[512];snprintf(path,sizeof(path),"/proc/self/task/%d/status",tid);
            int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0&&!close(fd));text[n]=0;
            char *state=strstr(text,"State:\t");if(state&&state[7]=='S')return;}
        CHECK(clock_ns(CLOCK_MONOTONIC)<limit);sched_yield();
    }
}
static void absolute_waits(void)
{
    const char *empty="/clock-empty",*full="/clock-full";mq_unlink(empty);mq_unlink(full);
    struct mq_attr attr={.mq_maxmsg=1,.mq_msgsize=1};
    mq_empty=mq_open(empty,O_CREAT|O_RDWR,0600,&attr);mq_full=mq_open(full,O_CREAT|O_RDWR,0600,&attr);
    CHECK(mq_empty!=(mqd_t)-1&&mq_full!=(mqd_t)-1&&!mq_send(mq_full,"x",1,0));
    struct waiter w[7]={0};pthread_t threads[7];uint64_t now=clock_ns(0);
    for(unsigned i=0;i<7;i++){w[i].kind=i;w[i].deadline=stamp(now+60000000000ULL);w[i].target=(uint32_t)syscall(SYS_gettid);
        CHECK(!pthread_create(&threads[i],NULL,absolute_wait,&w[i]));wait_blocked(&w[i]);}
    /* Every waiter is truly BLOCKED, rather than merely having printed ready. */
    wall_set(now-10000000000ULL);for(unsigned i=0;i<7;i++)CHECK(!atomic_load(&w[i].done));
    wall_set(now+120000000000ULL);
    for(unsigned i=0;i<7;i++){CHECK(!pthread_join(threads[i],NULL));
        CHECK(w[i].result==(i==0?0:-1));if(i)CHECK(w[i].error==ETIMEDOUT);}
    restore_wall();CHECK(!mq_close(mq_empty)&&!mq_close(mq_full)&&!mq_unlink(empty)&&!mq_unlink(full));
    puts("SYSTEM CASE absolute-waits PASS");
}
static void timerfd_clocks(void)
{
    int relative=timerfd_create(CLOCK_REALTIME,TFD_NONBLOCK),absolute=timerfd_create(CLOCK_REALTIME,TFD_NONBLOCK);
    int cancel=timerfd_create(CLOCK_REALTIME,TFD_NONBLOCK);CHECK(relative>=0&&absolute>=0&&cancel>=0);
    struct itimerspec value={.it_value={60,0}};CHECK(!timerfd_settime(relative,0,&value,NULL));
    uint64_t now=clock_ns(0);value.it_value=stamp(now+30000000000ULL);CHECK(!timerfd_settime(absolute,TFD_TIMER_ABSTIME,&value,NULL));
    value.it_value=(struct timespec){0};CHECK(!timerfd_settime(cancel,TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET,&value,NULL));
    wall_set(now+120000000000ULL);uint64_t ticks;
    CHECK(read(cancel,&ticks,8)==-1&&errno==ECANCELED);CHECK(read(cancel,&ticks,8)==-1&&errno==EAGAIN);
    CHECK(read(relative,&ticks,8)==-1&&errno==EAGAIN);
    struct itimerspec left;CHECK(!timerfd_gettime(relative,&left)&&left.it_value.tv_sec>=50);
    struct epoll_event event={.events=EPOLLIN,.data.fd=absolute};int ep=epoll_create1(0);CHECK(ep>=0&&!epoll_ctl(ep,EPOLL_CTL_ADD,absolute,&event));
    CHECK(epoll_wait(ep,&event,1,1000)==1&&read(absolute,&ticks,8)==8&&ticks==1);
    /* Unread cancellation does not prevent a new setting from being committed. */
    wall_set(now+130000000000ULL);value.it_value=stamp(now+180000000000ULL);
    CHECK(timerfd_settime(cancel,TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET,&value,NULL)==-1&&errno==ECANCELED);
    CHECK(!timerfd_gettime(cancel,&left)&&left.it_value.tv_sec>=40);
    CHECK(read(cancel,&ticks,8)==-1&&errno==EAGAIN);
    restore_wall();CHECK(read(cancel,&ticks,8)==-1&&errno==ECANCELED);
    CHECK(!close(ep)&&!close(relative)&&!close(absolute)&&!close(cancel));check_vdso();
    puts("SYSTEM CASE timerfd-clocks PASS");
}
static void posix_clocks(void)
{
    sigset_t set,old;sigemptyset(&set);sigaddset(&set,SIGRTMIN);CHECK(!pthread_sigmask(SIG_BLOCK,&set,&old));
    struct sigevent event={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGRTMIN,.sigev_value.sival_int=41};
    timer_t absolute,relative;CHECK(!timer_create(CLOCK_REALTIME,&event,&absolute));event.sigev_value.sival_int=42;
    CHECK(!timer_create(CLOCK_REALTIME,&event,&relative));
    uint64_t now=clock_ns(0);struct itimerspec value={.it_value=stamp(now+60000000000ULL)};
    CHECK(!timer_settime(absolute,TIMER_ABSTIME,&value,NULL));value.it_value=(struct timespec){60,0};CHECK(!timer_settime(relative,0,&value,NULL));
    wall_set(now+120000000000ULL);siginfo_t info;struct timespec timeout={1,0};
    CHECK(sigtimedwait(&set,&info,&timeout)==SIGRTMIN&&info.si_value.sival_int==41&&info.si_code==SI_TIMER);
    struct itimerspec left;CHECK(!timer_gettime(relative,&left)&&left.it_value.tv_sec>=50);
    CHECK(!timer_delete(absolute)&&!timer_delete(relative));restore_wall();CHECK(!pthread_sigmask(SIG_SETMASK,&old,NULL));
    puts("SYSTEM CASE posix-clocks PASS");
}
/* Merge into the current system-management workload. Requires its CHECK macro;
 * call system_uts_names() while euid 0 before permanently dropping privileges. */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void uts_expect_names(const char *host, const char *domain)
{
    struct utsname name;
    CHECK(!uname(&name));
    CHECK(!strcmp(name.nodename, host) && !strcmp(name.domainname, domain));
}

static void uts_wait(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void system_uts_names(void)
{
    struct utsname saved, current;
    CHECK(!uname(&saved));
    const char *host_path = "/proc/sys/kernel/hostname";
    const char *domain_path = "/proc/sys/kernel/domainname";
    CHECK(!syscall(SYS_sethostname, "boar-host", 9));
    CHECK(!syscall(SYS_setdomainname, "boar.example", 12));
    uts_expect_names("boar-host", "boar.example");
    CHECK(!uname(&current) && !strcmp(current.sysname, saved.sysname) &&
          !strcmp(current.release, saved.release) && !strcmp(current.version, saved.version) &&
          !strcmp(current.machine, saved.machine));
    char full[65]; memset(full, 'h', 64); full[64] = 0;
    CHECK(!syscall(SYS_sethostname, full, 64));
    uts_expect_names(full, "boar.example");
    int fd = open(host_path, O_RDWR); CHECK(fd >= 0);
    char text[128] = {0};
    CHECK(read(fd, text, sizeof(text)) == 65 && !memcmp(text, full, 64) && text[64] == '\n');
    errno = 0; CHECK(syscall(SYS_sethostname, full, 65) == -1 && errno == EINVAL);
    errno = 0; CHECK(syscall(SYS_setdomainname, (void *)-1, -1) == -1 && errno == EINVAL);
    CHECK(!syscall(SYS_sethostname, (void *)-1, 0));
    uts_expect_names("", "boar.example");
    CHECK(!syscall(SYS_sethostname, "abcdef", UINT64_C(0x100000006)));
    uts_expect_names("abcdef", "boar.example");

    /* proc strings read the current name at every offset, then the newline. */
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, text, 2) == 2 && !memcmp(text, "ab", 2));
    CHECK(!syscall(SYS_sethostname, "abXYZ", 5));
    CHECK(read(fd, text, sizeof(text)) == 4 && !memcmp(text, "XYZ\n", 4));
    CHECK(read(fd, text, sizeof(text)) == 0);
    CHECK(lseek(fd, 2, SEEK_SET) == 2 && write(fd, "-tail\nignored", 13) == 13);
    CHECK(lseek(fd, 0, SEEK_CUR) == 15);
    uts_expect_names("ab-tail", "boar.example");
    /* A write beyond the current string is accepted without moving position. */
    CHECK(write(fd, "q", 1) == 1 && lseek(fd, 0, SEEK_CUR) == 15);
    uts_expect_names("ab-tail", "boar.example");
    char long_name[100]; memset(long_name, 'p', sizeof(long_name));
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, long_name, sizeof(long_name)) == sizeof(long_name));
    CHECK(!uname(&current) && strlen(current.nodename) == 64 && current.nodename[63] == 'p');
    const char embedded[] = {'x', 0, 'y'};
    CHECK(!syscall(SYS_sethostname, embedded, sizeof(embedded)) && !uname(&current));
    CHECK(!memcmp(current.nodename, embedded, sizeof(embedded)) && current.nodename[3] == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, "z", 1) == 1 && !uname(&current));
    CHECK(current.nodename[0] == 'z' && !current.nodename[1] && current.nodename[2] == 'y');

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *map = mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map != MAP_FAILED && !mprotect(map + page, page, PROT_NONE));
    map[page - 1] = 'f';
    CHECK(!syscall(SYS_sethostname, "unchanged", 9));
    errno = 0; CHECK(syscall(SYS_sethostname, map + page - 1, 2) == -1 && errno == EFAULT);
    uts_expect_names("unchanged", "boar.example");
    errno = 0; CHECK(syscall(SYS_setdomainname, map + page - 1, 2) == -1 && errno == EFAULT);
    uts_expect_names("unchanged", "boar.example");
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    struct iovec iov[2] = {{(void *)"new", 3}, {map + page, 1}};
    errno = 0; CHECK(writev(fd, iov, 2) == -1 && errno == EFAULT && lseek(fd, 0, SEEK_CUR) == 0);
    uts_expect_names("unchanged", "boar.example");
    errno = 0; CHECK(read(fd, map + page - 1, 2) == -1 && errno == EFAULT);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == 0);
    errno = 0; CHECK(lseek(fd, 0, SEEK_DATA) == -1 && errno == ENXIO);
    errno = 0; CHECK(lseek(fd, 0, SEEK_HOLE) == -1 && errno == ENXIO);
    int domain_fd = open(domain_path, O_RDWR); CHECK(domain_fd >= 0);
    CHECK(write(domain_fd, "proc.domain\n", 12) == 12);
    uts_expect_names("unchanged", "proc.domain");

    /* SYS_ADMIN is a syscall permission; euid 0 still owns the 0644 sysctl. */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct { uint32_t version; int32_t pid; } header = {UINT32_C(0x20080522), 0};
        struct { uint32_t effective, permitted, inheritable; } caps[2] = {{0}};
        CHECK(!syscall(SYS_capget, &header, caps));
        caps[0].effective &= ~(UINT32_C(1) << 21);
        CHECK(!syscall(SYS_capset, &header, caps));
        errno = 0; CHECK(syscall(SYS_sethostname, (void *)-1, -1) == -1 && errno == EPERM);
        errno = 0; CHECK(syscall(SYS_setdomainname, (void *)-1, -1) == -1 && errno == EPERM);
        int root_fd = open(host_path, O_WRONLY); CHECK(root_fd >= 0);
        CHECK(write(root_fd, "proc-root", 9) == 9 && !close(root_fd));
        _exit(0);
    }
    uts_wait(child); uts_expect_names("proc-root", "proc.domain");
    child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(!setgroups(0, 0) && !setgid(1001) && !setuid(1001));
        errno = 0; CHECK(syscall(SYS_sethostname, (void *)-1, -1) == -1 && errno == EPERM);
        errno = 0; CHECK(syscall(SYS_setdomainname, (void *)-1, -1) == -1 && errno == EPERM);
        errno = 0; CHECK(write(fd, map + page, 1) == -1 && errno == EPERM);
        errno = 0; CHECK(open(host_path, O_WRONLY) == -1 && errno == EACCES);
        int reader = open(host_path, O_RDONLY); CHECK(reader >= 0);
        CHECK(read(reader, text, sizeof(text)) == 10 && !memcmp(text, "proc-root\n", 10));
        CHECK(!close(reader)); _exit(0);
    }
    uts_wait(child); uts_expect_names("proc-root", "proc.domain");
    child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(!syscall(SYS_setdomainname, "shared-domain", 13)); _exit(0); }
    uts_wait(child); uts_expect_names("proc-root", "shared-domain");
    CHECK(!close(fd) && !close(domain_fd) && !munmap(map, 2 * page));
    CHECK(!syscall(SYS_sethostname, saved.nodename, strlen(saved.nodename)));
    CHECK(!syscall(SYS_setdomainname, saved.domainname, strlen(saved.domainname)));
    uts_expect_names(saved.nodename, saved.domainname);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));
    for(unsigned i=0;i<CPU_SETSIZE&&cpu_count<8;i++)if(CPU_ISSET(i,&set))allowed_cpus[cpu_count++]=i;
    CHECK(cpu_count);bind_cpu(allowed_cpus[cpu_count-1]);
    printf("SYSTEM CPU setter=%d online=%u\n",sched_getcpu(),cpu_count);
    system_uts_names();clock_abi();absolute_waits();timerfd_clocks();posix_clocks();
    puts("SYSTEM CONTROL PASS cases=5");return 0;
}
