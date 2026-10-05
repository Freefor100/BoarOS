#include <kernel/cost.h>
#include <arch/riscv/context.h>
#include <arch/riscv/mm.h>
#include <kernel/open_file.h>
#include <kernel/socket.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/uaccess.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/timer.h>
#include <kernel/time.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>
#include "../../fs/vfs_internal.h"
#include "../../kernel/sched/private.h"
#include <string.h>

static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct kernel_vfs_file file;
static struct riscv_virtio_mmio_block device;
static unsigned char payload[4096];
static struct kernel_mm sync_mm;
static struct riscv_sv39_page_table protocol_table;
static uint64_t sync_satp, sync_address;
static int sync_edit, operation_testing;
static struct kernel_files operation_files;
static struct kernel_task *operation_writer_task, *operation_contender_task;
static struct kernel_wait_queue operation_held;
static uint64_t operation_second;
static unsigned operation_paused, operation_done, operation_mode;
static int64_t operation_fd[2];
static int pipe_copy_testing;
static unsigned pipe_copy_entered;
static int32_t pipe_copy_pair[2];
static unsigned send_copy_testing, send_copy_paused, send_copy_mode;
static struct kernel_task *send_copy_task;
static struct kernel_wait_queue send_copy_held;
static int64_t send_copy_fd;
#define OP_USER UINT64_C(0x21000000)
struct kernel_task *__real_kernel_task_current(void);
struct kernel_task *__wrap_kernel_task_current(void)
{ return operation_testing && sync_edit ? scheduler.current : __real_kernel_task_current(); }
void __real_kernel_vfs_node_lock(struct kernel_vfs_node *, struct kernel_lock_guard *, int);
void __wrap_kernel_vfs_node_lock(struct kernel_vfs_node *node, struct kernel_lock_guard *guard, int write)
{
    int edit = sync_edit;
    if (operation_testing) sync_edit = 0;
    __real_kernel_vfs_node_lock(node, guard, write);
    sync_edit = edit;
}
enum kernel_page_cache_status __real_kernel_open_file_get_page(struct kernel_open_file_description *, uint64_t, uint64_t *, size_t *);
enum kernel_page_cache_status __wrap_kernel_open_file_get_page(struct kernel_open_file_description *ofd, uint64_t index, uint64_t *page, size_t *valid)
{
    int edit = sync_edit;
    if (operation_testing) sync_edit = 0;
    enum kernel_page_cache_status result = __real_kernel_open_file_get_page(ofd, index, page, valid);
    sync_edit = edit;
    return result;
}
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *, void *, uint64_t, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *mm, void *buffer, uint64_t address, size_t size, size_t *copied);

static struct kernel_open_file_description *sync_source;
static struct kernel_wait_queue sync_held;
static struct kernel_task *sync_task;
static unsigned sync_entered, sync_active, sync_finished, sync_released;
static int sync_error, sync_close_error;
uint64_t __real_riscv_sv39_current_satp(void);
uint64_t __wrap_riscv_sv39_current_satp(void)
{ return sync_edit ? sync_satp : __real_riscv_sv39_current_satp(); }
int __real_kernel_open_file_sync_range(struct kernel_open_file_description *, uint64_t, uint64_t);
int __wrap_kernel_open_file_sync_range(struct kernel_open_file_description *ofd, uint64_t start, uint64_t end)
{
    if (ofd == sync_source) sync_active = 1;
    int result = __real_kernel_open_file_sync_range(ofd, start, end);
    if (ofd == sync_source) { sync_active = 0; if (sync_error) return -5; }
    return result;
}
enum kernel_open_file_status __real_kernel_open_file_release(struct kernel_open_file_description **);
enum kernel_open_file_status __wrap_kernel_open_file_release(struct kernel_open_file_description **owner);
static unsigned loading_probe, loads, done, write_probe, background_fail;
static uint64_t pressure_pages[1024], failed_offset;
static struct kernel_wait_queue held_write;
static unsigned stop_probe, stop_entered, stop_called, stop_done;
static struct kernel_page_cache instance_peer;
static struct kernel_vfs_node *instance_nodes[2];
static struct kernel_wait_queue instance_held[2];
static unsigned instance_hold, instance_entered[2], instance_waiting, instance_wait_done;
static unsigned instance_stop_called, instance_stop_done;
static uint64_t instance_peer_pin;
static uint64_t observed[2], inserted_page;
static struct kernel_page_cache_alias alias;
static void rearm(void *owner, uint64_t address) { (void)owner; (void)address; }
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("I/O sleep failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
enum kernel_open_file_status __wrap_kernel_open_file_release(struct kernel_open_file_description **owner)
{
    if (*owner == sync_source) {
        check(!sync_active, 220); /* 最后映射撤销不能释放仍在 msync 中使用的 OFD。 */
        if (sync_close_error) { sync_close_error = 0; return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED; }
        sync_released++;
    }
    return __real_kernel_open_file_release(owner);
}
static void sync_inode_holder(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(kernel_vfs_file_node(&file), &guard, 1);
    sync_entered = 1;
    enum kernel_wait_wake_reason reason;
    check(kernel_scheduler_block_current(&sync_held, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 221);
    riscv_interrupt_restore(irq);
}
static void sync_mapping_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    sync_task = kernel_task_current();
    check(kernel_mm_msync(&sync_mm, sync_address, 4096, 4) == sync_error, 222);
    sync_finished = 1;
    riscv_interrupt_restore(irq);
}
static void sync_source_probe(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    for (unsigned round = 0; round < 2; round++) {
        sync_entered = sync_active = sync_finished = sync_released = 0;
        sync_error = round ? -5 : 0; sync_close_error = round;
        sync_mm = (struct kernel_mm){0};
        struct riscv_sv39_page_table table = {0};
        struct riscv_sv39_user_space space = {0};
        struct kernel_open_file_description *owner = 0;
        int result;
        check(riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 235);
        /* 只测试 MM owner 与调度，借用空内核根；当前 hart 仍在 Bare 模式。 */
        table.state = RISCV_SV39_STATE_ACTIVE;
        check(riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
            riscv_kernel_mm_create(&sync_mm, &space) == KERNEL_MM_STATUS_OK &&
            kernel_mm_vma_enable(&sync_mm, &heap) == KERNEL_MM_STATUS_OK &&
            kernel_mm_brk_initialize(&sync_mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
            riscv_kernel_mm_satp(&sync_mm, &sync_satp) == KERNEL_MM_STATUS_OK &&
            kernel_open_file_create(&heap, &mount, "/concurrent", &owner, &result) == KERNEL_OPEN_FILE_STATUS_OK && result == 0, 223);
        sync_source = owner;
        check(kernel_mm_mmap_file_private(&sync_mm, &owner, 0x20000000, 4096, 0,
            KERNEL_MM_READ, KERNEL_MM_MAP_SHARED, &sync_address) == KERNEL_MM_STATUS_OK && !owner, 224);
        kernel_wait_queue_init(&sync_held);
        struct kernel_thread_join holder = {0}, synchronizer = {0};
        check(kernel_thread_create_joinable(sync_inode_holder, 0, &holder) == KERNEL_SCHEDULER_STATUS_OK, 225);
        while (!sync_entered) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 226);
        check(kernel_thread_create_joinable(sync_mapping_worker, 0, &synchronizer) == KERNEL_SCHEDULER_STATUS_OK, 227);
        while (!sync_task || sync_task->state != KERNEL_THREAD_STATE_BLOCKED)
            check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 228);
        check(sync_active && !sync_finished, 229);
        /* 被取消的同步仍拥有等待中的 OFD，不能把不可中断 I/O 当成已结束。 */
        sync_task->terminate_requested = 1;
        sync_edit = 1;
        check(kernel_mm_munmap(&sync_mm, sync_address, 4096) == KERNEL_MM_STATUS_OK && !sync_released, 230);
        sync_edit = 0;
        kernel_wait_queue_wake_all(&sync_held);
        kernel_thread_join(&holder);
        kernel_thread_join(&synchronizer);
        check(sync_finished && sync_released == (round ? 0U : 1U), 231);
        sync_task = 0; sync_satp = 0;
        check(kernel_mm_release(&sync_mm) == KERNEL_MM_STATUS_OK &&
            physical_page_release(&allocator, table.root_address) == PHYSICAL_PAGE_STATUS_OK, 232);
        check(sync_released == 1, 236);
        sync_source = 0;
    }
    virt_uart_puts("I/O msync source pin passed\n");
    riscv_interrupt_restore(irq);
}
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *mm, void *buffer, uint64_t address, size_t size, size_t *copied)
{
    if (send_copy_testing && mm == &sync_mm) {
        if (address == OP_USER && scheduler.current == send_copy_task && !send_copy_paused) {
            send_copy_paused = 1;
            enum kernel_wait_wake_reason reason;
            check(kernel_scheduler_block_current(&send_copy_held, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 310);
        }
        int previous = sync_edit; sync_edit = 1;
        enum kernel_uaccess_status result = __real_kernel_copy_from_user(mm, buffer, address, size, copied);
        sync_edit = previous; return result;
    }
    if (pipe_copy_testing && mm == &sync_mm) {
        if (address == OP_USER + 4096 && !pipe_copy_entered) {
            pipe_copy_entered = 1;
            /* 模型化合法的用户缺页调度，让另一写者进入；不改实际复制或发布结果。 */
            check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 290);
        }
        int edit = sync_edit;
        sync_edit = 1;
        enum kernel_uaccess_status result = __real_kernel_copy_from_user(mm, buffer, address, size, copied);
        sync_edit = edit;
        return result;
    }
    if (!operation_testing || mm != &sync_mm)
        return __real_kernel_copy_from_user(mm, buffer, address, size, copied);
    if (address == operation_second && kernel_task_current() == operation_writer_task && !operation_paused) {
        operation_paused = 1;
        enum kernel_wait_wake_reason reason;
        check(kernel_scheduler_block_current(&operation_held, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 240);
    }
    /* 使用真实 MM/uaccess；只模型化未激活 MM 的 satp，调度和 VFS 等待仍用真实 hart。 */
    sync_edit = 1;
    enum kernel_uaccess_status result = __real_kernel_copy_from_user(mm, buffer, address, size, copied);
    sync_edit = 0;
    return result;
}
static void operation_writer(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    operation_writer_task = kernel_task_current();
    int64_t result;
    enum kernel_files_status status = operation_mode == 1
        ? kernel_files_pwrite(&operation_files, &sync_mm, operation_fd[0], OP_USER + 4096, 8192, 0, &result)
        : operation_mode == 3
        ? kernel_files_write(&operation_files, &sync_mm, operation_fd[0], OP_USER + 4096, 8192, &result)
        : kernel_files_writev(&operation_files, &sync_mm, operation_fd[0], OP_USER + 128, 2, &result);
    check(status == KERNEL_FILES_STATUS_OK && result == 8192, 241);
    riscv_interrupt_restore(irq);
}
static void operation_contender(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    operation_contender_task = kernel_task_current();
    int64_t result;
    enum kernel_files_status status = operation_mode == 2
        ? kernel_files_ftruncate(&operation_files, operation_fd[1], 0, &result)
        : operation_mode == 0
        ? kernel_files_write(&operation_files, &sync_mm, operation_fd[1], OP_USER + 16384, 8192, &result)
        : kernel_files_pwrite(&operation_files, &sync_mm, operation_fd[1], OP_USER + 16384, 8192, 0, &result);
    check(status == KERNEL_FILES_STATUS_OK && result == (operation_mode == 2 ? 0 : 8192), 242);
    operation_done = 1;
    riscv_interrupt_restore(irq);
}
static void whole_write_probe(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    for (operation_mode = 0; operation_mode < 4; operation_mode++) {
        operation_testing = 1;
        operation_paused = operation_done = 0;
        operation_writer_task = operation_contender_task = 0;
        kernel_wait_queue_init(&operation_held);
        sync_mm = (struct kernel_mm){0};
        operation_files = (struct kernel_files){0};
        struct riscv_sv39_page_table table = {0};
        struct riscv_sv39_user_space space = {0};
        struct kernel_fs_context fs = {0};
        struct kernel_vfs_file observed = {0};
        check(riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 243);
        table.state = RISCV_SV39_STATE_ACTIVE;
        check(riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
            riscv_kernel_mm_create(&sync_mm, &space) == KERNEL_MM_STATUS_OK &&
            kernel_mm_vma_enable(&sync_mm, &heap) == KERNEL_MM_STATUS_OK &&
            kernel_mm_brk_initialize(&sync_mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
            riscv_kernel_mm_satp(&sync_mm, &sync_satp) == KERNEL_MM_STATUS_OK &&
            kernel_files_create(&operation_files, &heap) == KERNEL_FILES_STATUS_OK &&
            kernel_fs_context_create(&fs, &mount, &heap) == KERNEL_FS_CONTEXT_STATUS_OK, 244);
        uint64_t address;
        check(kernel_mm_mmap_anonymous(&sync_mm, OP_USER, 24576, KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK, 245);
        size_t copied;
        sync_edit = 1;
        check(kernel_copy_to_user(&sync_mm, OP_USER, "/write-operation", 17, &copied) == KERNEL_UACCESS_STATUS_OK, 246);
        memset(payload, 'A', sizeof(payload));
        check(kernel_copy_to_user(&sync_mm, OP_USER + 4096, payload, 4096, &copied) == KERNEL_UACCESS_STATUS_OK &&
            kernel_copy_to_user(&sync_mm, OP_USER + 8192, payload, 4096, &copied) == KERNEL_UACCESS_STATUS_OK, 247);
        memset(payload, 'B', sizeof(payload));
        check(kernel_copy_to_user(&sync_mm, OP_USER + 16384, payload, 4096, &copied) == KERNEL_UACCESS_STATUS_OK &&
            kernel_copy_to_user(&sync_mm, OP_USER + 20480, payload, 4096, &copied) == KERNEL_UACCESS_STATUS_OK, 248);
        sync_edit = 0;
        int64_t result;
        uint64_t flags = 2 | 0100 | 01000 | (operation_mode == 0 ? KERNEL_FILES_O_APPEND : 0);
        check(kernel_files_openat(&operation_files, &fs, &sync_mm, -100, OP_USER,
            flags | (operation_mode == 3 ? KERNEL_FILES_O_SYNC : 0), 0600, &operation_fd[0]) == KERNEL_FILES_STATUS_OK && operation_fd[0] >= 0 &&
            kernel_files_openat(&operation_files, &fs, &sync_mm, -100, OP_USER,
            flags & ~UINT64_C(01000), 0600, &operation_fd[1]) == KERNEL_FILES_STATUS_OK && operation_fd[1] >= 0 &&
            kernel_vfs_open(&mount, "/write-operation", &observed) == 0, 249);
        operation_second = OP_USER + 8192;
        if (!operation_mode) {
            memset(payload, 'A', sizeof(payload)); size_t written; uint64_t sequence = 0;
            check(kernel_vfs_pwrite(&observed, 0, payload, 4096, &written) == 0 && written == 4096 &&
                kernel_vfs_sync(&observed, 0, &sequence) == 0, 250);
            struct kernel_open_file_description *pin = 0;
            check(kernel_files_pin(&operation_files, operation_fd[0], &pin, &result) == KERNEL_FILES_STATUS_OK && !result &&
                kernel_mm_mmap_file_private(&sync_mm, &pin, 0x30000000, 4096, 0, KERNEL_MM_READ,
                    KERNEL_MM_MAP_FIXED_NOREPLACE, &operation_second) == KERNEL_MM_STATUS_OK && !pin, 251);
            (void)kernel_page_cache_reclaim(&cache, 64);
        }
        struct kernel_uaccess_iovec iov[2] = {{OP_USER + 4096, 4096}, {operation_second, 4096}};
        sync_edit = 1;
        check(kernel_copy_to_user(&sync_mm, OP_USER + 128, iov, sizeof(iov), &copied) == KERNEL_UACCESS_STATUS_OK, 252);
        sync_edit = 0;
        struct kernel_thread_join writer = {0}, contender = {0};
        check(kernel_thread_create_joinable(operation_writer, 0, &writer) == KERNEL_SCHEDULER_STATUS_OK, 253);
        while (!operation_paused) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 254);
        check(kernel_thread_create_joinable(operation_contender, 0, &contender) == KERNEL_SCHEDULER_STATUS_OK, 255);
        while (!operation_done && (!operation_contender_task || operation_contender_task->state != KERNEL_THREAD_STATE_BLOCKED))
            check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 256);
        if (operation_mode == 3) operation_writer_task->terminate_requested = 1;
        check(kernel_wait_queue_wake_all(&operation_held) == KERNEL_SCHEDULER_STATUS_OK, 257);
        kernel_thread_join(&writer); kernel_thread_join(&contender);
        uint64_t size = kernel_vfs_file_size(&observed);
        check(size == (operation_mode == 2 ? 0U : operation_mode == 0 ? 20480U : 8192U), 258);
        for (uint64_t offset = 0; offset < size; offset += 4096) {
            size_t read;
            check(kernel_vfs_pread(&observed, offset, payload, 4096, &read) == 0 && read == 4096, 259);
            unsigned char expected = operation_mode == 0 && offset < 12288 ? 'A' : 'B';
            for (unsigned i = 0; i < 4096; i++) check(payload[i] == expected, 260);
        }
        operation_testing = 0; sync_edit = 0; sync_satp = 0;
        check(kernel_files_release(&operation_files) == KERNEL_FILES_STATUS_OK &&
            kernel_mm_release(&sync_mm) == KERNEL_MM_STATUS_OK &&
            kernel_fs_context_release(&fs) == KERNEL_FS_CONTEXT_STATUS_OK &&
            kernel_vfs_close(&observed) == 0 &&
            physical_page_release(&allocator, table.root_address) == PHYSICAL_PAGE_STATUS_OK, 261);
    }
    virt_uart_puts("I/O whole write operation passed\n");
    riscv_interrupt_restore(irq);
}
static int receive_testing;
static void pipe_copy_writer(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    int64_t result;
    check(kernel_files_write(&operation_files, &sync_mm, pipe_copy_pair[1],
        OP_USER + 4096 + (uintptr_t)argument * 4096, 25, &result) == KERNEL_FILES_STATUS_OK && result == 25, 291);
    riscv_interrupt_restore(irq);
}
static void pipe_copy_probe(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    struct riscv_sv39_page_table table = {0};
    struct riscv_sv39_user_space space = {0};
    sync_mm = (struct kernel_mm){0};
    operation_files = (struct kernel_files){0};
    check(riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 292);
    table.state = RISCV_SV39_STATE_ACTIVE;
    check(riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
        riscv_kernel_mm_create(&sync_mm, &space) == KERNEL_MM_STATUS_OK &&
        kernel_mm_vma_enable(&sync_mm, &heap) == KERNEL_MM_STATUS_OK &&
        kernel_mm_brk_initialize(&sync_mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
        riscv_kernel_mm_satp(&sync_mm, &sync_satp) == KERNEL_MM_STATUS_OK &&
        kernel_files_create(&operation_files, &heap) == KERNEL_FILES_STATUS_OK, 293);
    uint64_t address;
    size_t copied;
    int64_t result;
    sync_edit = 1;
    check(kernel_mm_mmap_anonymous(&sync_mm, OP_USER, 16384, KERNEL_MM_READ | KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK &&
        kernel_files_pipe2(&operation_files, &sync_mm, OP_USER, 0, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
        kernel_copy_from_user(&sync_mm, pipe_copy_pair, OP_USER, sizeof(pipe_copy_pair), &copied) == KERNEL_UACCESS_STATUS_OK &&
        kernel_copy_to_user(&sync_mm, OP_USER + 4096, "AAAAAAAAAAAAAAAAAAAAAAAAA", 25, &copied) == KERNEL_UACCESS_STATUS_OK &&
        kernel_copy_to_user(&sync_mm, OP_USER + 8192, "BBBBBBBBBBBBBBBBBBBBBBBBB", 25, &copied) == KERNEL_UACCESS_STATUS_OK, 294);
    sync_edit = 0;
    pipe_copy_testing = 1;
    pipe_copy_entered = 0;
    struct kernel_thread_join first = {0}, second = {0};
    check(kernel_thread_create_joinable(pipe_copy_writer, 0, &first) == KERNEL_SCHEDULER_STATUS_OK &&
        kernel_thread_create_joinable(pipe_copy_writer, (void *)1, &second) == KERNEL_SCHEDULER_STATUS_OK, 295);
    kernel_thread_join(&first);
    kernel_thread_join(&second);
    sync_edit = 1;
    check(pipe_copy_entered && kernel_files_read(&operation_files, &sync_mm, pipe_copy_pair[0],
        OP_USER + 12288, 50, &result) == KERNEL_FILES_STATUS_OK && result == 50, 296);
    unsigned char received[50];
    check(kernel_copy_from_user(&sync_mm, received, OP_USER + 12288, sizeof(received), &copied) == KERNEL_UACCESS_STATUS_OK, 297);
    unsigned a = 0, b = 0;
    for (unsigned i = 0; i < sizeof(received); i++) { a += received[i] == 'A'; b += received[i] == 'B'; }
    check(a == 25 && b == 25, 298);
    pipe_copy_testing = 0;
    sync_edit = 0;
    sync_satp = 0;
    check(kernel_files_release(&operation_files) == KERNEL_FILES_STATUS_OK &&
        kernel_mm_release(&sync_mm) == KERNEL_MM_STATUS_OK &&
        physical_page_release(&allocator, table.root_address) == PHYSICAL_PAGE_STATUS_OK, 299);
    virt_uart_puts("I/O pipe copy sleep passed: two writers, complete content and cleanup\n");
    riscv_interrupt_restore(irq);
}
static unsigned receive_paused, receive_done, receive_mode;
static int32_t receive_pair[2];
static struct kernel_task *receive_holder, *receive_waiter;
static struct kernel_wait_queue receive_held, receive_completed;
enum kernel_uaccess_status __real_kernel_copy_to_user(struct kernel_mm *, uint64_t, const void *, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_to_user(struct kernel_mm *mm, uint64_t address, const void *buffer, size_t size, size_t *copied)
{
    if(!receive_testing || mm!=&sync_mm)
        return __real_kernel_copy_to_user(mm,address,buffer,size,copied);
    if(scheduler.current==receive_holder && address==OP_USER+256 && !receive_paused) {
        receive_paused=1;
        enum kernel_wait_wake_reason reason;
        check(kernel_scheduler_block_current(&receive_held,0,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,270);
    }
    int previous=sync_edit; sync_edit=1;
    enum kernel_uaccess_status result=__real_kernel_copy_to_user(mm,address,buffer,size,copied);
    sync_edit=previous;
    return result;
}
static void receive_owner(void *argument)
{
    (void)argument; uintptr_t irq=riscv_interrupt_save(); receive_holder=scheduler.current;
    int64_t result;
    check(kernel_files_read(&operation_files,&sync_mm,receive_pair[1],OP_USER+256,3,&result)==KERNEL_FILES_STATUS_OK && result==(receive_mode==3 ? -KERNEL_EFAULT : 3),271);
    riscv_interrupt_restore(irq);
}
static void receive_follower(void *argument)
{
    (void)argument; uintptr_t irq=riscv_interrupt_save(); receive_waiter=scheduler.current;
    int64_t result;
    enum kernel_files_status status=kernel_files_read(&operation_files,&sync_mm,receive_pair[1],OP_USER+300,3,&result);
    int64_t expected=(receive_mode==0 || receive_mode==3) ? 0 : receive_mode==1 ? -KERNEL_EAGAIN : -KERNEL_ERESTARTSYS;
    if(status!=KERNEL_FILES_STATUS_OK || result!=expected) {
        virt_uart_puts("I/O receive mode/status/result=");virt_uart_put_hex(receive_mode);virt_uart_putc(' ');
        virt_uart_put_hex(status);virt_uart_putc(' ');virt_uart_put_hex((uint64_t)result);virt_uart_putc('\n');
    }
    check(status==KERNEL_FILES_STATUS_OK && result==expected,272);
    receive_done=1; check(kernel_wait_queue_wake_all(&receive_completed)==KERNEL_SCHEDULER_STATUS_OK,273);
    riscv_interrupt_restore(irq);
}
static void receive_reservation_probe(void *argument)
{
    (void)argument; uintptr_t irq=riscv_interrupt_save();
    for(receive_mode=0;receive_mode<4;receive_mode++) {
        receive_testing=operation_testing=1; receive_paused=receive_done=0;
        receive_holder=receive_waiter=0;
        kernel_wait_queue_init(&receive_held); kernel_wait_queue_init(&receive_completed);
        sync_mm=(struct kernel_mm){0}; operation_files=(struct kernel_files){0};
        struct riscv_sv39_page_table table={0}; struct riscv_sv39_user_space space={0};
        check(riscv_sv39_page_table_init(&table,&allocator)==RISCV_SV39_STATUS_OK,274); table.state=RISCV_SV39_STATE_ACTIVE;
        check(riscv_sv39_user_space_init(&space,&allocator,&table)==RISCV_SV39_STATUS_OK &&
            riscv_kernel_mm_create(&sync_mm,&space)==KERNEL_MM_STATUS_OK &&
            kernel_mm_vma_enable(&sync_mm,&heap)==KERNEL_MM_STATUS_OK &&
            kernel_mm_brk_initialize(&sync_mm,0x10000000,0x70000000)==KERNEL_MM_STATUS_OK &&
            riscv_kernel_mm_satp(&sync_mm,&sync_satp)==KERNEL_MM_STATUS_OK &&
            kernel_files_create(&operation_files,&heap)==KERNEL_FILES_STATUS_OK,275);
        uint64_t address; size_t copied; int64_t result; sync_edit=1;
        check(kernel_mm_mmap_anonymous(&sync_mm,OP_USER,4096,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address)==KERNEL_MM_STATUS_OK &&
            kernel_files_socketpair_create(&operation_files,&sync_mm,2,0,OP_USER,&result)==KERNEL_FILES_STATUS_OK && result==0 &&
            kernel_copy_from_user(&sync_mm,receive_pair,OP_USER,sizeof(receive_pair),&copied)==KERNEL_UACCESS_STATUS_OK &&
            kernel_copy_to_user(&sync_mm,OP_USER+512,"xyz",3,&copied)==KERNEL_UACCESS_STATUS_OK,276);
        sync_edit=0;
        check(kernel_files_write(&operation_files,&sync_mm,receive_pair[0],OP_USER+512,3,&result)==KERNEL_FILES_STATUS_OK && result==3,277);
        struct kernel_socket *socket=kernel_open_file_socket(kernel_files_fd_borrow(&operation_files,receive_pair[1]));
        kernel_socket_set_receive_timeout(socket,receive_mode==1 ? 100000000 : 0);
        struct kernel_thread_join holder={0},waiter={0};
        check(kernel_thread_create_joinable(receive_owner,0,&holder)==KERNEL_SCHEDULER_STATUS_OK,278);
        while(!receive_paused) check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,279);
        check(kernel_socket_shutdown(socket,2)==0 && (kernel_socket_poll(socket,0)&KERNEL_POLLHUP),280);
        virt_uart_puts("I/O socket holder reserved; HUP published\n");
        check(kernel_thread_create_joinable(receive_follower,0,&waiter)==KERNEL_SCHEDULER_STATUS_OK,281);
        while(!receive_done && (!receive_waiter || receive_waiter->state!=KERNEL_THREAD_STATE_BLOCKED))
            check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,282);
        check(!receive_done && receive_paused,283);
        if(receive_mode==2) check(kernel_scheduler_wake_signal(receive_waiter)==KERNEL_SCHEDULER_STATUS_OK,284);
        if(receive_mode==1 || receive_mode==2) while(!receive_done) {
            enum kernel_wait_wake_reason reason;
            check(kernel_scheduler_block_current(&receive_completed,0,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,285);
        }
        if(receive_mode==3) {
            sync_edit=1;
            check(kernel_mm_mprotect(&sync_mm,OP_USER,4096,KERNEL_MM_READ)==KERNEL_MM_STATUS_OK,292);
            sync_edit=0;
        }
        check(kernel_wait_queue_wake_all(&receive_held)==KERNEL_SCHEDULER_STATUS_OK,286);
        kernel_thread_join(&holder); kernel_thread_join(&waiter);
        sync_edit=1; char bytes[3];
        check(kernel_copy_from_user(&sync_mm,bytes,OP_USER+256,3,&copied)==KERNEL_UACCESS_STATUS_OK && (receive_mode==3 ? bytes[0]==0 && bytes[1]==0 && bytes[2]==0 : !memcmp(bytes,"xyz",3)),287);
        sync_edit=receive_testing=operation_testing=0; sync_satp=0;
        check(kernel_files_release(&operation_files)==KERNEL_FILES_STATUS_OK && kernel_mm_release(&sync_mm)==KERNEL_MM_STATUS_OK &&
            physical_page_release(&allocator,table.root_address)==PHYSICAL_PAGE_STATUS_OK,288);
    }
    virt_uart_puts("I/O socket reservation passed: owner, HUP, timeout, signal and fault\n");
    riscv_interrupt_restore(irq);
}
static void stream_copy_owner(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save(); send_copy_task = scheduler.current;
    struct kernel_open_file_description *pin = 0;
    int64_t result;
    check(kernel_files_pin(&operation_files, send_copy_fd, &pin, &result) == KERNEL_FILES_STATUS_OK && !result, 311);
    check(kernel_files_socket_io(&operation_files, &sync_mm, &pin, OP_USER, 3,
        KERNEL_SOCKET_MSG_NOSIGNAL, 1, &result) == KERNEL_FILES_STATUS_OK &&
        result == (send_copy_mode == 0 ? 3 : send_copy_mode == 1 ? -KERNEL_EPIPE : -KERNEL_EFAULT), 312);
    check(!scheduler.current->socket_write_request && kernel_open_file_release(&pin) == KERNEL_OPEN_FILE_STATUS_OK, 313);
    riscv_interrupt_restore(irq);
}
static void stream_copy_probe(void)
{
    uintptr_t irq = riscv_interrupt_save();
    for (send_copy_mode = 0; send_copy_mode < 3; send_copy_mode++) {
        send_copy_testing = operation_testing = 1; send_copy_paused = 0; send_copy_task = 0;
        kernel_wait_queue_init(&send_copy_held);
        sync_mm = (struct kernel_mm){0}; operation_files = (struct kernel_files){0};
        struct riscv_sv39_page_table table = {0}; struct riscv_sv39_user_space space = {0};
        check(riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 314); table.state = RISCV_SV39_STATE_ACTIVE;
        check(riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
            riscv_kernel_mm_create(&sync_mm, &space) == KERNEL_MM_STATUS_OK && kernel_mm_vma_enable(&sync_mm, &heap) == KERNEL_MM_STATUS_OK &&
            kernel_mm_brk_initialize(&sync_mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
            riscv_kernel_mm_satp(&sync_mm, &sync_satp) == KERNEL_MM_STATUS_OK && kernel_files_create(&operation_files, &heap) == KERNEL_FILES_STATUS_OK, 315);
        uint64_t address; size_t copied; int64_t result, listener_fd;
        sync_edit = 1;
        check(kernel_mm_mmap_anonymous(&sync_mm, OP_USER, 4096, KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK &&
            kernel_copy_to_user(&sync_mm, OP_USER, "pin", 3, &copied) == KERNEL_UACCESS_STATUS_OK, 316);
        sync_edit = 0;
        struct kernel_open_file_description *listener = 0, *client = 0, *reader = 0;
        struct kernel_socket *accepted = 0;
        check(kernel_files_socket_create(&operation_files, 2, 1, 0, &listener_fd) == KERNEL_FILES_STATUS_OK && listener_fd >= 0 &&
            kernel_files_socket_create(&operation_files, 2, 1, 0, &send_copy_fd) == KERNEL_FILES_STATUS_OK && send_copy_fd >= 0 &&
            kernel_files_pin(&operation_files, listener_fd, &listener, &result) == KERNEL_FILES_STATUS_OK && !result &&
            kernel_files_pin(&operation_files, send_copy_fd, &client, &result) == KERNEL_FILES_STATUS_OK && !result, 317);
        struct kernel_socket *socket = kernel_open_file_socket(client), *listening = kernel_open_file_socket(listener);
        struct kernel_socket_address local = {.family = KERNEL_SOCKET_AF_INET}, remote = {.family = KERNEL_SOCKET_AF_INET, .bytes = {127,0,0,1}};
        check(kernel_socket_bind(listening, &local) == 0 && kernel_socket_getname(listening, &local) == 0 &&
            kernel_socket_listen(listening, 2) == 0 && (remote.port = local.port, kernel_socket_connect(socket, &remote, 0)) == 0 &&
            kernel_socket_accept(listening, &accepted) == 0 &&
            kernel_open_file_create_socket(&heap, accepted, 0, &reader) == KERNEL_OPEN_FILE_STATUS_OK, 318);
        struct kernel_thread_join writer = {0};
        check(kernel_thread_create_joinable(stream_copy_owner, 0, &writer) == KERNEL_SCHEDULER_STATUS_OK, 319);
        while (!send_copy_paused) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 320);
        check(send_copy_task->socket_write_request && send_copy_task->socket_write_request->reserved == 3, 321);
        check(kernel_files_close(&operation_files, send_copy_fd, &result) == KERNEL_FILES_STATUS_OK && !result, 322);
        int64_t replacement;
        check(kernel_files_socket_create(&operation_files, 2, 2, 0, &replacement) == KERNEL_FILES_STATUS_OK && replacement == send_copy_fd, 323);
        if (send_copy_mode == 1) check(kernel_socket_shutdown(socket, 1) == 0, 324);
        if (send_copy_mode == 2) { sync_edit = 1; check(kernel_mm_munmap(&sync_mm, OP_USER, 4096) == KERNEL_MM_STATUS_OK, 325); sync_edit = 0; }
        check(kernel_wait_queue_wake_all(&send_copy_held) == KERNEL_SCHEDULER_STATUS_OK, 326);
        kernel_thread_join(&writer);
        if (!send_copy_mode) {
            struct kernel_socket_read_request request = {0}; char data[3];
            check(kernel_socket_reserve_read(accepted, kernel_task_current(), &request, &reader, 3, 1) == 3, 327);
            kernel_socket_copy_read(&request, 0, data, 3); check(!memcmp(data, "pin", 3), 328); kernel_socket_finish_read(&request, 0);
        } else if (send_copy_mode == 2) {
            struct kernel_socket_write_request request = {0}; kernel_socket_stream_begin(&request, &client);
            check(kernel_socket_stream_reserve(&request, 3, 0) == 3, 329); kernel_socket_stream_finish(&request);
        }
        check(kernel_open_file_release(&client) == KERNEL_OPEN_FILE_STATUS_OK && kernel_open_file_release(&reader) == KERNEL_OPEN_FILE_STATUS_OK &&
            kernel_open_file_release(&listener) == KERNEL_OPEN_FILE_STATUS_OK && kernel_files_release(&operation_files) == KERNEL_FILES_STATUS_OK, 330);
        send_copy_testing = operation_testing = 0; sync_satp = 0;
        check(kernel_mm_release(&sync_mm) == KERNEL_MM_STATUS_OK && physical_page_release(&allocator, table.root_address) == PHYSICAL_PAGE_STATUS_OK, 331);
    }
    virt_uart_puts("I/O TCP copy sleep passed: reservation, close/reuse, shutdown and fault rollback\n");
    riscv_interrupt_restore(irq);
}
static void socket_and_pipe_copy_probes(void *argument)
{
    receive_reservation_probe(argument);
    pipe_copy_probe(argument);
    stream_copy_probe();
}
static void print_counters(const char *phase, const struct riscv_virtio_mmio_block_statistics *stats)
{
    virt_uart_puts("I/O counters: "); virt_uart_puts(phase);
    virt_uart_puts(" submitted="); virt_uart_put_hex(stats->requests);
    virt_uart_puts(" max-inflight="); virt_uart_put_hex(stats->max_inflight);
    virt_uart_puts(" irq="); virt_uart_put_hex(stats->interrupts);
    virt_uart_puts(" sleeps="); virt_uart_put_hex(stats->sleeps);
    virt_uart_puts(" wakes="); virt_uart_put_hex(stats->wakes);
    virt_uart_puts(" queue-waits="); virt_uart_put_hex(stats->queue_waits);
    virt_uart_puts(" runtime-polls="); virt_uart_put_hex(stats->runtime_polls);
    virt_uart_puts(" inflight-ticks="); virt_uart_put_hex(stats->inflight_ticks);
    virt_uart_puts(" busy-ticks="); virt_uart_put_hex(stats->busy_ticks);
    virt_uart_puts(" total-ticks="); virt_uart_put_hex(stats->total_ticks);
    virt_uart_puts(" wait-ticks="); virt_uart_put_hex(stats->queue_wait_ticks);
    virt_uart_puts(" service-ticks="); virt_uart_put_hex(stats->service_ticks);
    virt_uart_putc('\n');
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static int dma(const void *p, uint64_t n, uint64_t *out) { (void)n; *out = (uintptr_t)p; return 1; }
int __real_kernel_vfs_node_pread(struct kernel_vfs_node *, uint64_t, void *, size_t, size_t *);
int __wrap_kernel_vfs_node_pread(struct kernel_vfs_node *node, uint64_t offset, void *buffer, size_t size, size_t *count)
{
    if (loading_probe) {
        loads++;
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 10);
        riscv_interrupt_restore(irq);
    }
    return __real_kernel_vfs_node_pread(node, offset, buffer, size, count);
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *, uint64_t, const void *, size_t, size_t *);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node, uint64_t offset, const void *buffer, size_t size, size_t *written)
{
    if (instance_hold && kernel_io_context_current()->background_reclaim) {
        for (unsigned i = 0; i < 2; i++) if (node == instance_nodes[i] && !instance_entered[i]) {
            instance_entered[i] = 1;
            enum kernel_wait_wake_reason reason;
            check(kernel_scheduler_block_current(&instance_held[i], 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 180);
        }
    }
    if (stop_probe && kernel_io_context_current()->background_reclaim) {
        stop_probe = 0;
        stop_entered = 1;
        enum kernel_wait_wake_reason reason;
        check(kernel_scheduler_block_current(&held_write, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 116);
    }
    if (background_fail && kernel_io_context_current()->background_reclaim) {
        background_fail = 0;
        failed_offset = offset;
        *written = 0;
        return -5;
    }
    if (write_probe) {
        unsigned char first = *(const unsigned char *)buffer;
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 20);
        check(*(const unsigned char *)buffer == first, 21);
        riscv_interrupt_restore(irq);
    }
    return __real_kernel_vfs_node_writeback(node, offset, buffer, size, written);
}
static void write_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!argument) {
        uint64_t sequence = 0;
        check(kernel_vfs_sync(&file, 0, &sequence) == 0, 22);
    } else {
        size_t valid;
        check(kernel_page_cache_get(&cache, &file, 1, &inserted_page, &valid) == KERNEL_PAGE_CACHE_STATUS_OK && valid == 4096, 28);
        check(physical_page_release(&allocator, inserted_page) == PHYSICAL_PAGE_STATUS_OK, 29);
        payload[0]++;
        kernel_page_cache_alias_mark_dirty(&alias);
        ((unsigned char *)access_page(observed[0]))[0] = payload[0];
    }
    riscv_interrupt_restore(irq);
}
static struct kernel_vfs_file orphan;
static void orphan_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!argument)
        check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(&orphan)) == 0, 71);
    else
        check(kernel_vfs_close(&orphan) == 0, 72);
    riscv_interrupt_restore(irq);
}
static void reader(void *arg)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned index = (uintptr_t)arg;
    size_t valid;
    check(kernel_page_cache_get(&cache, &file, 0, &observed[index], &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 11);
    check(valid == sizeof(payload) && !memcmp(access_page(observed[index]), payload, valid), 12);
    check(physical_page_release(&allocator, observed[index]) == PHYSICAL_PAGE_STATUS_OK, 13);
    done++;
    riscv_interrupt_restore(irq);
}
static struct kernel_vfs_file cold[2];
static unsigned cold_done, progress_done;
static struct kernel_task *cold_tasks[2];
static void cold_reader(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned index = (uintptr_t)argument;
    cold_tasks[index] = kernel_task_current();
    uint64_t address;
    size_t valid;
    check(kernel_page_cache_get(&cache, &cold[index], 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 30);
    unsigned char *data = access_page(address);
    check(valid == 4096, 31);
    for (unsigned i = 0; i < 4096; i++) check(data[i] == (unsigned char)(i * 19 + index), 32);
    check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK, 33);
    cold_done++;
    riscv_interrupt_restore(irq);
}
static void progress_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    uint64_t address;
    size_t valid;
    check(!cold_done && kernel_page_cache_get(&cache, &file, 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 34);
    check(valid == sizeof(payload) && !memcmp(access_page(address), payload, valid), 35);
    check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK, 36);
    unsigned char cached[64];
    size_t copied;
    check(kernel_vfs_pread(&file, 17, cached, sizeof(cached), &copied) == 0 &&
          copied == sizeof(cached) && !memcmp(cached, payload + 17, copied) && !cold_done, 78);
    /* Pending termination must not detach an internal DMA wait. The normal
     * return-to-user path handles the pending request after stack unwind. */
    for (unsigned i = 0; i < 2; i++) {
        check(cold_tasks[i] && cold_tasks[i]->state == KERNEL_THREAD_STATE_BLOCKED, 76);
        cold_tasks[i]->terminate_requested = 1;
        check(kernel_scheduler_wake_signal(cold_tasks[i]) == KERNEL_SCHEDULER_STATUS_OK &&
              cold_tasks[i]->state == KERNEL_THREAD_STATE_BLOCKED, 77);
    }
    volatile unsigned sum = 0;
    for (unsigned i = 0; i < 10000; i++) sum += i;
    check(sum == 49995000 && !cold_done, 37);
    progress_done = 1;
    virt_uart_puts("I/O handshake: progress\n");
    riscv_interrupt_restore(irq);
}
static unsigned queue_writers, flush_done, queue_done;
static void queue_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    uintptr_t index = (uintptr_t)argument;
    unsigned char sector[512] __attribute__((aligned(16)));
    if (index == 8) {
        check(kernel_block_flush(&device.block) == KERNEL_BLOCK_STATUS_OK && queue_writers == 8, 50);
        flush_done = 1;
    } else {
        memset(sector, (int)index + 1, sizeof(sector));
        enum kernel_block_status result = index < 8
            ? kernel_block_write_at(&device.block, 120 * 1024 * 1024 + index * 4096, sector, sizeof(sector))
            : kernel_block_read_at(&device.block, 120 * 1024 * 1024, sector, sizeof(sector));
        check(result == KERNEL_BLOCK_STATUS_OK, 51);
        if (index < 8) queue_writers++;
        else {
            check(flush_done, 52);
            for (unsigned i = 0; i < sizeof(sector); i++) check(sector[i] == 1, 79);
        }
    }
    queue_done++;
    riscv_interrupt_restore(irq);
}
static unsigned timed_out;
static uint64_t timebase;
static unsigned batch_mode, batch_fillers, batch_done, batch_finished, batch_flushed;
static struct kernel_task *batch_task;
static uint64_t batch_errors;
static void batch_filler(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sector[512];
    memset(sector, (int)(uintptr_t)argument + 1, sizeof(sector));
    check(kernel_block_write_at(&device.block, 126 * 1024 * 1024 + (uintptr_t)argument * 4096,
                               sector, sizeof(sector)) == KERNEL_BLOCK_STATUS_OK, 270);
    batch_done++;
    riscv_interrupt_restore(irq);
}
static void batch_writer(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sectors[8][512];
    struct kernel_block_span spans[8];
    batch_task = kernel_task_current();
    for (unsigned i = 0; i < 8; i++) {
        memset(sectors[i], (int)i + 32, sizeof(sectors[i]));
        spans[i] = (struct kernel_block_span){(uint64_t)(122 + batch_mode) * 1024 * 1024 + i * 4096,
                                             sectors[i], sizeof(sectors[i])};
    }
    check(kernel_block_write_batch(&device.block, spans, 8) ==
          (batch_mode == 2 ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK), 271);
    batch_finished = 1; batch_done++;
    riscv_interrupt_restore(irq);
}
static void batch_barrier(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    check(kernel_block_flush(&device.block) == KERNEL_BLOCK_STATUS_OK && batch_finished, 272);
    batch_flushed = 1; batch_done++;
    riscv_interrupt_restore(irq);
}
static void batch_readback(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sector[512];
    for (unsigned i = 0; i < 8; i++) {
        check(kernel_block_read_at(&device.block,
            (uint64_t)(122 + batch_mode) * 1024 * 1024 + i * 4096, sector, sizeof(sector)) ==
            KERNEL_BLOCK_STATUS_OK && batch_flushed, 273);
        unsigned expected = batch_mode == 2 && i ? 0 : i + 32;
        for (unsigned j = 0; j < sizeof(sector); j++) check(sector[j] == expected, 274);
    }
    batch_done++;
    riscv_interrupt_restore(irq);
}
static void batch_cancel_probe(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    while (!batch_task || (batch_mode == 2 && (device.statistics.io_errors == batch_errors ||
                                              batch_task->state == KERNEL_THREAD_STATE_READY))) {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 275);
        riscv_interrupt_restore(RISCV_SSTATUS_SIE);
        (void)riscv_interrupt_save();
    }
    check(!batch_finished && batch_task->state == KERNEL_THREAD_STATE_BLOCKED, 276);
    batch_task->terminate_requested = 1;
    check(kernel_scheduler_wake_signal(batch_task) == KERNEL_SCHEDULER_STATUS_OK &&
          batch_task->state == KERNEL_THREAD_STATE_BLOCKED, 277);
    virt_uart_puts("I/O handshake: batch-pending\n");
    batch_done++;
    riscv_interrupt_restore(irq);
}
static void test_batch(unsigned mode)
{
    batch_mode = mode; batch_fillers = mode == 0 ? 0 : mode == 1 ? 2 : 6;
    batch_done = batch_finished = batch_flushed = 0; batch_task = 0;
    batch_errors = device.statistics.io_errors;
    uint64_t before = device.statistics.requests;
    virt_uart_puts(mode == 0 ? "I/O handshake: batch\n" :
                   mode == 1 ? "I/O handshake: batch-partial\n" : "I/O handshake: batch-error\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 278);
    for (uintptr_t i = 0; i < batch_fillers; i++)
        check(kernel_thread_create(batch_filler, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 279);
    check(kernel_thread_create(batch_writer, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(batch_barrier, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(batch_readback, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(batch_cancel_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 280);
    unsigned reaped = 0;
    while (reaped < batch_fillers + 4) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 281);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    unsigned requests = mode == 2 ? 16 : batch_fillers + 16;
    requests += device.block.cache_mode == KERNEL_BLOCK_CACHE_WRITEBACK;
    check(batch_done == batch_fillers + 4 && device.statistics.requests - before == requests &&
          !device.active && !device.inflight && !device.barrier, 282);
    struct riscv_virtio_mmio_block_statistics stats;
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    print_counters(mode == 0 ? "batch" : mode == 1 ? "batch-partial" : "batch-error", &stats);
}
static struct kernel_task *read_batch_task;
static unsigned read_batch_finished;
static void batch_reader(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sectors[8][512];
    struct kernel_block_read_span spans[8];
    read_batch_task = kernel_task_current();
    for (unsigned i = 0; i < 8; i++) spans[i] = (struct kernel_block_read_span){
        .offset = 120 * 1024 * 1024 + i * 4096, .buffer = sectors[i], .size = 512};
    check(kernel_block_read_batch(&device.block, spans, 8) == KERNEL_BLOCK_STATUS_OK, 340);
    for (unsigned i = 0; i < 8; i++) {
        check(spans[i].status == KERNEL_BLOCK_STATUS_OK && spans[i].completed == 512, 341);
        for (unsigned j = 0; j < 512; j++) check(sectors[i][j] == i + 1, 342);
    }
    read_batch_finished = 1;
    riscv_interrupt_restore(irq);
}
static void batch_reader_owner_probe(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    while (!read_batch_task || read_batch_task->state != KERNEL_THREAD_STATE_BLOCKED)
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 343);
    read_batch_task->terminate_requested = 1;
    check(kernel_scheduler_wake_signal(read_batch_task) == KERNEL_SCHEDULER_STATUS_OK &&
        read_batch_task->state == KERNEL_THREAD_STATE_BLOCKED && !read_batch_finished, 344);
    virt_uart_puts("I/O handshake: read-batch-pending\n");
    while (device.inflight != 1 || read_batch_task->state != KERNEL_THREAD_STATE_BLOCKED) {
        check(!read_batch_finished && kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 345);
        riscv_interrupt_restore(RISCV_SSTATUS_SIE); (void)riscv_interrupt_save();
    }
    check(!read_batch_finished, 346);
    virt_uart_puts("I/O handshake: read-batch-held\n");
    riscv_interrupt_restore(irq);
}
static void test_read_batch(void)
{
    read_batch_task = 0; read_batch_finished = 0;
    uint64_t before = device.statistics.requests;
    virt_uart_puts("I/O handshake: read-batch\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 347);
    check(kernel_thread_create(batch_reader, 0) == KERNEL_SCHEDULER_STATUS_OK &&
        kernel_thread_create(batch_reader_owner_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 348);
    unsigned reaped = 0;
    while (reaped < 2) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 349);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    check(read_batch_finished && device.statistics.requests - before == 8 && !device.inflight && !device.active, 350);
    virt_uart_puts("I/O batch read passed: eight in flight, reversed prefix, held DMA and cancellation owner\n");
}

static void timeout_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sector[512] __attribute__((aligned(16)));
    check(kernel_block_read_at(&device.block, 121 * 1024 * 1024 + (uintptr_t)argument * 4096,
                              sector, sizeof(sector)) == KERNEL_BLOCK_STATUS_TIMEOUT, 60);
    check(kernel_block_read_at(&device.block, 0, sector, sizeof(sector)) == KERNEL_BLOCK_STATUS_IO, 61);
    timed_out++;
    riscv_interrupt_restore(irq);
}
static void timeout_batch_worker(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sectors[8][512];
    struct kernel_block_span spans[8];
    for (unsigned i = 0; i < 8; i++) {
        memset(sectors[i], (int)i + 1, sizeof(sectors[i]));
        spans[i] = (struct kernel_block_span){121 * 1024 * 1024 + 65536 + i * 4096,
                                            sectors[i], sizeof(sectors[i])};
    }
    check(kernel_block_write_batch(&device.block, spans, 8) == KERNEL_BLOCK_STATUS_TIMEOUT, 283);
    check(kernel_block_write_batch(&device.block, spans, 8) == KERNEL_BLOCK_STATUS_IO, 284);
    timed_out++;
    riscv_interrupt_restore(irq);
}
static void stop_writeback_worker(void *unused)
{
    (void)unused;
    (void)riscv_interrupt_save();
    while (!stop_entered) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 117);
    stop_called = 1;
    kernel_page_cache_stop_worker(&cache);
    stop_done = 1;
}
static void instance_yield(void)
{
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 181);
    riscv_interrupt_restore(RISCV_SSTATUS_SIE);
    (void)riscv_interrupt_save();
}
static void instance_write(struct kernel_page_cache *c, struct kernel_vfs_file *f,
                           unsigned pages)
{
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(kernel_vfs_file_node(f), &guard, 1);
    for (unsigned i = 0; i < pages; i++) {
        size_t written;
        check(kernel_page_cache_write(c, f, (uint64_t)i * 4096, payload, sizeof(payload),
                                      &written) == 0 && written == sizeof(payload), 182);
    }
}
static void instance_waiter(void *unused)
{
    (void)unused;
    (void)riscv_interrupt_save();
    unsigned held = 0;
    /* 触发 low 压力，同时给真实 ext4 事务留下元数据分配空间。 */
    uint64_t low = physical_page_total(&allocator) / 50;
    while (physical_page_available(&allocator) > low) {
        check(held < 1024 && physical_page_allocate(&allocator, &pressure_pages[held]) == PHYSICAL_PAGE_STATUS_OK, 183);
        held++;
    }
    instance_waiting = 1;
    allocator.pressure_wait(allocator.pressure_context);
    instance_wait_done = 1;
    while (held) check(physical_page_release(&allocator, pressure_pages[--held]) == PHYSICAL_PAGE_STATUS_OK, 184);
}
static void instance_stopper(void *unused)
{
    (void)unused;
    (void)riscv_interrupt_save();
    while (!instance_entered[1]) instance_yield();
    instance_stop_called = 1;
    kernel_page_cache_stop_worker(&instance_peer);
    if (instance_peer_pin) {
        check(physical_page_release(&allocator, instance_peer_pin) == PHYSICAL_PAGE_STATUS_OK, 206);
        instance_peer_pin = 0;
    }
    check(kernel_page_cache_destroy(&instance_peer) == KERNEL_PAGE_CACHE_STATUS_OK, 185);
    instance_stop_done = 1;
}
static void cache_instances_probe(void)
{
    struct kernel_vfs_file files[2] = {{0}, {0}};
    struct kernel_page_cache_statistics a, b, a_after, b_after;
    struct kernel_memory_statistics memory;
    (void)kernel_page_cache_reclaim(&cache, UINT64_MAX);
    kernel_page_cache_get_statistics(&cache, &a);
    check(!a.current_pages, 186);
    check(kernel_page_cache_init(&instance_peer, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK &&
          kernel_vfs_create(&mount, "/instance-root", 0600, &files[0]) == 0 &&
          kernel_vfs_create(&mount, "/instance-peer", 0600, &files[1]) == 0, 187);
    /* 两个不同 inode 各由一个 cache 拥有，后端仍是真实 ext4。 */
    instance_write(&cache, &files[0], 2);
    instance_write(&instance_peer, &files[1], 2);
    kernel_memory_snapshot(&allocator, &memory);
    uint64_t low = physical_page_total(&allocator) / 50 * 4096;
    check(memory.cached == 4 * 4096 && memory.dirty == 4 * 4096 &&
          !memory.writeback && memory.reclaimable == 4 * 4096 &&
          memory.available == memory.free - low + memory.reclaimable / 2, 188);
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(&files[0])) == 0 &&
          kernel_page_cache_writeback(&instance_peer, kernel_vfs_file_node(&files[1])) == 0, 189);
    kernel_memory_snapshot(&allocator, &memory);
    check(memory.cached == 4 * 4096 && !memory.dirty, 190);
    check(allocator.reclaimer(allocator.reclaimer_context, 1) == 1 &&
          allocator.reclaimer(allocator.reclaimer_context, 1) == 1, 191);
    kernel_page_cache_get_statistics(&cache, &a);
    kernel_page_cache_get_statistics(&instance_peer, &b);
    check(a.current_pages == 1 && b.current_pages == 1, 192);
    /* 每个实例单独低于 10%，合计超过 10% 才能请求两边的 worker。 */
    unsigned pages = (unsigned)(physical_page_total(&allocator) / 20 + 8);
    check(pages < physical_page_total(&allocator) / 10, 193);
    instance_write(&cache, &files[0], pages);
    instance_write(&instance_peer, &files[1], pages);
    kernel_memory_snapshot(&allocator, &memory);
    check(memory.dirty == (uint64_t)pages * 2 * 4096, 194);
    check(kernel_page_cache_start_worker(&cache) == 0 && kernel_page_cache_start_worker(&instance_peer) == 0, 195);
    uint64_t deadline = riscv_time_read() + 10 * timebase;
    do { instance_yield(); kernel_memory_snapshot(&allocator, &memory); }
    while (memory.dirty > physical_page_total(&allocator) / 20 * 4096 && riscv_time_read() < deadline);
    kernel_page_cache_get_statistics(&cache, &a_after);
    kernel_page_cache_get_statistics(&instance_peer, &b_after);
    check(memory.dirty <= physical_page_total(&allocator) / 20 * 4096 &&
          a_after.worker_written > a.worker_written && b_after.worker_written > b.worker_written, 196);
    kernel_page_cache_stop_worker(&cache);
    kernel_page_cache_stop_worker(&instance_peer);
    (void)kernel_page_cache_reclaim(&cache, UINT64_MAX);
    (void)kernel_page_cache_reclaim(&instance_peer, UINT64_MAX);
    for (unsigned pass = 0; pass < 2; pass++) {
        if (pass) {
            instance_peer = (struct kernel_page_cache){0};
            check(kernel_page_cache_init(&instance_peer, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 207);
        }
        instance_entered[0] = instance_entered[1] = instance_waiting = instance_wait_done = 0;
        instance_stop_called = instance_stop_done = 0;
        instance_write(&cache, &files[0], 1);
        instance_write(&instance_peer, &files[1], 1);
        if (!pass) {
            size_t valid;
            check(kernel_page_cache_get(&instance_peer, &files[1], 0, &instance_peer_pin, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 208);
        }
        instance_nodes[0] = kernel_vfs_file_node(&files[0]);
        instance_nodes[1] = kernel_vfs_file_node(&files[1]);
        for (unsigned i = 0; i < 2; i++) kernel_wait_queue_init(&instance_held[i]);
        instance_hold = 1;
        check(kernel_page_cache_start_worker(&instance_peer) == 0 && kernel_page_cache_start_worker(&cache) == 0, 197);
        struct kernel_thread_join waiter = {0}, stopper = {0};
        check(kernel_thread_create_joinable(instance_stopper, 0, &stopper) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_thread_create_joinable(instance_waiter, 0, &waiter) == KERNEL_SCHEDULER_STATUS_OK, 198);
        deadline = riscv_time_read() + 10 * timebase;
        while ((!instance_entered[0] || !instance_entered[1] || !instance_stop_called) && riscv_time_read() < deadline)
            instance_yield();
        check(instance_entered[0] && instance_entered[1] && instance_waiting &&
              instance_stop_called && !instance_stop_done && !instance_wait_done, 199);
        kernel_memory_snapshot(&allocator, &memory);
        check(memory.cached == 2 * 4096 && memory.writeback == 2 * 4096 && memory.dirty == 2 * 4096, 200);
        if (!pass) {
            /* peer 页被外部 pin，worker 完成没有释放进展；销毁 peer 时 waiter 仍等 root。 */
            check(kernel_wait_queue_wake_all(&instance_held[1]) == KERNEL_SCHEDULER_STATUS_OK, 209);
            deadline = riscv_time_read() + 10 * timebase;
            while (!instance_stop_done && riscv_time_read() < deadline) instance_yield();
            check(instance_stop_done && !instance_wait_done, 210);
            check(kernel_wait_queue_wake_all(&instance_held[0]) == KERNEL_SCHEDULER_STATUS_OK, 211);
        } else {
            /* peer 正在 stop/join 且 I/O 未完成；root 的首次释放必须唤醒共同等待者。 */
            check(kernel_wait_queue_wake_all(&instance_held[0]) == KERNEL_SCHEDULER_STATUS_OK, 201);
            deadline = riscv_time_read() + 10 * timebase;
            while (!instance_wait_done && riscv_time_read() < deadline) instance_yield();
            check(instance_wait_done && !instance_stop_done, 202);
            check(kernel_wait_queue_wake_all(&instance_held[1]) == KERNEL_SCHEDULER_STATUS_OK, 203);
        }
        kernel_thread_join(&waiter); kernel_thread_join(&stopper);
        instance_hold = 0;
        check(instance_stop_done && allocator.pressure_wait && allocator.pressure_notify, 204);
        /* peer 注销后，真实 root worker 仍能收到压力请求并完成。 */
        allocator.pressure_wait(allocator.pressure_context);
        kernel_page_cache_stop_worker(&cache);
        (void)kernel_page_cache_reclaim(&cache, UINT64_MAX);
    }
    check(kernel_vfs_close(&files[0]) == 0 && kernel_vfs_close(&files[1]) == 0, 205);
}
static void background_writeback_probe(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_vfs_file target = {0};
    check(kernel_vfs_create(&mount, "/background", 0600, &target) == 0, 90);
    /* 分别耗尽快照页和线程构造资源；启动失败必须完整回滚。 */
    unsigned startup_held = 0;
    while (startup_held < 1024 && physical_page_allocate(&allocator,
                &pressure_pages[startup_held]) == PHYSICAL_PAGE_STATUS_OK) startup_held++;
    check(startup_held && startup_held < 1024 && !physical_page_available(&allocator), 111);
    check(kernel_page_cache_start_worker(&cache) == -12 && !physical_page_available(&allocator), 112);
    check(physical_page_release(&allocator, pressure_pages[--startup_held]) == PHYSICAL_PAGE_STATUS_OK, 113);
    check(kernel_page_cache_start_worker(&cache) == -12 && physical_page_available(&allocator) == 1, 114);
    while (startup_held) check(physical_page_release(&allocator,
                pressure_pages[--startup_held]) == PHYSICAL_PAGE_STATUS_OK, 115);
    check(kernel_page_cache_start_worker(&cache) == 0, 91);
    struct kernel_page_cache peer = {0};
    check(kernel_page_cache_init(&peer, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK &&
          kernel_page_cache_start_worker(&peer) == 0, 163);
    allocator.pressure_wait(allocator.pressure_context);
    kernel_page_cache_stop_worker(&peer);
    check(allocator.pressure_wait && allocator.pressure_notify, 164);
    check(kernel_page_cache_destroy(&peer) == KERNEL_PAGE_CACHE_STATUS_OK, 165);
    check(allocator.pressure_wait && allocator.pressure_notify, 166);

    /* 没有候选的一轮必须返回并休眠，不能自行反复扫描。 */
    allocator.pressure_wait(allocator.pressure_context);
    struct kernel_page_cache_statistics empty_before, empty_after;
    kernel_page_cache_get_statistics(&cache, &empty_before);
    for (unsigned i = 0; i < 8; i++)
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 127);
    kernel_page_cache_get_statistics(&cache, &empty_after);
    check(empty_before.worker_scanned == empty_after.worker_scanned &&
          empty_before.worker_written == empty_after.worker_written, 128);
    background_fail = 1;
    unsigned pages = (unsigned)(physical_page_total(&allocator) / 10 + 16);
    for (unsigned i = 0; i < pages; i++) {
        size_t written;
        memset(payload, (int)(i % 251 + 1), sizeof(payload));
        check(kernel_vfs_pwrite(&target, (uint64_t)i * sizeof(payload), payload,
                               sizeof(payload), &written) == 0 && written == sizeof(payload), 92);
    }
    struct kernel_memory_statistics memory;
    uint64_t deadline = riscv_time_read() + 10 * timebase;
    do {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 93);
        kernel_memory_snapshot(&allocator, &memory);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
        (void)riscv_interrupt_save();
    } while (memory.dirty > physical_page_total(&allocator) / 20 * 4096 && riscv_time_read() < deadline);
    check(memory.dirty <= physical_page_total(&allocator) / 20 * 4096, 94);
    struct kernel_page_cache_statistics worker_stats = {0};
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    check(!background_fail && worker_stats.worker_failed == 1 && worker_stats.worker_written > 0, 100);
    check(worker_stats.worker_batches > 1 &&
          worker_stats.worker_scanned <= worker_stats.worker_batches * 64, 125);
    struct riscv_virtio_mmio_block_statistics query_before, query_after;
    riscv_virtio_mmio_block_get_statistics(&device, &query_before);
    kernel_memory_snapshot(&allocator, &memory);
    riscv_virtio_mmio_block_get_statistics(&device, &query_after);
    check(query_before.requests == query_after.requests && memory.available <= memory.total &&
          memory.dirty <= memory.cached && memory.writeback <= memory.cached, 126);
    /* 改写失败页只允许重试，不能提前把它计入可回收预算。 */
    kernel_memory_snapshot(&allocator, &memory);
    uint64_t reclaimable_before = memory.reclaimable;
    size_t changed;
    check(kernel_vfs_pwrite(&target, failed_offset, "R", 1, &changed) == 0 && changed == 1, 109);
    kernel_memory_snapshot(&allocator, &memory);
    check(memory.reclaimable == reclaimable_before, 110);
    uint64_t seq = 0;
    check(kernel_vfs_sync(&target, 0, &seq) == -5, 101);
    check(kernel_vfs_sync(&target, 0, &seq) == 0, 95);
    unsigned held = 0;
    uint64_t pinned;
    size_t valid;
    check(kernel_page_cache_get(&cache, &target, 0, &pinned, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 102);
    while (physical_page_available(&allocator) > physical_page_total(&allocator) / 50) {
        check(held < 1024 && physical_page_allocate(&allocator, &pressure_pages[held]) == PHYSICAL_PAGE_STATUS_OK, 103);
        held++;
    }
    deadline = riscv_time_read() + 10 * timebase;
    while (physical_page_available(&allocator) < physical_page_total(&allocator) / 25 && riscv_time_read() < deadline) {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 104);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
        (void)riscv_interrupt_save();
    }
    check(physical_page_available(&allocator) >= physical_page_total(&allocator) / 25, 105);
    uint32_t references;
    check(physical_page_reference_count(&allocator, pinned, &references) == PHYSICAL_PAGE_STATUS_OK && references == 2, 106);
    check(physical_page_release(&allocator, pinned) == PHYSICAL_PAGE_STATUS_OK, 107);
    while (held) check(physical_page_release(&allocator, pressure_pages[--held]) == PHYSICAL_PAGE_STATUS_OK, 108);
    kernel_page_cache_stop_worker(&cache);
    /* 暂扣在途写回，stop 必须等待它完成且不能继续提交本批其他页。 */
    for (unsigned i = 0; i < pages; i++) {
        size_t n;
        memset(payload, (int)(i % 251 + 1), sizeof(payload));
        check(kernel_vfs_pwrite(&target, (uint64_t)i * sizeof(payload), payload,
                               sizeof(payload), &n) == 0 && n == sizeof(payload), 118);
    }
    kernel_wait_queue_init(&held_write);
    stop_probe = 1;
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    uint64_t writes_before_stop = worker_stats.worker_written;
    check(kernel_page_cache_start_worker(&cache) == 0, 119);
    struct kernel_thread_join stopper = {0};
    check(kernel_thread_create_joinable(stop_writeback_worker, 0, &stopper) == KERNEL_SCHEDULER_STATUS_OK, 120);
    while (!stop_called) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 121);
    check(stop_entered && !stop_done, 122);
    check(kernel_wait_queue_wake_all(&held_write) == KERNEL_SCHEDULER_STATUS_OK, 123);
    kernel_thread_join(&stopper);
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    check(stop_done && worker_stats.worker_written == writes_before_stop + 1, 124);
    (void)kernel_page_cache_reclaim(&cache, UINT64_MAX);
    size_t count;
    check(kernel_vfs_pread(&target, (uint64_t)(pages - 1) * 4096, payload, sizeof(payload), &count) == 0 &&
          count == sizeof(payload) && payload[0] == (pages - 1) % 251 + 1 && payload[4095] == payload[0], 96);
    check(kernel_vfs_close(&target) == 0, 97);
    cache_instances_probe();
    riscv_interrupt_restore(irq);
}
static void cleanup_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    for (unsigned i = 0; i < 2; i++) check(kernel_vfs_close(&cold[i]) == 0, 38);
    check(kernel_vfs_close(&file) == 0, 150);
    int unmounted = kernel_vfs_unmount(&mount);
    if (unmounted) { virt_uart_puts("unmount result="); virt_uart_put_hex((unsigned long)unmounted); virt_uart_putc('\n'); }
    check(unmounted == 0, 151);
    check(kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK, 152);
    check(riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 153);
    riscv_interrupt_restore(irq);
}
#if BOAROS_COST_DIAGNOSTICS
static void cost_finish(const char *name, unsigned epoch)
{
    check(kernel_cost_end(1, 0) == 0, 212);
    char *report;
    check(kernel_heap_allocate(&heap, kernel_cost_format_capacity(), (void **)&report) == KERNEL_HEAP_STATUS_OK, 213);
    check(kernel_cost_format(report, kernel_cost_format_capacity()) > 0, 214);
    virt_uart_puts("COST SNAPSHOT "); virt_uart_puts(name); virt_uart_putc(' ');
    virt_uart_putc((char)('0' + epoch)); virt_uart_putc('\n');
    virt_uart_puts(report); virt_uart_puts("COST END\n");
    check(kernel_heap_release(&heap, report) == KERNEL_HEAP_STATUS_OK, 215);
}
#endif
void kernel_main(unsigned long hart, const void *dtb)
{
    (void)hart;
    struct dtb_boot_info info;
    struct boot_memory_layout layout = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK, 1);
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    check(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK &&
          kernel_heap_init(&heap, &allocator, physical) == KERNEL_HEAP_STATUS_OK, 2);
    /* lwIP静态表含链接的高地址；启用双别名后再初始化调度器，其页计入fixture固定基线。 */
    check(riscv_sv39_page_table_init(&protocol_table, &allocator) == RISCV_SV39_STATUS_OK &&
        riscv_sv39_map_range(&protocol_table, 0x80000000, 0x80000000, 512 * 1024 * 1024,
            RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
        riscv_sv39_map_range(&protocol_table, UINT64_C(0xffffffff80000000), 0x80200000, 128 * 1024 * 1024,
            RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
        riscv_sv39_map_range(&protocol_table, 0x10000000, 0x10000000, 0x200000,
            RISCV_SV39_READ | RISCV_SV39_WRITE) == RISCV_SV39_STATUS_OK &&
        riscv_sv39_map_range(&protocol_table, 0xc000000, 0xc000000, 0x400000,
            RISCV_SV39_READ | RISCV_SV39_WRITE) == RISCV_SV39_STATUS_OK &&
        riscv_sv39_activate(&protocol_table) == RISCV_SV39_STATUS_OK, 333);
    uint64_t baseline = physical_page_available(&allocator);
    timebase = info.timebase_frequency;
    check(kernel_page_cache_init(&cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 3);
    /* 同 allocator 的第二缓存必须可独立注册、注销；不改变首缓存统计。 */
    struct kernel_page_cache second_cache = {0};
    check(kernel_page_cache_init(&second_cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 160);
    struct kernel_memory_statistics two_caches;
    kernel_memory_snapshot(&allocator, &two_caches);
    uint64_t low_bytes = (physical_page_total(&allocator) / 50) * BOAROS_PAGE_SIZE;
    check(two_caches.available == (two_caches.free > low_bytes ? two_caches.free - low_bytes : 0), 161);
    check(kernel_page_cache_destroy(&second_cache) == KERNEL_PAGE_CACHE_STATUS_OK, 162);

    int found = 0;
    for (unsigned i = 0; i < info.virtio_mmio_count; i++)
        if (riscv_virtio_mmio_block_init(&device, (void *)(uintptr_t)info.virtio_mmio[i].base,
            info.virtio_mmio[i].size, &allocator, dma, info.timebase_frequency) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) { found = 1; break; }
    check(found && kernel_vfs_mount_root(&mount, &device.block, &heap, &cache) == 0 &&
          kernel_vfs_create(&mount, "/concurrent", 0600, &file) == 0, 4);
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = i * 31;
    size_t written;
    uint64_t sequence = 0;
    check(kernel_vfs_pwrite(&file, 0, payload, sizeof(payload), &written) == 0 && written == sizeof(payload) &&
          kernel_vfs_sync(&file, 0, &sequence) == 0, 5);
    check(kernel_vfs_pwrite(&file, 4096, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_sync(&file, 0, &sequence) == 0, 5);
    check(kernel_page_cache_reclaim(&cache, 64) != 0, 6);
    check(kernel_scheduler_init(&allocator, (uintptr_t)__boot_stack_bottom, (uintptr_t)__boot_stack_top) == KERNEL_SCHEDULER_STATUS_OK, 7);
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(1, info.timebase_frequency, 1, 0) == 0, 210);
#endif
    loading_probe = 1;
    for (uintptr_t i = 0; i < 2; i++) check(kernel_thread_create(reader, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 8);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 9);
    }
    loading_probe = 0;
    check(done == 2 && loads == 1 && observed[0] == observed[1], 14);
    payload[0]++;
    check(kernel_vfs_pwrite(&file, 0, payload, sizeof(payload), &written) == 0, 24);
    check(kernel_page_cache_alias_attach(&cache, &file, 0, observed[0],
          &alias, &file, 0, rearm) == KERNEL_PAGE_CACHE_STATUS_OK, 23);
    write_probe = 1;
    check(kernel_thread_create(write_worker, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(write_worker, (void *)1) == KERNEL_SCHEDULER_STATUS_OK, 25);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 26);
    }
    write_probe = 0;
    uint32_t references;
    check(physical_page_reference_count(&allocator, inserted_page, &references) == PHYSICAL_PAGE_STATUS_OK && references == 1, 70);
    kernel_page_cache_alias_detach(&alias);
    check(kernel_vfs_sync(&file, 0, &sequence) == 0, 27);
    check(kernel_vfs_create(&mount, "/orphan", 0600, &orphan) == 0 &&
          kernel_vfs_pwrite(&orphan, 0, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_unlink(&mount, "/orphan") == 0, 73);
    write_probe = 1;
    check(kernel_thread_create(orphan_worker, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(orphan_worker, (void *)1) == KERNEL_SCHEDULER_STATUS_OK, 74);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 75);
    }
    write_probe = 0;
    /* Clean eviction must release healthy inode metadata before unmount. */
    struct kernel_vfs_file reclaim_file = {0};
    check(kernel_vfs_create(&mount, "/reclaim-owner", 0600, &reclaim_file) == 0 &&
          kernel_vfs_pwrite(&reclaim_file, 0, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_sync(&reclaim_file, 0, &sequence) == 0 &&
          kernel_vfs_close(&reclaim_file) == 0, 84);
    (void)kernel_page_cache_reclaim(&cache, 64);
    struct kernel_heap_statistics before_reclaim, after_reclaim;
    for (unsigned i = 0; i < 16; i++) {
        struct kernel_vfs_file temporary = {0};
        uint64_t address;
        size_t valid;
        check(kernel_vfs_open(&mount, "/reclaim-owner", &temporary) == 0 &&
              kernel_page_cache_get(&cache, &temporary, 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 80);
        check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK &&
              kernel_vfs_close(&temporary) == 0, 81);
        check(kernel_page_cache_reclaim(&cache, 64) != 0, 82);
        if (!i) kernel_heap_get_statistics(&heap, &before_reclaim);
    }
    kernel_heap_get_statistics(&heap, &after_reclaim);
    check(before_reclaim.live_allocations == after_reclaim.live_allocations, 83);
    size_t warm_valid;
    check(kernel_page_cache_get(&cache, &file, 0, &inserted_page, &warm_valid) == KERNEL_PAGE_CACHE_STATUS_OK &&
          physical_page_release(&allocator, inserted_page) == PHYSICAL_PAGE_STATUS_OK, 85);
    check(kernel_vfs_open(&mount, "/cold0", &cold[0]) == 0 &&
          kernel_vfs_open(&mount, "/cold1", &cold[1]) == 0, 39);
    struct dtb_irq_info irq_info;
    check(dtb_read_irq_info(dtb, hart, &irq_info) == DTB_STATUS_OK &&
          riscv_plic_init((void *)(uintptr_t)irq_info.plic.base, irq_info.plic.size,
                          irq_info.context, irq_info.source_count), 40);
    uint32_t source = 0;
    for (unsigned i = 0; i < irq_info.route_count; i++)
        if (irq_info.routes[i].base == (uintptr_t)device.mmio) source = irq_info.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device, source), 41);
    virt_uart_puts("I/O handshake: ready\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 42);
    for (uintptr_t i = 0; i < 2; i++) check(kernel_thread_create(cold_reader, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 43);
    check(kernel_thread_create(progress_worker, 0) == KERNEL_SCHEDULER_STATUS_OK, 44);
    unsigned reaped = 0;
    while (reaped < 3) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 45);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    struct riscv_virtio_mmio_block_statistics stats;
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    check(cold_done == 2 && progress_done && stats.max_inflight >= 2 && stats.sleeps >= 2 && !stats.runtime_polls, 46);
    struct riscv_virtio_mmio_block_statistics queue_before = stats;
    virt_uart_puts("I/O handshake: queue\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 53);
    for (uintptr_t i = 0; i < 10; i++) check(kernel_thread_create(queue_worker, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 54);
    reaped = 0;
    while (reaped < 10) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 55);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    check(queue_done == 10 && stats.max_inflight == 8 && stats.queue_waits >= 2, 56);
    /* 队列场景持握 8 个请求：时间加权在途深度必须显著高于 1，
     * 等待与设备服务时间必须真实累计，忙时不超过总span。 */
    print_counters("queue", &stats);
    uint64_t queue_busy = stats.busy_ticks - queue_before.busy_ticks;
    uint64_t queue_weighted = stats.inflight_ticks - queue_before.inflight_ticks;
    uint64_t queue_waited = stats.queue_wait_ticks - queue_before.queue_wait_ticks;
    virt_uart_puts("I/O deltas: busy="); virt_uart_put_hex(queue_busy);
    virt_uart_puts(" weighted="); virt_uart_put_hex(queue_weighted);
    virt_uart_puts(" wait="); virt_uart_put_hex(queue_waited);
    virt_uart_putc('\n');
    /* 语义不变量：忙时每 tick 深度>=1，故 忙时<=加权；加权不超过观测峰值×忙时。
     * 只按事件计数的错误实现会给出 加权 << 忙时 而被拒绝。 */
    check(queue_busy > 0 && queue_weighted >= queue_busy &&
          queue_weighted <= stats.max_inflight * queue_busy && queue_waited > 0 &&
          stats.service_ticks > queue_before.service_ticks &&
          stats.total_ticks >= stats.busy_ticks, 270);
    for (unsigned mode = 0; mode < 3; mode++) test_batch(mode);
    test_read_batch();
    check(kernel_thread_create(background_writeback_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 98);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 99);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    check(kernel_thread_create(sync_source_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 233);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 234);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    check(kernel_thread_create(whole_write_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 262);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 263);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    check(kernel_thread_create(cleanup_worker, 0) == KERNEL_SCHEDULER_STATUS_OK, 47);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 48);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }

    check(physical_page_available(&allocator) == baseline, 16);
#if BOAROS_COST_DIAGNOSTICS
    cost_finish("pressure-io", 1);
#endif
    volatile void *mmio = device.mmio;
    uint64_t mmio_size = device.mmio_size;
    device = (struct riscv_virtio_mmio_block){0};
    check(riscv_virtio_mmio_block_init(&device, mmio, mmio_size, &allocator, dma,
                                      info.timebase_frequency) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK &&
          riscv_virtio_mmio_block_enable_irq(&device, source), 62);
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(1, info.timebase_frequency, 1, 0) == 0, 211);
#endif
    virt_uart_puts("I/O handshake: timeout\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 63);
    check(kernel_time_init(info.timebase_frequency, 0) == KERNEL_TIME_STATUS_OK &&
          riscv_timer_start(info.timebase_frequency, 100) == RISCV_TIMER_STATUS_OK, 64);
    for (uintptr_t i = 0; i < 4; i++) check(kernel_thread_create(timeout_worker, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 65);
    check(kernel_thread_create(timeout_batch_worker, 0) == KERNEL_SCHEDULER_STATUS_OK, 285);
    reaped = 0;
    while (reaped < 5) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 66);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    print_counters("timeout", &stats);
    check(timed_out == 5 && stats.timeouts == 1 && !stats.runtime_polls &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK &&
          physical_page_available(&allocator) == baseline, 67);
#if BOAROS_COST_DIAGNOSTICS
    cost_finish("timeout-cancel", 2);
#endif
    check(kernel_thread_create(socket_and_pipe_copy_probes, 0) == KERNEL_SCHEDULER_STATUS_OK, 289);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 290);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    check(physical_page_available(&allocator) == baseline, 291);
    virt_uart_puts("BoarOS: I/O sleep tests passed\n"); sbi_shutdown();
}
