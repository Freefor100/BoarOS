#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/sem.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "CLOCK DISCIPLINE FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
static long adjust(struct timex *t) { return syscall(SYS_adjtimex, t); }
static int64_t ns(struct timespec t) { return t.tv_sec * INT64_C(1000000000) + t.tv_nsec; }
static int64_t read_ns(int clock)
{
    struct timespec t;
    assert(syscall(SYS_clock_gettime, clock, &t) == 0);
    return ns(t);
}
static int (*vdso_clock(void))(clockid_t,struct timespec *)
{
    uintptr_t base=getauxval(AT_SYSINFO_EHDR);
    const Elf64_Ehdr *eh=(void *)base;
    if (!base) return NULL;
    const Elf64_Phdr *ph=(void *)(base+eh->e_phoff);
    const Elf64_Dyn *dyn=NULL;
    const Elf64_Sym *syms=NULL;
    const char *names=NULL;
    const uint32_t *hash=NULL;
    for (unsigned i=0;i<eh->e_phnum;i++)
        if (ph[i].p_type==PT_DYNAMIC) dyn=(void *)(base+ph[i].p_vaddr);
    if (!dyn) return NULL;
    for (;dyn->d_tag!=DT_NULL;dyn++) switch (dyn->d_tag) {
        case DT_STRTAB: names=(void *)(base+dyn->d_un.d_ptr); break;
        case DT_SYMTAB: syms=(void *)(base+dyn->d_un.d_ptr); break;
        case DT_HASH: hash=(void *)(base+dyn->d_un.d_ptr); break;
    }
    if (!hash || !syms || !names) return NULL;
    for (unsigned i=1;i<hash[1];i++)
        if (!strcmp(names+syms[i].st_name,"__vdso_clock_gettime"))
            return (void *)(base+syms[i].st_value);
    return NULL;
}

static struct timespec stamp(int64_t n)
{
    return (struct timespec){n/1000000000,n%1000000000};
}

enum wait_api { WAIT_SLEEP, WAIT_POLL, WAIT_SELECT, WAIT_EPOLL, WAIT_EPOLL2, WAIT_SEM };

static int timed_call(enum wait_api api,int64_t target)
{
    int64_t duration=target-read_ns(CLOCK_MONOTONIC);
    CHECK(duration>0);
    struct timespec at=stamp(target),relative=stamp(duration);
    if (api==WAIT_SLEEP) {
        CHECK(!clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&at,NULL));
    } else if (api==WAIT_POLL) {
        CHECK(!syscall(SYS_ppoll,NULL,0,&relative,NULL,0));
    } else if (api==WAIT_SELECT) {
        CHECK(!syscall(SYS_pselect6,0,NULL,NULL,NULL,&relative,NULL));
    } else if (api==WAIT_EPOLL || api==WAIT_EPOLL2) {
        int ep=epoll_create1(0);
        struct epoll_event event;
        CHECK(ep>=0);
        if (api==WAIT_EPOLL)
            CHECK(!syscall(SYS_epoll_pwait,ep,&event,1,(duration+999999)/1000000,NULL,0));
        else
            CHECK(!syscall(441,ep,&event,1,&relative,NULL,0)); /* epoll_pwait2 */
        CHECK(!close(ep));
    } else {
        int sem=semget(IPC_PRIVATE,1,0600);
        struct sembuf op={.sem_num=0,.sem_op=-1};
        CHECK(sem>=0 && semtimedop(sem,&op,1,&relative)==-1 && errno==EAGAIN);
        CHECK(!semctl(sem,0,IPC_RMID));
    }
    return 0;
}

/* At the original rate, the hardware alarms would be about 85 ms late. */
static int retarget_blocked(enum wait_api api)
{
    struct timex t={.modes=ADJ_TICK,.tick=9000};
    CHECK(adjust(&t)>=0);
    int channel[2];
    CHECK(!pipe(channel));
    int fd=timerfd_create(CLOCK_MONOTONIC,0);
    CHECK(fd>=0);
    int64_t target=read_ns(CLOCK_MONOTONIC)+500000000;
    struct itimerspec timer={.it_value=stamp(target)};
    CHECK(!timerfd_settime(fd,TFD_TIMER_ABSTIME,&timer,NULL));
    pid_t pid=fork();
    CHECK(pid>=0);
    if (!pid) {
        CHECK(!close(channel[0]));
        CHECK(!timed_call(api,target));
        int64_t done=read_ns(CLOCK_MONOTONIC);
        CHECK(write(channel[1],&done,sizeof(done))==sizeof(done));
        _exit(0);
    }
    CHECK(!close(channel[1]));
    CHECK(!nanosleep(&(struct timespec){.tv_nsec=40000000},NULL));
    t=(struct timex){.modes=ADJ_TICK,.tick=11000};
    CHECK(adjust(&t)>=0);
    uint64_t count;
    CHECK(read(fd,&count,sizeof(count))==sizeof(count) && count==1);
    int64_t fired=read_ns(CLOCK_MONOTONIC),woke;
    CHECK(read(channel[0],&woke,sizeof(woke))==sizeof(woke));
    CHECK(woke>=target && woke-target<60000000);
    CHECK(fired>=target && fired-target<60000000);
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!close(channel[0]) && !close(fd));
    return 0;
}

int main(void)
{
    _Static_assert(sizeof(struct timex) == 208, "native 64-bit timex");
    struct timex t = {0};
    CHECK(adjust(&t) >= 0 && t.precision == 1 && t.tick == 10000);
    CHECK(syscall(SYS_clock_adjtime, CLOCK_MONOTONIC, &t) == -1 && errno == EOPNOTSUPP);
    CHECK(syscall(SYS_clock_adjtime, 99, &t) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_clock_adjtime, 99, (void *)1) == -1 && errno == EFAULT);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setuid(65534) == 0 && adjust(&t) >= 0);
        t.modes = ADJ_OFFSET_SS_READ;
        CHECK(adjust(&t) >= 0);
        t.modes = ADJ_FREQUENCY;
        CHECK(adjust(&t) == -1 && errno == EPERM);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    t = (struct timex){.modes = ADJ_TICK, .tick = 20000};
    CHECK(adjust(&t) == -1 && errno == EINVAL);
    /* adjtimex copies back even after validation failure. clock_adjtime
     * copies back only after a nonnegative return. */
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    struct timex *ro=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(ro!=MAP_FAILED);
    *ro=t;
    CHECK(!mprotect(ro,page,PROT_READ));
    CHECK(adjust(ro)==-1 && errno==EFAULT);
    CHECK(syscall(SYS_clock_adjtime,CLOCK_REALTIME,ro)==-1 && errno==EINVAL);
    CHECK(!munmap(ro,page));
    t = (struct timex){.modes = ADJ_SETOFFSET, .time.tv_usec = -1};
    CHECK(adjust(&t) == -1 && errno == EINVAL);
    t = (struct timex){.modes = ADJ_TICK | ADJ_FREQUENCY | ADJ_STATUS | ADJ_MAXERROR,
                       .tick = 11000, .freq = 600 * 65536L, .status = STA_PLL};
    CHECK(syscall(SYS_clock_adjtime, CLOCK_REALTIME, &t) == TIME_OK && t.freq == 500 * 65536L);
    int64_t r0 = read_ns(CLOCK_MONOTONIC_RAW), m0 = read_ns(CLOCK_MONOTONIC);
    CHECK(nanosleep(&(struct timespec){.tv_nsec=250000000}, NULL) == 0);
    int64_t raw = read_ns(CLOCK_MONOTONIC_RAW)-r0, mono = read_ns(CLOCK_MONOTONIC)-m0;
    CHECK(mono-raw > 10000000 && mono-raw < 60000000);
    int (*get)(clockid_t,struct timespec *)=vdso_clock();
    CHECK(get);
    for (int c = 0; c <= CLOCK_BOOTTIME; c++) {
        if (c == 2 || c == 3) continue;
        int64_t before = read_ns(c);
        struct timespec sample;
        CHECK(!get(c, &sample) && ns(sample) >= before && ns(sample) <= read_ns(c));
    }
    for (enum wait_api api=WAIT_SLEEP;api<=WAIT_SEM;api++) {
        if (retarget_blocked(api)) {
            dprintf(2,"CLOCK DISCIPLINE wait api=%d failed\n",api);
            return 1;
        }
    }
    t = (struct timex){.modes = ADJ_TICK | ADJ_FREQUENCY | ADJ_NANO | ADJ_OFFSET, .tick = 10000, .offset = 1000000};
    CHECK(adjust(&t) == TIME_OK && (t.status & STA_NANO) && t.offset > 0);
    t = (struct timex){.modes = ADJ_OFFSET_SINGLESHOT, .offset = -5000};
    CHECK(adjust(&t) == TIME_OK && t.offset == 0);
    t.modes = ADJ_OFFSET_SS_READ;
    CHECK(adjust(&t) == TIME_OK && t.offset < -4000 && t.offset >= -5000);
    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    CHECK(fd >= 0);
    struct itimerspec spec = {0};
    CHECK(!clock_gettime(CLOCK_REALTIME, &spec.it_value));
    spec.it_value.tv_sec++;
    CHECK(!timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, NULL));
    t = (struct timex){.modes = ADJ_FREQUENCY};
    CHECK(adjust(&t) == TIME_OK);
    uint64_t count;
    CHECK(read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    int64_t wall = read_ns(CLOCK_REALTIME);
    t = (struct timex){.modes = ADJ_SETOFFSET, .time.tv_sec = 2};
    CHECK(adjust(&t) == TIME_ERROR && read_ns(CLOCK_REALTIME) >= wall + 2000000000);
    CHECK(read(fd, &count, sizeof(count)) == -1 && errno == ECANCELED);
    CHECK(close(fd) == 0);
    puts("CLOCK DISCIPLINE PASS");
    fflush(stdout);
    if (getpid()==1) { sync(); reboot(RB_POWER_OFF); }
    return 0;
}
