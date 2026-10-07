#include <arch/task.h>
#include <arch/timer.h>
#include <arch/elf.h>
#include <kernel/elf_image.h>
#include <kernel/heap.h>
#include <kernel/tmpfs.h>
#include <kernel/vfs.h>
#include <kernel/fs_context.h>
#include <kernel/files.h>
#include <kernel/time.h>
#include <kernel/stack.h>
#include <platform/loongarch_virt.h>
#include <string.h>
void la_fault_injection_set(int);
void la_fault_injection_arm(void);
void la_elf_failure_contract(struct kernel_heap *);
extern const unsigned char la_user_elf_start[], la_user_elf_end[];
static struct kernel_heap heap;
static struct kernel_vfs_mount *root;
static volatile unsigned worker_started;
static void worker(void *argument)
{
    unsigned bit=1U<<(uintptr_t)argument;
    worker_started|=bit;
    uint64_t deadline=arch_time_read()+la_timer_frequency()*3;
    while (worker_started!=3) if ((int64_t)(arch_time_read()-deadline)>=0) la_virt_fatal("kernel preemption");
}
static struct kernel_thread_completion reap(void)
{
    struct kernel_thread_completion completion;
    uint64_t deadline=arch_time_read()+la_timer_frequency()*10;
    for (;;) {
        uintptr_t irq=arch_interrupt_save();
        enum kernel_scheduler_status status=kernel_scheduler_reap_one(&completion);
        arch_interrupt_restore(irq);
        if (status==KERNEL_SCHEDULER_STATUS_OK) return completion;
        if (status!=KERNEL_SCHEDULER_STATUS_EMPTY) {
            la_virt_puts("reap status=");la_virt_hex(status);la_virt_puts("\n");la_virt_fatal("reap");
        }
        if ((int64_t)(arch_time_read()-deadline)>=0) la_virt_fatal("task timeout");
        arch_cpu_wait();
    }
}
static void run(const char *name,enum kernel_thread_exit_reason expected_reason,uint64_t expected_status)
{
    uintptr_t irq=arch_interrupt_save();
    uint64_t before=physical_page_available(heap.page_allocator);
    struct kernel_read_source reader;
    struct kernel_elf64_source *source=0;
    struct kernel_exec_image image={0};
    if (kernel_read_source_from_memory(la_user_elf_start,la_user_elf_end-la_user_elf_start,&reader) ||
        kernel_elf64_source_create_reader(&heap,&reader,BOAROS_PAGE_SIZE,ARCH_ELF_MACHINE,&source)!=KERNEL_ELF64_SOURCE_STATUS_OK)
        la_virt_fatal("ELF source");
    struct kernel_exec_string argv[]={{"/la-probe",9},{name,strlen(name)}};
    struct kernel_exec_image_request request={.executable_source=source,.executable=argv[0],.arguments=argv,.argument_count=2};
    int64_t error;
    enum kernel_exec_image_status status=kernel_exec_image_prepare(&request,&heap,&image,&error);
    if (status!=KERNEL_EXEC_IMAGE_STATUS_OK) {
        la_virt_puts("image status=");la_virt_hex(status);la_virt_puts(" errno=");la_virt_hex(error);la_virt_puts("\n");
        la_virt_fatal("ELF image");
    }
    if (kernel_elf64_source_release(&source)!=KERNEL_ELF64_SOURCE_STATUS_OK) la_virt_fatal("source pin");
    struct kernel_files files={0}; struct kernel_fs_context fs={0};
    if (kernel_files_create(&files,&heap)!=KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_create(&fs,root,&heap)!=KERNEL_FS_CONTEXT_STATUS_OK) la_virt_fatal("user files");
    if (!strcmp(name,"console")) {
        int64_t opened;
        if (kernel_files_open_console(&files,0,&opened)!=KERNEL_FILES_STATUS_OK || opened)
            la_virt_fatal("console fd");
    }
    if (kernel_user_thread_create(&image.mm,&files,&fs,0,image.stack_pointer,image.thread_pointer)!=KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        kernel_user_thread_create(&image.mm,&files,&fs,image.entry,image.stack_pointer+1,image.thread_pointer)!=KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT)
        la_virt_fatal("invalid user creation");
    for (unsigned failure=0;failure<2;failure++) {
        uint64_t prepared=physical_page_available(heap.page_allocator);
        la_fault_injection_set((int)failure);
        enum kernel_scheduler_status creation=kernel_user_thread_create(&image.mm,&files,&fs,image.entry,image.stack_pointer,image.thread_pointer);
        la_fault_injection_set(-1);
        if (creation!=KERNEL_SCHEDULER_STATUS_NO_MEMORY || image.mm.state!=KERNEL_MM_LIVE ||
            files.state!=KERNEL_FILES_LIVE || fs.state!=KERNEL_FS_CONTEXT_LIVE ||
            physical_page_available(heap.page_allocator)!=prepared) la_virt_fatal("creation failure ownership");
    }
    if (kernel_user_thread_create(&image.mm,&files,&fs,image.entry,image.stack_pointer,image.thread_pointer)!=KERNEL_SCHEDULER_STATUS_OK)
        la_virt_fatal("user create");
    if (expected_reason==KERNEL_THREAD_EXIT_RESOURCE) la_fault_injection_arm();
    arch_interrupt_restore(irq);
    struct kernel_thread_completion result=reap();
    la_fault_injection_set(-1);
    if (result.reason!=expected_reason || result.status!=expected_status) {
        la_virt_puts("case=");la_virt_puts(name);la_virt_puts(" reason=");la_virt_hex(result.reason);
        la_virt_puts(" status=");la_virt_hex(result.status);la_virt_puts("\n");la_virt_fatal("user result");
    }
    if (kernel_exec_image_cleanup(&heap,&image)!=KERNEL_EXEC_IMAGE_STATUS_OK) la_virt_fatal("image cleanup");
    irq=arch_interrupt_save();
    uint64_t after=physical_page_available(heap.page_allocator);
    arch_interrupt_restore(irq);
    la_virt_puts(expected_reason==KERNEL_THREAD_EXIT_RESOURCE ? "LA mechanism " : "LA case ");la_virt_puts(name);la_virt_puts(" reason=");la_virt_hex(result.reason);
    la_virt_puts(" status=");la_virt_hex(result.status);la_virt_puts(" pages ");la_virt_hex(before);la_virt_puts("/");la_virt_hex(after);la_virt_puts("\n");
    if (before!=after) la_virt_fatal("user resource baseline");
}
void la_user_contract(struct physical_page_allocator *allocator)
{
    static struct arch_mmu_page_table table;
    table.allocator=allocator;table.state=ARCH_MMU_STATE_ACTIVE;
    if (kernel_heap_init(&heap,allocator,la_virt_physical_address)!=KERNEL_HEAP_STATUS_OK ||
        kernel_exec_image_bind(allocator,&table) || kernel_tmpfs_create(&heap,0,"",&root)) la_virt_fatal("user environment");
    if (kernel_thread_create(worker,(void *)0)!=KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(worker,(void *)1)!=KERNEL_SCHEDULER_STATUS_OK) la_virt_fatal("kernel workers");
    if (la_timer_start(la_timer_frequency(),100)!=ARCH_TIMER_STATUS_OK) la_virt_fatal("timer start");
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    for (unsigned i=0;i<2;i++) {
        struct kernel_thread_completion result=reap();
        if (result.kind!=KERNEL_THREAD_KIND_KERNEL || result.reason!=KERNEL_THREAD_EXIT_RETURNED) la_virt_fatal("worker result");
    }
    la_virt_puts("LA kernel timer preemption passed\n");
    uintptr_t irq=arch_interrupt_save();
    la_elf_failure_contract(&heap);
    arch_interrupt_restore(irq);
    run("contracts",KERNEL_THREAD_EXIT_SYSCALL,0);run("spin",KERNEL_THREAD_EXIT_SYSCALL,0);
    run("exitgroup",KERNEL_THREAD_EXIT_SYSCALL,0);
    run("readonly",KERNEL_THREAD_EXIT_SIGNAL,11);run("none",KERNEL_THREAD_EXIT_SIGNAL,11);
    run("nonexec",KERNEL_THREAD_EXIT_SIGNAL,11);run("unmapped",KERNEL_THREAD_EXIT_SIGNAL,11);
    run("kernel",KERNEL_THREAD_EXIT_SIGNAL,7);
    run("console",KERNEL_THREAD_EXIT_SYSCALL,0);
    run("faultoom",KERNEL_THREAD_EXIT_RESOURCE,KERNEL_THREAD_RESOURCE_NO_MEMORY);
    if (kernel_vfs_unmount(root)) la_virt_fatal("root lifetime");
    struct kernel_heap_statistics statistics;
    kernel_heap_get_statistics(&heap,&statistics);
    if (statistics.live_allocations) la_virt_fatal("heap lifetime");
    struct kernel_stack_statistics stacks;
    kernel_scheduler_stack_statistics(&stacks);
    if (!stacks.stacks_released || stacks.minimum_free_bytes<KERNEL_STACK_MINIMUM_RESERVE)
        la_virt_fatal("stack reserve");
    la_virt_puts("LA task stacks released=");la_virt_hex(stacks.stacks_released);
    la_virt_puts(" minimum reserve=");la_virt_hex(stacks.minimum_free_bytes);
    la_virt_puts(" peak use=");la_virt_hex(stacks.maximum_used_bytes);la_virt_puts("\n");
    la_virt_puts("LA user contracts passed\n");
}
