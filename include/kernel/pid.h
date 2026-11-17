#ifndef BOAROS_KERNEL_PID_H
#define BOAROS_KERNEL_PID_H

#include <stdint.h>
#include <stddef.h>

#define KERNEL_PID_BITMAP_WORDS(pid_count) \
    (((uint32_t)(pid_count) + 63U) / 64U)

typedef int32_t kernel_pid_t;

enum kernel_pid_status {
    KERNEL_PID_STATUS_OK = 0,
    KERNEL_PID_STATUS_INVALID,
    KERNEL_PID_STATUS_EXHAUSTED,
    KERNEL_PID_STATUS_NOT_ALLOCATED,
    KERNEL_PID_STATUS_STATE,
    KERNEL_PID_STATUS_EXISTS,
    KERNEL_PID_STATUS_NAMESPACE_DEAD,
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

/* Zero requests automatic allocation; a positive number reserves exactly it. */
enum kernel_pid_status kernel_pid_allocate_number(
    struct kernel_pid_allocator *, kernel_pid_t requested, kernel_pid_t *number);

enum kernel_pid_status kernel_pid_release(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t pid);

/* Mutations and borrowed lookups require the caller's identity lock.  Storage
 * is prepared before publication and retired storage is freed after mutation. */
enum kernel_pid_role { KERNEL_PID_TID, KERNEL_PID_TGID,
    KERNEL_PID_PGID, KERNEL_PID_SID, KERNEL_PID_ROLES };
struct kernel_pid;
/* Global task identity owners. Number zero selects the current TGID. */
int kernel_task_identity_get(int32_t number,struct kernel_pid **owner);
/* Exclusive owner slot and stable task TGID role (current or stopped cleanup
 * owner), outside raw locks. NULL task clears; identical identity does not
 * enter the PID domain. */
struct kernel_task;
void kernel_task_identity_replace_tgid(struct kernel_pid **,const struct kernel_task *);
/* Caller-view input; zero selects current TGID. */
int kernel_task_identity_get_user(int32_t number,struct kernel_pid **owner);
/* Caller retains identity; immutable historical mapping, invisible becomes 0. */
kernel_pid_t kernel_task_identity_user_number(const struct kernel_pid *);
void kernel_task_identity_retain(struct kernel_pid *);
void kernel_task_identity_put(struct kernel_pid *);

struct kernel_pid_member {
    struct kernel_pid *identity;
    struct kernel_pid_member *previous, *next;
    void *task;
    enum kernel_pid_role role;
};
#define KERNEL_PID_BUCKETS 256U
/* procfs encodes the existing identity above 30 inode bits. */
#define KERNEL_PID_GENERATION_MAX (UINT64_MAX >> 30U)
struct kernel_pid_registry {
    struct kernel_pid_allocator *numbers;
    struct kernel_pid *buckets[KERNEL_PID_BUCKETS];
    struct kernel_pid *retired;
    uint64_t next_generation;
    /* Managed registries are PID namespace indexes. Zero-initialized legacy
     * roots remain externally owned for standalone allocator consumers. */
    struct kernel_pid_registry *parent, *retired_namespaces, *retired_next;
    uint32_t references, depth;
    unsigned managed, accepting;
    unsigned reboot_signal; /* Final namespace-init wait status, not a machine request. */
    struct kernel_task *shutdown_reaper; /* Weak task owner until final TGID detach. */
    struct kernel_pid_registry *shutdown_next;
    struct kernel_heap *heap; /* Non-NULL only for scheduler-owned heap storage. */
    uint64_t namespace_id;
    struct kernel_pid *child_reaper; /* Weak: the TGID role owns the identity. */
};
#define KERNEL_PID_NAMESPACE_MAX_DEPTH 32U
/* Only non-initial identities carry this tail, indexed by ancestor depth.
 * The active namespace uses kernel_pid's existing number/hash_next fields. */
struct kernel_pid_binding {
    struct kernel_pid_registry *registry;
    struct kernel_pid *hash_next;
    kernel_pid_t number;
};
struct kernel_pid {
    kernel_pid_t number;
    uint32_t references;
    /* 状态与代次共用一字，保持64字节slab；引用仍为完整32位。
     * 状态变更受identity域保护；数字与存储寿命分别结束。 */
    uint64_t generation : 62;
    uint64_t published : 2; /* 0 reserved, 1 indexed, 2 detached, 3 retired */
    struct kernel_pid_registry *registry;
    struct kernel_pid *hash_next;
    struct kernel_pid_member *members[KERNEL_PID_ROLES];
};
_Static_assert(KERNEL_PID_GENERATION_MAX <= (UINT64_MAX >> 2U),
               "legal PID generations must fit beside the state bits");
/* All registry/identity mutations and borrowed reads share the identity lock.
 * Storage is caller-prepared outside that lock; retirement never frees it. */
enum kernel_pid_status kernel_pid_registry_init(struct kernel_pid_registry *,
    struct kernel_pid_allocator *, struct kernel_pid_registry *parent);
void kernel_pid_registry_get(struct kernel_pid_registry *);
void kernel_pid_registry_put(struct kernel_pid_registry *);
void kernel_pid_registry_disable(struct kernel_pid_registry *);
enum kernel_pid_status kernel_pid_registry_admit(const struct kernel_pid_registry *);
struct kernel_pid_registry *kernel_pid_take_retired_namespace(struct kernel_pid_registry *root);
size_t kernel_pid_storage_size(const struct kernel_pid_registry *);
/* Number in an unrelated/descendant view is zero. Detached owners retain
 * historical mappings; callers must not resolve them again by integer. */
kernel_pid_t kernel_pid_number_in(const struct kernel_pid *, const struct kernel_pid_registry *);
/* Exact requests use clone3 leaf-to-root order; omitted entries are automatic.
 * Complete storage includes the ancestor tail reported by storage_size. */
enum kernel_pid_status kernel_pid_reserve_numbers(struct kernel_pid_registry *,
    struct kernel_pid *, size_t storage_size, const kernel_pid_t *, size_t count);

/* reserve owns a number and generation but is invisible to find. Roles may
 * be prepared while private; publish_reserved commits the lookup identity.
 * Last-role detach removes the lookup/number while references retain storage.
 * Final put retires storage and leaves heap disposal to the caller. */
enum kernel_pid_status kernel_pid_reserve(struct kernel_pid_registry *registry,
                                         struct kernel_pid *prepared);
enum kernel_pid_status kernel_pid_reserve_number(struct kernel_pid_registry *,
    struct kernel_pid *, kernel_pid_t requested);
enum kernel_pid_status kernel_pid_publish_admit(const struct kernel_pid *prepared);
enum kernel_pid_status kernel_pid_publish_reserved(struct kernel_pid *prepared);
enum kernel_pid_status kernel_pid_publish(struct kernel_pid_registry *registry,
                                          struct kernel_pid *prepared);
struct kernel_pid *kernel_pid_find(struct kernel_pid_registry *registry,
                                   kernel_pid_t number);
/* Borrow the first published identity with number > after, under the caller's
 * identity lock. Reservations and retired numbers are not enumeration entries;
 * callers filter the returned identity's live roles. */
struct kernel_pid *kernel_pid_next_published(struct kernel_pid_registry *registry,
                                             kernel_pid_t after);
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
