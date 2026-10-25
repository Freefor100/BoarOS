#include "private.h"

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_PROT_READ UINT64_C(0x1)
#define LINUX_PROT_WRITE UINT64_C(0x2)
#define LINUX_PROT_EXEC UINT64_C(0x4)
#define LINUX_MAP_PRIVATE UINT64_C(0x2)
#define LINUX_MAP_SHARED UINT64_C(0x1)
#define LINUX_MAP_TYPE_MASK UINT64_C(0xf)
#define LINUX_MAP_SHARED_VALIDATE UINT64_C(0x3)
#define LINUX_MAP_FIXED UINT64_C(0x10)
#define LINUX_MAP_ANONYMOUS UINT64_C(0x20)
#define LINUX_MAP_GROWSDOWN UINT64_C(0x100)
#define LINUX_MAP_LOCKED UINT64_C(0x2000)
#define LINUX_MAP_HUGETLB UINT64_C(0x40000)
#define LINUX_MAP_DENYWRITE UINT64_C(0x800)
#define LINUX_MAP_POPULATE UINT64_C(0x8000)
#define LINUX_MAP_EXECUTABLE UINT64_C(0x1000)
#define LINUX_MAP_FIXED_NOREPLACE UINT64_C(0x100000)
/* Fixed Linux LEGACY_MAP_MASK also accepts huge-page size encoding bits even
 * without MAP_HUGETLB; they do not request huge pages on their own. */
#define LINUX_MAP_LEGACY_EXTRA UINT64_C(0x7c010000)
#define LINUX_MAP_KNOWN_FLAGS                                             \
    (LINUX_MAP_TYPE_MASK | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |       \
     LINUX_MAP_DENYWRITE | LINUX_MAP_EXECUTABLE | LINUX_MAP_NORESERVE |  \
     LINUX_MAP_POPULATE | LINUX_MAP_STACK |                               \
     LINUX_MAP_FIXED_NOREPLACE | LINUX_MAP_LEGACY_EXTRA | LINUX_MAP_LOCKED)
#define LINUX_MAP_NORESERVE UINT64_C(0x4000)
#define LINUX_MAP_STACK UINT64_C(0x20000)

enum kernel_syscall_status syscall_handle_brk(
    struct kernel_task *caller,
    uint64_t requested,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t program_break;

    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_mm_brk(mm, requested, &program_break) !=
            KERNEL_MM_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = (int64_t)program_break;
    return KERNEL_SYSCALL_STATUS_OK;
}

static uint32_t mm_permissions_from_linux(uint64_t protections)
{
    uint32_t permissions = 0U;

    if ((protections & LINUX_PROT_READ) != 0U) {
        permissions |= KERNEL_MM_READ;
    }
    if ((protections & LINUX_PROT_WRITE) != 0U) {
        permissions |= KERNEL_MM_WRITE;
    }
    if ((protections & LINUX_PROT_EXEC) != 0U) {
        permissions |= KERNEL_MM_EXECUTE;
    }
    return permissions;
}

static int64_t mmap_error(enum kernel_mm_status status)
{
    if (status == KERNEL_MM_STATUS_LOCK_PERMISSION) return -KERNEL_EPERM;
    if (status == KERNEL_MM_STATUS_LOCK_LIMIT) return -KERNEL_EAGAIN;
    if (status == KERNEL_MM_STATUS_NO_MEMORY) {
        return -KERNEL_ENOMEM;
    }
    if (status == KERNEL_MM_STATUS_CONFLICT) {
        return -KERNEL_EEXIST;
    }
    return -KERNEL_EINVAL;
}

enum kernel_syscall_status syscall_handle_mmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    struct kernel_open_file_description *file = 0;
    uint64_t protections = request->arguments[2];
    uint64_t flags = request->arguments[3];
    uint64_t mapped_address;
    uint32_t mm_flags = 0U;
    int64_t linux_result;
    enum kernel_mm_status status;
    enum kernel_task_status task_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((protections & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                         LINUX_PROT_EXEC)) != 0U ||
        ((flags & LINUX_MAP_FIXED) != 0U &&
         (flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) ||
        (request->arguments[5] & BOAROS_PAGE_MASK) != 0U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    uint64_t mapping_type = flags & LINUX_MAP_TYPE_MASK;
    int type_error = 0;
    if (mapping_type != LINUX_MAP_PRIVATE && mapping_type != LINUX_MAP_SHARED &&
        (mapping_type != LINUX_MAP_SHARED_VALIDATE || (flags & LINUX_MAP_ANONYMOUS))) {
        type_error = -KERNEL_EINVAL;
    }
    if (!type_error && mapping_type == LINUX_MAP_SHARED_VALIDATE &&
        (flags & ~LINUX_MAP_KNOWN_FLAGS)) {
        type_error = -KERNEL_EOPNOTSUPP;
    }
    /* No huge-page hstate exists here. Linux rejects unknown huge sizes before
     * do_mmap; accepting ordinary pages would silently change the contract. */
    if (flags & LINUX_MAP_HUGETLB)
        type_error = -KERNEL_EINVAL;
    else if (!request->arguments[1])
        type_error = -KERNEL_EINVAL;
    else if (!type_error && (flags & LINUX_MAP_GROWSDOWN))
        type_error = -KERNEL_EOPNOTSUPP;
    /* Legacy mmap ignores extension flags; SHARED_VALIDATE rejects them. */
    if (mapping_type == LINUX_MAP_SHARED_VALIDATE)
        flags = (flags & ~LINUX_MAP_TYPE_MASK) | LINUX_MAP_SHARED;
    if ((flags & LINUX_MAP_FIXED) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED;
    } else if ((flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED_NOREPLACE;
    }
    if (flags & LINUX_MAP_LOCKED) mm_flags |= KERNEL_MM_MAP_LOCKED;
    if ((flags & LINUX_MAP_TYPE_MASK) == LINUX_MAP_SHARED)
        mm_flags |= KERNEL_MM_MAP_SHARED;
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & LINUX_MAP_ANONYMOUS) != 0U) {
        if (type_error) {
            decoded->value = type_error;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        status = kernel_mm_mmap_anonymous(
            mm,
            request->arguments[0],
            request->arguments[1],
            mm_permissions_from_linux(protections),
            mm_flags,
            &mapped_address);
    } else {
        task_status = kernel_task_files_borrow(caller, &files);
        if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
            decoded->value = -KERNEL_EBADF;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (task_status != KERNEL_TASK_STATUS_OK ||
            kernel_files_pin_data(files,
                             (int64_t)request->arguments[4],
                             &file,
                             &linux_result) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        if (linux_result != 0) {
            decoded->value = linux_result;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (type_error) {
            if (kernel_open_file_release(&file) != KERNEL_OPEN_FILE_STATUS_OK)
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            decoded->value = type_error;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        status = kernel_mm_validate_file_private_mapping(
            mm,
            request->arguments[0],
            request->arguments[1],
            request->arguments[5],
            mm_permissions_from_linux(protections),
            mm_flags);
        if (status != KERNEL_MM_STATUS_OK) {
            if (kernel_open_file_release(&file) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
        } else {
            const struct kernel_vfs_file *source=kernel_open_file_vfs_borrow(file);
            /* mmap检查打开模式；epoll等对象没有read操作并不表示OFD无读权限。 */
            uint32_t access_mode=kernel_open_file_flags(file)&3U;
            int error=0;
            if ((access_mode!=0U && access_mode!=2U) ||
                ((flags & LINUX_MAP_TYPE_MASK)==LINUX_MAP_SHARED &&
                 (protections & LINUX_PROT_WRITE) && access_mode!=1U && access_mode!=2U))
                error=-KERNEL_EACCES;
            else if ((protections & LINUX_PROT_EXEC) && source->mount && source->mount->noexec)
                error=-KERNEL_EPERM;
            else if (kernel_open_file_kind(file)!=KERNEL_OPEN_FILE_KIND_REGULAR)
                error=-KERNEL_ENODEV;
            if (error) {
                if (kernel_open_file_release(&file)!=KERNEL_OPEN_FILE_STATUS_OK)
                    return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
                decoded->value=error;
                return KERNEL_SYSCALL_STATUS_OK;
            }
            status = kernel_mm_mmap_file_private(
                mm,
                &file,
                request->arguments[0],
                request->arguments[1],
                request->arguments[5],
                mm_permissions_from_linux(protections),
                mm_flags,
                &mapped_address);
            if (status != KERNEL_MM_STATUS_OK &&
                kernel_open_file_release(&file) !=
                    KERNEL_OPEN_FILE_STATUS_OK) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NO_MEMORY &&
        status != KERNEL_MM_STATUS_CONFLICT &&
        status != KERNEL_MM_STATUS_LOCK_LIMIT && status != KERNEL_MM_STATUS_LOCK_PERMISSION) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = status == KERNEL_MM_STATUS_OK
                         ? (int64_t)mapped_address
                         : mmap_error(status);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_munmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    enum kernel_mm_status status;

    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_mm_munmap(mm,
                              request->arguments[0],
                              request->arguments[1]);
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NO_MEMORY) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = status == KERNEL_MM_STATUS_OK
                         ? 0
                         : mmap_error(status);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_mremap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t address;
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    int result = kernel_mm_mremap(mm, request->arguments[0],
        request->arguments[1], request->arguments[2], request->arguments[3],
        request->arguments[4], &address);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = result ? result : (int64_t)address;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_mprotect(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t protections = request->arguments[2];
    enum kernel_mm_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((protections & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                         LINUX_PROT_EXEC)) != 0U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_mm_mprotect(mm,
                                request->arguments[0],
                                request->arguments[1],
                                mm_permissions_from_linux(protections));
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NOT_MAPPED &&
        status != KERNEL_MM_STATUS_NO_MEMORY &&
        status != KERNEL_MM_STATUS_ACCESS) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (status == KERNEL_MM_STATUS_ACCESS) {
        decoded->value = -KERNEL_EACCES;
    } else if (status == KERNEL_MM_STATUS_NOT_MAPPED ||
        status == KERNEL_MM_STATUS_NO_MEMORY) {
        decoded->value = -KERNEL_ENOMEM;
    } else {
        decoded->value = status == KERNEL_MM_STATUS_OK
                             ? 0
                             : -KERNEL_EINVAL;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_msync(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_mm_msync(mm, request->arguments[0],
        request->arguments[1], (uint32_t)request->arguments[2]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_madvise(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_mm_madvise(mm, request->arguments[0], request->arguments[1],
        (int32_t)request->arguments[2]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_mlock(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    uint64_t address = request->arguments[0], length = request->arguments[1];
    switch (request->number) {
    case 228: decoded->value = kernel_mm_mlock(mm, address, length, 0); break;
    case 284: decoded->value = kernel_mm_mlock(mm, address, length, (uint32_t)request->arguments[2]); break;
    case 229: decoded->value = kernel_mm_munlock(mm, address, length); break;
    case 230: decoded->value = kernel_mm_mlockall(mm, (uint32_t)address); break;
    case 231: decoded->value = kernel_mm_munlockall(mm); break;
    default: return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_mincore(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t address = request->arguments[0], length = request->arguments[1], destination = request->arguments[2];
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = -KERNEL_EINVAL;
    if (address & BOAROS_PAGE_MASK) return KERNEL_SYSCALL_STATUS_OK;
    decoded->value = -KERNEL_ENOMEM;
    if (kernel_user_range_check(address, length) || length > UINT64_MAX - BOAROS_PAGE_MASK) return KERNEL_SYSCALL_STATUS_OK;
    uint64_t pages = (length + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    decoded->value = -KERNEL_EFAULT;
    if (kernel_user_range_check(destination, pages)) return KERNEL_SYSCALL_STATUS_OK;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    struct kernel_task_io_buffer workspace = {0};
    if (pages && kernel_task_io_buffer_acquire(&workspace, mm->allocator) != KERNEL_TASK_STATUS_OK) {
        decoded->value = -KERNEL_EAGAIN; return KERNEL_SYSCALL_STATUS_OK;
    }
    decoded->value = 0;
    while (pages) {
        unsigned batch = pages > 16 ? 16 : (unsigned)pages, completed = 0;
        int error = kernel_mm_mincore(mm, address, batch, workspace.data, &completed);
        size_t copied = 0;
        if (completed && kernel_copy_to_user(mm, destination, workspace.data, completed, &copied)) error = -KERNEL_EFAULT;
        if (error) { decoded->value = error; break; }
        address += (uint64_t)batch * BOAROS_PAGE_SIZE; destination += batch; pages -= batch;
    }
    if (workspace.allocator) kernel_task_io_buffer_release(&workspace);
    return KERNEL_SYSCALL_STATUS_OK;
}
