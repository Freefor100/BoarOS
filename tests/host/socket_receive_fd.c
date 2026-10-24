/* Reuse the real MM/FD/OFD fixture without running its unrelated cases. */
#define main file_io_full_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/socket_call.h>

int main(int argc, char **argv)
{
    (void)argv; assert(argc == 2); setvbuf(stdout, NULL, _IONBF, 0);
    mm_test_attach(0); mm_test_activate_task();
    struct physical_page_allocator pages = {0}; struct kernel_heap heap = {0};
    initialize(&pages, &heap, arena, sizeof(arena));
    uint64_t baseline = physical_page_available(&pages);
    struct arch_mmu_page_table kernel = {0}; struct arch_mmu_user_space user = {0};
    assert(!riscv_sv39_page_table_init(&kernel, &pages)); kernel.state = ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user, &pages, &kernel) && !kernel_mm_create(&mm, &user) &&
        !kernel_mm_vma_enable(&mm, &heap) && !kernel_mm_brk_initialize(&mm, 0x10000, 0x20000));
    assert(!kernel_mm_context(&mm, &context)); mm_test_context = context;
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&mm, 0x200000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED_NOREPLACE, &address));
    struct kernel_files source = {0}, receiver = {0};
    assert(!kernel_files_create(&source, &heap) && !kernel_files_create(&receiver, &heap));
    struct kernel_socket_call call = {.files = &receiver};
    int64_t result; struct kernel_open_file_description *owner = NULL, *pin = NULL;
    assert(!kernel_files_pipe2(&source, &mm, address,
        KERNEL_FILES_O_NONBLOCK, &result) && !result);
    assert(!kernel_files_pin(&source, 0, &owner, &result) && !result && owner);
    struct kernel_open_file_description *original = owner;
    assert(!kernel_files_close(&source, 0, &result) && !result);
    assert(kernel_files_receive_fd(&receiver, &mm, address, 2, &owner, &call) ==
        -KERNEL_EINVAL && owner == original && !call.fd_reserved);
    assert(kernel_files_receive_fd(&receiver, &mm, address + BOAROS_PAGE_SIZE, 0, &owner, &call) ==
        -KERNEL_EFAULT && owner == original && !call.fd_reserved);
    assert(!receiver.record->statistics.current_open_fds && !receiver.record->slots[0].flags);
    assert(kernel_files_receive_fd(&receiver, &mm, address + BOAROS_PAGE_SIZE - 2, 1, &owner, &call) ==
        -KERNEL_EFAULT && owner == original && !call.fd_reserved);
    assert(!receiver.record->statistics.current_open_fds && !receiver.record->slots[0].flags);
    assert(!kernel_files_receive_fd(&receiver, &mm, address, 1, &owner, &call) && !owner && !call.fd_reserved);
    int32_t received = -1; size_t copied;
    assert(!kernel_copy_from_user(&mm, &received, address, sizeof(received), &copied) &&
        copied == sizeof(received) && received == 0);
    assert(receiver.record->slots[0].flags == KERNEL_FILES_FD_CLOEXEC &&
        !kernel_files_pin(&receiver, 0, &pin, &result) && !result && pin == original &&
        (kernel_open_file_flags(pin) & KERNEL_FILES_O_NONBLOCK));
    /* A later control entry fault preserves the descriptor already installed. */
    owner = pin; pin = NULL;
    uint64_t limit = receiver.nofile_limit; receiver.nofile_limit = 1;
    assert(kernel_files_receive_fd(&receiver, &mm, address, 0, &owner, &call) ==
        -KERNEL_EMFILE && owner == original && !call.fd_reserved);
    receiver.nofile_limit = limit;
    assert(kernel_files_receive_fd(&receiver, &mm, address + BOAROS_PAGE_SIZE, 0, &owner, &call) ==
        -KERNEL_EFAULT && owner == original && !call.fd_reserved && receiver.record->statistics.current_open_fds == 1 &&
        !receiver.record->slots[1].flags);
    assert(!kernel_files_receive_fd(&receiver, &mm, address + sizeof(received), 0, &owner, NULL) && !owner);
    assert(receiver.record->statistics.current_open_fds == 2 && !receiver.record->slots[1].flags &&
        receiver.record->slots[1].description == original);
    assert(!kernel_files_release(&source) && !kernel_files_release(&receiver));
    mm_test_context = 0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages, kernel.root_address));
    mm_test_finish();
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap, &stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&pages) &&
        physical_page_available(&pages) == baseline);
    puts("PASS receive FD: full/partial copy faults cancel reservation, shared OFD/flags, installed prefix and final owners");
}
