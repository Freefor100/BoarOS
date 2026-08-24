#ifndef BOAROS_ARCH_RISCV_USER_ELF_H
#define BOAROS_ARCH_RISCV_USER_ELF_H

#include <arch/riscv/sv39.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_USER_ELF_LIMIT (UINT64_C(1) << 38U)
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
    RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED,
};

struct riscv_user_elf_entry {
    uint64_t entry;
    uint64_t stack_pointer;
};

struct riscv_user_elf_string {
    const char *bytes;
    size_t length;
};

struct riscv_user_elf_request {
    const void *image;
    size_t image_size;
    const struct riscv_user_elf_string *arguments;
    size_t argument_count;
    const struct riscv_user_elf_string *environment;
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

#endif
