#ifndef BOAROS_KERNEL_PID_H
#define BOAROS_KERNEL_PID_H

#include <stdint.h>

#define KERNEL_PID_BITMAP_WORDS(pid_count) \
    (((uint32_t)(pid_count) + 63U) / 64U)

typedef int32_t kernel_pid_t;

enum kernel_pid_status {
    KERNEL_PID_STATUS_OK = 0,
    KERNEL_PID_STATUS_INVALID,
    KERNEL_PID_STATUS_EXHAUSTED,
    KERNEL_PID_STATUS_NOT_ALLOCATED,
    KERNEL_PID_STATUS_STATE,
};

struct kernel_pid_allocator {
    uint64_t *bitmap;
    uint32_t limit;
    uint32_t cursor;
    uint32_t allocated;
    uint32_t initialized;
};

enum kernel_pid_status kernel_pid_allocator_init(
    struct kernel_pid_allocator *allocator,
    uint64_t *bitmap,
    uint32_t limit);

enum kernel_pid_status kernel_pid_allocate(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t *pid);

enum kernel_pid_status kernel_pid_release(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t pid);

/* Mutations and borrowed lookups require the caller's identity lock.  Storage
 * is prepared before publication and retired storage is freed after mutation. */
enum kernel_pid_role { KERNEL_PID_TID, KERNEL_PID_TGID,
    KERNEL_PID_PGID, KERNEL_PID_SID, KERNEL_PID_ROLES };
struct kernel_pid;
struct kernel_pid_member {
    struct kernel_pid *identity;
    struct kernel_pid_member *previous, *next;
    void *task;
    enum kernel_pid_role role;
};
#define KERNEL_PID_BUCKETS 256U
struct kernel_pid_registry {
    struct kernel_pid_allocator *numbers;
    struct kernel_pid *buckets[KERNEL_PID_BUCKETS];
    struct kernel_pid *retired;
    uint64_t next_generation;
};
struct kernel_pid {
    kernel_pid_t number;
    uint32_t references;
    uint64_t generation;
    struct kernel_pid_registry *registry;
    struct kernel_pid *hash_next;
    struct kernel_pid_member *members[KERNEL_PID_ROLES];
};
enum kernel_pid_status kernel_pid_publish(struct kernel_pid_registry *registry,
                                          struct kernel_pid *prepared);
struct kernel_pid *kernel_pid_find(struct kernel_pid_registry *registry,
                                   kernel_pid_t number);
void kernel_pid_get(struct kernel_pid *identity);
void kernel_pid_put(struct kernel_pid *identity);
void kernel_pid_attach(struct kernel_pid_member *member,
                       struct kernel_pid *identity, enum kernel_pid_role role,
                       void *task);
void kernel_pid_detach(struct kernel_pid_member *member);
void kernel_pid_transfer(struct kernel_pid_member *old,
                         struct kernel_pid_member *replacement, void *task);
struct kernel_pid *kernel_pid_take_retired(struct kernel_pid_registry *registry);

#endif
