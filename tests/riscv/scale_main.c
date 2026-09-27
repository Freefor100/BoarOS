#include <arch/riscv/mm.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/socket.h>
#include <kernel/scheduler.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#define USER UINT64_C(0x10000)
#define BUFFER UINT64_C(0x100000)
#define MIB UINT64_C(1048576)
static unsigned char pool[128 * MIB] __attribute__((aligned(4096)));
static unsigned char payload[4096];
static uint64_t active_satp;
/* This boot fixture has no scheduled tasks. Reject any attempted blocking;
 * only an empty socket queue's notification may be ignored. */
enum kernel_scheduler_status __wrap_kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    if (queue->head != 0) __builtin_trap();
    return KERNEL_SCHEDULER_STATUS_OK;
}
static unsigned fail_page;
static unsigned fail_metadata;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t n, size_t size, void **out)
{
    if (fail_metadata && --fail_metadata == 0) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(heap, n, size, out);
}
uint64_t __wrap_riscv_sv39_current_satp(void) { return active_satp; }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a, uint64_t *p)
{
    if (fail_page && --fail_page == 0) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a, p);
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static int dma(const void *p, size_t n, uint64_t *out) { (void)n; return physical(p, out); }
static void check(int good, unsigned test)
{
    if (!good) {
        virt_uart_puts("scale failed: "); virt_uart_put_hex(test); virt_uart_putc('\n');
        sbi_shutdown();
    }
}
static void number(const char *label, uint64_t value)
{
    virt_uart_puts(label); virt_uart_put_hex(value); virt_uart_putc('\n');
}
static void tcp_cost(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t listener_fd, client_fd, result;
    struct kernel_open_file_description *listener = 0, *client = 0;
    struct kernel_socket *accepted = 0;
    uint32_t address; uint16_t port;
    check(kernel_files_socket_create(files, 1, 0, &listener_fd) == KERNEL_FILES_STATUS_OK && listener_fd >= 0 &&
          kernel_files_socket_create(files, 1, 00004000, &client_fd) == KERNEL_FILES_STATUS_OK && client_fd >= 0 &&
          kernel_files_pin(files, listener_fd, &listener, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_pin(files, client_fd, &client, &result) == KERNEL_FILES_STATUS_OK && result == 0, 30);
    struct kernel_socket *server_socket = kernel_open_file_socket(listener);
    struct kernel_socket *client_socket = kernel_open_file_socket(client);
    check(kernel_socket_bind(server_socket, 0, 0) == 0 &&
          kernel_socket_getname(server_socket, &address, &port) == 0 &&
          kernel_socket_listen(server_socket, 1) == 0 &&
          kernel_socket_connect(client_socket, 0x0100007fU, port, 0) == 0 &&
          kernel_socket_accept(server_socket, &accepted) == 0, 31);
    struct kernel_socket_statistics before, after;
    kernel_socket_get_statistics(&before);
    uint64_t resolutions = kernel_uaccess_page_resolutions();
    check(kernel_files_write(files, mm, client_fd, BUFFER + 1, 8192, &result) ==
          KERNEL_FILES_STATUS_OK && result == 8192, 32);
    kernel_socket_get_statistics(&after);
    resolutions = kernel_uaccess_page_resolutions() - resolutions;
    number("TCP 8192-byte submissions: ", after.tcp_write_calls - before.tcp_write_calls);
    number("TCP user-page resolutions: ", resolutions);
    check(after.tcp_write_calls - before.tcp_write_calls <= 8 && resolutions <= 8 &&
          after.tcp_written_bytes - before.tcp_written_bytes == 8192, 33);
    kernel_socket_destroy(accepted);
    check(kernel_open_file_release(&client) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_open_file_release(&listener) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_files_close(files, client_fd, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_close(files, listener_fd, &result) == KERNEL_FILES_STATUS_OK && result == 0, 34);
}

static void udp_buffer_oom(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t fd, result;
    struct kernel_open_file_description *pin = 0;
    check(kernel_files_socket_create(files, 2, 00004000, &fd) == KERNEL_FILES_STATUS_OK && fd >= 0 &&
          kernel_files_pin(files, fd, &pin, &result) == KERNEL_FILES_STATUS_OK && result == 0, 36);
    struct kernel_socket *socket = kernel_open_file_socket(pin);
    uint32_t address; uint16_t port;
    check(kernel_socket_bind(socket, 0, 0) == 0 && kernel_socket_getname(socket, &address, &port) == 0 &&
          kernel_socket_sendto(socket, mm, BUFFER, 8192, 0x0100007fU, port) == 8192, 37);
    fail_page = 1;
    check(kernel_files_read(files, mm, fd, BUFFER, 8192, &result) == KERNEL_FILES_STATUS_OK &&
          result == -KERNEL_ENOMEM && fail_page == 0, 38);
    /* Failure must leave the complete datagram owned by the socket queue. */
    check(kernel_socket_recvfrom(socket, mm, BUFFER, 8192, &address, &port) == 8192, 39);
    for (unsigned page = 0; page < 2; page++) {
        size_t copied;
        check(kernel_copy_from_user(mm, payload, BUFFER + page * 4096, sizeof(payload), &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == sizeof(payload), 40);
        for (unsigned i = 0; i < sizeof(payload); i++) check(payload[i] == (unsigned char)(i * 17 + 3), 41);
    }
    check(kernel_open_file_release(&pin) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_files_close(files, fd, &result) == KERNEL_FILES_STATUS_OK && result == 0, 42);
}

static struct riscv_mm_statistics mapped_cost(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t bytes)
{
    int64_t result;
    struct kernel_open_file_description *pin = 0;
    uint64_t address;
    check(kernel_files_ftruncate(files, fd, bytes, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_pin(files, fd, &pin, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_mm_mmap_file_private(mm, &pin, 0x20000000, bytes, 0,
              KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED_NOREPLACE | KERNEL_MM_MAP_SHARED,
              &address) == KERNEL_MM_STATUS_OK && pin == 0, 20);
    uint64_t pages = bytes / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        if (i < 128) {
            for (unsigned failure = 1; failure <= 2; failure++) {
                struct kernel_mm_mapping mapping;
                fail_metadata = failure;
                enum kernel_mm_status status = kernel_mm_resolve_user_fault(mm, address + i * 4096, KERNEL_MM_READ);
                fail_metadata = 0;
                if (status == KERNEL_MM_STATUS_OK) break;
                check(status == KERNEL_MM_STATUS_NO_MEMORY &&
                      kernel_mm_lookup(mm, address + i * 4096, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED, 27);
            }
        }
        struct kernel_mm_mapping mapping;
        if (kernel_mm_lookup(mm, address + i * 4096, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED)
            check(kernel_mm_resolve_user_fault(mm, address + i * 4096, KERNEL_MM_READ) == KERNEL_MM_STATUS_OK, 21);
    }
    struct riscv_mm_statistics before, after;
    if (bytes == 16 * MIB) {
        struct kernel_mm clone = {0};
        check(kernel_mm_fork(&clone, mm) == KERNEL_MM_STATUS_OK, 43);
        riscv_kernel_mm_get_statistics(&clone, &after);
        check(after.resident_probes == 0 && after.protect_visits == 0 &&
              after.protect_address_flushes == 0 && after.protect_global_flushes == 0, 44);
        check(kernel_mm_release(&clone) == KERNEL_MM_STATUS_OK, 45);
    }
    riscv_kernel_mm_get_statistics(mm, &before);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t index = (i * 4093) & (pages - 1);
        unsigned char value = (unsigned char)i;
        size_t copied;
        check(kernel_copy_to_user(mm, address + index * 4096, &value, 1, &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == 1, 22);
    }
    riscv_kernel_mm_get_statistics(mm, &after);
    after.resident_probes -= before.resident_probes;
    after.protect_visits -= before.protect_visits;
    after.protect_address_flushes -= before.protect_address_flushes;
    after.protect_global_flushes -= before.protect_global_flushes;
    number("mapped bytes: ", bytes);
    number("resident probes: ", after.resident_probes);
    number("protect visits: ", after.protect_visits);
    number("address flushes: ", after.protect_address_flushes);
    number("global flushes: ", after.protect_global_flushes);
    for (uint64_t i = 0; i < pages; i++) {
        unsigned char value; size_t copied;
        uint64_t index = (i * 4093) & (pages - 1);
        check(kernel_copy_from_user(mm, &value, address + index * 4096, 1, &copied) ==
              KERNEL_UACCESS_STATUS_OK && value == (unsigned char)i, 23);
    }
    check(kernel_mm_munmap(mm, address, bytes) == KERNEL_MM_STATUS_OK &&
          kernel_files_ftruncate(files, fd, 0, &result) == KERNEL_FILES_STATUS_OK && result == 0, 24);
    return after;
}

void kernel_main(unsigned long hart, const void *dtb)
{
    (void)hart;
    struct dtb_boot_info info;
    struct boot_memory_layout layout = {0};
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    struct riscv_sv39_page_table table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_page_cache cache = {0};
    struct riscv_virtio_mmio_block device = {0};
    struct kernel_vfs_mount mount = {0};
    struct kernel_files files = {0};
    struct kernel_fs_context fs = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK, 1);
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    check(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK &&
          kernel_heap_init(&heap, &allocator, physical) == KERNEL_HEAP_STATUS_OK &&
          riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 2);
    /* Bare boot tests normally use physical aliases. lwIP's static tables
     * also contain linked high-half pointers, so map both aliases here. */
    check(riscv_sv39_map_range(&table, 0x80000000, 0x80000000, 256 * MIB,
              RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_map_range(&table, UINT64_C(0xffffffff80000000), 0x80200000, 256 * MIB,
              RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_map_range(&table, 0x10000000, 0x10000000, 0x200000,
              RISCV_SV39_READ | RISCV_SV39_WRITE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_activate(&table) == RISCV_SV39_STATUS_OK, 35);
    uint64_t baseline = physical_page_available(&allocator);
    check(kernel_page_cache_init(&cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 3);
    int found = 0;
    for (unsigned i = 0; i < info.virtio_mmio_count; i++) {
        if (riscv_virtio_mmio_block_init(&device, (void *)(uintptr_t)info.virtio_mmio[i].base,
                info.virtio_mmio[i].size, &allocator, dma, info.timebase_frequency) ==
                RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) { found = 1; break; }
    }
    check(found && kernel_vfs_mount_root(&mount, &device.block, &heap, &cache) == 0 &&
          riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
          riscv_kernel_mm_create(&mm, &space) == KERNEL_MM_STATUS_OK &&
          kernel_mm_vma_enable(&mm, &heap) == KERNEL_MM_STATUS_OK &&
          kernel_mm_brk_initialize(&mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
          kernel_fs_context_create(&fs, &mount, &heap) == KERNEL_FS_CONTEXT_STATUS_OK &&
          kernel_files_create(&files, &heap) == KERNEL_FILES_STATUS_OK &&
          riscv_kernel_mm_satp(&mm, &active_satp) == KERNEL_MM_STATUS_OK, 4);
    uint64_t address;
    check(kernel_mm_mmap_anonymous(&mm, USER, 4096, KERNEL_MM_READ | KERNEL_MM_WRITE,
              KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK &&
          kernel_mm_mmap_anonymous(&mm, BUFFER, MIB + 4096, KERNEL_MM_READ | KERNEL_MM_WRITE,
              KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK, 5);
    size_t copied;
    check(kernel_copy_to_user(&mm, USER, "/scale", 7, &copied) == KERNEL_UACCESS_STATUS_OK, 6);
    int64_t fd, result;
    check(kernel_files_openat(&files, &fs, &mm, -100, USER, 2 | 0100, 0600, &fd) ==
          KERNEL_FILES_STATUS_OK && fd >= 0, 7);
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)(i * 17 + 3);
    for (uint64_t at = 0; at < MIB + 4096; at += 4096)
        check(kernel_copy_to_user(&mm, BUFFER + at, payload, sizeof(payload), &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == sizeof(payload), 8);
    struct kernel_files_statistics before, after;
    kernel_files_get_statistics(&files, &before);
    uint64_t resolutions = kernel_uaccess_page_resolutions();
    check(kernel_files_write(&files, &mm, fd, BUFFER, MIB, &result) == KERNEL_FILES_STATUS_OK &&
          result == MIB, 9);
    resolutions = kernel_uaccess_page_resolutions() - resolutions;
    kernel_files_get_statistics(&files, &after);
    number("file write chunks: ", after.write_chunks - before.write_chunks);
    number("file user-page resolutions: ", resolutions);
    check(after.write_chunks - before.write_chunks <= 512 && resolutions <= 512, 10);
    for (uint64_t at = 0; at < MIB; at += 4096) {
        check(kernel_files_pread(&files, &mm, fd, BUFFER, 4096, at, &result) == KERNEL_FILES_STATUS_OK && result == 4096, 11);
        check(kernel_copy_from_user(&mm, payload, BUFFER, sizeof(payload), &copied) == KERNEL_UACCESS_STATUS_OK, 12);
        for (unsigned i = 0; i < sizeof(payload); i++) check(payload[i] == (unsigned char)(i * 17 + 3), 13);
    }
    fail_page = 1;
    check(kernel_files_pwrite(&files, &mm, fd, BUFFER, 4096, 0, &result) == KERNEL_FILES_STATUS_OK &&
          result == -KERNEL_ENOMEM && fail_page == 0, 14);
    tcp_cost(&files, &mm);
    udp_buffer_oom(&files, &mm);
    struct riscv_mm_statistics small = mapped_cost(&files, &mm, fd, 16 * MIB);
    struct riscv_mm_statistics large = mapped_cost(&files, &mm, fd, 64 * MIB);
    check(large.resident_probes <= 6 * small.resident_probes, 25);
    check(small.protect_visits <= 6 * (16 * MIB / 4096) &&
          large.protect_visits <= 6 * (64 * MIB / 4096) &&
          small.protect_global_flushes == 0 && large.protect_global_flushes == 0 &&
          small.protect_address_flushes == 16 * MIB / 4096 &&
          large.protect_address_flushes == 64 * MIB / 4096, 26);
    check(kernel_files_close(&files, fd, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_release(&files) == KERNEL_FILES_STATUS_OK &&
          kernel_mm_release(&mm) == KERNEL_MM_STATUS_OK &&
          kernel_fs_context_release(&fs) == KERNEL_FS_CONTEXT_STATUS_OK &&
          kernel_vfs_unmount(&mount) == 0 &&
          kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 15);
    check(physical_page_available(&allocator) == baseline, 16);
    virt_uart_puts("BoarOS: scale tests passed\n");
    sbi_shutdown();
}
