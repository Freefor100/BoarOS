#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../smp/composition_protocol.h"

#define CHECK(v) do { if (!(v)) { dprintf(2,"COMPOSITION FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while(0)
static struct composition_control *control;
static uint64_t online, request, wait_epoch;
static int controlled, observed, control_fd;
static _Thread_local sigjmp_buf fault_return;
static _Thread_local int fault_expected;
static void pin(unsigned cpu)
{ uint64_t mask=UINT64_C(1)<<(cpu && (online&2) ? 1 : 0);CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask)); }
static uint64_t now(void)
{ struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void wait_value(uint64_t *value,uint64_t expected)
{
    uint64_t end=now()+UINT64_C(10000000000);
    while (composition_load(value)!=expected) { CHECK(now()<end);sched_yield(); }
}
static void command(unsigned action)
{
    if (!controlled && !observed) return;
    if (!controlled && action!=COMPOSITION_STOP && action!=COMPOSITION_FAULT_RACE &&
        action!=COMPOSITION_FILE_FAULT_RACE) return;
    composition_store(&control->command,action);
    composition_store(&control->request,++request);
    wait_value(&control->reply,request);
}
static void record(const char *name)
{ printf("COMPOSITION CASE %s OK\n",name);fflush(stdout); }
static void fault(int signal)
{ CHECK(signal==fault_expected);siglongjmp(fault_return,1); }
static void setup(void)
{
    CHECK(syscall(SYS_sched_getaffinity,0,sizeof(online),&online)==sizeof(online));pin(0);
    controlled=!access("/controlled",F_OK);
    observed=controlled || !access("/observer",F_OK);
    CHECK(!mkdir("/proc",0755) || errno==EEXIST);
    CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY);
    CHECK(!mkdir("/dev/shm",01777) || errno==EEXIST);
    CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,"size=16777216") || errno==EBUSY);
    control_fd=open("/dev/shm/smp-control",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(control_fd>=0);
    CHECK(!ftruncate(control_fd,4096));
    control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control_fd,0);CHECK(control!=MAP_FAILED);
    memset(control,0,4096);composition_store(&control->magic,COMPOSITION_MAGIC);
    if (observed) wait_value(&control->observer_ready,1);
    struct sigaction action={.sa_handler=fault};sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV,&action,0) && !sigaction(SIGBUS,&action,0));
}
struct mapping_test { unsigned char *anonymous,*file;uint64_t phase,ack,ready; };
static void *remote_mapping(void *opaque)
{
    struct mapping_test *test=opaque;pin(1);
    CHECK(test->anonymous[0]==17 && test->file[4096]==19);
    composition_store(&test->ready,1);
    uint64_t completed=0;
    while (completed<4) {
        __atomic_fetch_add(&control->remote_progress,1,__ATOMIC_RELAXED);
        if (composition_load(&control->retirement_entered)) {
            __atomic_fetch_add(&control->remote_progress,1,__ATOMIC_RELAXED);
            composition_store(&control->retirement_release,1);
        }
        uint64_t phase=composition_load(&test->phase);
        if (phase==completed) continue;
        CHECK(phase==completed+1);
        if (phase==1) CHECK(test->anonymous[0]==42);
        else if (phase==2 || phase==3) {
            fault_expected=phase==2 ? SIGSEGV : SIGBUS;
            if (!sigsetjmp(fault_return,1)) {
                if (phase==2) test->anonymous[0]=99;
                else { volatile unsigned char byte=test->file[4096];(void)byte; }
                CHECK(0);
            }
        } else CHECK(test->file[4096]==0);
        completed=phase;composition_store(&test->ack,completed);
    }
    return 0;
}
struct fault_race_test { unsigned char *page;uint64_t ready[2],go; };
struct fault_race_writer { struct fault_race_test *test;unsigned index; };
static void *fault_race_write(void *opaque)
{
    struct fault_race_writer *writer=opaque;
    unsigned index=writer->index;
    pin(index);
    control->fault_tasks[index]=syscall(SYS_gettid);
    composition_store(&writer->test->ready[index],1);
    while (!composition_load(&writer->test->go)) sched_yield();
    writer->test->page[index*64]=(unsigned char)(0x31+index);
    return 0;
}
static void concurrent_first_fault(void)
{
    struct fault_race_test test={0};
    struct fault_race_writer writers[2]={{&test,0},{&test,1}};
    pthread_t threads[2];
    test.page=mmap(0,4096,PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(test.page!=MAP_FAILED);
    CHECK(!pthread_create(&threads[0],0,fault_race_write,&writers[0]) &&
          !pthread_create(&threads[1],0,fault_race_write,&writers[1]));
    for (unsigned i=0;i<2;i++) wait_value(&test.ready[i],1);
    if (observed) {
        control->fault_address=(uintptr_t)test.page;
        command(COMPOSITION_FAULT_RACE);
    }
    composition_store(&test.go,1);
    CHECK(!pthread_join(threads[0],0) && !pthread_join(threads[1],0));
    CHECK(test.page[0]==0x31 && test.page[64]==0x32);
    CHECK(!munmap(test.page,4096));
    record("concurrent-first-fault");
}
struct file_fault_test {
    const unsigned char *page;
    uint64_t ready[2], go[2], value[2];
};
struct file_fault_reader { struct file_fault_test *test; unsigned index; };
static void *file_fault_read(void *opaque)
{
    struct file_fault_reader *reader=opaque;
    struct file_fault_test *test=reader->test;
    unsigned index=reader->index;
    pin(index);
    control->fault_tasks[index]=(uint64_t)syscall(SYS_gettid);
    composition_store(&test->ready[index],1);
    wait_value(&test->go[index],1);
    test->value[index]=*(volatile const unsigned char *)test->page;
    return 0;
}
static void concurrent_file_first_fault(void)
{
    int fd=open("/file-first-fault",O_CREAT|O_RDWR|O_TRUNC,0600);
    CHECK(fd>=0 && !ftruncate(fd,4096));
    unsigned char contents[4096];memset(contents,0x5d,sizeof(contents));
    CHECK(write(fd,contents,sizeof(contents))==(ssize_t)sizeof(contents));
    const unsigned char *page=mmap(0,4096,PROT_READ,MAP_SHARED,fd,0);
    CHECK(page!=MAP_FAILED);
    struct file_fault_test test={.page=page};
    struct file_fault_reader readers[2]={{&test,0},{&test,1}};
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0],0,file_fault_read,&readers[0]) &&
          !pthread_create(&threads[1],0,file_fault_read,&readers[1]));
    for (unsigned i=0;i<2;i++) wait_value(&test.ready[i],1);
    if (observed) {
        control->fault_address=(uintptr_t)page;
        command(COMPOSITION_FILE_FAULT_RACE);
        composition_store(&test.go[0],1);
        wait_value(&control->fault_file_paused,1);
        composition_store(&test.go[1],1);
    } else {
        composition_store(&test.go[0],1);
        composition_store(&test.go[1],1);
    }
    CHECK(!pthread_join(threads[0],0) && !pthread_join(threads[1],0));
    CHECK(test.value[0]==0x5d && test.value[1]==0x5d && page[0]==0x5d);
    CHECK(!munmap((void *)page,4096) && !close(fd));
    record("concurrent-file-first-fault");
}
static void mappings(void)
{
    int file=open("/mapped-pressure",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(file>=0);
    unsigned char page[4096];memset(page,19,sizeof(page));
    for (unsigned i=0;i<1024;i++) CHECK(write(file,page,sizeof(page))==sizeof(page));
    CHECK(!fsync(file));
    struct mapping_test test={0};
    test.file=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,file,0);CHECK(test.file!=MAP_FAILED);
    test.anonymous=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(test.anonymous!=MAP_FAILED);
    test.anonymous[0]=17;
    pthread_t thread;CHECK(!pthread_create(&thread,0,remote_mapping,&test));wait_value(&test.ready,1);
    uint64_t before=composition_load(&control->remote_progress);command(COMPOSITION_PRESSURE);
    uint64_t progress_end=now()+UINT64_C(10000000000);
    while (composition_load(&control->remote_progress)<=before) { CHECK(now()<progress_end);sched_yield(); }
    control->target_tid=syscall(SYS_gettid);control->target_address=(uintptr_t)test.anonymous;
    command(COMPOSITION_RFENCE);CHECK(!munmap(test.anonymous,4096));
    CHECK(mmap(test.anonymous,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==test.anonymous);
    test.anonymous[0]=42;composition_store(&test.phase,1);wait_value(&test.ack,1);
    CHECK(!mprotect(test.anonymous,4096,PROT_READ));composition_store(&test.phase,2);wait_value(&test.ack,2);
    test.file[0]=31;CHECK(!msync(test.file,8192,MS_SYNC) && !fsync(file));
    CHECK(!ftruncate(file,4096));composition_store(&test.phase,3);wait_value(&test.ack,3);
    CHECK(!ftruncate(file,8192));composition_store(&test.phase,4);wait_value(&test.ack,4);
    CHECK(!pthread_join(thread,0));command(COMPOSITION_RELEASE);
    if (controlled) {
        control->target_tid=syscall(SYS_gettid);command(COMPOSITION_OOM);
        errno=0;pid_t failed=fork();CHECK(failed==-1 && errno==ENOMEM);
        CHECK(test.anonymous[0]==42 && test.file[0]==31);
    }
    pid_t child=fork();CHECK(child>=0);if (!child) _exit(test.anonymous[0]==42 ? 0 : 1);
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(!munmap(test.anonymous,4096) && !munmap(test.file,8192) && !close(file));
    CHECK(!unlink("/mapped-pressure"));record("mm-reclaim");
}
struct waiter { unsigned index;int fd,futex;uint64_t ready;unsigned char value; };
static uint32_t futex_value;
static void *waiter(void *opaque)
{
    struct waiter *test=opaque;pin(test->index&1);
    composition_store(&control->participants[test->index],syscall(SYS_gettid));
    composition_store(&test->ready,1);
    if (test->futex) {
        while (!__atomic_load_n(&futex_value,__ATOMIC_ACQUIRE))
            CHECK(!syscall(SYS_futex,&futex_value,128,0,0,0,0) || errno==EAGAIN || errno==EINTR);
    } else CHECK(read(test->fd,&test->value,1)==1 && test->value=='A');
    return 0;
}
static void epoch_begin(void)
{
    composition_store(&control->epoch,0);
    for (unsigned i=0;i<32;i++) composition_store(&control->participants[i],0);
    composition_store(&control->registered,0);composition_store(&control->epoch,++wait_epoch);
}
static void wait_registered(unsigned count)
{
    if (observed) { wait_value(&control->registered,count);return; }
    /* Linux/普通核没有测试挂钩；实际S状态确认空pipe/futex调用已阻塞，
     * 不把调用之前的ready标志当作fd pin或等待登记证明。 */
    uint64_t end=now()+UINT64_C(10000000000);
    for (;;) {
        unsigned sleeping=0;
        for (unsigned i=0;i<count;i++) {
            char path[64],buffer[512];snprintf(path,sizeof(path),"/proc/%llu/stat",(unsigned long long)composition_load(&control->participants[i]));
            int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t length=read(fd,buffer,sizeof(buffer)-1);CHECK(length>0 && !close(fd));buffer[length]=0;
            char *end_name=strrchr(buffer,')');CHECK(end_name);sleeping+=end_name[2]=='S';
        }
        if (sleeping==count) return;
        CHECK(now()<end);sched_yield();
    }
}
static void waiters(unsigned count,int use_futex)
{
    int pipefd[2],replacement[2];CHECK(!pipe(pipefd) && !pipe(replacement));
    struct waiter tests[32]={0};pthread_t threads[32];futex_value=0;epoch_begin();
    for (unsigned i=0;i<count;i++) {
        tests[i]=(struct waiter){.index=i,.fd=pipefd[0],.futex=use_futex};
        CHECK(!pthread_create(&threads[i],0,waiter,&tests[i]));
    }
    for (unsigned i=0;i<count;i++) wait_value(&tests[i].ready,1);
    wait_registered(count);
    unsigned cancelled=use_futex ? 0 : count>1 ? 2 : 1;
    if (use_futex) {
        __atomic_store_n(&futex_value,1,__ATOMIC_RELEASE);
        CHECK(syscall(SYS_futex,&futex_value,129,INT_MAX,0,0,0)>=0);
    } else {
        CHECK(!close(pipefd[0]) && dup2(replacement[0],pipefd[0])==pipefd[0]);
        CHECK(write(replacement[1],"Z",1)==1);
        for (unsigned i=0;i<cancelled;i++) CHECK(!pthread_cancel(threads[i]));
        /* 取消只是请求；先确认完成，再发布数据，避免合法成功read抢先结束。 */
        for (unsigned i=0;i<cancelled;i++) { void *result=0;
            CHECK(!pthread_join(threads[i],&result) && result==PTHREAD_CANCELED); }
        if (count>cancelled) { char bytes[32];memset(bytes,'A',sizeof(bytes));CHECK(write(pipefd[1],bytes,count-cancelled)==(ssize_t)(count-cancelled)); }
    }
    for (unsigned i=cancelled;i<count;i++) { void *result=0;CHECK(!pthread_join(threads[i],&result));CHECK(!result); }
    if (!use_futex) { char value;CHECK(read(pipefd[0],&value,1)==1 && value=='Z'); }
    CHECK(!close(pipefd[0]) && !close(pipefd[1]) && !close(replacement[0]) && !close(replacement[1]));
    char name[40];snprintf(name,sizeof(name),"%s-%u",use_futex ? "futex" : "fd-pipe",count);record(name);
}
static void group_exit(unsigned count)
{
    int pipefd[2];CHECK(!pipe(pipefd));epoch_begin();
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        struct waiter tests[32]={0};pthread_t threads[32];
        for (unsigned i=0;i<count;i++) { tests[i]=(struct waiter){.index=i,.fd=pipefd[0]};CHECK(!pthread_create(&threads[i],0,waiter,&tests[i])); }
        for (;;) sched_yield();
    }
    for (unsigned i=0;i<count;i++) { uint64_t end=now()+UINT64_C(10000000000);
        while (!composition_load(&control->participants[i])) { CHECK(now()<end);sched_yield(); } }
    wait_registered(count);CHECK(!kill(child,SIGTERM));
    int status;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM);
    CHECK(!close(pipefd[0]) && !close(pipefd[1]));
    char name[40];snprintf(name,sizeof(name),"group-exit-%u",count);record(name);
}
static uint64_t compute_stop;
static void *compute(void *unused)
{
    (void)unused;pin(1);uint64_t value=1;
    while (!composition_load(&compute_stop)) { value=(value<<3)^(value>>7)^17;
        __atomic_fetch_add(&control->remote_progress,1,__ATOMIC_RELAXED); }
    return (void *)(uintptr_t)value;
}
static void sqlite(void)
{
    pthread_t thread;CHECK(!pthread_create(&thread,0,compute,0));
    uint64_t end=now()+UINT64_C(10000000000);
    while (!composition_load(&control->remote_progress)) { CHECK(now()<end);sched_yield(); }
    command(COMPOSITION_PRESSURE);uint64_t before=composition_load(&control->remote_progress);
    pid_t child=fork();CHECK(child>=0);if (!child) { execl("/init","/init",(char *)0);_exit(127); }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==42);
    CHECK(composition_load(&control->remote_progress)>before);command(COMPOSITION_RELEASE);
    composition_store(&compute_stop,1);CHECK(!pthread_join(thread,0));
    record("sqlite-pressure");puts("COMPOSITION END 1");
}
int main(void)
{
    setup();
    if (!access("/sqlite-scene",F_OK)) sqlite();
    else {
        concurrent_first_fault();
        concurrent_file_first_fault();
        mappings();
        for (unsigned count=1;count<=32;count=count==1 ? 8 : 32) {
            waiters(count,0);waiters(count,1);group_exit(count);if (count==32) break;
        }
        puts("COMPOSITION END 12");
    }
    command(COMPOSITION_STOP);
    CHECK(!munmap(control,4096) && !close(control_fd) && !unlink("/dev/shm/smp-control"));
    sync();fflush(stdout);reboot(RB_POWER_OFF);return 42;
}
