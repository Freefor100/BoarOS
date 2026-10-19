#include <arch/riscv/smp.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/files.h>
#include <kernel/mm_backend.h>
#include <kernel/uaccess.h>
#include <kernel/irq.h>
#include <kernel/open_file.h>
#include <kernel/errno.h>
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/pipe_internal.h"
#include "../../fs/vfs_internal.h"
#include "../../kernel/sched/private.h"
#include <string.h>
#define SOURCE UINT64_C(0xc0000000)
#define TARGET UINT64_C(0xd0000000)
static struct kernel_files table;
static struct kernel_mm *space;
static struct kernel_heap *pool;
static struct kernel_open_file_description *buffer_file;
static struct kernel_page_cache *cache;
static struct kernel_thread_join peers[2];
static unsigned cpus, stop, progress, handoff, resume, watch;
static int64_t result;
static unsigned destination_cpu;
static struct kernel_wait_queue *observed_queue;
static struct kernel_wait_token observed_token;
static uint64_t work_ticks;
static unsigned user_overlap;
static unsigned char payload[4096];
void riscv_smp_test_fail_task_creation(unsigned);
static void check(int valid,const char *stage) { if (!valid) riscv_smp_fail(stage,-1); }
static void yield(void)
{ KERNEL_IRQ_SCOPE(irq); check(!kernel_scheduler_yield_current(),"files-controller-yield"); }
static void flag(unsigned *value,const char *stage)
{
    uint64_t end=riscv_time_read()+UINT64_C(50000000);
    while (!__atomic_load_n(value,__ATOMIC_ACQUIRE)) {
        check((int64_t)(riscv_time_read()-end)<0,stage); yield();
    }
}
enum kernel_scheduler_status __real_kernel_wait_park(struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    if (watch && kernel_task_current()==peers[1].task) {
        check(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks,"files-park-protection-released");
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          check(observed_queue && observed_queue->registrations==1,"files-actual-registration"); }
        watch=0; observed_token=*token;
        __atomic_store_n(&handoff,1,__ATOMIC_RELEASE);
        /* Prepared waits must survive an actual context switch before park. */
        while (!__atomic_load_n(&resume,__ATOMIC_ACQUIRE)) yield();
    }
    return __real_kernel_wait_park(token,reason);
}
static struct kernel_files *current_files(void)
{
    struct kernel_files *files=0;
    check(!kernel_task_files_borrow(kernel_task_current(),&files) && files,"files-task-binding");
    return files;
}
static struct kernel_mm *current_mm(void)
{
    struct kernel_mm *mm=0;
    check(!kernel_task_mm_borrow_mutable(kernel_task_current(),&mm) && mm,"files-MM-binding");
    return mm;
}
static void finish_fd(struct kernel_files *files,unsigned fd)
{ int64_t value; check(!kernel_files_close(files,fd,&value) && !value,"files-endpoint-close"); }
static void one_read(void *unused)
{
    (void)unused;
    struct kernel_files *files=current_files(); struct kernel_mm *mm=current_mm();
    check(!kernel_files_read(files,mm,0,TARGET,1,&result),"files-pinned-read");
    destination_cpu=kernel_cpu_current()->logical_id;
}
static void producer(void *unused)
{
    (void)unused; struct kernel_files *files=current_files(); struct kernel_mm *mm=current_mm();
    for (unsigned n=0;!__atomic_load_n(&stop,__ATOMIC_ACQUIRE);n++) {
        memset(payload,(int)(0x30+(n & 15)),sizeof(payload));
        size_t copied;
        check(!kernel_copy_to_user(mm,SOURCE,payload,sizeof(payload),&copied) && copied==sizeof(payload),"files-producer-user-buffer");
        int64_t value;
        check(!kernel_files_write(files,mm,1,SOURCE,sizeof(payload),&value) && value==sizeof(payload),"files-producer-packet");
        __atomic_fetch_add(&progress,1,__ATOMIC_RELEASE);
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          struct kernel_task_usage usage = {0};
          if (scheduler.init_task) scheduler_usage_thread_locked(scheduler.init_task, &usage);
          if (scheduler.init_task && usage.user_ns &&
              scheduler.init_task->state!=KERNEL_THREAD_STATE_EXITED &&
              scheduler.init_task->state!=KERNEL_THREAD_STATE_GROUP_DEAD)
              __atomic_fetch_add(&user_overlap,1,__ATOMIC_RELAXED); }
        yield();
    }
    finish_fd(files,1);
}
static void consumer(void *unused)
{
    (void)unused; struct kernel_files *files=current_files(); struct kernel_mm *mm=current_mm();
    unsigned received=0; unsigned char bytes[64];
    for (;;) {
        int64_t value;
        check(!kernel_files_read(files,mm,0,TARGET,4096,&value),"files-consumer-read");
        if (!value) break;
        check(value==4096,"files-atomic-packet-size");
        unsigned char expected=(unsigned char)(0x30+(received++ & 15));
        for (unsigned offset=0;offset<4096;offset+=sizeof(bytes)) {
            size_t copied;
            check(!kernel_copy_from_user(mm,bytes,TARGET+offset,sizeof(bytes),&copied) && copied==sizeof(bytes),"files-consumer-user-buffer");
            for (unsigned i=0;i<sizeof(bytes);i++) check(bytes[i]==expected,"files-packet-content");
        }
    }
    check(received==__atomic_load_n(&progress,__ATOMIC_ACQUIRE),"files-stream-complete");
    finish_fd(files,0);
}
static void construct_pair(void *unused)
{
    (void)unused;
    struct kernel_files *files=current_files(); struct kernel_mm *mm=current_mm();
    int64_t value; size_t copied; int32_t fds[2];
    check(!kernel_mm_mprotect(mm,SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0),"files-output-fault-permission");
    check(!kernel_files_pipe2(files,mm,SOURCE+BOAROS_PAGE_SIZE-4,0,&value) && value==-KERNEL_EFAULT,
        "files-output-fault-cancels-own-reservations");
    struct kernel_files_statistics stats;
    kernel_files_get_statistics(files,&stats);
    check(!stats.current_open_fds && !stats.close_on_exec_fds,"files-output-fault-no-publication");
    check(!kernel_mm_mprotect(mm,SOURCE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE),
        "files-output-permission-restore");
    check(!kernel_files_pipe2(files,mm,SOURCE,0,&value) && !value &&
        !kernel_copy_from_user(mm,fds,SOURCE,sizeof(fds),&copied) && copied==sizeof(fds) && fds[0]==0 && fds[1]==1,
        "files-pair-publication");
}
static void prepare_table(void)
{
    table=(struct kernel_files){0}; check(!kernel_files_create(&table,pool),"files-table-create");
    check(!kernel_files_check_task_domain(&table,space),"files-domain-publish");
    struct kernel_thread_join constructor={0};
    check(!kernel_thread_create_joinable_files(construct_pair,0,&constructor,1,space,&table),"files-pair-bound-builder");
    kernel_thread_join(&constructor);
}
static uint32_t references(void)
{ KERNEL_FILES_SCOPE(guard,&table); return table.record->references; }
static void cleanup_table(void)
{
    check(references()==1,"files-task-handles-ended");
    check(!kernel_files_release(&table),"files-table-final-owner");
}
static void bind_peer(unsigned index,void (*entry)(void *),unsigned mask)
{
    peers[index]=(struct kernel_thread_join){0};
    check(!kernel_thread_create_joinable_files(entry,0,&peers[index],mask,space,&table),"files-MM-table-task-publish");
}
static void mapping_operation(void *argument)
{
    unsigned operation=(unsigned)(uintptr_t)argument;
    struct kernel_mm *mm=current_mm(); uint64_t address;
    if (operation==1) check(!kernel_mm_mmap_anonymous(mm,SOURCE,2*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address),"files-bound-source-replacement");
    else if (operation==2) check(!kernel_mm_munmap(mm,TARGET,BOAROS_PAGE_SIZE),"files-bound-target-remove");
    else if (operation==3) check(!kernel_mm_munmap(mm,SOURCE,2*BOAROS_PAGE_SIZE),"files-bound-source-remove");
    else __builtin_trap();
}
static void mutate_mapping(unsigned operation)
{
    struct kernel_mm owner={0}; struct kernel_thread_join task={0};
    check(!kernel_mm_acquire(&owner,space) &&
        !kernel_thread_create_joinable_mm(mapping_operation,(void *)(uintptr_t)operation,&task,1,&owner),"files-mapping-task-owner");
    kernel_thread_join(&task);
}
static void seed_file_target(void)
{
    check(!kernel_vfs_ftruncate(&buffer_file->file,3*BOAROS_PAGE_SIZE),"files-buffer-grow");
    memset(payload,0x71,sizeof(payload)); size_t written; uint64_t observed=0;
    check(!kernel_vfs_pwrite(&buffer_file->file,2*BOAROS_PAGE_SIZE,payload,sizeof(payload),&written) && written==sizeof(payload) &&
        !kernel_vfs_sync(&buffer_file->file,0,&observed),"files-buffer-durable-seed");
    check(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&buffer_file->file)),"files-buffer-cold-cache");
    struct kernel_open_file_description *owner=buffer_file;
    check(!kernel_open_file_acquire(owner),"files-buffer-source-owner");
    uint64_t address;
    check(!kernel_mm_mmap_file_private(space,&owner,TARGET,BOAROS_PAGE_SIZE,2*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE|KERNEL_MM_MAP_SHARED,&address) && !owner && address==TARGET,
        "files-controlled-disk-buffer");
}
void riscv_files_smp_prepare(struct kernel_heap *heap,struct kernel_mm *mm,unsigned count,
    struct kernel_open_file_description *file,struct kernel_page_cache *page_cache)
{
    pool=heap; space=mm; cpus=count; cache=page_cache;
    int error;
    check(!kernel_open_file_create(pool,file->file.mount,"/pipe-buffer",&buffer_file,&error),"files-buffer-open-status");
    if (error) check(error==-KERNEL_ENOENT &&
        !kernel_open_file_create_mode(pool,file->file.mount,"/pipe-buffer",0600,&buffer_file,&error) && !error,
        "files-preopened-buffer-source");
    buffer_file->open_flags=2;
    uint64_t address;
    check(!kernel_mm_mmap_anonymous(space,SOURCE,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address),"files-anonymous-source-buffer");
    seed_file_target();
}
void riscv_files_smp_overlap_start(void)
{
    prepare_table(); stop=progress=user_overlap=0;
    bind_peer(0,producer,1); bind_peer(1,consumer,cpus==2?2:1);
    flag(&progress,"files-root-transfer-start");
}
void riscv_files_smp_overlap_finish(void)
{
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    kernel_thread_join(&peers[0]); kernel_thread_join(&peers[1]);
    check(progress,"files-root-real-transfer");
    struct kernel_files_statistics stats; kernel_files_get_statistics(&table,&stats);
    check(!stats.current_open_fds && !stats.close_on_exec_fds,"files-root-no-endpoint-retention");
    cleanup_table();
    virt_uart_puts("BoarOS: FILES SMP root overlap packets="); virt_uart_put_hex(progress);
    virt_uart_puts(" FD/OFD/pipe/wait owners returned\n");
    if (user_overlap) {
        virt_uart_puts("BoarOS: FILES SMP measured live-user overlap packets=");
        virt_uart_put_hex(user_overlap); virt_uart_putc('\n');
    }
    mutate_mapping(2);
    check(!kernel_vfs_ftruncate(&buffer_file->file,0),"files-root-buffer-restore");
}
void riscv_files_smp_run(void)
{
    mutate_mapping(1);
    seed_file_target();
    uint64_t start=riscv_time_read();
    for (unsigned mode=0;mode<4;mode++) {
        prepare_table();
        struct kernel_mm_concurrency_state before,after;
        kernel_mm_get_concurrency_state(space,&before);
        for (unsigned failure=1;failure<=2;failure++) {
            struct kernel_thread_join rejected={0};
            riscv_smp_test_fail_task_creation(failure);
            check(kernel_thread_create_joinable_files(one_read,0,&rejected,cpus==2?2:1,space,&table)==KERNEL_SCHEDULER_STATUS_NO_MEMORY &&
                !rejected.task && references()==1,"files-task-failure-rolls-back-handles");
            kernel_mm_get_concurrency_state(space,&after);
            check(after.references==before.references && !after.pins,"files-task-failure-MM-owner");
        }
        handoff=resume=0; watch=mode==3?0:1;
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *read=kernel_files_hold_fd(&table,0,&pin);
        check(read!=0,"files-controller-observation-pin"); observed_queue=&read->pipe->read_queue;
        bind_peer(1,one_read,cpus==2?2:1);
        if (mode==3) {
            uint64_t deadline=riscv_time_read()+UINT64_C(50000000);
            for (;;) {
                int parked;
                { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
                  parked=peers[1].task->state==KERNEL_THREAD_STATE_BLOCKED &&
                      !peers[1].task->wait.on_cpu && observed_queue->registrations==1; }
                if (parked) break;
                check((int64_t)(riscv_time_read()-deadline)<0,"files-real-block-commit"); yield();
            }
            check(!kernel_thread_set_cpu_mask(&peers[1],1),"files-blocked-migration-owner");
        } else flag(&handoff,"files-prepared-before-park");
        kernel_files_pin_guard_release(&pin);
        int64_t value;
        if (!mode || mode==3) {
          if (!mode) {
            finish_fd(&table,0);
            uint32_t fd; int error;
            check(!kernel_files_find_free_fd(&table,&fd,&error) && !error && !fd,"files-lowest-reused-fd");
            struct kernel_pipe *replacement=0; struct kernel_open_file_description *reader=0;
            check(!kernel_pipe_create(pool,&replacement) && !kernel_open_file_create_pipe(pool,replacement,1,0,&reader) &&
                !kernel_files_install_new_owned_at(&table,fd,0,&reader),"files-replacement-pipe");
          }
            unsigned char byte=0x6a; size_t copied;
            check(!kernel_copy_to_user(space,SOURCE,&byte,1,&copied) && copied==1 &&
                !kernel_files_write(&table,space,1,SOURCE,1,&value) && value==1,"files-old-request-writer");
        } else if (mode==1) finish_fd(&table,1);
        else check(!kernel_wait_notify(&observed_token,KERNEL_WAIT_SIGNALLED),"files-prepared-signal-arbitration");
        __atomic_store_n(&resume,1,__ATOMIC_RELEASE);
        kernel_thread_join(&peers[1]);
        check(result==(!mode || mode==3?1:mode==1?0:-KERNEL_ERESTARTSYS),"files-old-request-result");
        if (!mode || mode==3) {
            unsigned char byte; size_t copied;
            check(!kernel_copy_from_user(space,&byte,TARGET,1,&copied) && copied==1 && byte==0x6a,"files-old-request-content");
        }
        if (mode==3) check(destination_cpu==0,"files-migrated-I/O-resumption");
        cleanup_table();
    }
    prepare_table(); stop=progress=0;
    bind_peer(0,producer,1); bind_peer(1,consumer,cpus==2?2:1);
    while (__atomic_load_n(&progress,__ATOMIC_ACQUIRE)<64) yield();
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    kernel_thread_join(&peers[0]); kernel_thread_join(&peers[1]);
    struct kernel_files_statistics stats; kernel_files_get_statistics(&table,&stats);
    check(!stats.current_open_fds && references()==1,"files-stream-last-close-owner"); cleanup_table();
    check(!kernel_mm_msync(space,TARGET,BOAROS_PAGE_SIZE,4),"files-disk-buffer-msync");
    mutate_mapping(2); mutate_mapping(3);
    check(!kernel_vfs_ftruncate(&buffer_file->file,0),"files-buffer-final-owners");
    check(!kernel_open_file_release(&buffer_file),"files-buffer-controller-owner");
    work_ticks=riscv_time_read()-start;
    virt_uart_puts("BoarOS: FILES SMP native FD reuse/early wake/EOF/cancel/content/task rollback passed\n");
    virt_uart_puts("BoarOS: FILES SMP record-bytes="); virt_uart_put_hex(sizeof(struct kernel_files_record));
    virt_uart_puts(" pipe-bytes="); virt_uart_put_hex(sizeof(struct kernel_pipe));
    virt_uart_puts(" task-bytes="); virt_uart_put_hex(sizeof(struct kernel_task));
    virt_uart_puts(" OFF-work-ticks="); virt_uart_put_hex(work_ticks); virt_uart_putc('\n');
}
