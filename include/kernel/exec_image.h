#ifndef BOAROS_KERNEL_EXEC_IMAGE_H
#define BOAROS_KERNEL_EXEC_IMAGE_H

#include <kernel/mm.h>
#include <kernel/rlimit.h>
#include <kernel/page.h>
#include <kernel/elf64_source.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_EXEC_ARGUMENT_FLOOR 131072U
#define KERNEL_EXEC_STRING_PAGES 32U
#define KERNEL_EXEC_STRING_LIMIT (KERNEL_EXEC_STRING_PAGES * BOAROS_PAGE_SIZE)

/* Linux's fixed _STK_LIM ceiling is independent of a raised soft limit. */
static inline size_t kernel_exec_argument_limit(uint64_t stack_limit)
{
    uint64_t limit = stack_limit / 4U;
    uint64_t ceiling = KERNEL_RLIMIT_STACK_DEFAULT / 4U * 3U;
    if (limit > ceiling) limit = ceiling;
    if (limit < KERNEL_EXEC_ARGUMENT_FLOOR) limit = KERNEL_EXEC_ARGUMENT_FLOOR;
    return (size_t)limit;
}

struct kernel_heap;

struct kernel_exec_string {
    const char *bytes;
    size_t length;
};

struct kernel_exec_image_request {
    struct kernel_elf64_source *executable_source;
    struct kernel_elf64_source *interpreter_source;
    uint64_t stack_limit;
    uint32_t stack_limit_valid;
    /* Captured string budget, including execfn, after reserving the original
     * argv/envp pointers. Script expansion does not reserve pointers again.
     * Zero lets an initial kernel-created image derive its own budget. */
    size_t string_limit;
    struct kernel_memory_limits memory_limits;
    uint32_t memory_limits_valid;
    struct kernel_exec_string executable;
    const struct kernel_exec_string *arguments;
    size_t argument_count;
    const struct kernel_exec_string *environment;
    size_t environment_count;
    uint32_t uid, euid, gid, egid, secureexec;
    uint32_t personality;
};

struct kernel_exec_image {
    struct kernel_mm mm;
    uintptr_t entry;
    uintptr_t stack_pointer;
    uintptr_t thread_pointer;
};

enum kernel_exec_image_status {
    KERNEL_EXEC_IMAGE_STATUS_OK = 0,
    KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR,
    KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED,
    KERNEL_EXEC_IMAGE_STATUS_STATE,
};

/* Implemented once by the architecture selected at build time. */
enum kernel_exec_image_status kernel_exec_image_prepare(
    const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,
    struct kernel_exec_image *image,
    int64_t *linux_result);

enum kernel_exec_image_status kernel_exec_image_cleanup(
    struct kernel_heap *heap,
    struct kernel_exec_image *image);

#endif
