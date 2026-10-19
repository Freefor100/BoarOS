#include <arch/riscv/smp.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/timer.h>
#include <arch/platform_io.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/files.h>
#include <kernel/mm_backend.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/page_cache.h>
#include <kernel/virtio_block.h>
#include <kernel/tmpfs.h>
#include <kernel/memory_object.h>
#include "../../fs/vfs_internal.h"
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../kernel/sched/private.h"
#include "../../mm/uaccess_internal.h"
#include <string.h>

#define IO_BUFFER UINT64_C(0xe0000000)
#define IO_ANON UINT64_C(0xe0100000)
static struct kernel_files table;
static struct kernel_mm *space;
static struct kernel_open_file_description *control;
static struct kernel_open_file_description *memory_control;
static struct kernel_open_file_description *cold_control;
static struct kernel_vfs_mount *memory_mount;
static struct kernel_page_cache *cache;
static struct kernel_thread_join peer;
static unsigned cpus,stop,progress,user_overlap;
static unsigned watch_copy,copy_cpu,local_copies[2],cold_loads;
static unsigned watch_timeout,timeout_copies;
static struct kernel_block_device *raw_device;
static struct virtio_split_queue *raw_queue;
static unsigned raw_read_active,raw_read_cpu,raw_submissions;
static unsigned char raw_expected[18],raw_bytes[64];
enum virtio_status __real_virtio_split_enqueue(struct virtio_split_queue *,struct virtio_transport *,
    uint16_t,void *,uint32_t,uint32_t);
enum virtio_status __wrap_virtio_split_enqueue(struct virtio_split_queue *queue,struct virtio_transport *transport,
    uint16_t head,void *token,uint32_t minimum,uint32_t maximum)
{
    if (raw_read_active && queue==raw_queue && kernel_cpu_current()->logical_id==raw_read_cpu)
        raw_submissions++;
    return __real_virtio_split_enqueue(queue,transport,head,token,minimum,maximum);
}
enum kernel_uaccess_status __real_kernel_uaccess_write_cursor_copy(struct kernel_mm *,
    struct kernel_uaccess_write_cursor *,uint64_t,const void *,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_uaccess_write_cursor_copy(struct kernel_mm *mm,
    struct kernel_uaccess_write_cursor *cursor,uint64_t address,const void *source,size_t size,size_t *copied)
{
    if(watch_copy) {
        unsigned cpu=kernel_cpu_current()->logical_id;
        if(cpu!=copy_cpu)riscv_smp_fail("file-io-user-copy-left-caller-hart",cpu);
        local_copies[cpu]++;
    }
    return __real_kernel_uaccess_write_cursor_copy(mm,cursor,address,source,size,copied);
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *,struct file_read_span *,size_t,struct file_read_workspace *, const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,struct file_read_span *spans,size_t count,
    struct file_read_workspace *workspace, const struct file_read_completion *completion)
{
    if(cold_control && node==kernel_open_file_node(cold_control)) {
        if(kernel_cpu_current()->logical_id)riscv_smp_fail("file-io-cold-backend-left-cpu0",-1);
        cold_loads++;
    }
    return __real_kernel_vfs_node_pread_batch(node,spans,count,workspace, completion);
}
uint64_t riscv_file_io_retained_shared_pages(void)
{
    return kernel_memory_object_resident_pages(kernel_vfs_file_memory(&memory_control->file));
}
static unsigned wrong_interrupt;
int __real_riscv_plic_in_interrupt(void);
int __wrap_riscv_plic_in_interrupt(void)
{
    int active=__real_riscv_plic_in_interrupt();
    if (kernel_cpu_current()->logical_id && active) wrong_interrupt++;
    return active;
}
enum kernel_page_cache_status __real_kernel_page_cache_run_cpu0(struct kernel_page_cache *,void (*)(void *),void *);
enum kernel_page_cache_status __wrap_kernel_page_cache_run_cpu0(struct kernel_page_cache *c,void (*fn)(void *),void *arg)
{
    enum kernel_page_cache_status s=__real_kernel_page_cache_run_cpu0(c,fn,arg);
    if (s) {
        struct kernel_io_context *io=kernel_io_context_current();
        virt_uart_puts("BoarOS: FILE IO queue status="); virt_uart_put_hex(s);
        virt_uart_puts(" pins="); virt_uart_put_hex((uintptr_t)io->user_pins);
        virt_uart_puts(" writes="); virt_uart_put_hex((uintptr_t)io->cache_writes);
        virt_uart_puts(" locks="); virt_uart_put_hex((uintptr_t)io->locks); virt_uart_putc('\n');
        virt_uart_puts(" cpu="); virt_uart_put_hex(kernel_cpu_current()->logical_id);
        virt_uart_puts(" preempt="); virt_uart_put_hex(kernel_cpu_current()->preempt_depth);
        virt_uart_puts(" external-IRQ="); virt_uart_put_hex(arch_external_interrupt_active()); virt_uart_putc('\n');
        virt_uart_puts(" wrong-local-IRQ="); virt_uart_put_hex(wrong_interrupt); virt_uart_putc('\n');
    }
    return s;
}
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *,void *,uint64_t,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *m,void *d,uint64_t a,size_t n,size_t *copied)
{
    if(watch_timeout && a==IO_ANON+512) {
        if(kernel_cpu_current()->logical_id!=copy_cpu)
            riscv_smp_fail("empty-poll-copy-left-caller-hart",-1);
        timeout_copies++;
    }
    enum kernel_uaccess_status s=__real_kernel_copy_from_user(m,d,a,n,copied);
    if (s && a==IO_BUFFER) {
        virt_uart_puts("BoarOS: FILE IO source copy status="); virt_uart_put_hex(s);
        virt_uart_puts(" copied="); virt_uart_put_hex(*copied); virt_uart_putc('\n');
    }
    return s;
}
static void check(int valid,const char *stage) { if (!valid) riscv_smp_fail(stage,-1); }
static void yield(void)
{ KERNEL_IRQ_SCOPE(irq); check(!kernel_scheduler_yield_current(),"file-io-controller-yield"); }
static void wait_progress(void)
{
    uint64_t deadline=riscv_time_read()+UINT64_C(50000000);
    while (!__atomic_load_n(&progress,__ATOMIC_ACQUIRE)) {
        check((int64_t)(riscv_time_read()-deadline)<0,"file-io-start-budget"); yield();
    }
}
static void current(struct kernel_mm **mm,struct kernel_files **files)
{
    check(!kernel_task_mm_borrow_mutable(kernel_task_current(),mm) &&
        !kernel_task_files_borrow(kernel_task_current(),files),"file-io-task-owner");
}
static void transfer(void)
{
    struct kernel_mm *mm=0; struct kernel_files *files=0; current(&mm,&files);
    int64_t result; size_t copied; char bytes[4];
    check(!kernel_files_pread(files,mm,0,IO_BUFFER,4,0,&result) && result==4,"file-io-remote-read-mapped-buffer");
    check(!kernel_copy_from_user(mm,bytes,IO_BUFFER,sizeof(bytes),&copied) &&
        copied==sizeof(bytes) && !memcmp(bytes,"IO00",4),"file-io-read-content");
    enum kernel_files_status status=kernel_files_pwrite(files,mm,0,IO_BUFFER,4,16,&result);
    if (status || result!=4) {
        virt_uart_puts("BoarOS: FILE IO pwrite status="); virt_uart_put_hex(status);
        virt_uart_puts(" result="); virt_uart_put_hex(result); virt_uart_putc('\n');
    }
    check(!status && result==4,"file-io-remote-write-mapped-buffer");
}
static void overlap(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        transfer(); __atomic_fetch_add(&progress,1,__ATOMIC_RELEASE);
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task_usage usage = {0};
          if (scheduler.init_task) scheduler_usage_thread_locked(scheduler.init_task, &usage);
          if (scheduler.init_task && usage.user_ns &&
            scheduler.init_task->state!=KERNEL_THREAD_STATE_EXITED &&
            scheduler.init_task->state!=KERNEL_THREAD_STATE_GROUP_DEAD)
            __atomic_fetch_add(&user_overlap,1,__ATOMIC_RELAXED);
    }
}
static void verify(void *unused)
{
    (void)unused;
    raw_read_cpu=kernel_cpu_current()->logical_id;raw_submissions=0;raw_read_active=1;
    memset(raw_bytes,0xa5,sizeof(raw_bytes));
    check(!kernel_block_read_at(raw_device,1128,raw_bytes,16) &&
        !memcmp(raw_bytes,raw_expected,16),"block-read-caller-uuid");
    struct block_read_span raw_spans[2]={{1128,raw_bytes,16,0,0},{1080,raw_bytes+32,2,0,0}};
    check(!kernel_block_read_batch(raw_device,raw_spans,2,NULL) &&
        raw_spans[0].status==KERNEL_BLOCK_STATUS_OK && raw_spans[0].completed==16 &&
        raw_spans[1].status==KERNEL_BLOCK_STATUS_OK && raw_spans[1].completed==2 &&
        !memcmp(raw_bytes,raw_expected,16) && !memcmp(raw_bytes+32,raw_expected+16,2),
        "block-read-caller-batch-prefix");
    raw_read_active=0;
    for (unsigned i=0;i<sizeof(raw_bytes);i++)
        if ((i>=16 && i<32) || i>=34) check(raw_bytes[i]==0xa5,"block-read-caller-canary");
    check(raw_submissions && raw_read_cpu==(cpus==2?1U:0U),"block-read-caller-actual-virtio-submit");
    virt_uart_puts("BoarOS: BLOCK caller reads hart=");virt_uart_put_hex(raw_read_cpu);
    virt_uart_puts(" submissions=");virt_uart_put_hex(raw_submissions);virt_uart_putc('\n');
    struct kernel_mm *mm=0; struct kernel_files *files=0; current(&mm,&files);
    int64_t result; size_t copied;
    struct kernel_page_cache_concurrency_state before,after;
    /* The hot-copy probe excludes a first fault of its file-backed output. */
    char initialized=0;
    check(!kernel_copy_to_user(mm,IO_BUFFER,&initialized,1,&copied) && copied==1,
        "file-io-hot-destination-resident");
    kernel_page_cache_get_concurrency_state(cache,&before);
    copy_cpu=kernel_cpu_current()->logical_id;watch_copy=1;
    check(!kernel_files_pread(files,mm,0,IO_BUFFER,4,0,&result) && result==4,"file-io-hot-local-pread");
    watch_copy=0;
    kernel_page_cache_get_concurrency_state(cache,&after);
    if (!local_copies[copy_cpu] || before.requests_queued!=after.requests_queued) {
        virt_uart_puts("BoarOS: hot copies=");virt_uart_put_hex(local_copies[copy_cpu]);
        virt_uart_puts(" requests before=");virt_uart_put_hex(before.requests_queued);
        virt_uart_puts(" after=");virt_uart_put_hex(after.requests_queued);virt_uart_putc('\n');
    }
    check(local_copies[copy_cpu] && before.requests_queued==after.requests_queued,"file-io-hot-no-cpu0-request");
    char zero=0;
    check(!kernel_copy_to_user(mm,IO_ANON,&zero,1,&copied) && copied==1,"file-io-cold-destination-ready");
    int64_t timeout[2]={0,1000};
    check(!kernel_copy_to_user(mm,IO_ANON+512,&timeout,sizeof(timeout),&copied) &&
        copied==sizeof(timeout),"empty-poll-timeout-ready");
    watch_timeout=1;
    check(!kernel_files_ppoll(files,mm,kernel_task_current(),0,0,IO_ANON+512,0,0,&result) &&
        !result,"empty-ppoll-local-timeout");
    check(!kernel_copy_to_user(mm,IO_ANON+512,&timeout,sizeof(timeout),&copied) &&
        copied==sizeof(timeout),"empty-pselect-timeout-ready");
    check(!kernel_files_pselect6(files,mm,kernel_task_current(),0,0,0,0,IO_ANON+512,0,&result) &&
        !result,"empty-pselect-local-timeout");
    watch_timeout=0;
    check(timeout_copies==2 && kernel_cpu_current()->logical_id==copy_cpu,
        "empty-poll-kept-caller-hart");
    virt_uart_puts("BoarOS: FILE IO empty poll timeout copy hart=");virt_uart_put_hex(copy_cpu);
    virt_uart_puts(" copies=");virt_uart_put_hex(timeout_copies);virt_uart_putc('\n');
    kernel_page_cache_get_concurrency_state(cache,&before);
    unsigned loads=cold_loads;
    watch_copy=1;
    check(!kernel_files_pread(files,mm,3,IO_ANON,4,0,&result) && result==4,"file-io-cold-request");
    watch_copy=0;
    kernel_page_cache_get_concurrency_state(cache,&after);
    check(cold_loads>loads && after.requests_queued>before.requests_queued &&
        after.executions_queued==before.executions_queued,"file-io-cold-only-backend-dispatched");
    char cold[4];
    check(!kernel_copy_from_user(mm,cold,IO_ANON,4,&copied) && copied==4 && !memcmp(cold,"COLD",4),"file-io-cold-content");
    virt_uart_puts("BoarOS: FILE IO local hot copy hart=");virt_uart_put_hex(copy_cpu);
    virt_uart_puts(" copies=");virt_uart_put_hex(local_copies[copy_cpu]);
    virt_uart_puts(" cold backend CPU0 loads=");virt_uart_put_hex(cold_loads-loads);virt_uart_putc('\n');
    transfer();
    check(!kernel_files_pread(files,mm,0,IO_ANON,4,0,&result) && result==4,"file-io-cold-anonymous-buffer");
    struct kernel_mm_user_pin pin={0};
    check(!kernel_mm_pin_user_page(mm,IO_ANON,KERNEL_MM_READ,&pin),"file-io-self-pin");
    kernel_page_cache_get_concurrency_state(cache,&before);
    check(!kernel_files_pread(files,mm,0,IO_ANON,0,0,&result) && !result,
        "file-io-self-pin-empty-local");
    kernel_mm_unpin_user_page(&pin);
    kernel_page_cache_get_concurrency_state(cache,&after);
    check(before.executions_queued==after.executions_queued,"file-io-rejected-request-owner");
    check(!kernel_files_read(files,mm,0,IO_ANON,4,&result) && result==4 &&
        !kernel_files_lseek(files,0,0,1,&result) && result==4,"file-io-shared-offset");
    struct kernel_uaccess_iovec iov[2]={{IO_ANON+64,2},{IO_ANON+128,2}};
    check(!kernel_copy_to_user(mm,IO_ANON+256,iov,sizeof(iov),&copied) && copied==sizeof(iov) &&
        !kernel_files_lseek(files,0,0,0,&result) && !result,"file-io-readv-position");
    kernel_page_cache_get_concurrency_state(cache,&before);
    watch_copy=1;
    check(!kernel_files_readv(files,mm,0,IO_ANON+256,2,&result) && result==4,"file-io-readv-local");
    watch_copy=0;
    kernel_page_cache_get_concurrency_state(cache,&after);
    char vector[4];
    check(before.requests_queued==after.requests_queued && control->offset==4 &&
        !kernel_copy_from_user(mm,vector,IO_ANON+64,2,&copied) && copied==2 &&
        !kernel_copy_from_user(mm,vector+2,IO_ANON+128,2,&copied) && copied==2 &&
        !memcmp(vector,"IO00",4),"file-io-readv-content-shared-offset");
    char sentinel=(char)0xa5;
    check(!kernel_copy_to_user(mm,IO_ANON+BOAROS_PAGE_SIZE,&sentinel,1,&copied) && copied==1,"file-io-prefix-sentinel");
    check(!kernel_mm_mprotect(mm,IO_ANON+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0),"file-io-fault-buffer-protect");
    check(!kernel_files_pread(files,mm,0,IO_ANON+BOAROS_PAGE_SIZE-2,4,0,&result) && result==2,
        "file-io-read-fault-prefix");
    check(!kernel_files_pwrite(files,mm,0,IO_ANON+BOAROS_PAGE_SIZE-2,4,32,&result) && result==2,
        "file-io-write-fault-prefix");
    check(!kernel_files_lseek(files,0,0,0,&result) && !result &&
        !kernel_files_read(files,mm,0,IO_ANON+BOAROS_PAGE_SIZE-2,4,&result) && result==2 &&
        !kernel_files_lseek(files,0,0,1,&result) && result==2,"file-io-read-short-prefix-offset");
    check(!kernel_mm_mprotect(mm,IO_ANON+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE),
        "file-io-buffer-reopen");
    char prefix[4];
    check(!kernel_copy_from_user(mm,&sentinel,IO_ANON+BOAROS_PAGE_SIZE,1,&copied) &&
        copied==1 && (unsigned char)sentinel==0xa5,"file-io-prefix-boundary-untouched");
    check(!kernel_files_pread(files,mm,0,IO_ANON+128,4,32,&result) && result==4 &&
        !kernel_copy_from_user(mm,prefix,IO_ANON+128,4,&copied) && copied==4 &&
        !memcmp(prefix,"IO\0\0",4),"file-io-prefix-file-content");
    check(!kernel_files_sync(files,0,0,&result) && !result,"file-io-fsync");
    check(!kernel_files_ftruncate(files,0,3*BOAROS_PAGE_SIZE,&result) && !result,"file-io-ftruncate");
    check(!kernel_files_fstat(files,mm,0,IO_ANON,&result) && !result,"file-io-fstat");
    struct kernel_linux_stat stat;
    check(!kernel_copy_from_user(mm,&stat,IO_ANON,sizeof(stat),&copied) && copied==sizeof(stat) &&
        stat.st_size==3*BOAROS_PAGE_SIZE,"file-io-stat-size");
    check(!kernel_files_dup(files,0,&result) && result==2,"file-io-dup");
    check(!kernel_files_close(files,2,&result) && !result,"file-io-close");
    check(!kernel_files_pread(files,mm,1,IO_ANON+128,4,0,&result) && result==4 &&
        !kernel_files_pwrite(files,mm,1,IO_ANON+128,4,8,&result) && result==4 &&
        !kernel_files_lseek(files,1,0,1,&result) && !result,"file-io-local-tmpfs-positioned");
    check(!kernel_files_read(files,mm,1,IO_ANON+128,4,&result) && result==4 &&
        !kernel_files_lseek(files,1,0,1,&result) && result==4,"file-io-local-tmpfs-offset");
    check(!kernel_files_ftruncate(files,1,0,&result) && !result && !kernel_open_file_size(memory_control),
        "file-io-tmpfs-truncate-common-path");
    /* 命名空间/网络不由这个绑定文件任务执行；创建时检查实际消费者，
     * 不再用FD表的资格标记模拟另一套业务语义。 */
}
void riscv_file_io_prepare(struct kernel_heap *heap,struct kernel_mm *mm,unsigned count,
    struct kernel_open_file_description *source,struct kernel_page_cache *page_cache)
{
    space=mm; cpus=count; cache=page_cache; int error; size_t written;
    raw_device=kernel_vfs_file_block_device(&source->file);check(raw_device!=NULL,"block-read-caller-device");
    kernel_block_get_device(raw_device);
    struct kernel_block_device *physical=raw_device->parent?raw_device->parent:raw_device;
    struct virtio_block_device *virtio=physical->context;
    check(&virtio->block==physical,"block-read-caller-virtio-owner");raw_queue=&virtio->queue;
    check(!kernel_block_read_at(raw_device,1128,raw_expected,16) &&
        !kernel_block_read_at(raw_device,1080,raw_expected+16,2) &&
        raw_expected[16]==0x53 && raw_expected[17]==0xef,"block-read-caller-reference");
    check(!kernel_open_file_create(heap,source->file.mount,"/file-io-smp",&control,&error),"file-io-open");
    if (error) check(error==-KERNEL_ENOENT &&
        !kernel_open_file_create_mode(heap,source->file.mount,"/file-io-smp",0600,&control,&error) && !error,"file-io-create");
    else {
        char marker[8]; size_t read;
        check(!kernel_open_file_pread(control,2*BOAROS_PAGE_SIZE,marker,sizeof(marker),&read) &&
            read==sizeof(marker) && !memcmp(marker,"IODONE!!",8),"file-io-independent-restart");
        virt_uart_puts("BoarOS: FILE IO independent restart content passed\n");
    }
    control->open_flags=2;
    check(!kernel_vfs_file_acquire_write(&control->file) && !kernel_vfs_ftruncate(&control->file,3*BOAROS_PAGE_SIZE),"file-io-write-lease");
    check(!kernel_vfs_pwrite(&control->file,0,"IO00",4,&written) && written==4,"file-io-seed");
    check(!kernel_open_file_create(heap,source->file.mount,"/file-io-cold",&cold_control,&error),"file-io-cold-open");
    if(error)check(error==-KERNEL_ENOENT &&
        !kernel_open_file_create_mode(heap,source->file.mount,"/file-io-cold",0600,&cold_control,&error) && !error,"file-io-cold-create");
    cold_control->open_flags=2;
    uint64_t observed=0;
    check(!kernel_vfs_file_acquire_write(&cold_control->file) &&
        !kernel_vfs_pwrite(&cold_control->file,0,"COLD",4,&written) && written==4 &&
        !kernel_vfs_sync(&cold_control->file,0,&observed) &&
        !kernel_page_cache_invalidate_node(cache,kernel_open_file_node(cold_control)),"file-io-cold-home");
    uint64_t address; struct kernel_open_file_description *mapping=control;
    check(!kernel_open_file_acquire(mapping) && !kernel_mm_mmap_file_private(space,&mapping,IO_BUFFER,
        BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED_NOREPLACE,&address) && !mapping,"file-io-buffer-source-owner");
    check(!kernel_mm_mmap_anonymous(space,IO_ANON,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address),"file-io-anonymous-buffer");
    check(!kernel_files_create(&table,heap),"file-io-table");
    struct kernel_open_file_description *slot=control;
    check(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&table,0,0,&slot),"file-io-disk-slot");
    slot=cold_control;
    check(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&table,3,0,&slot),"file-io-cold-slot");
    check(!kernel_tmpfs_create(heap,0,"",&memory_mount) &&
        !kernel_open_file_create_mode(heap,memory_mount,"/memory-io",0600,&memory_control,&error) && !error,
        "file-io-tmpfs-construction");
    memory_control->open_flags=2;
    check(!kernel_vfs_file_acquire_write(&memory_control->file) &&
        !kernel_vfs_pwrite(&memory_control->file,0,"MEM0",4,&written) && written==4,"file-io-tmpfs-seed");
    slot=memory_control;
    check(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&table,1,0,&slot) &&
        !kernel_files_check_task_domain(&table,space),"file-io-table-publication");
}
void riscv_file_io_overlap_start(void)
{
    stop=progress=user_overlap=0;
    check(!kernel_thread_create_joinable_files(overlap,0,&peer,cpus==2?2:1,space,&table),"file-io-overlap-task");
    wait_progress();
}
void riscv_file_io_overlap_finish(void)
{
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE); kernel_thread_join(&peer);
    check(progress,"file-io-root-overlap");
    virt_uart_puts("BoarOS: FILE IO root overlap transfers="); virt_uart_put_hex(progress); virt_uart_putc('\n');
    if (user_overlap) {
        virt_uart_puts("BoarOS: FILE IO live user overlap transfers=");
        virt_uart_put_hex(user_overlap); virt_uart_putc('\n');
    }
}
void riscv_file_io_run(void)
{
    uint64_t started=riscv_time_read();
    check(!kernel_thread_create_joinable_files(verify,0,&peer,cpus==2?2:1,space,&table),"file-io-native-task");
    kernel_thread_join(&peer);
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache,&state);
    check(!state.pending && !state.running && state.executions_queued==state.executions_completed,
        "file-io-request-final-owner");
    virt_uart_puts("BoarOS: FILE IO foreground ticks="); virt_uart_put_hex(riscv_time_read()-started);
    virt_uart_puts(" requests="); virt_uart_put_hex(state.executions_completed);
    virt_uart_puts(" queue-ns="); virt_uart_put_hex(state.queue_ns);
    virt_uart_puts(" service-ns="); virt_uart_put_hex(state.execute_ns);
    virt_uart_puts(" max-service-ns="); virt_uart_put_hex(state.max_execute_ns); virt_uart_putc('\n');
    started=riscv_time_read();
    check(!kernel_files_release(&table),"file-io-table-retirement");
    check(!kernel_open_file_release(&cold_control),"file-io-cold-owner-return");
    check(!kernel_open_file_release(&memory_control) && !kernel_vfs_unmount(memory_mount),
        "file-io-tmpfs-final-owner");
    size_t written;
    check(!kernel_vfs_pwrite(&control->file,2*BOAROS_PAGE_SIZE,"IODONE!!",8,&written) && written==8 &&
        !kernel_open_file_sync_range(control,0,UINT64_MAX),"file-io-durable-marker");
    /* MM来源仍持实际引用；后端末次归还由原CPU0 arena收口执行。 */
    check(!kernel_open_file_release(&control),"file-io-control-owner-return");
    kernel_block_put(raw_device);raw_device=NULL;raw_queue=NULL;
    virt_uart_puts("BoarOS: FILE IO durability/close ticks="); virt_uart_put_hex(riscv_time_read()-started); virt_uart_putc('\n');
    virt_uart_puts("BoarOS: FILE IO native cold buffer/offset/prefix/sync/close owners passed\n");
}
