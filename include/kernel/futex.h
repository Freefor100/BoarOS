#ifndef BOAROS_KERNEL_FUTEX_H
#define BOAROS_KERNEL_FUTEX_H

#include <stdint.h>
#include <kernel/scheduler.h>

struct kernel_task;
struct kernel_mm;
struct kernel_memory_object;
struct kernel_vfs_node;

enum kernel_futex_key_kind {
    KERNEL_FUTEX_KEY_PRIVATE = 0,
    KERNEL_FUTEX_KEY_SHARED_MEMORY,
    KERNEL_FUTEX_KEY_SHARED_FILE,
};

/* 排队者持有后备引用；身份、映射与所需用户字pin在同一 MM operation 内取得。 */
struct kernel_futex_key {
    uint64_t identity;
    uint64_t offset;
    union {
        struct kernel_memory_object *shared_object;
        struct kernel_vfs_node *file_node;
    };
    enum kernel_futex_key_kind kind;
};

int64_t kernel_futex(struct kernel_task *task, uint64_t address,
                     uint32_t operation, uint32_t value,
                     uint64_t timeout_or_count, uint64_t address2,
                     uint32_t bitset,
                     enum kernel_scheduler_status *status);
int64_t kernel_futex_waitv(struct kernel_task *,uint64_t,unsigned,unsigned,uint64_t,int,enum kernel_scheduler_status *);
int64_t kernel_futex_restart_timed(
    struct kernel_task *task, uint64_t address, uint32_t operation,
    uint32_t value, uint64_t deadline_ns, uint32_t bitset,
    enum kernel_scheduler_status *status);
/* Drop one live MM user; clear/wake the TID only while another user remains. */
void kernel_futex_release_mm(struct kernel_task *task);
void kernel_futex_release_robust(struct kernel_task *task, int32_t owner_tid);
void kernel_futex_pi_abort(struct kernel_task *task);
void kernel_futex_abort(struct kernel_task *task);
void kernel_futex_set_robust_list(struct kernel_task *task, uint64_t head);
uint64_t kernel_futex_get_robust_list(const struct kernel_task *task);

#endif
