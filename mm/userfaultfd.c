#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/page.h>
#include <kernel/userfaultfd.h>
#include <kernel/wait_internal.h>
#include <arch/mmu.h>
#include "../fs/char_device_internal.h"
#include "../fs/files/private.h"
#include "../fs/open_file_internal.h"
#include "../fs/uaccess_iov_internal.h"
#include "userfaultfd_internal.h"

#include <stddef.h>

enum {
    UFFD_NONBLOCK = 04000, UFFD_CLOEXEC = 02000000,
    UFFD_API = 0xaa,
    UFFD_REGISTER_MISSING = 1, UFFD_DONTWAKE = 1,
    UFFDIO_API = 0xc018aa3f, UFFDIO_REGISTER = 0xc020aa00,
    UFFDIO_UNREGISTER = 0x8010aa01, UFFDIO_WAKE = 0x8010aa02,
    UFFDIO_COPY = 0xc028aa03, UFFDIO_ZEROPAGE = 0xc020aa04,
};

#define UFFD_API_IOCTLS ((UINT64_C(1) << 63) | 3U)
#define UFFD_RANGE_IOCTLS (4U | 8U | 16U)

struct uffd_api { uint64_t api, features, ioctls; };
struct uffd_range { uint64_t start, length; };
struct uffd_register { struct uffd_range range; uint64_t mode, ioctls; };
struct uffd_copy { uint64_t destination, source, length, mode; int64_t result; };
struct uffd_zeropage { struct uffd_range range; uint64_t mode; int64_t result; };
_Static_assert(sizeof(struct uffd_message) == 32, "Linux userfaultfd message");
_Static_assert(sizeof(struct uffd_copy) == 40 && sizeof(struct uffd_register) == 32 &&
    sizeof(struct uffd_zeropage) == 32 && sizeof(struct uffd_api) == 24, "Linux userfaultfd ioctls");

static void release_userfault(void *object)
{
    struct kernel_userfault_ctx *ctx = object;
    kernel_mm_userfault_detach(ctx);
    kernel_userfault_context_put(ctx);
}

static uint32_t poll_userfault(void *object, uint32_t flags, uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)requested;
    struct kernel_userfault_ctx *ctx = object;
    KERNEL_RAW_SCOPE(guard, &ctx->lock);
    if (queue) *queue = &ctx->ready;
    if (!__atomic_load_n(&ctx->initialized, __ATOMIC_RELAXED) || !(flags & UFFD_NONBLOCK))
        return KERNEL_POLLERR;
    if (ctx->dead) return KERNEL_POLLHUP;
    return ctx->pending ? KERNEL_POLLIN : 0;
}

static enum kernel_files_status read_userfault(void *object, struct kernel_task *task,
    struct kernel_files *files, struct kernel_open_file_description **owner, void **iov_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov, size_t count,
    uint64_t length, uint32_t flags, int64_t *result)
{
    (void)task;
    (void)files;
    (void)owner;
    (void)iov_owner;
    struct kernel_uaccess_iov_cursor cursor = {iov, count, 0, 0};
    struct uffd_message message;
    *result = 0;
    if (length < sizeof(message)) {
        *result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    while (length >= sizeof(message)) {
        int ready = kernel_userfault_next(object, &message, *result || (flags & UFFD_NONBLOCK));
        if (ready <= 0) {
            if (!*result) *result = ready;
            break;
        }
        size_t copied = 0;
        if (kernel_copy_to_user_iov(mm, &cursor, &message, sizeof(message), &copied) ||
            copied != sizeof(message)) {
            if (!*result) *result = -KERNEL_EFAULT;
            break;
        }
        length -= sizeof(message);
        *result += sizeof(message);
    }
    return KERNEL_FILES_STATUS_OK;
}

static int copy_input(struct kernel_mm *mm, void *value, uint64_t address, size_t size)
{
    size_t copied = 0;
    return kernel_copy_from_user(mm, value, address, size, &copied) || copied != size ?
        -KERNEL_EFAULT : 0;
}

static int copy_output(struct kernel_mm *mm, uint64_t address, const void *value, size_t size)
{
    size_t copied = 0;
    return kernel_copy_to_user(mm, address, value, size, &copied) || copied != size ?
        -KERNEL_EFAULT : 0;
}

static int negotiate(struct kernel_userfault_ctx *ctx, struct kernel_mm *mm, uint64_t argument)
{
    struct uffd_api api;
    if (copy_input(mm, &api, argument, sizeof(api))) return -KERNEL_EFAULT;
    if (api.api == UFFD_API && !api.features) {
        api.ioctls = UFFD_API_IOCTLS;
        if (copy_output(mm, argument, &api, sizeof(api))) return -KERNEL_EFAULT;
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        if (!__atomic_load_n(&ctx->initialized, __ATOMIC_RELAXED)) {
            __atomic_store_n(&ctx->initialized, 1, __ATOMIC_RELEASE);
            return 0;
        }
    }
    api = (struct uffd_api){0};
    return copy_output(mm, argument, &api, sizeof(api)) ? -KERNEL_EFAULT : -KERNEL_EINVAL;
}

static int valid_range(struct uffd_range range)
{
    return range.start >= BOAROS_PAGE_SIZE && range.start < ARCH_MMU_USER_LIMIT &&
        range.length && !((range.start | range.length) & BOAROS_PAGE_MASK) &&
        range.length <= ARCH_MMU_USER_LIMIT - range.start;
}

static int get_zero_page(struct kernel_userfault_ctx *ctx, uint64_t *physical)
{
    struct physical_page_allocator *allocator = ctx->target.allocator;
    uint64_t candidate;
    if (physical_page_allocate(allocator, &candidate)) {
        *physical = __atomic_load_n(&ctx->zero_page, __ATOMIC_ACQUIRE);
        return *physical == KERNEL_MM_NO_PAGE ? -KERNEL_ENOMEM : 0;
    }
    void *bytes;
    if (physical_page_map_owned(allocator, candidate, &bytes)) __builtin_trap();
    arch_zero_owned_memory(bytes, BOAROS_PAGE_SIZE);
    /* Each installed COW leaf gets a separate reference. The context's
     * reference prevents upgrading any live alias to writable in place. */
    *physical = KERNEL_MM_NO_PAGE;
    if (__atomic_compare_exchange_n(&ctx->zero_page, physical, candidate, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        *physical = candidate;
    } else if (physical_page_release(allocator, candidate)) __builtin_trap();
    return 0;
}

static int fill_range(struct kernel_userfault_ctx *ctx, struct kernel_mm *caller,
    struct uffd_range range, uint64_t source, int zero, uint64_t mode, uint64_t result_address)
{
    if (!valid_range(range) || (mode & ~UFFD_DONTWAKE) ||
        (!zero && (source < BOAROS_PAGE_SIZE || source >= ARCH_MMU_USER_LIMIT ||
            range.length > ARCH_MMU_USER_LIMIT - source))) return -KERNEL_EINVAL;
    uint64_t end = range.start + range.length;
    uint64_t generation = 0;
    uint64_t zero_page = zero ? __atomic_load_n(&ctx->zero_page, __ATOMIC_ACQUIRE) :
        KERNEL_MM_NO_PAGE;
    int error = 0;
    if (zero_page == KERNEL_MM_NO_PAGE) {
        /* Private allocation and source copying may sleep. Validate the
         * original destination before dropping its MM qualification. */
        error = kernel_mm_userfault_check_fill(ctx, range.start, end, &generation);
        if (error == -KERNEL_ESRCH) return error;
        if (!error && zero) error = get_zero_page(ctx, &zero_page);
    }
    uint64_t done = 0;
    struct physical_page_allocator *allocator = ctx->target.allocator;
    while (!error && done < range.length) {
        uint64_t physical = zero_page;
        if (zero) {
            if (physical_page_acquire(allocator, physical)) __builtin_trap();
        } else {
            if (physical_page_allocate(allocator, &physical)) {
                error = -KERNEL_ENOMEM;
                break;
            }
            void *bytes;
            if (physical_page_map_owned(allocator, physical, &bytes)) __builtin_trap();
            error = copy_input(caller, bytes, source + done, BOAROS_PAGE_SIZE);
        }
        if (!error)
            error = kernel_mm_userfault_fill(ctx, range.start + done, end, &generation, physical, zero);
        if (error) {
            if (physical_page_release(allocator, physical)) __builtin_trap();
            if (!generation && error == -KERNEL_ESRCH) return error;
            break;
        }
        done += BOAROS_PAGE_SIZE;
    }
    int64_t result = done ? (int64_t)done : error;
    if (copy_output(caller, result_address, &result, sizeof(result))) return -KERNEL_EFAULT;
    if (!done) return error;
    if (!(mode & UFFD_DONTWAKE)) kernel_userfault_wake(ctx, range.start, range.start + done);
    return done == range.length ? 0 : -KERNEL_EAGAIN;
}

static int ioctl_userfault(void *object, struct kernel_task *task, struct kernel_files *files,
    struct kernel_open_file_description **owner, struct kernel_mm *mm, uint64_t command,
    uint64_t argument)
{
    (void)task;
    (void)files;
    (void)owner;
    struct kernel_userfault_ctx *ctx = object;
    if (command == UFFDIO_API) return negotiate(ctx, mm, argument);
    if (!__atomic_load_n(&ctx->initialized, __ATOMIC_ACQUIRE)) return -KERNEL_EINVAL;
    switch (command) {
    case UFFDIO_REGISTER: {
        struct uffd_register registration;
        if (copy_input(mm, &registration, argument, offsetof(struct uffd_register, ioctls)))
            return -KERNEL_EFAULT;
        struct uffd_range range = registration.range;
        if (!valid_range(range) || registration.mode != UFFD_REGISTER_MISSING)
            return -KERNEL_EINVAL;
        int error = kernel_mm_userfault_register(ctx, range.start, range.start + range.length, 1);
        if (error) return error;
        const uint64_t ioctls = UFFD_RANGE_IOCTLS;
        return copy_output(mm, argument + offsetof(struct uffd_register, ioctls), &ioctls, sizeof(ioctls));
    }
    case UFFDIO_UNREGISTER:
    case UFFDIO_WAKE: {
        struct uffd_range range;
        if (copy_input(mm, &range, argument, sizeof(range))) return -KERNEL_EFAULT;
        if (!valid_range(range)) return -KERNEL_EINVAL;
        if (command == UFFDIO_UNREGISTER)
            return kernel_mm_userfault_register(ctx, range.start, range.start + range.length, 0);
        kernel_userfault_wake(ctx, range.start, range.start + range.length);
        return 0;
    }
    case UFFDIO_COPY: {
        struct uffd_copy copy;
        if (copy_input(mm, &copy, argument, offsetof(struct uffd_copy, result))) return -KERNEL_EFAULT;
        return fill_range(ctx, mm, (struct uffd_range){copy.destination, copy.length},
            copy.source, 0, copy.mode, argument + offsetof(struct uffd_copy, result));
    }
    case UFFDIO_ZEROPAGE: {
        struct uffd_zeropage zero;
        if (copy_input(mm, &zero, argument, offsetof(struct uffd_zeropage, result))) return -KERNEL_EFAULT;
        return fill_range(ctx, mm, zero.range, 0, 1, zero.mode,
            argument + offsetof(struct uffd_zeropage, result));
    }
    default:
        return -KERNEL_EINVAL;
    }
}

static const struct kernel_char_device uffd_operations = {
    .kind = KERNEL_OPEN_FILE_KIND_USERFAULTFD, .release = release_userfault,
    .stateless_io = 1, .readv = read_userfault, .poll = poll_userfault, .ioctl = ioctl_userfault,
};

enum kernel_files_status kernel_files_userfaultfd_create(struct kernel_files *files,
    struct kernel_mm *mm, uint32_t flags, int64_t *result)
{
    const struct kernel_credentials *credentials = kernel_credentials_current();
    if (!(flags & UFFD_USER_MODE_ONLY) &&
        !kernel_credentials_capable(credentials, KERNEL_CAP_SYS_PTRACE)) {
        *result = -KERNEL_EPERM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (flags & ~(UFFD_USER_MODE_ONLY | UFFD_NONBLOCK | UFFD_CLOEXEC)) {
        *result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }

    unsigned fd;
    int error = 0;
    enum kernel_files_status status = kernel_files_find_free_fd(files, &fd, &error);
    if (status || error) {
        *result = error;
        return status;
    }
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation = (struct kernel_files_fd_reservation){files, fd};

    struct kernel_userfault_ctx *ctx = NULL;
    struct kernel_open_file_description *file = NULL;
    if (kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*ctx), (void **)&ctx) ||
        kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*file), (void **)&file)) {
        error = -KERNEL_ENOMEM;
        goto fail;
    }
    ctx->heap = files->heap;
    ctx->flags = flags;
    ctx->references = 1;
    ctx->zero_page = KERNEL_MM_NO_PAGE;
    ctx->pending_tail = &ctx->pending;
    kernel_raw_lock_init(&ctx->lock, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&ctx->ready);
    error = kernel_mm_userfault_attach(mm, ctx);
    if (error) {
        if (kernel_wait_queue_close(&ctx->ready) ||
            kernel_wait_queue_destroy(&ctx->ready)) __builtin_trap();
        goto fail;
    }

    file->heap = files->heap;
    file->references = 1;
    file->kind = KERNEL_OPEN_FILE_KIND_USERFAULTFD;
    file->open_flags = flags & UFFD_NONBLOCK;
    file->device = &uffd_operations;
    file->device_instance = ctx;
    file->credentials = kernel_credentials_get(credentials);
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    status = kernel_files_install_new_owned_at(files, fd,
        flags & UFFD_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, &file);
    if (status) {
        (void)kernel_files_release_uninstalled_description(files, &file);
        return status;
    }
    reservation.files = NULL;
    *result = fd;
    return KERNEL_FILES_STATUS_OK;

fail:
    if (ctx) kernel_heap_release(files->heap, ctx);
    if (file) kernel_heap_release(files->heap, file);
    *result = error;
    return KERNEL_FILES_STATUS_OK;
}
