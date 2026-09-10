#ifndef BOAROS_KERNEL_EXEC_IMAGE_H
#define BOAROS_KERNEL_EXEC_IMAGE_H

#include <kernel/mm.h>
#include <kernel/elf64_source.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;

struct kernel_exec_string {
    const char *bytes;
    size_t length;
};

struct kernel_exec_image_request {
    struct kernel_elf64_source *executable_source;
    struct kernel_elf64_source *interpreter_source;
    struct kernel_exec_string executable;
    const struct kernel_exec_string *arguments;
    size_t argument_count;
    const struct kernel_exec_string *environment;
    size_t environment_count;
};

struct kernel_exec_image {
    struct kernel_mm mm;
    void *arch_private;
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
