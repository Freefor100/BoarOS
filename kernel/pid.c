#include <kernel/pid.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_PID_ALLOCATOR_INITIALIZED UINT32_C(0x50494441)

static int valid_allocator(const struct kernel_pid_allocator *allocator)
{
    return allocator != 0 &&
           allocator->initialized == KERNEL_PID_ALLOCATOR_INITIALIZED &&
           allocator->bitmap != 0 && allocator->limit != 0U &&
           allocator->limit <= (uint32_t)INT32_MAX &&
           allocator->cursor < allocator->limit &&
           allocator->allocated <= allocator->limit;
}

static uint64_t pid_mask(uint32_t index)
{
    return UINT64_C(1) << (index & 63U);
}

enum kernel_pid_status kernel_pid_allocator_init(
    struct kernel_pid_allocator *allocator,
    uint64_t *bitmap,
    uint32_t limit)
{
    uint32_t index;

    if (allocator == 0 || bitmap == 0 || limit == 0U ||
        limit > (uint32_t)INT32_MAX) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (allocator->initialized != 0U || allocator->bitmap != 0 ||
        allocator->limit != 0U || allocator->cursor != 0U ||
        allocator->allocated != 0U) {
        return KERNEL_PID_STATUS_STATE;
    }
    for (index = 0U; index < KERNEL_PID_BITMAP_WORDS(limit); index++) {
        bitmap[index] = 0U;
    }
    allocator->bitmap = bitmap;
    allocator->limit = limit;
    allocator->cursor = 0U;
    allocator->allocated = 0U;
    allocator->initialized = KERNEL_PID_ALLOCATOR_INITIALIZED;
    return KERNEL_PID_STATUS_OK;
}

enum kernel_pid_status kernel_pid_allocate(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t *pid)
{
    uint32_t scanned;

    if (pid == 0) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (!valid_allocator(allocator)) {
        return KERNEL_PID_STATUS_STATE;
    }
    if (allocator->allocated == allocator->limit) {
        return KERNEL_PID_STATUS_EXHAUSTED;
    }
    for (scanned = 0U; scanned < allocator->limit; scanned++) {
        uint32_t index = allocator->cursor + scanned;
        uint32_t word;
        uint64_t mask;

        if (index >= allocator->limit) {
            index -= allocator->limit;
        }
        word = index >> 6U;
        mask = pid_mask(index);
        if ((allocator->bitmap[word] & mask) != 0U) {
            continue;
        }
        allocator->bitmap[word] |= mask;
        allocator->allocated++;
        allocator->cursor = index + 1U;
        if (allocator->cursor == allocator->limit) {
            allocator->cursor = 0U;
        }
        *pid = (kernel_pid_t)(index + 1U);
        return KERNEL_PID_STATUS_OK;
    }
    return KERNEL_PID_STATUS_STATE;
}

enum kernel_pid_status kernel_pid_release(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t pid)
{
    uint32_t index;
    uint32_t word;
    uint64_t mask;

    if (pid <= 0) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (!valid_allocator(allocator)) {
        return KERNEL_PID_STATUS_STATE;
    }
    index = (uint32_t)pid - 1U;
    if (index >= allocator->limit) {
        return KERNEL_PID_STATUS_INVALID;
    }
    word = index >> 6U;
    mask = pid_mask(index);
    if ((allocator->bitmap[word] & mask) == 0U) {
        return KERNEL_PID_STATUS_NOT_ALLOCATED;
    }
    if (allocator->allocated == 0U) {
        return KERNEL_PID_STATUS_STATE;
    }
    allocator->bitmap[word] &= ~mask;
    allocator->allocated--;
    if (index < allocator->cursor) {
        allocator->cursor = index;
    }
    return KERNEL_PID_STATUS_OK;
}

struct kernel_pid *kernel_pid_find(struct kernel_pid_registry *registry,
                                   kernel_pid_t number)
{
    if (!registry || number <= 0) return 0;
    for (struct kernel_pid *id = registry->buckets[(uint32_t)number % KERNEL_PID_BUCKETS];
         id; id = id->hash_next)
        if (id->number == number) return id;
    return 0;
}

enum kernel_pid_status kernel_pid_publish(struct kernel_pid_registry *registry,
                                          struct kernel_pid *prepared)
{
    if (!registry || !prepared || prepared->registry || !registry->next_generation)
        return KERNEL_PID_STATUS_STATE;
    if (registry->next_generation > (UINT64_MAX >> 30U))
        return KERNEL_PID_STATUS_EXHAUSTED;
    kernel_pid_t number;
    enum kernel_pid_status status = kernel_pid_allocate(registry->numbers, &number);
    if (status != KERNEL_PID_STATUS_OK) return status;
    prepared->number = number;
    prepared->generation = registry->next_generation++;
    prepared->references = 1;
    prepared->registry = registry;
    unsigned bucket = (uint32_t)number % KERNEL_PID_BUCKETS;
    prepared->hash_next = registry->buckets[bucket];
    registry->buckets[bucket] = prepared;
    return KERNEL_PID_STATUS_OK;
}

void kernel_pid_get(struct kernel_pid *identity)
{
    if (!identity || !identity->registry || !identity->references ||
        identity->references == UINT32_MAX) __builtin_trap();
    identity->references++;
}

void kernel_pid_put(struct kernel_pid *identity)
{
    if (!identity || !identity->registry || !identity->references) __builtin_trap();
    if (--identity->references) return;
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        if (identity->members[role]) __builtin_trap();
    struct kernel_pid_registry *registry = identity->registry;
    struct kernel_pid **link = &registry->buckets[(uint32_t)identity->number % KERNEL_PID_BUCKETS];
    while (*link && *link != identity) link = &(*link)->hash_next;
    if (!*link) __builtin_trap();
    *link = identity->hash_next;
    if (kernel_pid_release(registry->numbers, identity->number) != KERNEL_PID_STATUS_OK)
        __builtin_trap();
    identity->registry = 0;
    identity->hash_next = registry->retired;
    registry->retired = identity;
}

void kernel_pid_attach(struct kernel_pid_member *member,
                       struct kernel_pid *identity, enum kernel_pid_role role,
                       void *task)
{
    if (!member || member->identity || !task || (unsigned)role >= KERNEL_PID_ROLES)
        __builtin_trap();
    kernel_pid_get(identity);
    member->identity = identity;
    member->role = role;
    member->task = task;
    member->previous = 0;
    member->next = identity->members[role];
    if (member->next) member->next->previous = member;
    identity->members[role] = member;
}

void kernel_pid_detach(struct kernel_pid_member *member)
{
    if (!member || !member->identity) __builtin_trap();
    struct kernel_pid *identity = member->identity;
    if (member->previous) member->previous->next = member->next;
    else identity->members[member->role] = member->next;
    if (member->next) member->next->previous = member->previous;
    member->identity = 0;
    member->task = 0;
    member->next = member->previous = 0;
    kernel_pid_put(identity);
}

void kernel_pid_transfer(struct kernel_pid_member *old,
                         struct kernel_pid_member *replacement, void *task)
{
    if (!old || !old->identity || !replacement || replacement->identity || !task)
        __builtin_trap();
    *replacement = *old;
    replacement->task = task;
    if (replacement->previous) replacement->previous->next = replacement;
    else replacement->identity->members[replacement->role] = replacement;
    if (replacement->next) replacement->next->previous = replacement;
    old->identity = 0;
    old->task = 0;
    old->next = old->previous = 0;
}

struct kernel_pid *kernel_pid_take_retired(struct kernel_pid_registry *registry)
{
    struct kernel_pid *retired = registry->retired;
    if (retired) {
        registry->retired = retired->hash_next;
        retired->hash_next = 0;
    }
    return retired;
}
