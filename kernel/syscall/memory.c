#include "private.h"

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/task.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_PROT_READ UINT64_C(0x1)
#define LINUX_PROT_WRITE UINT64_C(0x2)
#define LINUX_PROT_EXEC UINT64_C(0x4)
#define LINUX_MAP_PRIVATE UINT64_C(0x2)
#define LINUX_MAP_TYPE_MASK UINT64_C(0x3)
#define LINUX_MAP_FIXED UINT64_C(0x10)
#define LINUX_MAP_ANONYMOUS UINT64_C(0x20)
#define LINUX_MAP_DENYWRITE UINT64_C(0x800)
#define LINUX_MAP_POPULATE UINT64_C(0x8000)
#define LINUX_MAP_EXECUTABLE UINT64_C(0x1000)
#define LINUX_MAP_FIXED_NOREPLACE UINT64_C(0x100000)
#define LINUX_MAP_KNOWN_FLAGS                                             \
    (LINUX_MAP_TYPE_MASK | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |       \
     LINUX_MAP_DENYWRITE | LINUX_MAP_EXECUTABLE | LINUX_MAP_NORESERVE |  \
     LINUX_MAP_POPULATE | LINUX_MAP_STACK |                               \
     LINUX_MAP_FIXED_NOREPLACE)
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
        (flags & ~LINUX_MAP_KNOWN_FLAGS) != 0U ||
        ((flags & LINUX_MAP_FIXED) != 0U &&
         (flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) ||
        (request->arguments[5] & BOAROS_PAGE_MASK) != 0U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((flags & LINUX_MAP_TYPE_MASK) != LINUX_MAP_PRIVATE ||
        (flags & LINUX_MAP_POPULATE) != 0U) {
        decoded->value = -KERNEL_ENOTSUP;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((flags & LINUX_MAP_FIXED) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED;
    } else if ((flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED_NOREPLACE;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & LINUX_MAP_ANONYMOUS) != 0U) {
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
            kernel_files_pin(files,
                             (int64_t)request->arguments[4],
                             &file,
                             &linux_result) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        if (linux_result != 0) {
            decoded->value = linux_result;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (kernel_open_file_kind(file) !=
            KERNEL_OPEN_FILE_KIND_REGULAR) {
            /* Only regular files back the private-mapping page-cache path. */
            if (kernel_open_file_release(&file) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
            decoded->value = -KERNEL_ENODEV;
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
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NO_MEMORY &&
        status != KERNEL_MM_STATUS_CONFLICT) {
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
        status != KERNEL_MM_STATUS_NO_MEMORY) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (status == KERNEL_MM_STATUS_NOT_MAPPED ||
        status == KERNEL_MM_STATUS_NO_MEMORY) {
        decoded->value = -KERNEL_ENOMEM;
    } else {
        decoded->value = status == KERNEL_MM_STATUS_OK
                             ? 0
                             : -KERNEL_EINVAL;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
