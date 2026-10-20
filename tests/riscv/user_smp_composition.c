#include <arch/riscv/root_boot.h>
#include <arch/riscv/smp.h>
#include <arch/riscv/virt_uart.h>
#include <arch/context.h>
#include <arch/riscv/context.h>
#include <kernel/cpu.h>
#include <kernel/virtio_split_queue.h>
#include <kernel/irq.h>
#include <kernel/memory_object.h>
#include <kernel/mm.h>
#include <kernel/mm_backend.h>
#include <kernel/open_file.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/wait_internal.h>
#include "../../mm/uaccess_internal.h"
#include <string.h>
#include "../../fs/vfs_internal.h"
#include "../../fs/char_device_internal.h"
#include "../smp/composition_protocol.h"
#include "../../kernel/sched/private.h"

struct held_page { uint64_t address; uint32_t order; };
static struct riscv_root_boot *root;
static struct physical_page_allocator *allocator;
static struct kernel_thread_join controller;
static struct composition_control *control;
static struct held_page *ledger;
static uint64_t ledger_count, held, released, reclaimed, injections, retirements, waits;
static uint64_t oom_fork_tid;
static uint64_t failed_tid, retired_address, watched_tid, epoch, registered_mask;
static uint64_t fault_race_address, fault_race_tasks[2];
static uint64_t fault_race_arrivals, fault_race_stale, fault_race_resolved, fault_race_syncs;
static uint64_t fault_file_arrivals, fault_file_retries, fault_file_resumed;
static uint64_t fault_file_resolved, fault_file_syncs, fault_file_page_pauses;
static uint64_t remote_retirements;
static unsigned fault_race_armed;
static unsigned fault_race_file;
static unsigned stopping;
static unsigned boot_banner_waiting, boot_user_output;
static unsigned shutdown_tail_held;
static uint64_t read_tid,read_address,read_size,read_flags,read_hart;
static uint64_t read_copies[2],read_phases,read_local_phases,write_phases,write_pins[2],write_total_pins[2];
static uint64_t read_backend_calls,read_backend_bytes,read_batches,read_user_faults,read_submissions;
static uint64_t read_close_phases,read_seek_phases,read_fork_phases,read_total_copies[2];
static struct kernel_task *read_waiter;
static uint64_t read_wait_generation;
static struct kernel_mm *read_revoke_mm;
static unsigned read_copy_active;
static uint64_t wait_tid,wait_child,wait_address,wait_claims;
static uint64_t clock_tid,clock_pid,clock_claims,clock_active;

static void check_at(int valid,unsigned line)
{
    if (!valid) {
        virt_uart_puts("BoarOS: COMPOSITION check line=");virt_uart_put_hex(line);
        virt_uart_puts("\n");__builtin_trap();
    }
}
#define check(valid) check_at((valid),__LINE__)
static void relinquish(void);
size_t __real_kernel_tty_service_output(struct kernel_tty *,size_t);
size_t __wrap_kernel_tty_service_output(struct kernel_tty *tty,size_t budget)
{
    if (shutdown_tail_held) return 0;
    size_t accepted=__real_kernel_tty_service_output(tty,budget);
    if (boot_banner_waiting && accepted) boot_user_output=1;
    return accepted;
}
int __real_ns16550_stop_report(struct ns16550_port **,struct ns16550_statistics *);
int __wrap_ns16550_stop_report(struct ns16550_port **port,struct ns16550_statistics *statistics)
{
    shutdown_tail_held=0;
    return __real_ns16550_stop_report(port,statistics);
}
void __real_virt_uart_puts(const char *);
void __wrap_virt_uart_puts(const char *text)
{
    __real_virt_uart_puts(text);
    /* 在最后一条启动记录中强制可发生的切换，检出IRQ开启后才继续逐字输出。 */
    if (!strcmp(text,"BoarOS: timer frequency=")) {
        uintptr_t irq=arch_interrupt_save();
        if (irq & RISCV_SSTATUS_SIE) {
            boot_banner_waiting=1;
            while (!boot_user_output)
                relinquish();
            boot_banner_waiting=0;
        }
        arch_interrupt_restore(irq);
    }
}
static void relinquish(void)
{
    check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK);
    arch_interrupt_restore(arch_interrupt_save() | RISCV_SSTATUS_SIE);
    (void)arch_interrupt_save();
}
static void release_pages(void)
{
    while (ledger_count) {
        struct held_page p=ledger[--ledger_count];
        check(physical_page_release_order(allocator,p.address,p.order)==PHYSICAL_PAGE_STATUS_OK);
        released+=UINT64_C(1)<<p.order;
        if (!(ledger_count&127U)) relinquish();
    }
}
static int reading_task(void)
{
    kernel_pid_t tid=0;
    return read_tid && !kernel_task_tid(kernel_task_current(),&tid) && tid>0 && (uint64_t)tid==read_tid;
}
static void release_read_waiter(void)
{
    if (!read_waiter || !control || composition_load(&control->read_release)) return;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_wait_record *wait=kernel_wait_record_of(read_waiter);
    if (wait->generation==read_wait_generation && wait->phase==KERNEL_WAIT_COMMITTED && !wait->on_cpu) {
        /* 资格等待已实际park；release让保留真实pin的读取者继续复制和归还。 */
        composition_store(&control->read_waited,1);
        composition_store(&control->read_release,1);
        read_waiter=NULL;
    }
}
static void arm_read(void)
{
    check(!read_tid);
    read_tid=composition_load(&control->read_tid);read_address=composition_load(&control->read_address);
    read_size=composition_load(&control->read_size);read_flags=composition_load(&control->read_flags);
    read_hart=composition_load(&control->read_hart);
    check(read_tid && read_address && read_size && read_hart<2);
    read_copies[0]=read_copies[1]=write_pins[0]=write_pins[1]=0;
    read_backend_calls=read_backend_bytes=read_submissions=0;read_waiter=NULL;
    if(read_flags&COMPOSITION_READ_COLD) {
        struct kernel_vfs_file source={0};
        check(!kernel_vfs_open(&root->mount,"/ordinary-read-source",&source));
        check(!kernel_page_cache_invalidate_node(&root->page_cache,kernel_vfs_file_node(&source)));
        check(!kernel_vfs_close(&source));
    }
}
static void finish_read(void)
{
    check(read_tid && !read_waiter && !read_revoke_mm);
    if(read_flags&COMPOSITION_READ_LOCAL) {
        check(read_copies[read_hart] && !read_copies[read_hart^1]);
        read_local_phases++;
    }
    if(read_flags&COMPOSITION_READ_BATCH) {
        check(read_backend_calls==1 && read_backend_bytes==65536 && read_submissions);
        read_batches++;
    }
    if(read_flags&COMPOSITION_WRITE_LOCAL) {
        check(write_pins[read_hart] && !write_pins[read_hart^1]);
        write_phases++;
        write_total_pins[0]+=write_pins[0];write_total_pins[1]+=write_pins[1];
    }
    if(read_flags&COMPOSITION_READ_CLOSE) {
        read_close_phases++;
    }
    if(read_flags&COMPOSITION_READ_SEEK) {
        check(composition_load(&control->read_waited)==1);
        read_seek_phases++;
    }
    if(read_flags&COMPOSITION_READ_FORK) { check(composition_load(&control->read_waited)==1);read_fork_phases++; }
    read_total_copies[0]+=read_copies[0];read_total_copies[1]+=read_copies[1];
    if (!(read_flags&COMPOSITION_WRITE_LOCAL)) read_phases++;
    read_tid=0;
}
static void pressure(void)
{
    check(!ledger_count);
    struct kernel_vfs_file source={0};
    uint64_t page;size_t valid;
    struct kernel_page_cache_statistics before,after;
    check(!kernel_vfs_open(&root->mount,"/observer",&source));
    check(!kernel_page_cache_get(&root->page_cache,&source,0,&page,&valid) && valid);
    kernel_page_cache_get_statistics(&root->page_cache,&before);
    check(physical_page_release(allocator,page)==PHYSICAL_PAGE_STATUS_OK);
    uint64_t low=physical_page_total(allocator)/50;
    while (!stopping && physical_page_available(allocator)>low) {
        uint64_t free=physical_page_available(allocator), address;
        unsigned order=free>low+64 ? 6 : 0;
        enum physical_page_status status=physical_page_allocate_order(allocator,order,&address);
        if (status && order) { order=0;status=physical_page_allocate(allocator,&address); }
        check(status==PHYSICAL_PAGE_STATUS_OK);
        ledger[ledger_count++]=(struct held_page){address,order};
        held+=UINT64_C(1)<<order;
        if (!(ledger_count&31U)) relinquish();
    }
    /* observer页不映射到用户空间；计入加压分配触发的后台回收，
     * 不要求最后一次同步调用抢在worker之前回收同一张页。 */
    (void)kernel_page_cache_reclaim(&root->page_cache,128);
    uint64_t deadline=kernel_time_monotonic_ns()+UINT64_C(10000000000);
    for (;;) {
        kernel_page_cache_get_statistics(&root->page_cache,&after);
        if (after.pages_reclaimed>before.pages_reclaimed) break;
        /* worker按轮发布回收计数，摘除候选后可能尚未结束该轮。 */
        check(kernel_time_monotonic_ns()<deadline);relinquish();
    }
    reclaimed+=after.pages_reclaimed-before.pages_reclaimed;
    check(!kernel_vfs_close(&source));
}
static void close_observer(struct kernel_vfs_file *file, uint64_t page,
                          int page_reference)
{
    control=0;
    if (page_reference)
        check(physical_page_release(allocator,page)==PHYSICAL_PAGE_STATUS_OK);
    check(!kernel_vfs_close(file));
}
static void run(void *unused)
{
    (void)unused;(void)arch_interrupt_save();
    struct kernel_vfs_file file={0};uint64_t page=0;int opened=0;
    while (!stopping) {
        if (!kernel_vfs_open(&root->mount,"/dev/shm/smp-control",&file)) { opened=1;break; }
        relinquish();
    }
    if (stopping) {
        if (opened) close_observer(&file,0,0);
        return;
    }
    struct kernel_memory_object *object=kernel_vfs_file_memory(&file);
    check(object!=0);
    enum kernel_memory_object_status page_status=KERNEL_MEMORY_OBJECT_NOT_FOUND;
    while (!stopping && page_status!=KERNEL_MEMORY_OBJECT_OK) {
        page_status=kernel_memory_object_find_page(object,0,&page);
        if (page_status!=KERNEL_MEMORY_OBJECT_OK) relinquish();
    }
    if (stopping) {
        /* find_page lends an extra page reference; shutdown may preempt after
         * lookup, so return it before dropping the file/object owner. */
        close_observer(&file,page,page_status==KERNEL_MEMORY_OBJECT_OK);
        return;
    }
    check(physical_page_resolve(allocator,page,(void **)&control)==PHYSICAL_PAGE_STATUS_OK);
    while (!stopping && composition_load(&control->magic)!=COMPOSITION_MAGIC) relinquish();
    if (stopping) {
        close_observer(&file,page,1);
        return;
    }
    check(kernel_heap_allocate(&root->heap,physical_page_total(allocator)*sizeof(*ledger),(void **)&ledger)==KERNEL_HEAP_STATUS_OK);
    composition_store(&control->observer_ready,1);
    uint64_t completed=0;
    while (!stopping) {
        release_read_waiter();
        uint64_t request=composition_load(&control->request);
        if (request==completed) { relinquish();continue; }
        check(request==completed+1);
        switch (composition_load(&control->command)) {
        case COMPOSITION_PRESSURE:pressure();break;
        case COMPOSITION_RELEASE:release_pages();break;
        case COMPOSITION_READ_ARM:arm_read();break;
        case COMPOSITION_READ_FINISH:finish_read();break;
        case COMPOSITION_CLOCK_ARM:
            check(!clock_tid && !clock_active);
            clock_pid=composition_load(&control->clock_pid);
            clock_tid=composition_load(&control->clock_tid);
            check(clock_pid && clock_tid);
            break;
        case COMPOSITION_CLOCK_FINISH:
            check(clock_tid && !clock_active && composition_load(&control->clock_entered) &&
                  composition_load(&control->clock_release));
            clock_tid=clock_pid=0;
            break;
        case COMPOSITION_WAIT_ARM:
            check(!composition_load(&wait_tid));
            wait_child=composition_load(&control->wait_child);
            wait_address=composition_load(&control->wait_address);
            check(composition_load(&control->wait_tid) && wait_child && wait_address);
            composition_store(&wait_tid,composition_load(&control->wait_tid));
            break;
        case COMPOSITION_WAIT_FINISH:
            check(composition_load(&wait_tid) && composition_load(&control->wait_entered) &&
                  composition_load(&control->wait_release));
            composition_store(&wait_tid,0);
            wait_child=wait_address=0;
            break;
        case COMPOSITION_OOM:check(!failed_tid);failed_tid=control->target_tid;break;
        case COMPOSITION_RFENCE:check(!watched_tid);watched_tid=control->target_tid;break;
        case COMPOSITION_FAULT_RACE:
            check(!fault_race_armed && control->fault_address &&
                  control->fault_tasks[0] && control->fault_tasks[1] &&
                  control->fault_tasks[0]!=control->fault_tasks[1]);
            fault_race_address=control->fault_address;
            fault_race_tasks[0]=control->fault_tasks[0];
            fault_race_tasks[1]=control->fault_tasks[1];
            fault_race_arrivals=0;fault_race_stale=0;fault_race_resolved=0;fault_race_syncs=0;
            fault_race_file=0;
            __atomic_store_n(&fault_race_armed,1,__ATOMIC_RELEASE);
            break;
        case COMPOSITION_FILE_FAULT_RACE:
            check(!fault_race_armed && control->fault_address &&
                  control->fault_tasks[0] && control->fault_tasks[1] &&
                  control->fault_tasks[0]!=control->fault_tasks[1]);
            fault_race_address=control->fault_address;
            fault_race_tasks[0]=control->fault_tasks[0];
            fault_race_tasks[1]=control->fault_tasks[1];
            fault_file_arrivals=0;fault_file_retries=0;fault_file_resumed=0;
            fault_file_resolved=0;fault_file_syncs=0;fault_file_page_pauses=0;
            fault_race_file=1;
            composition_store(&control->fault_file_paused,0);
            __atomic_store_n(&fault_race_armed,1,__ATOMIC_RELEASE);
            break;
        case COMPOSITION_STOP:stopping=1;break;
        default:__builtin_trap();
        }
        completed=request;
        if (!stopping) composition_store(&control->reply,completed);
    }
    release_pages();
    kernel_heap_release(&root->heap,ledger);ledger=0;
    check(!kernel_vfs_close(&file));
    struct composition_control *final_control=control;
    composition_store(&final_control->reply,completed);
    control=0;
    check(physical_page_release(allocator,page)==PHYSICAL_PAGE_STATUS_OK);
}
void riscv_user_composition_start(struct riscv_root_boot *r,struct physical_page_allocator *a)
{ root=r;allocator=a;check(kernel_thread_create_joinable(run,0,&controller)==KERNEL_SCHEDULER_STATUS_OK); }
void riscv_user_composition_finish(void)
{
    stopping=1;kernel_thread_join(&controller);
    check(!control && !ledger && !ledger_count && held==released && !failed_tid && !watched_tid &&
          !clock_tid && !clock_active && !fault_race_armed && !read_tid && !read_waiter && !read_revoke_mm && !composition_load(&wait_tid));
    /* 留下超过worker单轮预算的真实TTY尾部，直到stop才允许发送。 */
    uintptr_t irq=arch_interrupt_save();
    const struct kernel_char_device *device=kernel_tty_device_lookup(0x440);
    void *instance=0;
    check(device && !device->open(&root->heap,kernel_task_current(),
          KERNEL_FILES_O_NONBLOCK|0400U,0,&instance));
    char tail[385];
    const char prefix[]="COMPOSITION shutdown-tail ";
    memcpy(tail,prefix,sizeof(prefix)-1);
    memset(tail+sizeof(prefix)-1,'x',sizeof(tail)-sizeof(prefix));
    tail[sizeof(tail)-1]='\n';
    shutdown_tail_held=1;
    size_t written=0;
    check(!device->write(instance,kernel_task_current(),KERNEL_FILES_O_NONBLOCK,
                        tail,sizeof(tail),&written) && written==sizeof(tail));
    device->release(instance);
    arch_interrupt_restore(irq);
}
void riscv_user_composition_report(void)
{
    virt_uart_puts("BoarOS: COMPOSITION clock claims=");virt_uart_put_hex(clock_claims);
    virt_uart_puts(" active=");virt_uart_put_hex(clock_active);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION wait claims=");virt_uart_put_hex(wait_claims);
    virt_uart_puts(" active=");virt_uart_put_hex(composition_load(&wait_tid));virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION ordinary-io phases=");virt_uart_put_hex(read_phases);
    virt_uart_puts(" local=");virt_uart_put_hex(read_local_phases);
    virt_uart_puts(" copy0=");virt_uart_put_hex(read_total_copies[0]);
    virt_uart_puts(" copy1=");virt_uart_put_hex(read_total_copies[1]);
    virt_uart_puts(" batch=");virt_uart_put_hex(read_batches);
    virt_uart_puts(" local-faults=");virt_uart_put_hex(read_user_faults);
    virt_uart_puts(" writes=");virt_uart_put_hex(write_phases);
    virt_uart_puts(" write-pin0=");virt_uart_put_hex(write_total_pins[0]);
    virt_uart_puts(" write-pin1=");virt_uart_put_hex(write_total_pins[1]);
    virt_uart_puts(" close=");virt_uart_put_hex(read_close_phases);
    virt_uart_puts(" seek=");virt_uart_put_hex(read_seek_phases);
    virt_uart_puts(" fork=");virt_uart_put_hex(read_fork_phases);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION owners held=");virt_uart_put_hex(held);
    virt_uart_puts(" released=");virt_uart_put_hex(released);
    virt_uart_puts(" reclaimed=");virt_uart_put_hex(reclaimed);
    virt_uart_puts(" oom=");virt_uart_put_hex(injections);
    virt_uart_puts(" retirement=");virt_uart_put_hex(retirements);
    virt_uart_puts(" remote-retirement=");virt_uart_put_hex(remote_retirements);
    virt_uart_puts(" waits=");virt_uart_put_hex(waits);
    virt_uart_puts(" ledger=0x0 control=0x0\n");
    virt_uart_puts("BoarOS: COMPOSITION fault-race arrivals=");virt_uart_put_hex(fault_race_arrivals);
    virt_uart_puts(" stale=");virt_uart_put_hex(fault_race_stale);
    virt_uart_puts(" resolved=");virt_uart_put_hex(fault_race_resolved);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION fault-race-syncs=");virt_uart_put_hex(fault_race_syncs);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION file-fault-race arrivals=");virt_uart_put_hex(fault_file_arrivals);
    virt_uart_puts(" retries=");virt_uart_put_hex(fault_file_retries);
    virt_uart_puts(" resumed=");virt_uart_put_hex(fault_file_resumed);
    virt_uart_puts(" resolved=");virt_uart_put_hex(fault_file_resolved);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: COMPOSITION file-fault-race-syncs=");virt_uart_put_hex(fault_file_syncs);virt_uart_puts("\n");
}
void __real_scheduler_group_usage_snapshot(struct kernel_task *,int,struct kernel_task_usage *);
void __wrap_scheduler_group_usage_snapshot(struct kernel_task *target,int children,struct kernel_task_usage *usage)
{
    kernel_pid_t tid=0;
    int hold=clock_tid && !kernel_task_tid(kernel_task_current(),&tid) &&
        (uint64_t)tid==clock_tid && (uint64_t)target->tid==clock_pid &&
        !composition_load(&control->clock_entered);
    if (hold) {
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_pid *id=kernel_pid_find(&scheduler.identities,(kernel_pid_t)clock_pid);
            check(id && id->members[KERNEL_PID_TGID] &&
                id->members[KERNEL_PID_TGID]->task==target && target->storage_borrows);
        }
        clock_claims++;clock_active++;
        composition_store(&control->clock_entered,1);
        while (!composition_load(&control->clock_release)) relinquish();
    }
    __real_scheduler_group_usage_snapshot(target,children,usage);
    if (hold) clock_active--;
}
enum kernel_uaccess_status __real_kernel_copy_to_user(struct kernel_mm *,uint64_t,const void *,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_to_user(struct kernel_mm *mm,uint64_t address,
    const void *source,size_t size,size_t *copied)
{
    kernel_pid_t tid=0;
    uint64_t selected=composition_load(&wait_tid);
    if (selected && !kernel_task_tid(kernel_task_current(),&tid) &&
        (uint64_t)tid==selected && address==wait_address) {
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_task *child=process_find_identity((kernel_pid_t)wait_child,KERNEL_PID_TID);
            check(child && child->reaping && child->storage_borrows &&
                  child->state==KERNEL_THREAD_STATE_ZOMBIE);
        }
        wait_claims++;
        composition_store(&control->wait_entered,1);
        while (!composition_load(&control->wait_release)) relinquish();
    }
    return __real_kernel_copy_to_user(mm,address,source,size,copied);
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __real_physical_page_allocate_order(struct physical_page_allocator *,uint32_t,uint64_t *);
static int fail_allocation(struct physical_page_allocator *a)
{
    if (!failed_tid || oom_fork_tid!=failed_tid || a!=allocator) return 0;
    kernel_pid_t tid=0;
    if (kernel_task_tid(kernel_task_current(),&tid) || tid<=0 || (uint64_t)tid!=failed_tid) return 0;
    failed_tid=0;injections++;return 1;
}
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *p)
{ return fail_allocation(a) ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate(a,p); }
enum physical_page_status __wrap_physical_page_allocate_order(struct physical_page_allocator *a,uint32_t o,uint64_t *p)
{ return fail_allocation(a) ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate_order(a,o,p); }
enum kernel_mm_status __real_kernel_mm_munmap(struct kernel_mm *,uint64_t,uint64_t);
enum kernel_mm_status __wrap_kernel_mm_munmap(struct kernel_mm *mm,uint64_t address,uint64_t length)
{
    kernel_pid_t tid=0;
    int watched=0;
    if (watched_tid && !kernel_task_tid(kernel_task_current(),&tid) && tid>0 && (uint64_t)tid==watched_tid &&
            address==composition_load(&control->target_address)) {
        struct kernel_mm_mapping mapping;
        check(kernel_mm_lookup(mm,address,&mapping)==KERNEL_MM_STATUS_OK);
        retired_address=mapping.physical_address;
        watched=1;
    }
    enum kernel_mm_status result=__real_kernel_mm_munmap(mm,address,length);
    if (watched) retired_address=0;
    return result;
}
void __real_riscv_smp_sync_user(uint32_t,uint64_t,uint64_t,int);
void __wrap_riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    kernel_pid_t publisher=0;
    if (__atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) &&
        ((address==(fault_race_address&~BOAROS_PAGE_MASK) && size==BOAROS_PAGE_SIZE) ||
         (!address && size==UINT64_MAX)) && !kernel_task_tid(kernel_task_current(),&publisher) &&
        ((uint64_t)publisher==fault_race_tasks[0] || (uint64_t)publisher==fault_race_tasks[1])) {
        if (fault_race_file) fault_file_syncs++;
        else fault_race_syncs++;
    }
    kernel_pid_t tid=0;
    if (retired_address && watched_tid && !kernel_task_tid(kernel_task_current(),&tid) && tid>0 && (uint64_t)tid==watched_tid) {
        uint32_t refs=0;
        check(!kernel_cpu_current()->raw_locks);
        check(physical_page_reference_count(allocator,retired_address,&refs)==PHYSICAL_PAGE_STATUS_OK && refs);
        void *data=0;
        check(physical_page_resolve(allocator,retired_address,&data)==PHYSICAL_PAGE_STATUS_OK && *(unsigned char *)data==17);
        uint64_t progress=composition_load(&control->remote_progress);
        composition_store(&control->retirement_entered,1);
        while (!stopping && !composition_load(&control->retirement_release)) relinquish();
        check(physical_page_reference_count(allocator,retired_address,&refs)==PHYSICAL_PAGE_STATUS_OK && refs);
        check(*(unsigned char *)data==17 && composition_load(&control->remote_progress)>progress);
        if (targets & (1U<<1)) remote_retirements++;
        watched_tid=0;retirements++;
    }
    __real_riscv_smp_sync_user(targets,address,size,instructions);
}
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,
    struct kernel_open_file_page_preparation *prepared)
{
    int status=__real_kernel_open_file_prepare_mapping_page(file,index,prepared);
    kernel_pid_t tid=0;
    if (!status && index==0 &&
        __atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) && fault_race_file &&
        !kernel_task_tid(kernel_task_current(),&tid) && tid>0 &&
        (uint64_t)tid==fault_race_tasks[0]) {
        if (__atomic_fetch_add(&fault_file_page_pauses,1,__ATOMIC_ACQ_REL)) __builtin_trap();
        composition_store(&control->fault_file_paused,1);
        while (!__atomic_load_n(&fault_file_resolved,__ATOMIC_ACQUIRE)) relinquish();
    }
    return status;
}
enum virtio_status __real_virtio_split_enqueue(struct virtio_split_queue *,struct virtio_transport *,
    uint16_t,void *,uint32_t,uint32_t);
enum virtio_status __wrap_virtio_split_enqueue(struct virtio_split_queue *queue,struct virtio_transport *transport,
    uint16_t head,void *token,uint32_t minimum,uint32_t maximum)
{
    if (reading_task() && (read_flags&COMPOSITION_READ_BATCH)) {
        check(kernel_cpu_current()->logical_id==read_hart);
        read_submissions++;
    }
    return __real_virtio_split_enqueue(queue,transport,head,token,minimum,maximum);
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *,struct file_read_span *,size_t,struct file_read_workspace *, const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,struct file_read_span *spans,size_t count,
    struct file_read_workspace *workspace, const struct file_read_completion *completion)
{
    kernel_pid_t tid=0;
    struct kernel_task *owner=kernel_task_current();
    if (!kernel_task_has_user_context(owner)) owner=kernel_io_context_current()->account_task;
    if(owner && read_tid && !kernel_task_tid(owner,&tid) && (uint64_t)tid==read_tid) {
        check(kernel_cpu_current()->logical_id==read_hart && owner==kernel_task_current());
        read_backend_calls++;
        for(size_t i=0;i<count;i++)read_backend_bytes+=spans[i].size;
    }
    return __real_kernel_vfs_node_pread_batch(node,spans,count,workspace, completion);
}
void riscv_user_composition_fault_retry(uint64_t address,int resumed)
{
    if (!__atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) || !fault_race_file ||
        (address&~BOAROS_PAGE_MASK)!=(fault_race_address&~BOAROS_PAGE_MASK)) return;
    fault_file_retries++;
    if (resumed) fault_file_resumed++;
}
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    if (control && token && token->task) {
        kernel_pid_t reader_waiter=0;
        if(read_tid && (read_flags&(COMPOSITION_READ_SEEK|COMPOSITION_READ_FORK)) &&
           (!(read_flags&COMPOSITION_READ_FORK) || read_revoke_mm) &&
           composition_load(&control->read_entered) &&
           !composition_load(&control->read_release) &&
           !kernel_task_tid(token->task,&reader_waiter) && reader_waiter>0 &&
           (uint64_t)reader_waiter==composition_load(&control->read_waiter_tid)) {
            if(read_flags&COMPOSITION_READ_FORK) {
                struct kernel_mm_concurrency_state state;
                kernel_mm_get_concurrency_state(read_revoke_mm,&state);check(state.write_pins);
            }
            read_waiter=token->task;read_wait_generation=token->generation;
        }
        uint64_t current_epoch=composition_load(&control->epoch);kernel_pid_t tid=0;
        if (current_epoch && !kernel_task_tid(token->task,&tid)) {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_wait_record *record=kernel_wait_record_of(token->task);
            if (record->generation==token->generation && record->phase==KERNEL_WAIT_PREPARED) {
                if (epoch!=current_epoch) { epoch=current_epoch;registered_mask=0; }
                for (unsigned i=0;i<32;i++) if (tid>0 && composition_load(&control->participants[i])==(uint64_t)tid && !(registered_mask&(UINT64_C(1)<<i))) {
                    registered_mask|=UINT64_C(1)<<i;waits++;
                    __atomic_fetch_add(&control->registered,1,__ATOMIC_RELEASE);break;
                }
            }
        }
    }
    return __real_kernel_wait_park(token,reason);
}

static void hold_read_pin(enum kernel_mm_status status,uint64_t address,uint32_t access,
    struct kernel_mm_user_pin *pin)
{
    if(!status && access==KERNEL_MM_WRITE && reading_task() && read_copy_active &&
       address>=read_address && address-read_address<read_size && (read_flags&COMPOSITION_READ_HOLD) &&
       !composition_load(&control->read_entered)) {
        check(pin->registration==pin);
        composition_store(&control->read_entered,1);
        while(!composition_load(&control->read_release))relinquish();
    }
}
enum kernel_mm_status __real_kernel_mm_pin_user_page(const struct kernel_mm *,uint64_t,uint32_t,struct kernel_mm_user_pin *);
enum kernel_mm_status __wrap_kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{
    enum kernel_mm_status status=__real_kernel_mm_pin_user_page(mm,address,access,pin);
    hold_read_pin(status,address,access,pin);
    if(!status && access==KERNEL_MM_READ && reading_task() && (read_flags&COMPOSITION_WRITE_LOCAL) &&
        address>=read_address && address-read_address<read_size) {
        unsigned cpu=kernel_cpu_current()->logical_id;check(cpu==read_hart);write_pins[cpu]++;
    }
    return status;
}
enum kernel_uaccess_status __real_kernel_uaccess_write_cursor_copy(struct kernel_mm *,struct kernel_uaccess_write_cursor *,uint64_t,const void *,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_uaccess_write_cursor_copy(struct kernel_mm *mm,
    struct kernel_uaccess_write_cursor *cursor,uint64_t destination,const void *source,size_t size,size_t *copied)
{
    int watched=reading_task() && destination>=read_address && destination-read_address<read_size;
    if(watched) { check(!read_copy_active);read_copy_active=1; }
    enum kernel_uaccess_status result=__real_kernel_uaccess_write_cursor_copy(mm,cursor,destination,source,size,copied);
    if(watched)read_copy_active=0;
    if(watched && *copied) {
        unsigned cpu=kernel_cpu_current()->logical_id;check(cpu<2);
        read_copies[cpu]+=*copied;
    }
    return result;
}
enum kernel_mm_status __real_kernel_mm_fork(struct kernel_mm *,struct kernel_mm *);
enum kernel_mm_status __wrap_kernel_mm_fork(struct kernel_mm *destination,struct kernel_mm *source)
{
    kernel_pid_t tid=0;
    int injecting=!kernel_task_tid(kernel_task_current(),&tid) && tid>0 && (uint64_t)tid==failed_tid;
    /* Dynamic libc can fault before entering fork. Fail an allocation owned
     * by the requested MM operation, preserving those unrelated user faults. */
    if (injecting) oom_fork_tid=(uint64_t)tid;
    int watched=read_tid && (read_flags&COMPOSITION_READ_FORK) &&
        !kernel_task_tid(kernel_task_current(),&tid) && tid>0 &&
        (uint64_t)tid==composition_load(&control->read_waiter_tid);
    if(watched) { check(!read_revoke_mm);read_revoke_mm=source; }
    enum kernel_mm_status result=__real_kernel_mm_fork(destination,source);
    if (injecting) {
        check(!failed_tid && result==KERNEL_MM_STATUS_NO_MEMORY);
        oom_fork_tid=0;
    }
    if(watched) {
        struct kernel_mm_concurrency_state state;kernel_mm_get_concurrency_state(source,&state);
        check(!result && !state.write_pins && composition_load(&control->read_waited)==1);
        read_revoke_mm=NULL;
    }
    return result;
}

enum kernel_mm_status __real_kernel_mm_resolve_user_fault(struct kernel_mm *,uint64_t,uint32_t);
enum kernel_mm_status __wrap_kernel_mm_resolve_user_fault(struct kernel_mm *mm,
    uint64_t address,uint32_t access)
{
    kernel_pid_t tid=0;
    uint64_t page=address&~BOAROS_PAGE_MASK;
    int participant=0;
    if (__atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) &&
        access==(fault_race_file ? KERNEL_MM_READ : KERNEL_MM_WRITE) && page==fault_race_address &&
        !kernel_task_tid(kernel_task_current(),&tid) && tid>0) {
        participant=tid==(kernel_pid_t)fault_race_tasks[0] ||
                    tid==(kernel_pid_t)fault_race_tasks[1];
    }
    if (!participant) {
        int local_io=reading_task() && address>=read_address && address-read_address<read_size;
        if(local_io)check(kernel_cpu_current()->logical_id==read_hart);
        enum kernel_mm_status status=__real_kernel_mm_resolve_user_fault(mm,address,access);
        if(local_io && !status) { check(kernel_cpu_current()->logical_id==read_hart);read_user_faults++; }
        return status;
    }

    if (fault_race_file) {
        __atomic_fetch_add(&fault_file_arrivals,1,__ATOMIC_ACQ_REL);
    } else {
        uint64_t arrival=__atomic_fetch_add(&fault_race_arrivals,1,__ATOMIC_ACQ_REL);
        check(arrival<2);
        if (arrival==0) {
        /* 等另一个真实硬件fault先完成映射，构造“已经可写但仍收到旧fault”的状态。 */
            while (!__atomic_load_n(&fault_race_resolved,__ATOMIC_ACQUIRE))
                check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK);
            struct kernel_mm_mapping mapping;
            check(kernel_mm_lookup(mm,address,&mapping)==KERNEL_MM_STATUS_OK &&
                  (mapping.permissions&KERNEL_MM_WRITE));
            __atomic_fetch_add(&fault_race_stale,1,__ATOMIC_RELEASE);
        }
    }
    int local_io=reading_task() && address>=read_address && address-read_address<read_size;
    if(local_io)check(kernel_cpu_current()->logical_id==read_hart);
    enum kernel_mm_status status=__real_kernel_mm_resolve_user_fault(mm,address,access);
    check(status==KERNEL_MM_STATUS_OK);
    uint64_t resolved=fault_race_file
        ? __atomic_fetch_add(&fault_file_resolved,1,__ATOMIC_ACQ_REL)+1
        : __atomic_fetch_add(&fault_race_resolved,1,__ATOMIC_ACQ_REL)+1;
    check(resolved<=2);
    if (resolved==2) __atomic_store_n(&fault_race_armed,0,__ATOMIC_RELEASE);
    if(local_io && !status) { check(kernel_cpu_current()->logical_id==read_hart);read_user_faults++; }
    return status;
}
