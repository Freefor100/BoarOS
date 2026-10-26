#include <arch/riscv/smp.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/direct_map.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/timer.h>
#include <kernel/mm_backend.h>
#include <kernel/memory_object.h>
#include <kernel/uaccess.h>
#include <kernel/heap.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/vfs.h>
#include <kernel/page_cache.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_internal.h"
#include <kernel/irq.h>
#include <string.h>
#include "../../kernel/sched/private.h"

#if BOAROS_FILES_SMP_TEST
void riscv_files_smp_prepare(struct kernel_heap *,struct kernel_mm *,unsigned,
    struct kernel_open_file_description *,struct kernel_page_cache *);
void riscv_files_smp_overlap_start(void),riscv_files_smp_overlap_finish(void),riscv_files_smp_run(void);
#endif
#if BOAROS_FILE_IO_SMP_TEST
void riscv_file_io_prepare(struct kernel_heap *,struct kernel_mm *,unsigned,
    struct kernel_open_file_description *,struct kernel_page_cache *);
void riscv_file_io_overlap_start(void),riscv_file_io_overlap_finish(void),riscv_file_io_run(void);
uint64_t riscv_file_io_retained_shared_pages(void);
#endif
#define DATA UINT64_C(0x40000)
#define CODE UINT64_C(0x40000000)
static struct physical_page_allocator *system_pages;
static struct physical_page_allocator arena;
static struct kernel_heap heap;
static struct kernel_mm parent;
static uint64_t outer, template_page, baseline, arena_baseline;
static struct kernel_thread_join controller_join, actors[2];
static struct kernel_wait_queue commands[2], fence_gate;
static unsigned done, root_mode, root_release, cpu_count;
static unsigned command[2], completed[2], pin_held, pin_release;
static unsigned pin_try, pin_conflict;
static unsigned hardware_cached, hardware_protected, hardware_denied, hardware_remapped;
static unsigned fence_armed, fence_entered, fence_release, remote_instruction;
static unsigned execute_cached, code_replaced;
static struct kernel_mm child;
static struct kernel_thread_join fork_join, cow_join;
static unsigned fork_done, cow_ready[2], cow_go;
static unsigned fail_create;
#if BOAROS_FILES_SMP_TEST
void riscv_smp_test_fail_task_creation(unsigned value) { fail_create=value; }
#endif

static unsigned migrated_pin_returned;
#if BOAROS_MM_SHARED_TEST
#define SHARED_DATA UINT64_C(0x80000000)
static unsigned shared_stage, shared_entered[2], shared_resume, shared_created;
static unsigned shared_cached, shared_protected, shared_written, shared_pin_held, shared_pin_release;
static unsigned shared_remove_done, shared_root_data;
static uint64_t shared_physical[2], shared_retired[5], shared_refs[5];
static struct kernel_wait_queue shared_prepare_gate;
static struct kernel_thread_join shared_remover;
#endif
#if BOAROS_MM_FILE_TEST
static struct kernel_vfs_mount *file_mount;
static struct kernel_open_file_description *file_controller;
static unsigned stale_fault, file_epoch_probe;
#define FILE_PRIVATE_DATA UINT64_C(0x90000000)
#endif
#if BOAROS_MM_DISK_TEST
static struct kernel_heap *disk_backend_heap;
static struct kernel_block_device *disk_backend;
static struct kernel_page_cache disk_cache;
static struct kernel_thread_join disk_setup_join;
static unsigned disk_setup_ready;
static uint64_t disk_setup_ticks;
static void require(int,const char *);
static unsigned disk_pin_ready,disk_pin_release,disk_other_go,disk_other_done,disk_snapshot_done;
#define DISK_COLD_DATA UINT64_C(0xa0000000)
static struct kernel_thread_join disk_sync_join;
static void disk_sync_body(void *unused)
{
    (void)unused;
    require(!kernel_mm_msync(&parent,SHARED_DATA,BOAROS_PAGE_SIZE,4),"disk-msync-source-handoff");
    __atomic_store_n(&disk_snapshot_done,1,__ATOMIC_RELEASE);
}
static struct kernel_vfs_mount disk_mount;
static const char disk_persist[]="BoarOS controlled disk durable";
void riscv_smp_test_disk_config(struct kernel_heap *backend_heap,struct kernel_block_device *block)
{ disk_backend_heap=backend_heap; disk_backend=block; }
#endif
static uint64_t old_pages[3], before_retire, background_count, background_begin;
static struct {
    uint64_t pc, cause, address, return_pc, return_sp, old_kernel_sp, old_user_sp;
    unsigned armed, caught;
} probes[2];
extern void mm_smp_store(uint64_t, uint64_t), mm_smp_store_pc(void);
extern uint64_t mm_smp_load(uint64_t), mm_smp_user(uint64_t, uint64_t, uint64_t);

static void require(int valid, const char *stage)
{ if (!valid) riscv_smp_fail(stage, -1); }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __real_physical_page_allocate_order(struct physical_page_allocator *, unsigned, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages, uint64_t *address)
{
    if (pages == system_pages && fail_create == 1) {
        fail_create = 0; return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    return __real_physical_page_allocate(pages, address);
}
enum physical_page_status __wrap_physical_page_allocate_order(struct physical_page_allocator *pages, unsigned order, uint64_t *address)
{
    if (pages == system_pages && fail_create == 2) {
        fail_create = 0; return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    return __real_physical_page_allocate_order(pages, order, address);
}
static void yield(void)
{
    KERNEL_IRQ_SCOPE(irq);
    require(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, "mm-controller-yield");
}
static void wait_flag(unsigned *flag, const char *stage)
{
    uint64_t end = riscv_time_read() + UINT64_C(50000000);
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        require((int64_t)(riscv_time_read() - end) < 0, stage);
        yield();
    }
}
static void wake(struct kernel_wait_queue *queue)
{
    KERNEL_IRQ_SCOPE(irq);
    require(kernel_wait_queue_wake_all(queue) == KERNEL_SCHEDULER_STATUS_OK, "mm-wake");
}
static void send(unsigned actor, unsigned operation)
{
    completed[actor] = 0;
    __atomic_store_n(&command[actor], operation, __ATOMIC_RELEASE);
    wake(&commands[actor]);
}
static void request(unsigned actor, unsigned operation)
{ send(actor, operation); wait_flag(&completed[actor], "mm-command-completion"); }
static unsigned next_command(unsigned id)
{
    for (;;) {
        struct kernel_wait_token token = {0};
        require(kernel_wait_prepare(&commands[id], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK, "mm-command-register");
        unsigned value = __atomic_exchange_n(&command[id], 0, __ATOMIC_ACQ_REL);
        if (!value) {
            enum kernel_wait_wake_reason reason;
            require(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK, "mm-command-park");
        }
        require(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK, "mm-command-finish");
        if (value) return value;
    }
}
static void *access(uint64_t address)
{
    uint64_t virtual;
    if (riscv_direct_map_pa_to_va(address, BOAROS_PAGE_SIZE, &virtual)) return 0;
    return (void *)(uintptr_t)virtual;
}
static int physical(const void *pointer, uint64_t *address)
{ return riscv_direct_map_va_to_pa((uintptr_t)pointer, 1, address) == RISCV_DIRECT_MAP_STATUS_OK; }

void mm_smp_user_enter(uint64_t sp, uint64_t pc)
{
    unsigned cpu = kernel_cpu_current()->logical_id;
    require(cpu < 2 && probes[cpu].armed, "mm-user-probe-owner");
    struct kernel_task *task = kernel_task_current();
    probes[cpu].return_sp = sp; probes[cpu].return_pc = pc;
    probes[cpu].old_kernel_sp = task->arch.kernel_sp;
    probes[cpu].old_user_sp = task->arch.user_sp;
    task->arch.kernel_sp = sp;
}
void __real_riscv_trap_dispatch(struct riscv_trap_frame *);
void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    unsigned cpu = kernel_cpu_current()->logical_id;
    if (cpu < 2 && probes[cpu].armed && frame->sepc == probes[cpu].pc &&
        frame->scause == probes[cpu].cause && frame->stval == probes[cpu].address) {
        require(!probes[cpu].caught, "mm-probe-repeated-fault");
        probes[cpu].caught = 1;
        if (!(frame->sstatus & RISCV_SSTATUS_SPP)) {
            struct kernel_task *task = kernel_task_current();
            require(frame->kernel_tp == (uintptr_t)task && task->bound_mm_context,
                "mm-user-probe-trap-owner");
            frame->sepc = probes[cpu].return_pc; frame->sp = probes[cpu].return_sp;
            frame->tp = (uintptr_t)task; frame->kernel_tp = 0;
            frame->sstatus = (frame->sstatus | RISCV_SSTATUS_SPP) & ~(RISCV_SSTATUS_SIE | RISCV_SSTATUS_SPIE);
            task->arch.kernel_sp = probes[cpu].old_kernel_sp;
            task->arch.user_sp = probes[cpu].old_user_sp;
        } else frame->sepc += 4;
        return;
    }
    __real_riscv_trap_dispatch(frame);
}
long __real_sbi_remote_sfence(uint64_t, uint64_t, uint64_t);
long __wrap_sbi_remote_sfence(uint64_t hart, uint64_t address, uint64_t size)
{
    if (fence_armed && (kernel_task_current() == actors[0].task
#if BOAROS_MM_SHARED_TEST
        || (shared_stage && kernel_task_current() == shared_remover.task)
#endif
        )) {
        require(!kernel_cpu_current()->raw_locks, "mm-RFENCE-inside-raw");
        fence_armed = 0;
        struct kernel_wait_token token; enum kernel_wait_wake_reason reason;
        require(kernel_wait_prepare(&fence_gate, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK, "mm-fence-register");
        __atomic_store_n(&fence_entered, 1, __ATOMIC_RELEASE);
        if (!__atomic_load_n(&fence_release, __ATOMIC_ACQUIRE))
            require(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK, "mm-fence-park");
        require(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK, "mm-fence-finish");
#if BOAROS_SMP_NEGATIVE == 5
#if BOAROS_MM_SHARED_TEST
        if (shared_stage) {
            struct kernel_mm_concurrency_state state;
            kernel_mm_get_concurrency_state(&parent, &state);
            require(state.pending_retirement == 1 && physical_page_available(&arena) == before_retire &&
                (BOAROS_MM_DISK_TEST || arena.shared_anon_pages == 3), "shared-failed-RFENCE-batch-and-object");
            for (unsigned i=0;i<5;i++) {
                uint32_t references;
                require(physical_page_reference_count(&arena,shared_retired[i],&references)==PHYSICAL_PAGE_STATUS_OK &&
                    references==shared_refs[i], "shared-failed-RFENCE-retained-reference");
            }
#if BOAROS_MM_FILE_TEST
#if BOAROS_MM_DISK_TEST
            virt_uart_emergency_begin();
#endif
            require(file_controller && file_controller->references>=2,"file-RFENCE-controller-source-owner");
#if BOAROS_MM_DISK_TEST
            struct kernel_page_cache_statistics cache_state;
            kernel_page_cache_get_statistics(&disk_cache,&cache_state);
            require(cache_state.current_pages>=3 && disk_mount.private_data && disk_backend,
                "disk-RFENCE-cache-mount-owner");
            virt_uart_puts("BoarOS: disk MM RFENCE error retains cache/mount/source/data/table owners\n");
#endif
            virt_uart_puts("BoarOS: file MM RFENCE error retains source/registration/data/table owners\n");
#endif
            virt_uart_puts("BoarOS: shared MM RFENCE error retains object/pin/data/table owners\n");
            return -1;
        }
#else
        struct kernel_mm_concurrency_state state;
        kernel_mm_get_concurrency_state(&parent, &state);
        require(state.pending_retirement == 1 && physical_page_available(&arena) == before_retire,
            "mm-failed-RFENCE-owner-batch");
        for (unsigned i = 0; i < 3; i++) {
            uint32_t references;
            require(physical_page_reference_count(&arena, old_pages[i], &references) == PHYSICAL_PAGE_STATUS_OK && references == 1,
                "mm-failed-RFENCE-owned-page");
        }
        virt_uart_puts("BoarOS: MM RFENCE error retains data/table batch owners\n");
        return -1;
#endif
#endif
    }
    return __real_sbi_remote_sfence(hart, address, size);
}
enum kernel_memory_object_status __real_kernel_memory_object_prepare_page(
    struct kernel_memory_object *,uint64_t,uint64_t *,int *);
enum kernel_memory_object_status __wrap_kernel_memory_object_prepare_page(
    struct kernel_memory_object *object,uint64_t index,uint64_t *physical,int *created)
{
    enum kernel_memory_object_status result=__real_kernel_memory_object_prepare_page(object,index,physical,created);
#if BOAROS_MM_SHARED_TEST
    unsigned id=kernel_task_current()==actors[1].task ? 1 : 0;
    if(result==KERNEL_MEMORY_OBJECT_OK && !BOAROS_MM_FILE_TEST && shared_stage && index==1 &&
        !__atomic_load_n(&shared_entered[id],__ATOMIC_ACQUIRE)) {
        require(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks,"shared-prepare-outside-MM-and-object");
        shared_physical[id]=*physical;
        __atomic_fetch_add(&shared_created,*created!=0,__ATOMIC_RELAXED);
        struct kernel_wait_token token={0}; enum kernel_wait_wake_reason reason;
        require(kernel_wait_prepare(&shared_prepare_gate,0,0,&token)==KERNEL_SCHEDULER_STATUS_OK,"shared-prepare-register");
        __atomic_store_n(&shared_entered[id],1,__ATOMIC_RELEASE);
        if(!__atomic_load_n(&shared_resume,__ATOMIC_ACQUIRE))
            require(kernel_wait_park(&token,&reason)==KERNEL_SCHEDULER_STATUS_OK,"shared-prepare-park");
        require(kernel_wait_finish(&token)==KERNEL_SCHEDULER_STATUS_OK,"shared-prepare-finish");
    }
#endif
    return result;
}
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,
    struct kernel_open_file_page_preparation *prepared)
{
    int result=__real_kernel_open_file_prepare_mapping_page(file,index,prepared);
#if BOAROS_MM_FILE_TEST
    unsigned id=kernel_task_current()==actors[1].task ? 1 : 0;
    if (!result && shared_stage && index==1 && !__atomic_load_n(&shared_entered[id],__ATOMIC_ACQUIRE)) {
        require(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks,"file-prepare-outside-MM-inode-object");
        shared_physical[id]=prepared->address;
        __atomic_fetch_add(&shared_created,prepared->created!=0,__ATOMIC_RELAXED);
        struct kernel_wait_token token={0}; enum kernel_wait_wake_reason reason;
        require(!kernel_wait_prepare(&shared_prepare_gate,0,0,&token),"file-prepare-register");
        __atomic_store_n(&shared_entered[id],1,__ATOMIC_RELEASE);
        if (!__atomic_load_n(&shared_resume,__ATOMIC_ACQUIRE)) require(!kernel_wait_park(&token,&reason),"file-prepare-park");
        require(!kernel_wait_finish(&token),"file-prepare-finish");
    }
#endif
    return result;
}
long __real_sbi_remote_fence_i(uint64_t);
long __wrap_sbi_remote_fence_i(uint64_t hart)
{
    require(!kernel_cpu_current()->raw_locks, "mm-FENCE.I-inside-raw");
    __atomic_fetch_add(&remote_instruction, 1, __ATOMIC_RELAXED);
#if BOAROS_SMP_NEGATIVE == 6
    if (kernel_task_current() == actors[0].task && execute_cached) {
        struct kernel_mm_concurrency_state state;
        kernel_mm_get_concurrency_state(&parent, &state);
        require(state.references >= 3 && (state.active_cpus & 2), "mm-failed-FENCE.I-owner");
        virt_uart_puts("BoarOS: MM FENCE.I error retains live MM/code owners\n");
        return -1;
    }
#endif
    return __real_sbi_remote_fence_i(hart);
}
static void user_probe(uint64_t cause, uint64_t fault_address, uint64_t argument,
    uint64_t expected)
{
    uintptr_t irq = arch_interrupt_save();
    uintptr_t enables;
    /* U态下SIE不屏蔽委托给S态的中断；微探针不是普通用户任务，
     * 必须暂关sie源，返回S态后原样恢复，不能把timer送入预期fault。 */
    asm volatile("csrrw %0, sie, zero" : "=r"(enables) :: "memory");
    unsigned cpu = kernel_cpu_current()->logical_id;
    probes[cpu].pc = CODE + (cause == RISCV_SCAUSE_USER_ECALL ? 4 : 0);
    probes[cpu].cause = cause; probes[cpu].address = fault_address;
    probes[cpu].caught = 0; probes[cpu].armed = 1;
    uint64_t result = mm_smp_user(CODE, DATA + BOAROS_PAGE_SIZE, argument);
    require(probes[cpu].caught && (cause != RISCV_SCAUSE_USER_ECALL || result == expected), "mm-real-U-probe");
    probes[cpu].armed = 0;
    asm volatile("csrw sie, %0" :: "r"(enables) : "memory");
    arch_interrupt_restore(irq);
}
static void actor(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    struct kernel_mm_user_pin migrating_pin = {0};
    uintptr_t pin_sp = 0;
    struct kernel_mm *mm;
    require(kernel_task_mm_borrow_mutable(kernel_task_current(), &mm) == KERNEL_TASK_STATUS_OK, "mm-bound-borrow");
    for (;;) {
        unsigned operation = next_command(id);
        size_t copied;
        if (operation == 1) {
            struct kernel_mm_user_pin pin = {0};
            require(kernel_mm_pin_user_page(mm, DATA, KERNEL_MM_WRITE, &pin) == KERNEL_MM_STATUS_OK, "mm-hold-pin");
            *(uint64_t *)pin.page = 77;
            __atomic_store_n(&pin_held, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&pin_release, __ATOMIC_ACQUIRE)) {
                if (__atomic_exchange_n(&pin_try, 0, __ATOMIC_ACQ_REL)) {
                    struct kernel_mm_user_pin untouched = {0};
                    struct kernel_mm_mapping before, after;
                    require(kernel_mm_lookup(mm, DATA, &before) == KERNEL_MM_STATUS_OK, "mm-self-pin-before");
                    require(kernel_mm_munmap(mm, DATA, BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_CONFLICT &&
                        kernel_mm_pin_user_page(mm, DATA, KERNEL_MM_READ, &untouched) == KERNEL_MM_STATUS_CONFLICT &&
                        kernel_mm_resolve_user_fault(mm, DATA, KERNEL_MM_WRITE) == KERNEL_MM_STATUS_CONFLICT,
                        "mm-self-pin-busy-conflict");
                    require(!untouched.registration && pin.registration == &pin && *(uint64_t *)pin.page == 77 &&
                        kernel_mm_lookup(mm, DATA, &after) == KERNEL_MM_STATUS_OK &&
                        before.physical_address == after.physical_address && before.permissions == after.permissions,
                        "mm-self-pin-conflict-keeps-owner");
                    __atomic_store_n(&pin_conflict, 1, __ATOMIC_RELEASE);
                }
                __atomic_fetch_add(&background_count, 1, __ATOMIC_RELAXED);
                if (cpu_count == 1) asm volatile("nop");
            }
            kernel_mm_unpin_user_page(&pin);
        } else if (operation == 2 || operation == 3) {
            require(kernel_mm_mprotect(mm, DATA, BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | (operation == 3 ? KERNEL_MM_WRITE : 0)) == KERNEL_MM_STATUS_OK, "mm-permission-commit");
        } else if (operation == 4) {
            mm_smp_store(DATA, 0x1234);
            require(mm_smp_load(DATA) == 0x1234, "mm-native-SUM-read-write");
            __atomic_store_n(&hardware_cached, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&hardware_protected, __ATOMIC_ACQUIRE))
                __atomic_fetch_add(&background_count, 1, __ATOMIC_RELAXED);
            uintptr_t irq = arch_interrupt_save(); unsigned cpu = kernel_cpu_current()->logical_id;
            probes[cpu].pc = (uintptr_t)mm_smp_store_pc; probes[cpu].cause = RISCV_SCAUSE_STORE_PAGE_FAULT;
            probes[cpu].address = DATA; probes[cpu].caught = 0; probes[cpu].armed = 1;
            mm_smp_store(DATA, 0xffff);
            require(probes[cpu].caught, "mm-stale-write-translation"); probes[cpu].armed = 0;
            arch_interrupt_restore(irq);
            __atomic_store_n(&hardware_denied, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&hardware_remapped, __ATOMIC_ACQUIRE))
                __atomic_fetch_add(&background_count, 1, __ATOMIC_RELAXED);
            require(mm_smp_load(DATA) == UINT64_C(0xaabbccdd), "mm-native-remap-value");
        } else if (operation == 5) {
            require(kernel_mm_munmap(mm, DATA, BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_OK, "mm-unmap-retire");
            uint64_t address;
            require(kernel_mm_mmap_anonymous(mm, DATA, BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &address) == KERNEL_MM_STATUS_OK, "mm-remap-vma");
            uint64_t value = UINT64_C(0xaabbccdd);
            require(kernel_copy_to_user(mm, DATA, &value, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK && copied == sizeof(value), "mm-remap-content");
        } else if (operation == 6) {
            user_probe(RISCV_SCAUSE_USER_ECALL, 0, 0, 42);
            __atomic_store_n(&execute_cached, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&code_replaced, __ATOMIC_ACQUIRE))
                __atomic_fetch_add(&background_count, 1, __ATOMIC_RELAXED);
            user_probe(RISCV_SCAUSE_USER_ECALL, 0, 0, 84);
        }
        else if (operation == 7) user_probe(RISCV_SCAUSE_USER_ECALL, 0, 0, 84);
        else if (operation == 8) user_probe(RISCV_SCAUSE_INSTRUCTION_PAGE_FAULT, CODE, 0, 0);
        else if (operation == 9) {
            require(kernel_mm_mprotect(mm, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE) == KERNEL_MM_STATUS_OK, "mm-code-write-permission");
            uint32_t instructions[2] = {UINT32_C(0x05400513), UINT32_C(0x00000073)};
            require(kernel_copy_to_user(mm, CODE, instructions, sizeof(instructions), &copied) == KERNEL_UACCESS_STATUS_OK && copied == sizeof(instructions), "mm-code-replacement");
            require(kernel_mm_mprotect(mm, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_EXECUTE) == KERNEL_MM_STATUS_OK, "mm-code-execute-permission");
        } else if (operation == 10) {
            require(kernel_mm_mprotect(mm, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE) == KERNEL_MM_STATUS_OK, "mm-NX-commit");
        } else if (operation == 16) {
            require(kernel_cpu_current()->logical_id == 0 &&
                kernel_mm_pin_user_page(mm, DATA, KERNEL_MM_READ, &migrating_pin) == KERNEL_MM_STATUS_OK,
                "mm-migration-pin-source");
            asm volatile("mv %0, sp" : "=r"(pin_sp));
        } else if (operation == 17) {
            uintptr_t current_sp; asm volatile("mv %0, sp" : "=r"(current_sp));
            require(kernel_cpu_current()->logical_id == 1 && current_sp == pin_sp &&
                migrating_pin.registration == &migrating_pin && *(uint64_t *)migrating_pin.page == 77,
                "mm-migration-pin-stack-content");
            kernel_mm_unpin_user_page(&migrating_pin);
            __atomic_store_n(&migrated_pin_returned, 1, __ATOMIC_RELEASE);
        } else if (operation == 14) {
            require(kernel_mm_mprotect(mm, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE) == KERNEL_MM_STATUS_OK,
                "mm-kernel-denial-code-write");
            uint32_t instructions[2] = {UINT32_C(0x00053503), UINT32_C(0x00000073)};
            require(kernel_copy_to_user(mm, CODE, instructions, sizeof(instructions), &copied) == KERNEL_UACCESS_STATUS_OK && copied == sizeof(instructions) &&
                kernel_mm_mprotect(mm, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_EXECUTE) == KERNEL_MM_STATUS_OK,
                "mm-kernel-denial-code-publish");
        } else if (operation == 15) {
            user_probe(RISCV_SCAUSE_LOAD_PAGE_FAULT, (uintptr_t)&parent, (uintptr_t)&parent, 0);
        } else if (operation == 11) {
            while (!__atomic_load_n(&root_release, __ATOMIC_ACQUIRE)) {
                struct kernel_mm_user_pin pin = {0};
                require(kernel_mm_pin_user_page(mm, DATA, KERNEL_MM_WRITE, &pin) == KERNEL_MM_STATUS_OK, "mm-root-overlap-pin");
                (*(uint64_t *)pin.page)++;
                kernel_mm_unpin_user_page(&pin);
#if BOAROS_MM_DISK_TEST
                uint64_t content;
                require(!kernel_copy_from_user(mm,&content,SHARED_DATA,sizeof(content),&copied),"disk-CPU1-user-overlap-read");
#endif
                __atomic_fetch_add(&background_count, 1, __ATOMIC_RELAXED);
            }
        } else if (operation == 12) {
            __atomic_store_n(&completed[id], 1, __ATOMIC_RELEASE);
            return;
        }
#if BOAROS_MM_SHARED_TEST
        else if(operation==20) {
            uint64_t value=11;
#if !BOAROS_MM_FILE_TEST
            uint64_t address;
            require(kernel_mm_mmap_anonymous(mm,SHARED_DATA,3*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
                KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED,&address)==KERNEL_MM_STATUS_OK,"shared-map-create");
#endif
            require(kernel_copy_to_user(mm,SHARED_DATA,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK,"shared-resident-before-fork");
            value=23;
            require(kernel_copy_to_user(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK &&
                kernel_mm_mprotect(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0)==KERNEL_MM_STATUS_OK,"shared-protected-before-fork");
        } else if(operation==21) {
            uint64_t value=0x90+id;
            require(kernel_copy_to_user(mm,SHARED_DATA+BOAROS_PAGE_SIZE+id*8,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK &&
                copied==sizeof(value),"shared-simultaneous-first-fault");
        } else if(operation==22) {
            require(kernel_mm_mprotect(mm,SHARED_DATA,BOAROS_PAGE_SIZE,KERNEL_MM_READ)==KERNEL_MM_STATUS_OK,"shared-parent-downgrade");
        } else if(operation==26) {
            uintptr_t irq=arch_interrupt_save(); asm volatile("csrs sstatus,%0"::"r"((UINT64_C(1)<<18)):"memory");
            require(mm_smp_load(SHARED_DATA)==11,"shared-child-cache");
            arch_interrupt_restore(irq); __atomic_store_n(&shared_cached,1,__ATOMIC_RELEASE);
            while(!__atomic_load_n(&shared_protected,__ATOMIC_ACQUIRE)) asm volatile("nop");
#if BOAROS_MM_DISK_TEST
            require(!kernel_mm_resolve_user_fault(mm,SHARED_DATA,KERNEL_MM_WRITE),"disk-child-write-fault-permission");
#endif
            mm_smp_store(SHARED_DATA,88); asm volatile("csrc sstatus,%0"::"r"((UINT64_C(1)<<18)):"memory");
            __atomic_store_n(&shared_written,1,__ATOMIC_RELEASE);
        } else if(operation==27) {
            uint64_t value;
            require(kernel_mm_mprotect(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE)==KERNEL_MM_STATUS_OK &&
                kernel_copy_from_user(mm,&value,SHARED_DATA+2*BOAROS_PAGE_SIZE,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK &&
                value==23,"shared-protected-leaf-remains-shared");
        } else if(operation==28) {
            require(kernel_mm_munmap(mm,SHARED_DATA,3*BOAROS_PAGE_SIZE)==KERNEL_MM_STATUS_OK,"shared-child-unmap");
        } else if(operation==29) {
            require(kernel_mm_pin_user_page(mm,SHARED_DATA,KERNEL_MM_READ,&migrating_pin)==KERNEL_MM_STATUS_OK,"shared-pin-before-migration");
            asm volatile("mv %0, sp":"=r"(pin_sp));
        } else if(operation==30) {
            uintptr_t sp; asm volatile("mv %0, sp":"=r"(sp));
            require(sp==pin_sp && migrating_pin.registration==&migrating_pin &&
                *(uint64_t *)migrating_pin.page==88 && kernel_cpu_current()->logical_id==(cpu_count==2?1U:0U),"shared-migrated-pin-owner");
            __atomic_store_n(&shared_pin_held,1,__ATOMIC_RELEASE);
            while(!__atomic_load_n(&shared_pin_release,__ATOMIC_ACQUIRE)) asm volatile("nop");
            require(*(uint64_t *)migrating_pin.page==88,"shared-last-pin-content-after-unmap");
            kernel_mm_unpin_user_page(&migrating_pin);
#if BOAROS_MM_FILE_TEST
#if BOAROS_MM_DISK_TEST
        } else if(operation==37) {
            uint64_t value=11;
            require(!kernel_copy_to_user(mm,SHARED_DATA,&value,sizeof(value),&copied),"disk-pin-writable-leaf");
            struct kernel_mm_user_pin writer={0};
            require(!kernel_mm_pin_user_page(mm,SHARED_DATA,KERNEL_MM_WRITE,&writer) && writer.cache_write.registration,
                "disk-actual-page-write-borrow");
            __atomic_store_n(&disk_pin_ready,1,__ATOMIC_RELEASE);
            while (!__atomic_load_n(&disk_other_go,__ATOMIC_ACQUIRE)) asm volatile("nop");
            value=1;
            require(!kernel_copy_to_user(mm,DATA,&value,sizeof(value),&copied),"disk-unrelated-page-progress");
            uintptr_t irq=arch_interrupt_save();
            unsigned cpu=kernel_cpu_current()->logical_id;
            probes[cpu].pc=(uintptr_t)mm_smp_store_pc; probes[cpu].cause=RISCV_SCAUSE_STORE_PAGE_FAULT;
            probes[cpu].address=SHARED_DATA; probes[cpu].caught=0; probes[cpu].armed=1;
            asm volatile("csrs sstatus,%0"::"r"((UINT64_C(1)<<18)):"memory");
            mm_smp_store(SHARED_DATA,999);
            asm volatile("csrc sstatus,%0"::"r"((UINT64_C(1)<<18)):"memory");
            require(probes[cpu].caught,"disk-hardware-write-revoked-after-RFENCE"); probes[cpu].armed=0;
            arch_interrupt_restore(irq);
            __atomic_store_n(&disk_other_done,1,__ATOMIC_RELEASE);
            while (!__atomic_load_n(&disk_pin_release,__ATOMIC_ACQUIRE)) asm volatile("nop");
            *(uint64_t *)writer.page=11; kernel_mm_unpin_user_page(&writer);
        } else if(operation==38) {
            struct kernel_open_file_description *owner=file_controller;
            uint64_t address;
            require(!kernel_open_file_acquire(owner) &&
                !kernel_mm_mmap_file_private(mm,&owner,DISK_COLD_DATA,BOAROS_PAGE_SIZE,3*BOAROS_PAGE_SIZE,
                    KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address),
                "disk-background-cold-page-map");
            uint64_t value=1;
            require(!kernel_copy_from_user(mm,&value,DISK_COLD_DATA,sizeof(value),&copied) &&
                copied==sizeof(value) && !value,"disk-cold-progress-while-snapshot-deferred");
            require(!kernel_mm_munmap(mm,DISK_COLD_DATA,BOAROS_PAGE_SIZE),"disk-extra-cold-map-owner-return");
        } else if(operation==39) {
            uint64_t value=11;
            require(!kernel_copy_to_user(mm,SHARED_DATA,&value,sizeof(value),&copied) &&
                copied==sizeof(value),"disk-background-next-generation-write");
#endif
        } else if(operation==32) {
            uint64_t value=44;
            require(kernel_copy_to_user(mm,SHARED_DATA+BOAROS_PAGE_SIZE,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_FAULT &&
                !copied,"file-stale-EOF-candidate-rejected");
            __atomic_store_n(&stale_fault,1,__ATOMIC_RELEASE);
        } else if(operation==33) {
            uint64_t value=55;
            require(kernel_copy_to_user(mm,FILE_PRIVATE_DATA,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK,"file-private-COW");
        } else if(operation==34) {
            uint64_t value;
            require(kernel_copy_from_user(mm,&value,FILE_PRIVATE_DATA,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK && value==11,
                "file-child-private-isolation");
        } else if(operation==36) {
            require(kernel_mm_munmap(mm,FILE_PRIVATE_DATA,BOAROS_PAGE_SIZE)==KERNEL_MM_STATUS_OK,"file-private-alias-return");
        } else if(operation==35) {
            uint64_t value=23;
            require(kernel_mm_mprotect(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE)==KERNEL_MM_STATUS_OK &&
                kernel_copy_to_user(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK &&
                kernel_mm_mprotect(mm,SHARED_DATA+2*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0)==KERNEL_MM_STATUS_OK,"file-repopulate-protected");
#endif
        } else if(operation==31) {
            uint64_t address,value=1;
            require(kernel_mm_mmap_anonymous(mm,DATA,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address)==KERNEL_MM_STATUS_OK &&
                kernel_copy_to_user(mm,DATA,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK,"shared-root-private-test-reset");
        }
#endif
        else if (operation == 13) {
            __atomic_store_n(&cow_ready[0], 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&cow_go, __ATOMIC_ACQUIRE)) asm volatile("nop");
            uint64_t value = UINT64_C(0x11111111);
            require(kernel_copy_to_user(mm, DATA, &value, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
                copied == sizeof(value), "mm-parent-concurrent-COW");
        } else require(0, "mm-unknown-command");
        __atomic_store_n(&completed[id], 1, __ATOMIC_RELEASE);
    }
}
static void fork_parent(void *unused)
{
    (void)unused;
    require(kernel_mm_fork(&child, &parent) == KERNEL_MM_STATUS_OK, "mm-native-fork");
    __atomic_store_n(&fork_done, 1, __ATOMIC_RELEASE);
}
static void child_writer(void *unused)
{
    (void)unused; struct kernel_mm *mm;
    require(kernel_task_mm_borrow_mutable(kernel_task_current(), &mm) == KERNEL_TASK_STATUS_OK, "mm-child-owner");
    __atomic_store_n(&cow_ready[1], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&cow_go, __ATOMIC_ACQUIRE)) asm volatile("nop");
    uint64_t value = UINT64_C(0x22222222); size_t copied;
    require(kernel_copy_to_user(mm, DATA, &value, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
        copied == sizeof(value), "mm-child-concurrent-COW");
}
static void fork_case(void)
{
    pin_held = pin_release = 0;
    send(1, 1); wait_flag(&pin_held, "mm-fork-old-write-pin");
    require(kernel_thread_create_joinable(fork_parent, 0, &fork_join) == KERNEL_SCHEDULER_STATUS_OK, "mm-fork-owner-create");
    for (;;) {
        int blocked;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
          blocked = fork_join.task->state == KERNEL_THREAD_STATE_BLOCKED && !fork_join.task->wait.on_cpu; }
        if (blocked) break;
        require(!__atomic_load_n(&fork_done, __ATOMIC_ACQUIRE), "mm-fork-returned-before-write-pin"); yield();
    }
    __atomic_store_n(&pin_release, 1, __ATOMIC_RELEASE);
    wait_flag(&fork_done, "mm-fork-after-pin-drain"); kernel_thread_join(&fork_join);
    uint64_t value; size_t copied;
    require(kernel_copy_from_user(&child, &value, DATA, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK && value == 77,
        "mm-fork-completed-write-content");
    struct kernel_mm owner = {0};
    require(kernel_mm_acquire(&owner, &child) == KERNEL_MM_STATUS_OK &&
        kernel_thread_create_joinable_mm(child_writer, 0, &cow_join, cpu_count == 2 ? 2 : 1, &owner) ==
            KERNEL_SCHEDULER_STATUS_OK, "mm-child-publication");
    send(0, 13); wait_flag(&cow_ready[0], "mm-parent-COW-ready"); wait_flag(&cow_ready[1], "mm-child-COW-ready");
    __atomic_store_n(&cow_go, 1, __ATOMIC_RELEASE);
    wait_flag(&completed[0], "mm-parent-COW-done"); kernel_thread_join(&cow_join);
    require(kernel_copy_from_user(&parent, &value, DATA, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
        value == UINT64_C(0x11111111), "mm-parent-isolated-content");
    require(kernel_copy_from_user(&child, &value, DATA, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
        value == UINT64_C(0x22222222), "mm-child-isolated-content");
    require(kernel_mm_release(&child) == KERNEL_MM_STATUS_OK, "mm-child-root-return");
    struct kernel_mm_mapping mapping;
    require(kernel_mm_lookup(&parent, DATA, &mapping) == KERNEL_MM_STATUS_OK, "mm-COW-data-owner");
    old_pages[0] = mapping.physical_address;
    virt_uart_puts("BoarOS: MM SMP fork write-pin drain and concurrent COW owners passed\n");
}

#if BOAROS_MM_SHARED_TEST
static void shared_remove(void *unused)
{
    (void)unused; struct kernel_mm *mm;
    require(kernel_task_mm_borrow_mutable(kernel_task_current(),&mm)==KERNEL_TASK_STATUS_OK &&
        kernel_mm_munmap(mm,SHARED_DATA,3*BOAROS_PAGE_SIZE)==KERNEL_MM_STATUS_OK,"shared-parent-retire");
    __atomic_store_n(&shared_remove_done,1,__ATOMIC_RELEASE);
}
static void blocked(struct kernel_thread_join *join)
{
    for(;;) {
        int stopped;
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          stopped=join->task->state==KERNEL_THREAD_STATE_BLOCKED && !join->task->wait.on_cpu; }
        if(stopped) return;
        yield();
    }
}
static void shared_case(void)
{
    shared_stage=1; kernel_wait_queue_init(&shared_prepare_gate);
    request(0,20);
    request(1,12); kernel_thread_join(&actors[1]);
    child=(struct kernel_mm){0};
    require(kernel_mm_fork(&child,&parent)==KERNEL_MM_STATUS_OK,"shared-fork-publication");
    struct kernel_mm_mapping a,b;
    require(kernel_mm_lookup(&parent,SHARED_DATA,&a)==KERNEL_MM_STATUS_OK &&
        kernel_mm_lookup(&child,SHARED_DATA,&b)==KERNEL_MM_STATUS_OK &&
        a.physical_address==b.physical_address && (BOAROS_MM_DISK_TEST || (a.permissions&KERNEL_MM_WRITE)) &&
        (BOAROS_MM_DISK_TEST || (b.permissions&KERNEL_MM_WRITE)),"shared-fork-parent-child-permission");
    require(kernel_mm_lookup(&parent,DATA,&a)==KERNEL_MM_STATUS_OK && !(a.permissions&KERNEL_MM_WRITE),"shared-mixed-private-COW");
    struct kernel_mm owner={0};
    require(kernel_mm_acquire(&owner,&child)==KERNEL_MM_STATUS_OK &&
        kernel_thread_create_joinable_mm(actor,(void *)(uintptr_t)1,&actors[1],cpu_count==2?2:1,&owner)==KERNEL_SCHEDULER_STATUS_OK,
        "shared-child-bound-task");
#if BOAROS_MM_FILE_TEST
    send(1,32); wait_flag(&shared_entered[1],"file-source-before-truncate");
    require(kernel_vfs_ftruncate(&file_controller->file,BOAROS_PAGE_SIZE)==0,"file-controller-shrink");
    uint32_t old_refs;
    require(physical_page_reference_count(&arena,shared_physical[1],&old_refs)==PHYSICAL_PAGE_STATUS_OK && old_refs==1 &&
        (BOAROS_MM_DISK_TEST || arena.shared_anon_pages==1),"file-prepare-reference-survives-truncate");
    __atomic_store_n(&shared_resume,1,__ATOMIC_RELEASE); wake(&shared_prepare_gate);
    wait_flag(&stale_fault,"file-stale-fault-complete"); wait_flag(&completed[1],"file-stale-command-complete");
    require(!kernel_vfs_ftruncate(&file_controller->file,3*BOAROS_PAGE_SIZE),"file-controller-regrow");
    request(0,35); request(1,35);
    shared_entered[0]=shared_entered[1]=shared_resume=shared_created=0;
    file_epoch_probe=1;
#endif
    send(0,21); send(1,21);
    wait_flag(&shared_entered[0],"shared-parent-source-ready"); wait_flag(&shared_entered[1],"shared-child-source-ready");
    require((BOAROS_MM_DISK_TEST || shared_created==1) && shared_physical[0]==shared_physical[1] && (BOAROS_MM_DISK_TEST || arena.shared_anon_pages==3),"shared-one-first-publication");
    __atomic_store_n(&shared_resume,1,__ATOMIC_RELEASE); wake(&shared_prepare_gate);
    wait_flag(&completed[0],"shared-parent-first-fault"); wait_flag(&completed[1],"shared-child-first-fault");
    uint64_t values[2]; size_t copied;
    require(kernel_copy_from_user(&parent,values,SHARED_DATA+BOAROS_PAGE_SIZE,sizeof(values),&copied)==KERNEL_UACCESS_STATUS_OK &&
        values[0]==0x90 && values[1]==0x91,"shared-published-content");
#if BOAROS_MM_FILE_TEST
    request(0,33); request(1,34);
    virt_uart_puts("BoarOS: file MM stale preparation/EOF/regrowth/private COW passed\n");
#endif
#if BOAROS_MM_DISK_TEST
    require(!kernel_vfs_ftruncate(&file_controller->file,4*BOAROS_PAGE_SIZE),"disk-background-cold-page-size");
    struct kernel_page_cache_concurrency_state background;
    kernel_page_cache_get_concurrency_state(&disk_cache,&background);
    uint64_t deferred_before=background.deferred_snapshots;
    send(1,37); wait_flag(&disk_pin_ready,"disk-writer-pin-held");
    require(!kernel_page_cache_request_writeback(&disk_cache),"disk-background-force");
    uint64_t deferred_deadline=riscv_time_read()+UINT64_C(50000000);
    do {
        kernel_page_cache_get_concurrency_state(&disk_cache,&background);
        require((int64_t)(riscv_time_read()-deferred_deadline)<0,"disk-background-defer-deadline"); yield();
    } while (background.deferred_snapshots==deferred_before);
    request(0,38);
    require(!kernel_thread_create_joinable(disk_sync_body,0,&disk_sync_join),"disk-sync-controller");
    for (;;) {
        struct kernel_mm_mapping leaf;
        require(!kernel_mm_lookup(&child,SHARED_DATA,&leaf),"disk-snapshot-leaf");
        if (!(leaf.permissions&KERNEL_MM_WRITE)) break;
        require(!disk_snapshot_done,"disk-snapshot-returned-before-revoke"); yield();
    }
    require(!disk_snapshot_done,"disk-pin-prevents-snapshot-completion");
    __atomic_store_n(&disk_other_go,1,__ATOMIC_RELEASE); wait_flag(&disk_other_done,"disk-other-page-and-native-denial");
    require(!disk_snapshot_done,"disk-other-progress-with-pin-held");
    __atomic_store_n(&disk_pin_release,1,__ATOMIC_RELEASE);
    wait_flag(&completed[1],"disk-pin-return-command"); kernel_thread_join(&disk_sync_join);
    require(disk_snapshot_done,"disk-snapshot-after-pin-return");
    require(!kernel_vfs_ftruncate(&file_controller->file,3*BOAROS_PAGE_SIZE),"disk-extra-cold-page-owner-return");
    struct kernel_page_cache_statistics background_stats;
    kernel_page_cache_get_statistics(&disk_cache,&background_stats);
    uint64_t background_before=background_stats.worker_written;
    request(0,39);
    require(!kernel_page_cache_request_writeback(&disk_cache),"disk-background-submit-force");
    uint64_t background_deadline=riscv_time_read()+UINT64_C(50000000);
    do {
        kernel_page_cache_get_statistics(&disk_cache,&background_stats);
        require((int64_t)(riscv_time_read()-background_deadline)<0,"disk-background-submit-deadline"); yield();
    } while (background_stats.worker_written==background_before);
    { KERNEL_LOCK_SCOPE(io);
      struct kernel_vfs_node *node=kernel_open_file_node(file_controller);
      kernel_vfs_node_lock(node,&io,0);
      uint64_t actual=0; size_t read;
      require(!kernel_vfs_node_pread(node,0,&actual,sizeof(actual),&read) && read==sizeof(actual) && actual==11,
          "disk-background-real-backend-content"); }
    virt_uart_puts("BoarOS: disk MM per-page write pin/native RFENCE/other-page/msync snapshot passed\n");
    virt_uart_puts("BoarOS: disk MM deferred snapshot/cold progress/CPU0 worker writeback passed\n");
#endif
    send(1,26); wait_flag(&shared_cached,"shared-child-hardware-ready"); request(0,22);
    __atomic_store_n(&shared_protected,1,__ATOMIC_RELEASE); wait_flag(&shared_written,"shared-child-write-still-allowed");
    wait_flag(&completed[1],"shared-child-hardware-done");
    request(0,27); request(1,27);
    require(kernel_mm_lookup(&parent,SHARED_DATA+2*BOAROS_PAGE_SIZE,&a)==KERNEL_MM_STATUS_OK &&
        kernel_mm_lookup(&child,SHARED_DATA+2*BOAROS_PAGE_SIZE,&b)==KERNEL_MM_STATUS_OK &&
        a.physical_address==b.physical_address && (BOAROS_MM_DISK_TEST || arena.shared_anon_pages==3),"shared-protected-owner-classification");
#if BOAROS_MM_FILE_TEST
    request(0,36); request(1,36);
#endif
    request(1,28); request(0,29); blocked(&actors[0]);
    if(cpu_count==2) require(kernel_thread_set_cpu_mask(&actors[0],2)==KERNEL_SCHEDULER_STATUS_OK,"shared-pin-migration-mask");
    send(0,30); wait_flag(&shared_pin_held,"shared-pin-on-destination");
    uint64_t root; uint64_t *entries;
    require(kernel_mm_context(&parent,&root)==KERNEL_MM_STATUS_OK &&
        physical_page_resolve(&arena,(root&((UINT64_C(1)<<44)-1))<<12,(void **)&entries)==PHYSICAL_PAGE_STATUS_OK,"shared-root-table");
    shared_retired[3]=(entries[SHARED_DATA>>30]>>10)<<12;
    require(physical_page_resolve(&arena,shared_retired[3],(void **)&entries)==PHYSICAL_PAGE_STATUS_OK,"shared-middle-table");
    shared_retired[4]=(entries[(SHARED_DATA>>21)&511]>>10)<<12;
    for(unsigned i=0;i<3;i++) {
        require(kernel_mm_lookup(&parent,SHARED_DATA+i*BOAROS_PAGE_SIZE,&a)==KERNEL_MM_STATUS_OK,"shared-retired-page");
        shared_retired[i]=a.physical_address;
    }
    for(unsigned i=0;i<5;i++) {
        uint32_t refs; require(physical_page_reference_count(&arena,shared_retired[i],&refs)==PHYSICAL_PAGE_STATUS_OK,"shared-retirement-references");
        shared_refs[i]=refs;
    }
    before_retire=physical_page_available(&arena); fence_entered=fence_release=0; fence_armed=cpu_count==2;
    owner=(struct kernel_mm){0};
    require(kernel_mm_acquire(&owner,&parent)==KERNEL_MM_STATUS_OK &&
        kernel_thread_create_joinable_mm(shared_remove,0,&shared_remover,1,&owner)==KERNEL_SCHEDULER_STATUS_OK,"shared-remover-owner");
    if(cpu_count==2) {
        wait_flag(&fence_entered,"shared-remote-fence-paused");
        struct kernel_mm_concurrency_state state; kernel_mm_get_concurrency_state(&parent,&state);
        require(state.pending_retirement==1 && physical_page_available(&arena)==before_retire &&
            (BOAROS_MM_DISK_TEST || arena.shared_anon_pages==3) && !shared_remove_done,"shared-no-free-before-RFENCE");
        __atomic_store_n(&fence_release,1,__ATOMIC_RELEASE); wake(&fence_gate);
    }
    wait_flag(&shared_remove_done,"shared-remove-after-ack"); kernel_thread_join(&shared_remover);
#if BOAROS_MM_FILE_TEST
    require(!kernel_vfs_ftruncate(&file_controller->file,0),"file-controller-final-backing-return");
#endif
    uint32_t refs; require(physical_page_reference_count(&arena,shared_retired[0],&refs)==PHYSICAL_PAGE_STATUS_OK && refs==1 &&
        arena.shared_anon_pages==
#if BOAROS_FILE_IO_SMP_TEST
        riscv_file_io_retained_shared_pages(),
#else
        0,
#endif
        "shared-only-last-pin-remains");
    __atomic_store_n(&shared_pin_release,1,__ATOMIC_RELEASE); wait_flag(&completed[0],"shared-last-pin-return");
    require(kernel_mm_release(&child)==KERNEL_MM_STATUS_OK,"shared-child-caller-owner-return");
#if BOAROS_MM_FILE_TEST
    require(file_epoch_probe,"file-epoch-test-ran");
#if BOAROS_MM_DISK_TEST
    virt_uart_puts("BoarOS: file MM real ext4 cross-root/source/registration/pin/RFENCE owners passed\n");
#else
    virt_uart_puts("BoarOS: file MM real tmpfs cross-root/source/registration/pin/RFENCE owners passed\n");
#endif
#endif
    virt_uart_puts("BoarOS: shared MM cross-root/fault/protected/migration/RFENCE owners passed\n");
}
#endif

static void capture_old_pages(void)
{
    uint64_t root;
    require(kernel_mm_context(&parent, &root) == KERNEL_MM_STATUS_OK, "mm-root-snapshot");
    uint64_t *entries;
    require(physical_page_resolve(&arena, (root & ((UINT64_C(1) << 44) - 1)) << 12, (void **)&entries) == PHYSICAL_PAGE_STATUS_OK, "mm-root-owner");
    old_pages[1] = (entries[DATA >> 30] >> 10) << 12;
    require(physical_page_resolve(&arena, old_pages[1], (void **)&entries) == PHYSICAL_PAGE_STATUS_OK, "mm-middle-owner");
    old_pages[2] = (entries[(DATA >> 21) & 511] >> 10) << 12;
    struct kernel_mm_mapping mapping;
    require(kernel_mm_lookup(&parent, DATA, &mapping) == KERNEL_MM_STATUS_OK, "mm-data-snapshot");
    old_pages[0] = mapping.physical_address;
}

static void prepare_mm(void)
{
#if BOAROS_MM_FILE_TEST
    int error;
#endif
    require(physical_page_allocate_order(system_pages, 12, &outer) == PHYSICAL_PAGE_STATUS_OK, "mm-arena-owner");
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1; layout.usable[0].base = outer; layout.usable[0].size = UINT64_C(16) << 20;
    require(physical_page_allocator_init(&arena, &layout) == PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_bind_access(&arena, access) == PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_finalize(&arena) == PHYSICAL_PAGE_STATUS_OK &&
        kernel_heap_init(&heap, &arena, physical) == KERNEL_HEAP_STATUS_OK, "mm-private-arena");
    arena_baseline = physical_page_available(&arena);
#if BOAROS_MM_DISK_TEST
    require(!kernel_page_cache_init(&disk_cache,&heap,&arena) &&
        !kernel_vfs_mount_ext4(&disk_mount,disk_backend,disk_backend_heap,&disk_cache,0),"disk-private-cache-mount");
    file_mount=&disk_mount;
    int opened=kernel_open_file_create(&heap,file_mount,"/controlled",&file_controller,&error);
    if (!opened && !error) {
        char content[sizeof(disk_persist)]; size_t count;
        require(!kernel_vfs_pread(&file_controller->file,0,content,sizeof(content),&count, NULL) &&
            count==sizeof(content) && !memcmp(content,disk_persist,sizeof(content)),"disk-independent-restart-content");
        virt_uart_puts("BoarOS: disk MM independent restart durable content passed\n");
    } else require(!kernel_open_file_create_mode(&heap,file_mount,"/controlled",0600,&file_controller,&error) && !error,
        "disk-controller-create");
    require(!kernel_page_cache_start_worker(&disk_cache),"disk-cache-worker");
    file_controller->open_flags=2;
    require(!kernel_vfs_ftruncate(&file_controller->file,3*BOAROS_PAGE_SIZE),"disk-controller-initial-size");
    require(!kernel_file_mapping_prepare(kernel_open_file_node(file_controller)),"disk-controller-mapping-state");
    /* 从临时activate到归还内核上下文只构造私有arena元数据，不执行I/O。
     * 这个未绑定的构造任务不能在临时SATP下被调度器当成内核context检查。 */
    uintptr_t construction_irq=arch_interrupt_save();
#endif
    require(physical_page_allocate(&arena, &template_page) == PHYSICAL_PAGE_STATUS_OK, "mm-owned-root-template");
    void *source, *destination;
    uint64_t kernel_context = riscv_sv39_current_satp();
    require(physical_page_resolve(system_pages, (kernel_context & ((UINT64_C(1) << 44) - 1)) << 12, &source) == PHYSICAL_PAGE_STATUS_OK &&
        physical_page_resolve(&arena, template_page, &destination) == PHYSICAL_PAGE_STATUS_OK, "mm-template-access");
    memset(destination, 0, BOAROS_PAGE_SIZE);
    memcpy((uint64_t *)destination + 256, (uint64_t *)source + 256, BOAROS_PAGE_SIZE / 2);
    struct riscv_sv39_page_table template = {.allocator = &arena, .root_address = template_page, .state = RISCV_SV39_STATE_ACTIVE};
    struct arch_mmu_user_space space = {0};
    require(arch_mmu_user_space_init(&space, &arena, &template) == ARCH_MMU_STATUS_OK &&
        kernel_mm_create(&parent, &space) == KERNEL_MM_STATUS_OK &&
        kernel_mm_vma_enable(&parent, &heap) == KERNEL_MM_STATUS_OK &&
        kernel_mm_brk_initialize(&parent, 0x10000, 0x80000000) == KERNEL_MM_STATUS_OK, "mm-private-record");
    require(kernel_mm_activate(&parent, kernel_context) == KERNEL_MM_STATUS_OK, "mm-initial-activate");
    uint64_t address;
    require(kernel_mm_mmap_anonymous(&parent, DATA, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED
#if BOAROS_MM_SHARED_TEST
        | (root_mode ? KERNEL_MM_MAP_SHARED : 0)
#endif
        , &address) == KERNEL_MM_STATUS_OK &&
        kernel_mm_mmap_anonymous(&parent, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED, &address) == KERNEL_MM_STATUS_OK, "mm-private-vmas");
    uint64_t value = 1; size_t copied; uint32_t instructions[2] = {0x02a00513, 0x00000073};
    require(kernel_copy_to_user(&parent, DATA, &value, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
        kernel_copy_to_user(&parent, CODE, instructions, sizeof(instructions), &copied) == KERNEL_UACCESS_STATUS_OK &&
        kernel_mm_mprotect(&parent, CODE, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_EXECUTE) == KERNEL_MM_STATUS_OK,
        "mm-private-initial-pages");
#if BOAROS_MM_SHARED_TEST
    shared_root_data=root_mode;
#endif
#if BOAROS_MM_FILE_TEST
#if !BOAROS_MM_DISK_TEST
    require(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&file_mount) &&
        !kernel_open_file_create_mode(&heap,file_mount,"/controlled",0600,&file_controller,&error) && !error,
        "file-controller-private-tmpfs");
#endif
#if !BOAROS_MM_DISK_TEST
    file_controller->open_flags=2;
    require(!kernel_vfs_ftruncate(&file_controller->file,3*BOAROS_PAGE_SIZE),"file-controller-initial-size");
#endif
    struct kernel_open_file_description *file_owner=file_controller;
    require(!kernel_open_file_acquire(file_owner) &&
        !kernel_mm_mmap_file_private(&parent,&file_owner,SHARED_DATA,3*BOAROS_PAGE_SIZE,0,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address) && !file_owner,
        "file-controlled-shared-map");
    file_owner=file_controller;
    require(!kernel_open_file_acquire(file_owner) &&
        !kernel_mm_mmap_file_private(&parent,&file_owner,FILE_PRIVATE_DATA,BOAROS_PAGE_SIZE,0,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address),"file-controlled-private-map");
#endif
    require(kernel_mm_activate(0, kernel_context) == KERNEL_MM_STATUS_OK &&
        kernel_mm_check_task_domain(&parent) == KERNEL_MM_STATUS_OK, "mm-private-publication");
    capture_old_pages();
#if BOAROS_MM_DISK_TEST
    arch_interrupt_restore(construction_irq);
#endif
}
static void execute(void *unused)
{
    (void)unused;
    if (root_mode) return;
#if BOAROS_MM_DISK_TEST
    uint64_t work_start=riscv_time_read(),work_end;
#endif
#if BOAROS_MM_SHARED_TEST
    if(shared_root_data) { request(0,31); capture_old_pages(); shared_root_data=0; }
#endif
    for (unsigned phase = 1; phase <= 2; phase++) {
        struct kernel_mm owner = {0}; struct kernel_thread_join failed = {0};
        require(kernel_mm_acquire(&owner, &parent) == KERNEL_MM_STATUS_OK, "mm-create-failure-input-owner");
        uint64_t available = physical_page_available(system_pages);
        fail_create = phase;
        require(kernel_thread_create_joinable_mm(actor, 0, &failed, 1, &owner) == KERNEL_SCHEDULER_STATUS_NO_MEMORY &&
            !fail_create && !failed.task && owner.state == KERNEL_MM_LIVE &&
            physical_page_available(system_pages) == available, "mm-create-failure-keeps-caller-owner");
        require(kernel_mm_release(&owner) == KERNEL_MM_STATUS_OK, "mm-create-failure-caller-return");
    }
    send(1, 1); wait_flag(&pin_held, "mm-old-write-pin-ready");
    send(0, 2);
    for (;;) {
        int blocked;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
          blocked = actors[0].task->state == KERNEL_THREAD_STATE_BLOCKED && !actors[0].task->wait.on_cpu; }
        if (blocked) break;
        require(!__atomic_load_n(&completed[0], __ATOMIC_ACQUIRE), "mm-downgrade-returned-early"); yield();
    }
    __atomic_store_n(&pin_try, 1, __ATOMIC_RELEASE);
    wait_flag(&pin_conflict, "mm-self-pin-conflict-return");
    __atomic_store_n(&pin_release, 1, __ATOMIC_RELEASE);
    wait_flag(&completed[0], "mm-downgrade-complete"); wait_flag(&completed[1], "mm-pin-return");
    request(0, 3);
    if (cpu_count == 2) {
        request(0, 16);
        for (;;) {
            int blocked;
            { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
              blocked = actors[0].task->state == KERNEL_THREAD_STATE_BLOCKED && !actors[0].task->wait.on_cpu; }
            if (blocked) break;
            yield();
        }
        require(kernel_thread_set_cpu_mask(&actors[0], 2) == KERNEL_SCHEDULER_STATUS_OK, "mm-migration-mask-destination");
        send(1, 2);
        for (;;) {
            int blocked;
            { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
              blocked = actors[1].task->state == KERNEL_THREAD_STATE_BLOCKED && !actors[1].task->wait.on_cpu; }
            if (blocked) break;
            require(!__atomic_load_n(&completed[1], __ATOMIC_ACQUIRE), "mm-migration-downgrade-before-pin"); yield();
        }
        request(0, 17); wait_flag(&completed[1], "mm-migration-drain-completion");
        require(migrated_pin_returned, "mm-migration-owner-return");
        for (;;) {
            int blocked;
            { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
              blocked = actors[0].task->state == KERNEL_THREAD_STATE_BLOCKED && !actors[0].task->wait.on_cpu; }
            if (blocked) break;
            yield();
        }
        require(kernel_thread_set_cpu_mask(&actors[0], 1) == KERNEL_SCHEDULER_STATUS_OK, "mm-migration-mask-return");
        request(0, 3);
    }
    fork_case();
    send(1, 4); wait_flag(&hardware_cached, "mm-hardware-cache-ready");
    struct kernel_mm_concurrency_state state;
    kernel_mm_get_concurrency_state(&parent, &state);
    require(cpu_count == 1 || state.active_cpus == 2, "mm-active-secondary-target");
    request(0, 2); __atomic_store_n(&hardware_protected, 1, __ATOMIC_RELEASE);
    wait_flag(&hardware_denied, "mm-hardware-write-denied");
    before_retire = physical_page_available(&arena);
    fence_armed = cpu_count == 2;
    send(0, 5);
    if (cpu_count == 2) {
        wait_flag(&fence_entered, "mm-delayed-RFENCE-ready");
        kernel_mm_get_concurrency_state(&parent, &state);
        require(state.pending_retirement == 1 && state.active_cpus == 2,
            "mm-retirement-and-activity-owner");
        require(physical_page_available(&arena) == before_retire, "mm-premature-owner-return");
        for (unsigned i = 0; i < 3; i++) {
            uint32_t references;
            require(physical_page_reference_count(&arena, old_pages[i], &references) == PHYSICAL_PAGE_STATUS_OK && references == 1,
                "mm-retired-owner-live");
        }
        __atomic_store_n(&fence_release, 1, __ATOMIC_RELEASE); wake(&fence_gate);
    }
    wait_flag(&completed[0], "mm-remap-complete");
    __atomic_store_n(&hardware_remapped, 1, __ATOMIC_RELEASE);
    wait_flag(&completed[1], "mm-hardware-remap-read");
    send(1, 6); wait_flag(&execute_cached, "mm-execute-cache-ready");
    request(0, 9);
    require(cpu_count == 1 || remote_instruction, "mm-remote-FENCE.I-required");
    __atomic_store_n(&code_replaced, 1, __ATOMIC_RELEASE);
    wait_flag(&completed[1], "mm-new-instruction-execution");
    request(1, 7);
    request(0, 10); request(1, 8);
    request(0, 14); request(1, 15);
#if BOAROS_MM_SHARED_TEST
    shared_case();
#endif
    virt_uart_puts("BoarOS: MM SMP native pin/permission/retirement/U-execute contracts passed\n");
#if BOAROS_MM_DISK_TEST
    work_end=riscv_time_read();
#endif
    for (unsigned id = 0; id < 2; id++) { request(id, 12); kernel_thread_join(&actors[id]); }
#if BOAROS_FILES_SMP_TEST
    riscv_files_smp_run();
#endif
#if BOAROS_FILE_IO_SMP_TEST
    riscv_file_io_run();
#endif
    kernel_mm_get_concurrency_state(&parent, &state);
    require(state.references == 1 && !state.pins && !state.write_pins && !state.active_cpus &&
        !state.pending_retirement, "mm-last-owner-quiescent");
    virt_uart_puts("BoarOS: MM metadata bytes="); virt_uart_put_hex(state.record_bytes);
    virt_uart_puts(" pin-bytes="); virt_uart_put_hex(state.pin_bytes);
    virt_uart_puts(" pin-acquire="); virt_uart_put_hex(state.pin_acquisitions);
    virt_uart_puts(" prepared-pages="); virt_uart_put_hex(state.prepared_pages);
    virt_uart_puts(" retired-batches="); virt_uart_put_hex(state.retirement_batches);
    virt_uart_puts(" retired-pages="); virt_uart_put_hex(state.retired_pages);
    virt_uart_puts(" generation="); virt_uart_put_hex(state.generation);
    virt_uart_puts(" fault-retries="); virt_uart_put_hex(state.fault_retries);
    virt_uart_puts(" pin-waits="); virt_uart_put_hex(state.pin_waits); virt_uart_putc('\n');
    require(kernel_mm_release(&parent) == KERNEL_MM_STATUS_OK, "mm-last-owner-release");
#if BOAROS_MM_FILE_TEST
#if BOAROS_MM_DISK_TEST
    size_t persisted; uint64_t observed=0;
    require(!kernel_vfs_pwrite(&file_controller->file,0,disk_persist,sizeof(disk_persist),&persisted) && persisted==sizeof(disk_persist) &&
        !kernel_vfs_sync(&file_controller->file,0,&observed),"disk-final-durable-write");
    kernel_page_cache_stop_worker(&disk_cache);
    struct kernel_page_cache_concurrency_state cache_state;
    struct kernel_page_cache_statistics cache_statistics;
    kernel_page_cache_get_concurrency_state(&disk_cache,&cache_state);
    kernel_page_cache_get_statistics(&disk_cache,&cache_statistics);
    require(!cache_state.pending && !cache_state.running && !cache_state.entry_borrows &&
        cache_state.requests_completed==cache_state.requests_queued,"disk-request-and-entry-drain");
    virt_uart_puts("BoarOS: disk MM cache requests="); virt_uart_put_hex(cache_state.requests_queued);
    virt_uart_puts(" peak-pages="); virt_uart_put_hex(cache_statistics.peak_pages);
    virt_uart_puts(" background-written="); virt_uart_put_hex(cache_statistics.worker_written);
    virt_uart_puts(" deferred-snapshots="); virt_uart_put_hex(cache_state.deferred_snapshots); virt_uart_putc('\n');
#endif
    require(!kernel_open_file_release(&file_controller) && !file_controller && !kernel_vfs_unmount(file_mount),
        "file-controller-last-source-and-mount");
#if BOAROS_MM_DISK_TEST
    require(!kernel_page_cache_destroy(&disk_cache),"disk-cache-last-owner");
    virt_uart_puts("BoarOS: disk MM CPU0 I/O/cache/alias/mount owners returned\n");
#endif
    virt_uart_puts("BoarOS: file MM controller/OFD/registration/private mount owners returned\n");
#endif
    physical_page_release(&arena, template_page);
    struct kernel_heap_statistics statistics; kernel_heap_get_statistics(&heap, &statistics);
    virt_uart_puts("BoarOS: MM arena closure live="); virt_uart_put_hex(statistics.live_allocations);
    virt_uart_puts(" heap-pages="); virt_uart_put_hex(statistics.current_pages);
    virt_uart_puts(" pages="); virt_uart_put_hex(physical_page_available(&arena));
    virt_uart_puts(" baseline="); virt_uart_put_hex(arena_baseline); virt_uart_putc('\n');
    require(!statistics.live_allocations && !statistics.current_pages && physical_page_available(&arena) == arena_baseline &&
        physical_page_allocator_audit(&arena) == PHYSICAL_PAGE_STATUS_OK, "mm-arena-owner-baseline");
    heap = (struct kernel_heap){0}; arena = (struct physical_page_allocator){0};
    physical_page_release_order(system_pages, outer, 12);
    riscv_smp_stop();
#if BOAROS_MM_DISK_TEST
    virt_uart_puts("BoarOS: disk MM OFF ticks setup="); virt_uart_put_hex(disk_setup_ticks);
    virt_uart_puts(" work="); virt_uart_put_hex(work_end-work_start);
    virt_uart_puts(" cleanup="); virt_uart_put_hex(riscv_time_read()-work_end); virt_uart_putc('\n');
#endif
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}
void riscv_smp_test_idle_boundary(struct kernel_cpu *cpu) { (void)cpu; }
static void start_mm_actors(void *unused)
{
    (void)unused;
#if BOAROS_MM_DISK_TEST
    uint64_t setup_start=riscv_time_read();
#endif
    prepare_mm();
    for (unsigned id = 0; id < 2; id++) {
        struct kernel_mm owner = {0};
        kernel_wait_queue_init(&commands[id]);
        require(kernel_mm_acquire(&owner, &parent) == KERNEL_MM_STATUS_OK &&
            kernel_thread_create_joinable_mm(actor, (void *)(uintptr_t)id, &actors[id],
                id == 1 && cpu_count == 2 ? 2 : 1, &owner) == KERNEL_SCHEDULER_STATUS_OK && owner.state == KERNEL_MM_MOVED,
            "mm-task-owner-publication");
    }
    kernel_wait_queue_init(&fence_gate);
#if BOAROS_FILES_SMP_TEST
    riscv_files_smp_prepare(&heap,&parent,cpu_count,file_controller,&disk_cache);
#endif
#if BOAROS_FILE_IO_SMP_TEST
    riscv_file_io_prepare(&heap,&parent,cpu_count,file_controller,&disk_cache);
#endif
    if (root_mode) {
#if BOAROS_FILES_SMP_TEST
        riscv_files_smp_overlap_start();
#endif
#if BOAROS_FILE_IO_SMP_TEST
        riscv_file_io_overlap_start();
#endif
        require(cpu_count == 2 || BOAROS_MM_DISK_TEST, "mm-root-needs-two-CPUs");
        send(1, 11);
#if !BOAROS_MM_DISK_TEST
        uint64_t end = riscv_time_read() + UINT64_C(50000000);
        while (!__atomic_load_n(&background_count, __ATOMIC_ACQUIRE))
            require((int64_t)(riscv_time_read() - end) < 0, "mm-root-background-start");
#endif
        background_begin = __atomic_load_n(&background_count, __ATOMIC_ACQUIRE);
        require(kernel_thread_create_joinable(execute, 0, &controller_join) == KERNEL_SCHEDULER_STATUS_OK, "mm-controller-create");
    } else require(kernel_thread_create(execute, 0) == KERNEL_SCHEDULER_STATUS_OK, "mm-controller-create");
#if BOAROS_MM_DISK_TEST
    disk_setup_ticks=riscv_time_read()-setup_start;
    __atomic_store_n(&disk_setup_ready,1,__ATOMIC_RELEASE);
#endif
}
void riscv_smp_test_start(struct physical_page_allocator *pages,int has_root)
{
    system_pages=pages; baseline=physical_page_available(pages);
    root_mode=has_root; cpu_count=riscv_smp_cpu(1)?2:1;
#if BOAROS_MM_DISK_TEST
    /* 已启用IRQ的真实块I/O须在可睡眠CPU0任务执行，不能借启动栈伪装。 */
    require(kernel_thread_create_joinable(start_mm_actors,0,&disk_setup_join)==KERNEL_SCHEDULER_STATUS_OK,
        "disk-setup-CPU0-task");
#else
    start_mm_actors(0);
#endif
}
void riscv_smp_test_root_finish(void)
{
#if BOAROS_MM_DISK_TEST
    /* cleanup worker入口关闭IRQ；这里仍等待普通服务任务的真实I/O。
     * yield在只剩本任务runnable时会选回自己，不能靠它接收设备IRQ。 */
    uintptr_t finish_irq=arch_interrupt_save();
    arch_interrupt_restore(finish_irq|RISCV_SSTATUS_SIE);
    wait_flag(&disk_setup_ready,"disk-setup-publication");
    kernel_thread_join(&disk_setup_join);
    while (!__atomic_load_n(&background_count,__ATOMIC_ACQUIRE)) yield();
#endif
#if BOAROS_FILES_SMP_TEST
    riscv_files_smp_overlap_finish();
#endif
#if BOAROS_FILE_IO_SMP_TEST
    riscv_file_io_overlap_finish();
#endif
    require(root_mode && __atomic_load_n(&background_count, __ATOMIC_ACQUIRE) > background_begin, "mm-real-user-overlap");
    __atomic_store_n(&root_release, 1, __ATOMIC_RELEASE);
    wait_flag(&completed[1], "mm-root-overlap-stop");
    root_mode = 0;
    kernel_thread_join(&controller_join);
    require(kernel_thread_create_joinable(execute, 0, &controller_join) == KERNEL_SCHEDULER_STATUS_OK, "mm-root-controller-restart");
    kernel_thread_join(&controller_join);
    require(done, "mm-root-native-completion");
    virt_uart_puts("BoarOS: SMP root overlap counter="); virt_uart_put_hex(background_count);
    virt_uart_puts(" controller/arena/wait owners returned\n");
#if BOAROS_MM_DISK_TEST
    arch_interrupt_restore(finish_irq);
#endif
}
void riscv_smp_test_poll(void)
{
    if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(!scheduler.all_tasks && !scheduler.task_count && !scheduler.runqueue.first &&
          !scheduler.retired_pending && !scheduler.retired_tasks && !scheduler.controlled_owners &&
          !kernel_cpu_current()->active_mm_record, "mm-final-task-and-CPU-owner-baseline"); }
    require(physical_page_available(system_pages) == baseline &&
        physical_page_allocator_audit(system_pages) == PHYSICAL_PAGE_STATUS_OK, "mm-system-owner-baseline");
    virt_uart_puts("BoarOS: MM SMP natural exit arena/MM/pin/table/stack/wait owners returned\n");
    sbi_shutdown();
}
