#ifndef BOAROS_TEST_LWEXT4_OPERATION_H
#define BOAROS_TEST_LWEXT4_OPERATION_H

#include <ext4_operation.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <kernel/sync.h>

/* Low-level journal fixtures explicitly lend their transaction to the real
 * per-thread I/O context, matching the filesystem entrypoint ownership. */
static inline void test_operation_bind(struct ext4_fs *fs, struct jbd_trans *trans)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (!trans) {
        if (!context->ext4_operations || context->ext4_operations->fs != fs) __builtin_trap();
        context->ext4_operations = context->ext4_operations->next;
        return;
    }
    if (ext4_current_operation(fs)) __builtin_trap();
    trans->operation = (struct ext4_operation){.fs = fs, .trans = trans,
        .depth = 1,
        .next = context->ext4_operations};
    context->ext4_operations = &trans->operation;
}

#endif
