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
#include <sys/uio.h>
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
        action!=COMPOSITION_FILE_FAULT_RACE && action!=COMPOSITION_READ_ARM &&
        action!=COMPOSITION_READ_FINISH && action!=COMPOSITION_WAIT_ARM &&
        action!=COMPOSITION_WAIT_FINISH && action!=COMPOSITION_CLOCK_ARM &&
        action!=COMPOSITION_CLOCK_FINISH) return;
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

struct ordinary_read {
    int fd, operation, signal_wait;
    unsigned char *buffer;
    size_t size;
    off_t offset;
    uint64_t tid,ready,go,done;
    ssize_t result;
};
static volatile sig_atomic_t read_signal_seen;
static void read_signal(int number) { CHECK(number==SIGUSR1);read_signal_seen=1; }
static void *ordinary_reader(void *opaque)
{
    struct ordinary_read *test=opaque;pin(1);
    test->tid=(uint64_t)syscall(SYS_gettid);
    composition_store(&test->ready,1);wait_value(&test->go,1);
    if (test->operation==1) test->result=pread(test->fd,test->buffer,test->size,test->offset);
    else if (test->operation==2) {
        struct iovec vector[2]={{test->buffer,5},{test->buffer+32,test->size-5}};
        test->result=readv(test->fd,vector,2);
    } else if(test->operation==3) test->result=write(test->fd,test->buffer,test->size);
    else if(test->operation==4) test->result=pwrite(test->fd,test->buffer,test->size,test->offset);
    else if(test->operation==5) {
        struct iovec vector[2]={{test->buffer,5},{test->buffer+32,test->size-5}};
        test->result=writev(test->fd,vector,2);
    } else test->result=read(test->fd,test->buffer,test->size);
    composition_store(&test->done,1);
    if(test->signal_wait) { uint64_t end=now()+UINT64_C(10000000000);
        while(!read_signal_seen) { CHECK(now()<end);sched_yield(); } }
    return 0;
}
static void read_arm(struct ordinary_read *test,unsigned flags)
{
    composition_store(&control->read_tid,test->tid);
    composition_store(&control->read_address,(uintptr_t)test->buffer);
    composition_store(&control->read_size,(test->operation==2 || test->operation==5) ? 64 : test->size);
    composition_store(&control->read_hart,(online&2) ? 1 : 0);
    composition_store(&control->read_flags,flags);
    composition_store(&control->read_entered,0);composition_store(&control->read_release,0);
    composition_store(&control->read_waiter_tid,0);composition_store(&control->read_waited,0);
    command(COMPOSITION_READ_ARM);composition_store(&test->go,1);
}
static void read_run(struct ordinary_read *test,unsigned flags)
{
    pthread_t thread;CHECK(!pthread_create(&thread,0,ordinary_reader,test));
    wait_value(&test->ready,1);read_arm(test,flags);
    CHECK(!pthread_join(thread,0));command(COMPOSITION_READ_FINISH);
}
static void *ordinary_seeker(void *opaque)
{
    struct ordinary_read *test=opaque;pin(1);
    test->tid=(uint64_t)syscall(SYS_gettid);composition_store(&test->ready,1);
    wait_value(&test->go,1);test->result=lseek(test->fd,0,SEEK_CUR);return 0;
}
static void *ordinary_sequential_reader(void *opaque)
{
    struct ordinary_read *test=opaque;pin(1);
    CHECK(read(test->fd,test->buffer,4096)==4096 &&
        read(test->fd,test->buffer+4096,4096)==4096);
    for(unsigned i=0;i<8192;i++)CHECK(test->buffer[i]==(unsigned char)(i*13+7));
    /* 第二个整页的顺序进度可排第三页预读，seek只取消已有计划。 */
    CHECK(!lseek(test->fd,0,SEEK_SET));
    return 0;
}
static void ordinary_reads(void)
{
    int fd=open("/ordinary-read-source",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(fd>=0);
    unsigned char source[4096];for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i*13+7);
    for(unsigned i=0;i<16;i++)CHECK(write(fd,source,sizeof(source))==sizeof(source));
    CHECK(!fsync(fd));
    unsigned char *buffer=mmap(0,17*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(buffer!=MAP_FAILED);memset(buffer,0,2*4096);
    CHECK(!lseek(fd,0,SEEK_SET));
    struct ordinary_read test={.fd=fd,.buffer=buffer,.size=16};
    read_run(&test,COMPOSITION_READ_LOCAL);CHECK(test.result==16 && !memcmp(buffer,source,16));
    test=(struct ordinary_read){.fd=fd,.operation=1,.buffer=buffer,.size=16,.offset=128};
    read_run(&test,COMPOSITION_READ_LOCAL);CHECK(test.result==16 && !memcmp(buffer,source+128,16));
    test=(struct ordinary_read){.fd=fd,.operation=2,.buffer=buffer,.size=16};
    read_run(&test,COMPOSITION_READ_LOCAL);
    CHECK(test.result==16 && !memcmp(buffer,source+16,5) && !memcmp(buffer+32,source+21,11));
    CHECK(lseek(fd,0,SEEK_CUR)==32);record("ordinary-hot-read-pread-readv");

    test=(struct ordinary_read){.fd=fd,.operation=1,.buffer=buffer,.size=16,.offset=64};
    read_run(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_COLD);
    CHECK(test.result==16 && !memcmp(buffer,source+64,16));record("ordinary-cold-cache");
    test=(struct ordinary_read){.fd=fd,.operation=1,.buffer=buffer,.size=65536};
    read_run(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_COLD|COMPOSITION_READ_BATCH);
    CHECK(test.result==65536);
    for(unsigned i=0;i<16;i++)CHECK(!memcmp(buffer+i*4096,source,4096));
    record("ordinary-cold-batch-local");
    CHECK(!munmap(buffer+8192,4096) && mmap(buffer+8192,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==buffer+8192);
    test=(struct ordinary_read){.fd=fd,.operation=1,.buffer=buffer+8192,.size=16,.offset=96};
    read_run(&test,COMPOSITION_READ_LOCAL);
    CHECK(test.result==16 && !memcmp(buffer+8192,source+96,16));record("ordinary-user-fault");
    CHECK(!mprotect(buffer+4096,4096,PROT_NONE) && !lseek(fd,0,SEEK_SET));
    test=(struct ordinary_read){.fd=fd,.buffer=buffer+4094,.size=4};
    read_run(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_PREFIX);
    CHECK(test.result==2 && !memcmp(buffer+4094,source,2) && lseek(fd,0,SEEK_CUR)==2);
    CHECK(!mprotect(buffer+4096,4096,PROT_READ|PROT_WRITE));record("ordinary-read-fault-prefix");

    CHECK(!munmap(buffer+4096,4096) &&
        mmap(buffer+4096,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==buffer+4096);
    CHECK(!lseek(fd,0,SEEK_SET));
    test=(struct ordinary_read){.fd=fd,.buffer=buffer+4094,.size=16};
    struct ordinary_read seek={.fd=fd};pthread_t reader,seeker;
    CHECK(!pthread_create(&reader,0,ordinary_reader,&test) &&
        !pthread_create(&seeker,0,ordinary_seeker,&seek));
    wait_value(&test.ready,1);wait_value(&seek.ready,1);
    read_arm(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_HOLD|COMPOSITION_READ_SEEK);
    if(observed)wait_value(&control->read_entered,1);
    else CHECK(!pthread_join(reader,0));
    composition_store(&control->read_waiter_tid,seek.tid);composition_store(&seek.go,1);
    if(observed)CHECK(!pthread_join(reader,0));
    CHECK(!pthread_join(seeker,0));command(COMPOSITION_READ_FINISH);
    CHECK(test.result==16 && seek.result==16 && !memcmp(buffer+4094,source,16));record("ordinary-read-seek");

    CHECK(!lseek(fd,0,SEEK_SET));
    struct sigaction action={.sa_handler=read_signal};sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,0));
    test=(struct ordinary_read){.fd=fd,.buffer=buffer,.size=16,.signal_wait=1};read_signal_seen=0;
    CHECK(!pthread_create(&reader,0,ordinary_reader,&test));wait_value(&test.ready,1);
    read_arm(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_HOLD|COMPOSITION_READ_CLOSE);
    if(observed)wait_value(&control->read_entered,1);
    else wait_value(&test.done,1);
    CHECK(!close(fd));int replacement=open("/ordinary-read-replacement",O_CREAT|O_RDWR|O_TRUNC,0600);
    CHECK(replacement==fd && write(replacement,"replacement",11)==11);
    CHECK(!pthread_kill(reader,SIGUSR1));
    if(observed)composition_store(&control->read_release,1);
    CHECK(!pthread_join(reader,0));
    command(COMPOSITION_READ_FINISH);
    CHECK(test.result==16 && !memcmp(buffer,source,16));
    CHECK(read_signal_seen);
    CHECK(pread(replacement,buffer,11,0)==11 && !memcmp(buffer,"replacement",11));
    CHECK(!close(replacement) && !unlink("/ordinary-read-replacement"));record("ordinary-read-close-signal");

    fd=open("/ordinary-read-source",O_RDONLY);CHECK(fd>=0);memset(buffer,0,16);
    test=(struct ordinary_read){.fd=fd,.buffer=buffer,.size=16};
    CHECK(!pthread_create(&reader,0,ordinary_reader,&test));wait_value(&test.ready,1);
    read_arm(&test,COMPOSITION_READ_LOCAL|COMPOSITION_READ_HOLD|COMPOSITION_READ_FORK);
    if(observed)wait_value(&control->read_entered,1);else CHECK(!pthread_join(reader,0));
    composition_store(&control->read_waiter_tid,(uint64_t)syscall(SYS_gettid));
    /* 绕过libc的atfork锁，直接clone检查内核排写pin的握手。 */
    pid_t child=(pid_t)syscall(SYS_clone,SIGCHLD,0,0,0,0);
    CHECK(child>=0);if(!child)_exit(!memcmp(buffer,source,16) ? 0 : 1);
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    if(observed)CHECK(!pthread_join(reader,0));
    command(COMPOSITION_READ_FINISH);
    CHECK(test.result==16 && !memcmp(buffer,source,16));
    /* 观察phase已结束；再次检查顺序读取，不把可选预读混入需求批次记录。 */
    CHECK(pread(fd,buffer,8192,0)==8192 && !lseek(fd,0,SEEK_SET));
    CHECK(!pthread_create(&reader,0,ordinary_sequential_reader,&test) && !pthread_join(reader,0));
    CHECK(!close(fd) && !unlink("/ordinary-read-source"));
    record("ordinary-read-fork-pin");
    int output=open("/ordinary-write-target",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(output>=0);
    memcpy(buffer,"local-data-write",16);memcpy(buffer+32,"vector-tail",11);
    for(unsigned operation=3;operation<=5;operation++) {
        test=(struct ordinary_read){.fd=output,.operation=(int)operation,.buffer=buffer,.size=16,.offset=32};
        read_run(&test,COMPOSITION_WRITE_LOCAL);CHECK(test.result==16);
    }
    CHECK(lseek(output,0,SEEK_CUR)==32 && !fsync(output));
    unsigned char actual[48],expected[48];
    memcpy(expected,buffer,16);memcpy(expected+16,buffer,5);memcpy(expected+21,buffer+32,11);memcpy(expected+32,buffer,16);
    CHECK(pread(output,actual,sizeof(actual),0)==sizeof(actual) && !memcmp(actual,expected,sizeof(actual)));
    pin(1);
    CHECK(!ftruncate(output,3*4096+17));
    CHECK(!fallocate(output,FALLOC_FL_KEEP_SIZE,6*4096,4096));
    struct stat space_stat;CHECK(!fstat(output,&space_stat) && space_stat.st_size==3*4096+17);
    CHECK(!fallocate(output,0,4*4096,4096));
    CHECK(!fstat(output,&space_stat) && space_stat.st_size==5*4096);
    unsigned char *shared=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,output,0);
    unsigned char *private=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE,output,0);
    CHECK(shared!=MAP_FAILED && private!=MAP_FAILED && !shared[4096+7]);
    private[4096+7]=0x6c;shared[4096+7]=0x5a;
    CHECK(!msync(shared,8192,MS_SYNC) && !fsync(output));
    CHECK(!fallocate(output,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,4096+3,4096-5));
    CHECK(!shared[4096+7] && private[4096+7]==0x6c && !fdatasync(output));
    CHECK(!munmap(shared,8192) && !munmap(private,8192) && !ftruncate(output,sizeof(actual)));
    CHECK(lseek(output,0,SEEK_CUR)==32 && pread(output,actual,sizeof(actual),0)==sizeof(actual) &&
        !memcmp(actual,expected,sizeof(actual)) && !fsync(output));
    pin(0);
    CHECK(!close(output) && !unlink("/ordinary-write-target"));record("ordinary-file-write-space-sync-local");
    CHECK(!munmap(buffer,17*4096));
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
    /* fd已关闭时，最后VMA仍拥有文件；撤映射和固定替换都要归还该owner。 */
    file=open("/mapped-pressure",O_RDONLY);CHECK(file>=0);
    unsigned char *retired=mmap(0,4096,PROT_READ,MAP_SHARED,file,0);CHECK(retired!=MAP_FAILED);
    CHECK(retired[0]==31 && !close(file) && !munmap(retired,4096));
    file=open("/mapped-pressure",O_RDONLY);CHECK(file>=0);
    unsigned char value=0;CHECK(pread(file,&value,1,0)==1 && value==31);
    retired=mmap(0,4096,PROT_READ,MAP_SHARED,file,0);CHECK(retired!=MAP_FAILED);
    CHECK(retired[0]==31 && !close(file));
    CHECK(mmap(retired,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==retired);
    retired[0]=53;CHECK(retired[0]==53 && !munmap(retired,4096));
    file=open("/mapped-pressure",O_RDONLY);CHECK(file>=0);
    CHECK(pread(file,&value,1,0)==1 && value==31 && !close(file));
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
struct child_wait { pid_t child;int group,nowait,result,error;siginfo_t info;uint64_t go,ready; };
static void *child_wait(void *opaque)
{
    struct child_wait *test=opaque;pin(test->nowait ? 1 : 0);
    composition_store(&control->participants[0],syscall(SYS_gettid));
    composition_store(&test->ready,1);wait_value(&test->go,1);
    if (test->group==2) {
        int status=0;
        test->result=waitpid(0,&status,0);
        if (test->result==test->child) CHECK(WIFEXITED(status) && WEXITSTATUS(status)==57);
    } else test->result=waitid(test->group ? P_PGID : P_PID,
        test->group ? 0 : test->child,&test->info,WEXITED|(test->nowait ? WNOWAIT : 0));
    test->error=errno;
    if (test->nowait) {
        /* 保留CPU1上的真实用户计算，覆盖本地timer返回；交接顺序由控制页决定。 */
        uint64_t end=now()+UINT64_C(100000000),value=1;
        do {
            for (unsigned i=0;i<100000;i++) {
                value=(value<<3)^(value>>7)^17;
                __asm__ volatile("" : "+r"(value));
            }
        } while (now()<end);
    }
    return 0;
}
static pid_t held_child(int pipefd[2])
{
    CHECK(!pipe(pipefd));pid_t child=fork();CHECK(child>=0);
    if (!child) { char byte;CHECK(!close(pipefd[1]) && read(pipefd[0],&byte,1)==1);_exit(57); }
    CHECK(!close(pipefd[0]));return child;
}
static void release_child(int pipefd[2])
{ CHECK(write(pipefd[1],"X",1)==1 && !close(pipefd[1])); }
static void wait_signal(int signal) { CHECK(signal==SIGUSR1); }
static void wait_processes(void)
{
    /* 子进程保留旧组，等待线程实际登记后，另一线程改变父进程的组。 */
    for (int group=1;group<=2;group++) {
        epoch_begin();
        pid_t parent=fork();CHECK(parent>=0);
        if (!parent) {
            int pipefd[2];struct child_wait test={.group=group};test.child=held_child(pipefd);
            pthread_t thread;CHECK(!pthread_create(&thread,0,child_wait,&test));
            wait_value(&test.ready,1);composition_store(&test.go,1);wait_registered(1);
            CHECK(!setpgid(0,0));release_child(pipefd);CHECK(!pthread_join(thread,0));
            if (test.result!=(group==2 ? test.child : 0))
                dprintf(2,"COMPOSITION wait group=%d result=%d errno=%d child=%d\n",group,test.result,test.error,test.child);
            CHECK(test.result==(group==2 ? test.child : 0));
            if (group==1) CHECK(test.info.si_pid==test.child && test.info.si_status==57);
            CHECK(waitpid(test.child,0,WNOHANG)==-1 && errno==ECHILD);_exit(0);
        }
        int status;CHECK(waitpid(parent,&status,0)==parent && WIFEXITED(status) && !WEXITSTATUS(status));
        record(group==1 ? "waitid-pgid-entry" : "wait4-pgid-entry");
    }
    int signal_pipe[2];struct child_wait interrupted={0};interrupted.child=held_child(signal_pipe);
    memset(&interrupted.info,0x5a,sizeof(interrupted.info));
    struct sigaction action={.sa_handler=wait_signal},previous;
    sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,&previous));
    epoch_begin();pthread_t thread;CHECK(!pthread_create(&thread,0,child_wait,&interrupted));
    wait_value(&interrupted.ready,1);composition_store(&interrupted.go,1);wait_registered(1);
    CHECK(!pthread_kill(thread,SIGUSR1) && !pthread_join(thread,0));
    CHECK(interrupted.result==-1 && interrupted.error==EINTR &&
        !interrupted.info.si_signo && !interrupted.info.si_errno && !interrupted.info.si_code &&
        !interrupted.info.si_pid && !interrupted.info.si_uid && !interrupted.info.si_status);
    CHECK(!sigaction(SIGUSR1,&previous,0));release_child(signal_pipe);
    CHECK(waitpid(interrupted.child,0,0)==interrupted.child);record("waitid-interrupted-output");
    int pipefd[2];struct child_wait first={.nowait=1};first.child=held_child(pipefd);
    composition_store(&control->epoch,0);
    pthread_t a,b;CHECK(!pthread_create(&a,0,child_wait,&first));wait_value(&first.ready,1);
    if (observed) {
        composition_store(&control->wait_tid,composition_load(&control->participants[0]));
        composition_store(&control->wait_child,first.child);
        composition_store(&control->wait_address,(uintptr_t)&first.info);
        command(COMPOSITION_WAIT_ARM);
    }
    release_child(pipefd);composition_store(&first.go,1);
    if (observed) wait_value(&control->wait_entered,1);
    else CHECK(!pthread_join(a,0));
    epoch_begin();struct child_wait second={.child=first.child};
    CHECK(!pthread_create(&b,0,child_wait,&second));wait_value(&second.ready,1);composition_store(&second.go,1);
    if (observed) {
        wait_registered(1);composition_store(&control->wait_release,1);
        CHECK(!pthread_join(a,0));
    }
    CHECK(!pthread_join(b,0));
    CHECK(!first.result && !second.result && first.info.si_pid==first.child &&
        second.info.si_pid==first.child && first.info.si_status==57 && second.info.si_status==57);
    CHECK(waitpid(first.child,0,WNOHANG)==-1 && errno==ECHILD);
    command(COMPOSITION_WAIT_FINISH);record("waitid-claim-handoff");puts("COMPOSITION END 4");
}
struct clock_query { pid_t pid;uint64_t ready,go;long result;int error;struct timespec value; };
static void *query_clock(void *opaque)
{
    struct clock_query *q=opaque;pin(1);
    uint64_t end=now()+UINT64_C(20000000),value=1;
    while (now()<end) {value=(value<<3)^(value>>7)^17;__asm__ volatile("" : "+r"(value));}
    composition_store(&control->clock_tid,syscall(SYS_gettid));
    composition_store(&q->ready,1);wait_value(&q->go,1);
    clockid_t id=(clockid_t)(~(unsigned)q->pid<<3 | 2U);
    errno=0;q->result=syscall(SYS_clock_gettime,id,&q->value);q->error=errno;
    return 0;
}
static pthread_t clock_arm(struct clock_query *q)
{
    composition_store(&control->clock_entered,0);composition_store(&control->clock_release,0);
    composition_store(&control->clock_pid,q->pid);
    pthread_t thread;CHECK(!pthread_create(&thread,0,query_clock,q));wait_value(&q->ready,1);
    command(COMPOSITION_CLOCK_ARM);
    if (observed) {composition_store(&q->go,1);wait_value(&control->clock_entered,1);}
    return thread;
}
static void clock_release(struct clock_query *q,pthread_t thread)
{
    if (observed) composition_store(&control->clock_release,1);
    else composition_store(&q->go,1);
    CHECK(!pthread_join(thread,0));command(COMPOSITION_CLOCK_FINISH);
}
struct clock_exec {int command,ack;};
static void *clock_exec_worker(void *opaque)
{
    struct clock_exec *p=opaque;char byte,command_fd[24],ack_fd[24];
    CHECK(read(p->command,&byte,1)==1 && byte=='X');
    snprintf(command_fd,sizeof(command_fd),"%d",p->command);snprintf(ack_fd,sizeof(ack_fd),"%d",p->ack);
    execl("/composition-supervisor","/composition-supervisor","--clock-exec-child",command_fd,ack_fd,(char *)0);
    _exit(97);
}
static void cpu_clock_processes(void)
{
    int pipefd[2];pid_t child=held_child(pipefd);struct clock_query reaped={.pid=child};
    pthread_t query=clock_arm(&reaped);release_child(pipefd);
    int status;CHECK(waitpid(child,&status,0)==child && status==(57<<8));
    clock_release(&reaped,query);CHECK(reaped.result==-1 && reaped.error==EINVAL);
    record("cpu-clock-reap-held-target");
    int commands[2],ack[2];CHECK(!pipe(commands) && !pipe(ack));child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!close(commands[1]) && !close(ack[0]));
        struct clock_exec p={commands[0],ack[1]};pthread_t worker;
        CHECK(!pthread_create(&worker,0,clock_exec_worker,&p));
        for (;;) pause();
    }
    CHECK(!close(commands[0]) && !close(ack[1]));struct clock_query exec={.pid=child};query=clock_arm(&exec);
    CHECK(write(commands[1],"X",1)==1);char byte;CHECK(read(ack[0],&byte,1)==1 && byte=='E');
    clock_release(&exec,query);CHECK(!exec.result && exec.value.tv_sec>=0 && exec.value.tv_nsec>=0 && exec.value.tv_nsec<1000000000L);
    CHECK(write(commands[1],"Q",1)==1 && waitpid(child,&status,0)==child && status==(57<<8));
    CHECK(!close(commands[1]) && !close(ack[0]));record("cpu-clock-nonleader-exec-held-target");
    puts("COMPOSITION END 2");
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
int main(int argc,char **argv)
{
    if (argc==4 && !strcmp(argv[1],"--clock-exec-child")) {
        int command_fd=atoi(argv[2]),ack_fd=atoi(argv[3]);char byte;
        CHECK(write(ack_fd,"E",1)==1 && read(command_fd,&byte,1)==1 && byte=='Q');
        _exit(57);
    }
    setup();
    if (!access("/cpu-clock-scene",F_OK)) cpu_clock_processes();
    else if (!access("/wait-scene",F_OK)) wait_processes();
    else if (!access("/sqlite-scene",F_OK)) sqlite();
    else if (!access("/ordinary-read-scene",F_OK)) {
        ordinary_reads();puts("COMPOSITION END 9");
    }
    else {
        concurrent_first_fault();
        concurrent_file_first_fault();
        ordinary_reads();
        mappings();
        for (unsigned count=1;count<=32;count=count==1 ? 8 : 32) {
            waiters(count,0);waiters(count,1);group_exit(count);if (count==32) break;
        }
        puts("COMPOSITION END 21");
    }
    command(COMPOSITION_STOP);
    CHECK(!munmap(control,4096) && !close(control_fd) && !unlink("/dev/shm/smp-control"));
    sync();fflush(stdout);reboot(RB_POWER_OFF);return 42;
}
