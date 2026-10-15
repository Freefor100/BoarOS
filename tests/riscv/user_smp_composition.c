#include <arch/riscv/root_boot.h>
#include <arch/riscv/smp.h>
#include <arch/riscv/virt_uart.h>
#include <arch/context.h>
#include <arch/riscv/context.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/memory_object.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/wait_internal.h>
#include <string.h>
#include "../../fs/vfs_internal.h"
#include "../smp/composition_protocol.h"

struct held_page { uint64_t address; uint32_t order; };
static struct riscv_root_boot *root;
static struct physical_page_allocator *allocator;
static struct kernel_thread_join controller;
static struct composition_control *control;
static struct held_page *ledger;
static uint64_t ledger_count, held, released, reclaimed, injections, retirements, waits;
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

static void check(int valid) { if (!valid) __builtin_trap(); }
static void relinquish(void);
size_t __real_kernel_tty_service_output(struct kernel_tty *,size_t);
size_t __wrap_kernel_tty_service_output(struct kernel_tty *tty,size_t budget)
{
    size_t accepted=__real_kernel_tty_service_output(tty,budget);
    if (boot_banner_waiting && accepted) boot_user_output=1;
    return accepted;
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
static void pressure(void)
{
    check(!ledger_count);
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
    /* 实际系统缓存回收；控制页属于tmpfs，不借用被驱逐的缓存entry。 */
    reclaimed+=kernel_page_cache_reclaim(&root->page_cache,128);
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
        uint64_t request=composition_load(&control->request);
        if (request==completed) { relinquish();continue; }
        check(request==completed+1);
        switch (composition_load(&control->command)) {
        case COMPOSITION_PRESSURE:pressure();break;
        case COMPOSITION_RELEASE:release_pages();break;
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
    check(kernel_heap_release(&root->heap,ledger)==KERNEL_HEAP_STATUS_OK);ledger=0;
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
          !fault_race_armed);
}
void riscv_user_composition_report(void)
{
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
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __real_physical_page_allocate_order(struct physical_page_allocator *,uint32_t,uint64_t *);
static int fail_allocation(struct physical_page_allocator *a)
{
    if (!failed_tid || a!=allocator) return 0;
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
    if (__atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) &&
        address==(fault_race_address&~BOAROS_PAGE_MASK) && size==BOAROS_PAGE_SIZE) {
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
enum kernel_page_cache_status __real_kernel_open_file_get_page(
    struct kernel_open_file_description *,uint64_t,uint64_t *,size_t *);
enum kernel_page_cache_status __wrap_kernel_open_file_get_page(
    struct kernel_open_file_description *file,uint64_t index,
    uint64_t *address,size_t *valid)
{
    enum kernel_page_cache_status status=
        __real_kernel_open_file_get_page(file,index,address,valid);
    kernel_pid_t tid=0;
    if (status==KERNEL_PAGE_CACHE_STATUS_OK && index==0 &&
        __atomic_load_n(&fault_race_armed,__ATOMIC_ACQUIRE) && fault_race_file &&
        !kernel_task_tid(kernel_task_current(),&tid) && tid>0 &&
        (uint64_t)tid==fault_race_tasks[0]) {
        if (__atomic_fetch_add(&fault_file_page_pauses,1,__ATOMIC_ACQ_REL)) __builtin_trap();
        composition_store(&control->fault_file_paused,1);
        while (!__atomic_load_n(&fault_file_resolved,__ATOMIC_ACQUIRE)) relinquish();
    }
    return status;
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
    if (!participant) return __real_kernel_mm_resolve_user_fault(mm,address,access);

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
    enum kernel_mm_status status=__real_kernel_mm_resolve_user_fault(mm,address,access);
    check(status==KERNEL_MM_STATUS_OK);
    uint64_t resolved=fault_race_file
        ? __atomic_fetch_add(&fault_file_resolved,1,__ATOMIC_ACQ_REL)+1
        : __atomic_fetch_add(&fault_race_resolved,1,__ATOMIC_ACQ_REL)+1;
    check(resolved<=2);
    if (resolved==2) __atomic_store_n(&fault_race_armed,0,__ATOMIC_RELEASE);
    return status;
}
