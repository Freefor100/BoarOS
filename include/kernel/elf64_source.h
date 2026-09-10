#ifndef BOAROS_KERNEL_ELF64_SOURCE_H
#define BOAROS_KERNEL_ELF64_SOURCE_H

#include <kernel/elf64.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_open_file_description;
struct physical_page_allocator;

enum kernel_elf64_source_status {
    KERNEL_ELF64_SOURCE_STATUS_OK = 0,
    KERNEL_ELF64_SOURCE_STATUS_INVALID_ARGUMENT,
    KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY,
    KERNEL_ELF64_SOURCE_STATUS_TRUNCATED,
    KERNEL_ELF64_SOURCE_STATUS_MALFORMED,
    KERNEL_ELF64_SOURCE_STATUS_UNSUPPORTED,
    KERNEL_ELF64_SOURCE_STATUS_WRONG_ARCH,
    KERNEL_ELF64_SOURCE_STATUS_IO,
    KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED,
    KERNEL_ELF64_SOURCE_STATUS_STATE,
};

enum kernel_elf64_source_run_kind {
    KERNEL_ELF64_SOURCE_RUN_FILE = 0,
    KERNEL_ELF64_SOURCE_RUN_ZERO,
    KERNEL_ELF64_SOURCE_RUN_COMPOSITE,
};

struct kernel_elf64_source_run {
    uint64_t start;
    uint64_t end;
    uint64_t file_offset;
    uint32_t flags;
    enum kernel_elf64_source_run_kind kind;
};

struct kernel_elf64_source;

/* Successful creation consumes *file; all failure paths leave it owned. */
enum kernel_elf64_source_status kernel_elf64_source_create(
    struct kernel_heap *heap,
    struct kernel_open_file_description **file,
    uint64_t page_size,
    uint16_t machine,
    struct kernel_elf64_source **source);

/* Success consumes one source reference; cleanup failure leaves it retryable. */
enum kernel_elf64_source_status kernel_elf64_source_release(
    struct kernel_elf64_source **source);

enum kernel_elf64_source_status kernel_elf64_source_acquire(
    struct kernel_elf64_source *source);

const struct kernel_elf64_header *kernel_elf64_source_header(
    const struct kernel_elf64_source *source);

uint16_t kernel_elf64_source_program_header_count(
    const struct kernel_elf64_source *source);

const struct kernel_elf64_program_header *kernel_elf64_source_program_header(
    const struct kernel_elf64_source *source,
    uint16_t index);

const char *kernel_elf64_source_interpreter(
    const struct kernel_elf64_source *source,
    size_t *length);

uint32_t kernel_elf64_source_run_count(
    const struct kernel_elf64_source *source);

const struct kernel_elf64_source_run *kernel_elf64_source_run_at(
    const struct kernel_elf64_source *source,
    uint32_t index);

/* Maps one source-relative page.  shared is nonzero for a page-cache ref. */
enum kernel_elf64_source_status kernel_elf64_source_page(
    struct kernel_elf64_source *source,
    struct physical_page_allocator *allocator,
    uint64_t virtual_offset,
    uint64_t *physical_address,
    int *shared);

uint64_t kernel_elf64_source_file_size(
    const struct kernel_elf64_source *source);

struct kernel_open_file_description *kernel_elf64_source_file(
    const struct kernel_elf64_source *source);

#endif
