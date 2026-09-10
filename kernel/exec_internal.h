#ifndef BOAROS_KERNEL_EXEC_INTERNAL_H
#define BOAROS_KERNEL_EXEC_INTERNAL_H

#include <kernel/exec_image.h>
#include <kernel/elf64_source.h>
#include <kernel/mm.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_task;

enum kernel_exec_transaction_state {
    KERNEL_EXEC_TRANSACTION_PREPARING = 1,
    KERNEL_EXEC_TRANSACTION_PREPARED,
    KERNEL_EXEC_TRANSACTION_COMMITTED_CLEANUP,
    KERNEL_EXEC_TRANSACTION_EMPTY_CLEANUP,
};

struct kernel_exec_transaction {
    struct kernel_heap *heap;
    char *original_path;
    char *resolved_path;
    char *string_bytes;
    size_t string_size;
    size_t string_capacity;
    struct kernel_exec_string *arguments;
    size_t argument_count;
    size_t argument_capacity;
    struct kernel_exec_string *environment;
    size_t environment_count;
    size_t environment_capacity;
    /* Until source creation succeeds, these are the retryable OFD owners. */
    struct kernel_open_file_description *executable_file;
    struct kernel_open_file_description *interpreter_file;
    struct kernel_elf64_source *executable_source;
    struct kernel_elf64_source *interpreter_source;
    struct kernel_exec_image image;
    struct kernel_mm retired_mm;
    enum kernel_exec_transaction_state state;
};

enum kernel_exec_status kernel_exec_transaction_cleanup(
    struct kernel_exec_transaction *transaction);

static inline int kernel_exec_transaction_valid(
    const struct kernel_exec_transaction *transaction,
    const struct kernel_heap *heap)
{
    if (transaction == 0) {
        return 1;
    }
    if (heap == 0 || transaction->heap != heap ||
        transaction->state < KERNEL_EXEC_TRANSACTION_PREPARING ||
        transaction->state > KERNEL_EXEC_TRANSACTION_EMPTY_CLEANUP ||
        transaction->argument_count > transaction->argument_capacity ||
        transaction->environment_count >
            transaction->environment_capacity ||
        transaction->string_size > transaction->string_capacity) {
        return 0;
    }
    return 1;
}

enum kernel_exec_status kernel_task_exec_attach(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction);

enum kernel_exec_status kernel_task_exec_borrow(
    struct kernel_task *task,
    struct kernel_exec_transaction **transaction);

enum kernel_exec_status kernel_task_exec_finish(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction);

#endif
