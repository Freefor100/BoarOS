/* Real pipe/MM/uaccess; only task execution and notification timing are modeled. */
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <string.h>
extern _Thread_local uint64_t mm_test_context;
extern struct kernel_raw_lock kernel_wait_domain;
#define PIPE_SOURCE UINT64_C(0x100000)
#define PIPE_TARGET UINT64_C(0x400000)
static uint64_t pipe_context;
static unsigned char pipe_payload[16*16384+4096];
static unsigned pause_park,park_ready,resume_park,pause_copy,copy_ready,resume_copy;
static struct kernel_wait_queue *expected_queue;
static struct kernel_wait_token observed_token;
enum kernel_scheduler_status __real_kernel_wait_park(struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    if (kernel_cpu_current()->hardware_id==1 && __atomic_exchange_n(&pause_park,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          assert(expected_queue && expected_queue->registrations==1); }
        observed_token=*token;
        __atomic_store_n(&park_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_park,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_wait_park(token,reason);
}
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *,void *,uint64_t,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *mm,void *out,uint64_t address,size_t size,size_t *done)
{
    assert(!kernel_cpu_current()->raw_locks && "pipe copied user memory under raw lock");
    enum kernel_uaccess_status status=__real_kernel_copy_from_user(mm,out,address,size,done);
    if (kernel_cpu_current()->hardware_id==1 && __atomic_exchange_n(&pause_copy,0,__ATOMIC_ACQ_REL)) {
        assert(kernel_io_context_current()->locks && "selected fragment lacks copy ownership");
        __atomic_store_n(&copy_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_copy,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
static void pipe_bind(unsigned id,struct kernel_files *handle)
{
    mm_test_attach(id); mm_test_bind(&domain_mm); mm_test_bind_files(handle); mm_test_context=pipe_context;
}
static struct kernel_pipe *pipe_table(struct kernel_heap *heap)
{
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    struct kernel_pipe *pipe=0; struct kernel_open_file_description *a=0,*b=0;
    assert(!kernel_pipe_create(heap,&pipe));
    assert(!kernel_open_file_create_pipe(heap,pipe,1,0,&a));
    assert(!kernel_open_file_create_pipe(heap,pipe,2,0,&b));
    assert(!kernel_files_install_new_owned_at(&files,0,0,&a));
    assert(!kernel_files_install_new_owned_at(&files,1,0,&b));
    assert(!kernel_files_enable_concurrency(&files,&domain_mm));
    return pipe;
}
struct pipe_actor { unsigned write,fd; uint64_t address,count; int64_t bytes; unsigned broken; };
static void *one_pipe_actor(void *argument)
{
    struct pipe_actor *actor=argument; pipe_bind(1,&views[1]);
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(&views[1],actor->fd,&pin);
    assert(file);
    struct kernel_uaccess_iovec iov={actor->address,actor->count};
    if (actor->write) {
        struct kernel_pipe_result result;
        assert(!kernel_pipe_writev(file->pipe,&domain_mm,&iov,1,actor->count,kernel_open_file_flags(file),&result));
        actor->bytes=result.bytes; actor->broken=result.broken;
    } else assert(!kernel_pipe_readv(file->pipe,&domain_mm,&iov,1,actor->count,kernel_open_file_flags(file),&actor->bytes));
    kernel_files_pin_guard_release(&pin);
    assert(!kernel_files_release(&views[1]));
    mm_test_bind_files(0); mm_test_bind(0);
    return 0;
}
static void start_actor(struct pipe_actor *actor,pthread_t *thread)
{
    views[1]=(struct kernel_files){0}; assert(!kernel_files_acquire(&views[1],&files));
    park_ready=resume_park=copy_ready=resume_copy=0;
    assert(!pthread_create(thread,0,one_pipe_actor,actor));
}
static void pipe_boundary_cases(struct kernel_heap *heap)
{
    for (unsigned mode=0;mode<5;mode++) {
        pipe_bind(0,0); struct kernel_pipe *pipe=pipe_table(heap); mm_test_bind_files(&files);
        unsigned writing=mode==2 || mode==3;
        struct pipe_actor actor={.write=writing,.fd=writing?1:0,.address=writing?PIPE_SOURCE:PIPE_TARGET,
            .count=mode==3?KERNEL_PIPE_CAPACITY+1:1};
        if (mode==2) {
            struct kernel_pipe_result fill;
            KERNEL_IRQ_SCOPE(irq);
            assert(!kernel_pipe_write_buffer(pipe,pipe_payload,KERNEL_PIPE_CAPACITY,1,&fill) && fill.bytes==KERNEL_PIPE_CAPACITY);
        }
        pause_park=1; expected_queue=actor.write?&pipe->write_queue:&pipe->read_queue;
        pthread_t thread; start_actor(&actor,&thread);
        while (!__atomic_load_n(&park_ready,__ATOMIC_ACQUIRE)) sched_yield();
        int64_t result;
        if (mode==0) {
            /* fd0 now names a different pipe, while the blocked operation owns its old OFD. */
            assert(!kernel_files_close(&files,0,&result) && !result);
            struct kernel_pipe *replacement=0; struct kernel_open_file_description *read=0;
            assert(!kernel_pipe_create(heap,&replacement));
            assert(!kernel_open_file_create_pipe(heap,replacement,1,0,&read));
            assert(!kernel_files_install_new_owned_at(&files,0,0,&read));
            KERNEL_IRQ_SCOPE(irq);
            struct kernel_pipe_result written;
            assert(!kernel_pipe_write_buffer(pipe,pipe_payload,1,1,&written) && written.bytes==1);
        } else if (mode==1) {
            assert(!kernel_files_close(&files,1,&result) && !result);
        } else if (mode==2) {
            struct kernel_uaccess_iovec iov={PIPE_TARGET,BOAROS_PAGE_SIZE};
            assert(!kernel_pipe_readv(pipe,&domain_mm,&iov,1,BOAROS_PAGE_SIZE,0,&result) && result==BOAROS_PAGE_SIZE);
        } else if (mode==3) {
            assert(!kernel_files_close(&files,0,&result) && !result);
        } else {
            assert(!kernel_wait_notify(&observed_token,KERNEL_WAIT_SIGNALLED));
        }
        __atomic_store_n(&resume_park,1,__ATOMIC_RELEASE);
        assert(!pthread_join(thread,0));
        if (mode==0) {
            unsigned char byte; size_t done;
            assert(actor.bytes==1 && !kernel_copy_from_user(&domain_mm,&byte,PIPE_TARGET,1,&done) && done==1 && byte==pipe_payload[0]);
        } else if (mode==1) assert(!actor.bytes);
        else if (mode==2) assert(actor.bytes==1 && !actor.broken);
        else if (mode==3) assert(actor.bytes==KERNEL_PIPE_CAPACITY && actor.broken);
        else assert(actor.bytes==-KERNEL_ERESTARTSYS && !actor.broken && !signal_calls);
        assert(!kernel_files_release(&files));
    }
    /* A poll snapshot must progress while another CPU owns the copy mutex. */
    pipe_bind(0,0); struct kernel_pipe *pipe=pipe_table(heap); mm_test_bind_files(&files);
    struct pipe_actor actor={.write=1,.fd=1,.address=PIPE_SOURCE,.count=4096};
    pause_copy=1; pause_park=0;
    pthread_t thread; start_actor(&actor,&thread);
    while (!__atomic_load_n(&copy_ready,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!(kernel_pipe_poll(pipe,1,0,0)&KERNEL_POLLIN));
    assert(kernel_pipe_poll(pipe,2,0,0)&KERNEL_POLLOUT);
    __atomic_store_n(&resume_copy,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,0) && actor.bytes==4096);
    assert(kernel_pipe_poll(pipe,1,0,0)&KERNEL_POLLIN);
    assert(!kernel_files_release(&files));
    puts("PASS pipe registration/park boundary: old FD identity, EOF, full write, partial EPIPE, signal and nonblocking poll");
}
static unsigned packet_seen[2][64];
static pthread_mutex_t packet_check=PTHREAD_MUTEX_INITIALIZER;
struct packet_actor { unsigned id,fd,writer,stream; };
static void *packet_actor(void *argument)
{
    struct packet_actor *actor=argument; pipe_bind(actor->id,&views[actor->id]);
    unsigned char packet[4096]; uint64_t address=actor->writer?
        PIPE_SOURCE+(uint64_t)actor->id*BOAROS_PAGE_SIZE:PIPE_TARGET+(uint64_t)actor->id*BOAROS_PAGE_SIZE;
    if (actor->writer) {
        for (unsigned n=0;n<64;n++) {
            memset(packet,(int)(0x40+actor->stream),sizeof(packet));
            packet[0]=(unsigned char)actor->stream; packet[1]=(unsigned char)n;
            size_t done; assert(!kernel_copy_to_user(&domain_mm,address,packet,sizeof(packet),&done) && done==sizeof(packet));
            KERNEL_FILES_PIN_SCOPE(pin); struct kernel_open_file_description *file=kernel_files_hold_fd(&views[actor->id],actor->fd,&pin);
            assert(file); struct kernel_uaccess_iovec iov={address,sizeof(packet)};
            struct kernel_pipe_result result;
            assert(!kernel_pipe_writev(file->pipe,&domain_mm,&iov,1,sizeof(packet),1,&result) && result.bytes==sizeof(packet) && !result.broken);
        }
    } else {
        for (;;) {
            KERNEL_FILES_PIN_SCOPE(pin); struct kernel_open_file_description *file=kernel_files_hold_fd(&views[actor->id],actor->fd,&pin);
            assert(file); struct kernel_uaccess_iovec iov={address,sizeof(packet)}; int64_t result;
            assert(!kernel_pipe_readv(file->pipe,&domain_mm,&iov,1,sizeof(packet),0,&result));
            if (!result) break;
            assert(result==sizeof(packet));
            size_t done; assert(!kernel_copy_from_user(&domain_mm,packet,address,sizeof(packet),&done) && done==sizeof(packet));
            assert(packet[0]<2 && packet[1]<64);
            for (unsigned i=2;i<sizeof(packet);i++) assert(packet[i]==0x40+packet[0]);
            assert(!pthread_mutex_lock(&packet_check));
            assert(!packet_seen[packet[0]][packet[1]]++);
            assert(!pthread_mutex_unlock(&packet_check));
        }
    }
    int64_t result; assert(!kernel_files_close(&views[actor->id],actor->fd,&result) && !result);
    assert(!kernel_files_release(&views[actor->id]));
    mm_test_bind_files(0); mm_test_bind(0);
    return 0;
}
static void pipe_packet_case(struct kernel_heap *heap,unsigned actors)
{
    pipe_bind(0,0); pipe_table(heap); mm_test_bind_files(&files);
    memset(packet_seen,0,sizeof(packet_seen)); pause_park=pause_copy=0;
    struct packet_actor arguments[4]; pthread_t threads[4]; unsigned writers=actors/2;
    for (unsigned i=0;i<actors;i++) {
        arguments[i]=(struct packet_actor){.id=i,.writer=i<writers,.stream=i};
        int64_t fd; assert(!kernel_files_dup(&files,i<writers?1:0,&fd) && fd>=2);
        arguments[i].fd=(unsigned)fd;
        views[i]=(struct kernel_files){0}; assert(!kernel_files_acquire(&views[i],&files));
    }
    int64_t result;
    assert(!kernel_files_close(&files,0,&result) && !result);
    assert(!kernel_files_close(&files,1,&result) && !result);
    for (unsigned i=0;i<actors;i++) assert(!pthread_create(&threads[i],0,packet_actor,&arguments[i]));
    for (unsigned i=0;i<actors;i++) assert(!pthread_join(threads[i],0));
    for (unsigned w=0;w<writers;w++) for (unsigned n=0;n<64;n++) assert(packet_seen[w][n]==1);
    struct kernel_files_statistics stats; kernel_files_get_statistics(&files,&stats);
    assert(!stats.current_open_fds && !kernel_files_release(&files));
    printf("PASS %u threads: PIPE_BUF packets isolated, ring wraps, EOF without retained endpoint owners\n",actors);
}
static void pipe_geometry_case(struct kernel_heap *heap)
{
    pipe_bind(0,0); struct kernel_pipe *pipe=pipe_table(heap); mm_test_bind_files(&files);
    for (unsigned n=4095;n<=4097;n++) {
        struct kernel_uaccess_iovec source[2]={{PIPE_SOURCE,n/2},{PIPE_SOURCE+n/2,n-n/2}};
        struct kernel_uaccess_iovec target[2]={{PIPE_TARGET,n/3},{PIPE_TARGET+n/3,n-n/3}};
        struct kernel_pipe_result result; int64_t bytes;
        assert(!kernel_pipe_writev(pipe,&domain_mm,source,2,n,1,&result) && result.bytes==n && !result.broken);
        assert(!kernel_pipe_readv(pipe,&domain_mm,target,2,n,0,&bytes) && bytes==n);
        size_t done;
        assert(!kernel_copy_from_user(&domain_mm,pipe_payload,PIPE_TARGET,n,&done) && done==n);
        for (unsigned i=0;i<n;i++) assert(pipe_payload[i]==0x71);
    }
    assert(!kernel_mm_mprotect(&domain_mm,PIPE_SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    struct kernel_uaccess_iovec broken={PIPE_SOURCE+BOAROS_PAGE_SIZE-16,32};
    struct kernel_pipe_result result;
    assert(!kernel_pipe_writev(pipe,&domain_mm,&broken,1,32,1,&result) && result.bytes==-KERNEL_EFAULT && !result.broken);
    assert(!(kernel_pipe_poll(pipe,1,0,0)&KERNEL_POLLIN));
    assert(!kernel_mm_mprotect(&domain_mm,PIPE_SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_files_release(&files));
    pipe_bind(0,0); pipe=pipe_table(heap); mm_test_bind_files(&files);
    struct pipe_actor actor={.write=1,.fd=1,.address=PIPE_SOURCE,.count=BOAROS_PAGE_SIZE+16};
    pause_copy=1; pause_park=0; pthread_t thread; start_actor(&actor,&thread);
    while (!__atomic_load_n(&copy_ready,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_munmap(&domain_mm,PIPE_SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    __atomic_store_n(&resume_copy,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,0) && actor.bytes==BOAROS_PAGE_SIZE && !actor.broken);
    uint64_t restored;
    assert(!kernel_mm_mmap_anonymous(&domain_mm,PIPE_SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&restored));
    assert(!kernel_files_release(&files));
    puts("PASS pipe 4095/4096/4097 vectored geometry, unreadable fragment rollback and munmap after copied prefix");
}

static void pipe_cases(struct kernel_heap *heap)
{
    uint64_t address;
    assert(!kernel_mm_context(&domain_mm,&pipe_context)); mm_test_context=pipe_context; mm_test_bind(&domain_mm);
    assert(!kernel_mm_mmap_anonymous(&domain_mm,PIPE_SOURCE,17*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_mm_mmap_anonymous(&domain_mm,PIPE_TARGET,17*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    memset(pipe_payload,0x71,sizeof(pipe_payload)); size_t done;
    assert(!kernel_copy_to_user(&domain_mm,PIPE_SOURCE,pipe_payload,KERNEL_PIPE_CAPACITY+1,&done) && done==KERNEL_PIPE_CAPACITY+1);
    pipe_boundary_cases(heap); pipe_geometry_case(heap); pipe_packet_case(heap,2); pipe_packet_case(heap,4);
    mm_test_bind_files(0); mm_test_bind(0); mm_test_context=0;
}
