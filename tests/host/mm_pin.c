#define _POSIX_C_SOURCE 200809L
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/cpu.h>
#include <kernel/uaccess.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern _Thread_local uint64_t mm_test_context;
extern _Thread_local int mm_test_wrong_owner;
void mm_test_attach(unsigned);
static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(16384)));
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void mm_test_unavailable(void) { fputs("excluded business path reached\n", stderr); abort(); }
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }
static void fatal_pins(struct kernel_mm *mm, uint64_t address)
{
    struct rlimit limit = {0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    for (unsigned which = 0; which < 5; which++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            struct kernel_mm_user_pin pin = {0};
            assert(!kernel_mm_pin_user_page(mm, address, KERNEL_MM_READ, &pin));
            if (which == 0) mm_test_wrong_owner = 1;
            if (which == 1) {
                struct kernel_mm_user_pin copy = pin;
                kernel_mm_unpin_user_page(&copy);
            }
            if (which == 2) kernel_mm_unpin_user_page(&pin);
            if (which == 3) kernel_mm_pin_user_page(mm, address, KERNEL_MM_READ, &pin);
            if (which == 4) { mm_test_context = 0; kernel_mm_release(mm); }
            kernel_mm_unpin_user_page(&pin);
            _exit(0);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
}
int main(void)
{
    struct physical_page_allocator allocator = {0};
    struct boot_memory_layout layout = {0};
    struct arch_mmu_page_table kernel_table = {0};
    struct arch_mmu_user_space space = {0};
    struct kernel_heap heap = {0};
    struct kernel_mm mm = {0};
    mm_test_attach(0);
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(physical_page_allocator_init(&allocator, &layout) == 0);
    assert(physical_page_allocator_bind_access(&allocator, page_access) == 0);
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(kernel_heap_init(&heap, &allocator, page_address) == 0);
    assert(riscv_sv39_page_table_init(&kernel_table, &allocator) == 0);
    kernel_table.state = ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline = physical_page_available(&allocator);
    assert(arch_mmu_user_space_init(&space, &allocator, &kernel_table) == 0);
    assert(kernel_mm_create(&mm, &space) == 0);
    assert(kernel_mm_vma_enable(&mm, &heap) == 0);
    assert(kernel_mm_brk_initialize(&mm, 0x10000, 0x20000) == 0);
    uint64_t address, context;
    assert(kernel_mm_context(&mm, &context) == 0);
    mm_test_context = context;
    assert(kernel_mm_mmap_anonymous(&mm, 0x40000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &address) == 0);
    /* 构造resident页，准备完成后测试lookup到复制结束的物理owner。 */
    assert(kernel_mm_resolve_user_fault(&mm, address, KERNEL_MM_WRITE) == 0);
    unsigned char source[4] = {1, 2, 3, 4}, destination[4] = {0}; size_t copied;
    assert(kernel_copy_to_user(&mm, address + BOAROS_PAGE_SIZE - 2,
        source, sizeof(source), &copied) == KERNEL_UACCESS_STATUS_FAULT && copied == 2);
    assert(kernel_copy_from_user(&mm, destination, address + BOAROS_PAGE_SIZE - 2,
        sizeof(destination), &copied) == KERNEL_UACCESS_STATUS_FAULT && copied == 2);
    assert(destination[0] == 1 && destination[1] == 2 && !destination[2]);
    assert(kernel_copy_to_user(&mm, address, "hi", 3, &copied) == 0 && copied == 3);
    char string[8]; size_t length;
    assert(kernel_copy_string_from_user(&mm, string, address, sizeof(string), &length) == 0);
    assert(length == 2 && !strcmp(string, "hi"));
    uint32_t value = 17, observed;
    assert(!kernel_copy_to_user(&mm, address, &value, sizeof(value), &copied));
    assert(!kernel_user_cmpxchg_u32(&mm, address, 17, 39, &observed) && observed == 17);
    assert(!kernel_user_cmpxchg_u32(&mm, address, 17, 91, &observed) && observed == 39);
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    struct kernel_mm_user_pin rejected = {0};
    assert(kernel_mm_pin_user_page(&mm, address, KERNEL_MM_WRITE, &rejected) == KERNEL_MM_STATUS_ACCESS);
    assert(!rejected.registration && !rejected.page);
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
    fatal_pins(&mm, address);
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&mm, address, &mapping) == 0);
    struct kernel_mm_user_pin pin = {0};
    assert(kernel_mm_pin_user_page(&mm, address, KERNEL_MM_READ, &pin) == 0);
    uint32_t references;
    assert(physical_page_reference_count(&allocator,
        mapping.physical_address, &references) == 0);
    assert(references == 2);
    memset(pin.page, 0x37, BOAROS_PAGE_SIZE);
    assert(kernel_mm_munmap(&mm, address, BOAROS_PAGE_SIZE) == 0);
    assert(physical_page_reference_count(&allocator,
        pin.physical_address, &references) == 0 && references == 1);
    uint64_t allocated[1024]; unsigned count = 0;
    while (count < 1024 && physical_page_allocate(&allocator, &allocated[count]) == 0) {
        assert(allocated[count] != pin.physical_address);
        count++;
    }
    for (size_t i = 0; i < BOAROS_PAGE_SIZE; i++)
        assert(((unsigned char *)pin.page)[i] == 0x37);
    kernel_mm_unpin_user_page(&pin);
    for (unsigned i = 0; i < count; i++)
        assert(physical_page_release(&allocator, allocated[i]) == 0);
    mm_test_context = 0;
    assert(kernel_mm_release(&mm) == 0);
    assert(physical_page_available(&allocator) == baseline);
    assert(physical_page_allocator_audit(&allocator) == 0);
    puts("MM pin ownership and cleanup verified");
}
