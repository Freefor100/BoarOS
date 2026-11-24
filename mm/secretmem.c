#include <arch/mmu.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/secretmem.h>
#include <kernel/task.h>
#include "../fs/char_device_internal.h"
#include "../fs/files/private.h"
#include "../fs/open_file_internal.h"
#include "../third_party/lwext4/include/misc/tree.h"
#include <string.h>

struct secret_page {
    RB_ENTRY(secret_page) by_offset, by_frame;
    uint64_t offset, physical;
};
RB_HEAD(secret_offsets, secret_page);
RB_HEAD(secret_frames, secret_page);

struct secretmem {
    struct kernel_mutex lock;
    struct kernel_heap *heap;
    struct secret_offsets pages;
};

static struct secret_frames frames;
static struct kernel_raw_lock frames_lock = {.rank = KERNEL_RAW_RANK_PAGE,
    .initialized = KERNEL_RAW_INITIALIZED};
static unsigned frame_count;
static uint64_t next_inode = 1;

static int compare_offset(const struct secret_page *a, const struct secret_page *b)
{ return (a->offset > b->offset) - (a->offset < b->offset); }
static int compare_frame(const struct secret_page *a, const struct secret_page *b)
{ return (a->physical > b->physical) - (a->physical < b->physical); }
RB_GENERATE_INTERNAL(secret_offsets, secret_page, by_offset, compare_offset, static inline)
RB_GENERATE_INTERNAL(secret_frames, secret_page, by_frame, compare_frame, static inline)

int kernel_secretmem_page_protected(uint64_t physical)
{
    if (!__atomic_load_n(&frame_count, __ATOMIC_ACQUIRE)) return 0;
    struct secret_page key = {.physical = physical & ~BOAROS_PAGE_MASK};
    KERNEL_RAW_SCOPE(guard, &frames_lock);
    return RB_FIND(secret_frames, &frames, &key) != NULL;
}

static void release_page(struct secretmem *secret, struct secret_page *page)
{
    struct physical_page_allocator *allocator = secret->heap->page_allocator;
    /* The mapping owner survives all user translations, so restoration
     * needs no allocation and the restored alias is private until release. */
    if (arch_mmu_secret_page_restore(allocator, page->physical)) __builtin_trap();
    void *bytes;
    if (physical_page_map_owned(allocator, page->physical, &bytes)) __builtin_trap();
    memset(bytes, 0, BOAROS_PAGE_SIZE);
    {
        KERNEL_RAW_SCOPE(guard, &frames_lock);
        RB_REMOVE(secret_frames, &frames, page);
        __atomic_sub_fetch(&frame_count, 1, __ATOMIC_RELEASE);
    }
    if (physical_page_release(allocator, page->physical)) __builtin_trap();
    kernel_heap_release(secret->heap, page);
}

static void release_secretmem(void *instance)
{
    struct secretmem *secret = instance;
    struct secret_page *page;
    while ((page = RB_MIN(secret_offsets, &secret->pages)) != NULL) {
        RB_REMOVE(secret_offsets, &secret->pages, page);
        release_page(secret, page);
    }
    kernel_heap_release(secret->heap, secret);
}

static const struct kernel_char_device secret_operations = {
    .kind = KERNEL_OPEN_FILE_KIND_SECRETMEM,
    .release = release_secretmem,
};

static int prepare_page(struct secretmem *secret, uint64_t offset, struct secret_page **output)
{
    struct physical_page_allocator *allocator = secret->heap->page_allocator;
    struct secret_page *page;
    if (kernel_heap_allocate(secret->heap, sizeof(*page), (void **)&page)) return -KERNEL_ENOMEM;
    *page = (struct secret_page){.offset = offset};
    if (physical_page_allocate(allocator, &page->physical)) {
        kernel_heap_release(secret->heap, page);
        return -KERNEL_ENOMEM;
    }
    void *bytes;
    if (physical_page_map_owned(allocator, page->physical, &bytes)) __builtin_trap();
    memset(bytes, 0, BOAROS_PAGE_SIZE);
    enum arch_mmu_status status = arch_mmu_secret_page_protect(allocator, page->physical);
    if (status) {
        if (physical_page_release(allocator, page->physical)) __builtin_trap();
        kernel_heap_release(secret->heap, page);
        return status == ARCH_MMU_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    {
        KERNEL_RAW_SCOPE(index, &frames_lock);
        if (RB_INSERT(secret_frames, &frames, page)) __builtin_trap();
        __atomic_add_fetch(&frame_count, 1, __ATOMIC_RELEASE);
    }
    *output = page;
    return 0;
}

int kernel_secretmem_acquire_page(struct kernel_open_file_description *file,
    uint64_t offset, int writable, uint64_t *physical)
{
    (void)writable;
    if (offset >= __atomic_load_n(&file->file.size, __ATOMIC_ACQUIRE)) return -KERNEL_EINVAL;
    struct secretmem *secret = file->device_instance;
    struct secret_page key = {.offset = offset};
    struct secret_page *page;
    {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&secret->lock, &guard);
        page = RB_FIND(secret_offsets, &secret->pages, &key);
    }
    if (!page) {
        struct secret_page *candidate;
        int error = prepare_page(secret, offset, &candidate);
        if (error) return error;
        {
            KERNEL_LOCK_SCOPE(guard);
            kernel_mutex_lock(&secret->lock, &guard);
            page = RB_INSERT(secret_offsets, &secret->pages, candidate);
        }
        if (!page) page = candidate;
        else release_page(secret, candidate);
    }
    /* The caller owns the file through the fault. All indexed pages live
     * until last close, so the offset lock need not cover the page borrow. */
    if (physical_page_acquire(secret->heap->page_allocator, page->physical)) __builtin_trap();
    *physical = page->physical;
    return 0;
}

enum kernel_syscall_status syscall_handle_memfd_secret(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *result)
{
    result->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (!arch_mmu_secret_page_supported()) {
        result->value = -KERNEL_ENOSYS;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    uint32_t flags = (uint32_t)request->arguments[0];
    if (flags & ~02000000U) {
        result->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_files *files;
    if (kernel_task_files_borrow(caller, &files)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    uint32_t fd;
    int error;
    if (kernel_files_find_free_fd(files, &fd, &error)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (error) {
        result->value = error;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation = (struct kernel_files_fd_reservation){files, fd};
    struct secretmem *secret = NULL;
    struct kernel_open_file_description *file = NULL;
    if (kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*secret), (void **)&secret) ||
        kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*file), (void **)&file)) {
        if (secret) kernel_heap_release(files->heap, secret);
        result->value = -KERNEL_ENOMEM;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    secret->heap = files->heap;
    kernel_mutex_init(&secret->lock, 35, (uintptr_t)secret);
    file->heap = files->heap;
    file->references = 1;
    file->kind = KERNEL_OPEN_FILE_KIND_SECRETMEM;
    file->proc_identity = __atomic_fetch_add(&next_inode, 1, __ATOMIC_RELAXED);
    file->open_flags = 0100002U;
    file->device = &secret_operations;
    file->device_instance = secret;
    file->credentials = kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    if (kernel_files_install_new_owned_at(files, fd,
            flags ? KERNEL_FILES_FD_CLOEXEC : 0, &file)) {
        kernel_files_release_uninstalled_description(files, &file);
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    reservation.files = NULL;
    result->value = fd;
    return KERNEL_SYSCALL_STATUS_OK;
}
