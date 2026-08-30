#ifndef BOAROS_ARCH_RISCV_USER_ELF_H
#define BOAROS_ARCH_RISCV_USER_ELF_H

#include <arch/riscv/sv39.h>
#include <kernel/exec_image.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_mm;

#define RISCV_USER_ELF_LIMIT RISCV_SV39_USER_LIMIT
#define RISCV_USER_ELF_STACK_TOP RISCV_USER_ELF_LIMIT
#define RISCV_USER_ELF_STACK_RESERVE_SIZE UINT64_C(0x800000)
#define RISCV_USER_ELF_STACK_RESERVE_BASE \
    (RISCV_USER_ELF_STACK_TOP - RISCV_USER_ELF_STACK_RESERVE_SIZE)
#define RISCV_USER_ELF_STACK_GUARD_BASE \
    (RISCV_USER_ELF_STACK_RESERVE_BASE - BOAROS_PAGE_SIZE)
#define RISCV_USER_ELF_STACK_INITIAL_HEADROOM UINT64_C(0x10000)
#define RISCV_USER_ELF_STACK_IMAGE_LIMIT UINT64_C(0x20000)

enum riscv_user_elf_status {
    RISCV_USER_ELF_STATUS_OK = 0,
    RISCV_USER_ELF_STATUS_INVALID_ARGUMENT,
    RISCV_USER_ELF_STATUS_TRUNCATED,
    RISCV_USER_ELF_STATUS_MALFORMED,
    RISCV_USER_ELF_STATUS_WRONG_ARCH,
    RISCV_USER_ELF_STATUS_UNSUPPORTED,
    RISCV_USER_ELF_STATUS_INVALID_LAYOUT,
    RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE,
    RISCV_USER_ELF_STATUS_NO_MEMORY,
    RISCV_USER_ELF_STATUS_ADDRESS_SPACE,
    RISCV_USER_ELF_STATUS_IO,
    RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED,
};

struct riscv_user_elf_entry {
    uint64_t entry;
    uint64_t stack_pointer;
};

struct riscv_user_elf_request {
    struct kernel_read_source source;
    struct kernel_exec_string executable;
    const struct kernel_exec_string *arguments;
    size_t argument_count;
    const struct kernel_exec_string *environment;
    size_t environment_count;
};

/*
 * Success moves a complete LIVE address space into space.  Ordinary failure
 * leaves both outputs unchanged.  CLEANUP_REQUIRED instead moves a retryable
 * LIVE or CLEANUP space to the caller so ownership is never lost.
 */
enum riscv_user_elf_status riscv_user_elf_load(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry);

/* image_failure preserves the pre-cleanup result when cleanup must retry. */
enum riscv_user_elf_status riscv_user_elf_load_detailed(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry,
    enum riscv_user_elf_status *image_failure);

/*
 * Registers the static-ELF layout for a just-created MM.  The image remains
 * eagerly resident in this stage; ELF pages are anonymous VMAs and the stack
 * VMA covers its full reserve while the guard page remains unmapped.
 *
 * On failure the caller must release mm rather than run it.
 */
enum riscv_user_elf_status riscv_user_elf_register_static_vmas(
    const struct riscv_user_elf_request *request,
    struct kernel_mm *mm,
    struct kernel_heap *heap);

#endif
