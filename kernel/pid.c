#include <kernel/pid.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

enum kernel_pid_status kernel_pid_allocate_number(
    struct kernel_pid_allocator *allocator, kernel_pid_t requested, kernel_pid_t *number)
{
    if (!number || requested < 0) return KERNEL_PID_STATUS_INVALID;
    if (!requested) return kernel_pid_allocate(allocator, number);
    if (!valid_allocator(allocator)) return KERNEL_PID_STATUS_STATE;
    if ((uint32_t)requested > allocator->limit) return KERNEL_PID_STATUS_INVALID;
    uint32_t index = (uint32_t)requested - 1U;
    if (allocator->bitmap[index >> 6U] & pid_mask(index)) return KERNEL_PID_STATUS_EXISTS;
    /* Exact and automatic allocation share unpublished reservations. */
    allocator->bitmap[index >> 6U] |= pid_mask(index);
    allocator->allocated++;
    *number = requested;
    return KERNEL_PID_STATUS_OK;
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

static struct kernel_pid_binding *ancestor_binding(struct kernel_pid *id, unsigned depth)
{ return &((struct kernel_pid_binding *)(id + 1))[depth]; }

static struct kernel_pid_registry *registry_root(struct kernel_pid_registry *registry)
{
    while (registry->parent) registry = registry->parent;
    return registry;
}

size_t kernel_pid_storage_size(const struct kernel_pid_registry *registry)
{
    return registry ? sizeof(struct kernel_pid) + registry->depth * sizeof(struct kernel_pid_binding) : 0;
}

enum kernel_pid_status kernel_pid_registry_init(struct kernel_pid_registry *registry,
    struct kernel_pid_allocator *numbers, struct kernel_pid_registry *parent)
{
    if (!registry || !valid_allocator(numbers) || numbers->allocated) return KERNEL_PID_STATUS_INVALID;
    if (registry->numbers || registry->managed || registry->references || registry->next_generation ||
        registry->parent || registry->retired || registry->retired_namespaces || registry->retired_next ||
        registry->depth || registry->accepting || registry->child_reaper || registry->shutdown_reaper ||
        registry->shutdown_next || registry->heap || registry->namespace_id) return KERNEL_PID_STATUS_STATE;
    for (unsigned i = 0; i < KERNEL_PID_BUCKETS; i++)
        if (registry->buckets[i]) return KERNEL_PID_STATUS_STATE;
    for (struct kernel_pid_registry *r = parent; r; r = r->parent)
        if (!r->managed || !r->references || !r->accepting) return KERNEL_PID_STATUS_STATE;
    if (parent && parent->depth == KERNEL_PID_NAMESPACE_MAX_DEPTH) return KERNEL_PID_STATUS_EXHAUSTED;
    registry->numbers = numbers;
    registry->next_generation = 1;
    registry->parent = parent;
    registry->depth = parent ? parent->depth + 1 : 0;
    registry->references = registry->managed = registry->accepting = 1;
    if (parent) kernel_pid_registry_get(parent);
    return KERNEL_PID_STATUS_OK;
}

void kernel_pid_registry_get(struct kernel_pid_registry *registry)
{
    if (!registry || !registry->managed || !registry->references || registry->references == UINT32_MAX)
        __builtin_trap();
    registry->references++;
}

void kernel_pid_registry_put(struct kernel_pid_registry *registry)
{
    struct kernel_pid_registry *root = registry_root(registry);
    while (registry) {
        if (!registry->managed || !registry->references) __builtin_trap();
        if (--registry->references) return;
        if (registry->numbers->allocated || registry->shutdown_reaper || registry->shutdown_next)
            __builtin_trap();
        registry->accepting = 0;
        struct kernel_pid_registry *parent = registry->parent;
        if (parent) {
            registry->retired_next = root->retired_namespaces;
            root->retired_namespaces = registry;
        }
        registry = parent;
    }
}

void kernel_pid_registry_disable(struct kernel_pid_registry *registry)
{
    if (!registry || !registry->managed || !registry->references) __builtin_trap();
    registry->accepting = 0;
}

enum kernel_pid_status kernel_pid_registry_admit(const struct kernel_pid_registry *registry)
{
    if (!registry) return KERNEL_PID_STATUS_STATE;
    for (const struct kernel_pid_registry *r = registry; r; r = r->parent) {
        if (r->managed && !r->references) return KERNEL_PID_STATUS_STATE;
        if (r->managed && !r->accepting) return KERNEL_PID_STATUS_NAMESPACE_DEAD;
    }
    return KERNEL_PID_STATUS_OK;
}

struct kernel_pid_registry *kernel_pid_take_retired_namespace(struct kernel_pid_registry *root)
{
    if (!root || root->parent) __builtin_trap();
    struct kernel_pid_registry *retired = root->retired_namespaces;
    if (retired) { root->retired_namespaces = retired->retired_next; retired->retired_next = NULL; }
    return retired;
}

kernel_pid_t kernel_pid_number_in(const struct kernel_pid *id, const struct kernel_pid_registry *registry)
{
    if (!id || !id->registry || !registry || registry->depth > id->registry->depth) return 0;
    if (registry == id->registry) return id->number;
    if (registry->depth == id->registry->depth) return 0;
    const struct kernel_pid_binding *binding = &((const struct kernel_pid_binding *)(id + 1))[registry->depth];
    return binding->registry == registry ? binding->number : 0;
}

static struct kernel_pid **identity_next(struct kernel_pid *id, const struct kernel_pid_registry *registry)
{
    if (registry == id->registry) return &id->hash_next;
    if (registry->depth >= id->registry->depth) __builtin_trap();
    struct kernel_pid_binding *binding = ancestor_binding(id, registry->depth);
    if (binding->registry != registry) __builtin_trap();
    return &binding->hash_next;
}

struct kernel_pid *kernel_pid_find(struct kernel_pid_registry *registry,
                                   kernel_pid_t number)
{
    if (!registry || number <= 0) return 0;
    for (struct kernel_pid *id = registry->buckets[(uint32_t)number % KERNEL_PID_BUCKETS];
         id; id = *identity_next(id, registry))
        if (kernel_pid_number_in(id, registry) == number) return id;
    return 0;
}

struct kernel_pid *kernel_pid_next_published(struct kernel_pid_registry *registry,
                                             kernel_pid_t after)
{
    if (!registry || !valid_allocator(registry->numbers) || after < 0 ||
        (uint32_t)after >= registry->numbers->limit) return 0;
    struct kernel_pid_allocator *numbers = registry->numbers;
    uint32_t word = (uint32_t)after >> 6U;
    uint64_t bits = numbers->bitmap[word] & (UINT64_MAX << ((uint32_t)after & 63U));
    for (;;) {
        while (bits) {
            uint32_t bit = 0;
            uint64_t low = bits;
            /* 非零字至多移位63次；基础RV ISA不能依赖libgcc的ctz实现。 */
            while ((low & UINT64_C(1)) == 0U) {
                low >>= 1U;
                bit++;
            }
            uint32_t index = (word << 6U) + bit;
            if (index >= numbers->limit) return 0;
            struct kernel_pid *id = kernel_pid_find(registry, (kernel_pid_t)(index + 1U));
            if (id) return id;
            /* 已分配的数字也可能还属于未发布的fork/clone预留。 */
            bits &= bits - 1U;
        }
        if (++word >= KERNEL_PID_BITMAP_WORDS(numbers->limit)) return 0;
        bits = numbers->bitmap[word];
    }
}

static enum kernel_pid_status reserve_numbers(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared, size_t storage_size, const kernel_pid_t *requests, size_t count,
    int task_identity, int may_set_tid)
{
    if (!registry || !prepared || storage_size < kernel_pid_storage_size(registry) ||
        count > registry->depth + 1U || (count && !requests)) return KERNEL_PID_STATUS_INVALID;
    if (prepared->registry || prepared->published || prepared->references) return KERNEL_PID_STATUS_STATE;
    struct kernel_pid_registry *root = registry_root(registry);

    if (!root->next_generation) return KERNEL_PID_STATUS_STATE;
    if (root->next_generation > KERNEL_PID_GENERATION_MAX) return KERNEL_PID_STATUS_EXHAUSTED;
    const struct kernel_pid_registry *level = registry;
    for (size_t i = 0; i < count; i++, level = level->parent) {
        if (requests[i] <= 0 || (uint32_t)requests[i] > level->numbers->limit)
            return KERNEL_PID_STATUS_INVALID;
        /* Today all levels have the initial user namespace as owner. */
        if (task_identity && !may_set_tid) return KERNEL_PID_STATUS_PERMISSION;
    }
    size_t reserved = 0;
    enum kernel_pid_status status = KERNEL_PID_STATUS_OK;
    prepared->registry = registry;
    for (struct kernel_pid_registry *r = registry; r; r = r->parent) {
        kernel_pid_t number;
        status = kernel_pid_allocate_number(r->numbers, reserved < count ? requests[reserved] : 0, &number);
        if (status) break;
        if (r == registry) prepared->number = number;
        else {
            struct kernel_pid_binding *binding = ancestor_binding(prepared, r->depth);
            *binding = (struct kernel_pid_binding){.registry = r, .number = number};
        }
        reserved++;
        if (task_identity && r->accepting && !r->child_reaper && number != 1) {
            status = KERNEL_PID_STATUS_INVALID;
            break;
        }
    }
    if (!status) status = kernel_pid_registry_admit(registry);
    if (status) {
        struct kernel_pid_registry *r = registry;
        for (size_t i = 0; i < reserved; i++, r = r->parent)
            if (kernel_pid_release(r->numbers, kernel_pid_number_in(prepared, r))) __builtin_trap();
        memset(prepared, 0, kernel_pid_storage_size(registry));
        return status;
    }
    prepared->generation = root->next_generation++;
    prepared->references = 1;
    if (registry->managed) kernel_pid_registry_get(registry);
    return KERNEL_PID_STATUS_OK;
}

enum kernel_pid_status kernel_pid_reserve_task_numbers(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared, size_t bytes, const kernel_pid_t *requests, size_t count, int may_set_tid)
{ return reserve_numbers(registry, prepared, bytes, requests, count, 1, may_set_tid); }

enum kernel_pid_status kernel_pid_reserve_numbers(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared, size_t bytes, const kernel_pid_t *requests, size_t count)
{ return reserve_numbers(registry, prepared, bytes, requests, count, 0, 0); }

enum kernel_pid_status kernel_pid_reserve_number(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared, kernel_pid_t requested)
{
    if (requested < 0) return KERNEL_PID_STATUS_INVALID;
    return kernel_pid_reserve_numbers(registry, prepared, sizeof(*prepared),
                                      requested ? &requested : NULL, requested ? 1 : 0);
}

enum kernel_pid_status kernel_pid_reserve(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared)
{ return kernel_pid_reserve_number(registry, prepared, 0); }

enum kernel_pid_status kernel_pid_publish_admit(const struct kernel_pid *prepared)
{
    if (!prepared || !prepared->registry || !prepared->references || prepared->published)
        return KERNEL_PID_STATUS_STATE;
    return kernel_pid_registry_admit(prepared->registry);
}

enum kernel_pid_status kernel_pid_publish_reserved(struct kernel_pid *prepared)
{
    enum kernel_pid_status status = kernel_pid_publish_admit(prepared);
    if (status) return status;
    for (struct kernel_pid_registry *r = prepared->registry; r; r = r->parent) {
        kernel_pid_t number = kernel_pid_number_in(prepared, r);
        unsigned bucket = (uint32_t)number % KERNEL_PID_BUCKETS;
        struct kernel_pid **next = identity_next(prepared, r);
        if (kernel_pid_find(r, number) || *next) __builtin_trap();
        *next = r->buckets[bucket];
        r->buckets[bucket] = prepared;
    }
    prepared->published = 1;
    return KERNEL_PID_STATUS_OK;
}

enum kernel_pid_status kernel_pid_publish(struct kernel_pid_registry *registry,
    struct kernel_pid *prepared)
{
    enum kernel_pid_status status = kernel_pid_reserve(registry, prepared);
    if (status != KERNEL_PID_STATUS_OK) return status;
    return kernel_pid_publish_reserved(prepared);
}

void kernel_pid_get(struct kernel_pid *identity)
{
    if (!identity || !identity->registry || identity->published > 2 || !identity->references ||
        identity->references == UINT32_MAX) __builtin_trap();
    identity->references++;
}

static void detach_number(struct kernel_pid *identity)
{
    for (struct kernel_pid_registry *r = identity->registry; r; r = r->parent) {
        kernel_pid_t number = kernel_pid_number_in(identity, r);
        struct kernel_pid **next = identity_next(identity, r);
        if (identity->published == 1) {
            struct kernel_pid **link = &r->buckets[(uint32_t)number % KERNEL_PID_BUCKETS];
            while (*link && *link != identity) link = identity_next(*link, r);
            if (!*link) __builtin_trap();
            *link = *next;
        } else if (identity->published || *next) __builtin_trap();
        if (kernel_pid_release(r->numbers, number) != KERNEL_PID_STATUS_OK) __builtin_trap();
        *next = NULL;
    }
    identity->published = 2;
}

void kernel_pid_put(struct kernel_pid *identity)
{
    if (!identity || !identity->registry || identity->published > 2 || !identity->references) __builtin_trap();
    if (--identity->references) return;
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        if (identity->members[role]) __builtin_trap();
    struct kernel_pid_registry *registry = identity->registry;
    if (identity->published != 2) detach_number(identity);
    identity->published = 3;
    identity->registry = 0;
    struct kernel_pid_registry *root = registry_root(registry);
    identity->hash_next = root->retired;
    root->retired = identity;
    if (registry->managed) kernel_pid_registry_put(registry);
}

void kernel_pid_attach(struct kernel_pid_member *member,
                       struct kernel_pid *identity, enum kernel_pid_role role,
                       void *task)
{
    if (!member || member->identity || !task || (unsigned)role >= KERNEL_PID_ROLES ||
        !identity || identity->published > 1)
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
    if (member->role == KERNEL_PID_TGID && !identity->members[KERNEL_PID_TGID] &&
        identity->registry->child_reaper == identity) {
        identity->registry->child_reaper = NULL;
        identity->registry->shutdown_reaper = NULL;
        if (identity->registry->managed) kernel_pid_registry_disable(identity->registry);
    }
    member->identity = 0;
    member->task = 0;
    member->next = member->previous = 0;
    if (identity->published == 1) {
        unsigned role = 0;
        while (role < KERNEL_PID_ROLES && !identity->members[role]) role++;
        if (role == KERNEL_PID_ROLES) detach_number(identity);
    }
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
        if (retired->published != 3) __builtin_trap();
        retired->published = 0;
    }
    return retired;
}
