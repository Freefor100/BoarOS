#include "private.h"
#include <kernel/avl.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/keys.h>
#include <kernel/sync.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <string.h>

#define KEY_DESC_MAX 4096U
#define KEY_TEXT_INLINE 96U
#define KEY_PAYLOAD_MAX 32767U
#define KEY_LINK_CHARGE 4U
#define KEY_SEARCH_DEPTH 6
#define KEY_SEARCH_RINGS 4096U
#define KEY_REAP_BATCH 64U
#define KEY_NEGATIVE_SECONDS 60U
#define KEY_GC_DELAY_SECONDS 300U
#define CONTAINER(node, type, member) \
    ((type *)((char *)(node) - offsetof(type, member)))

enum key_type { KEY_RING, KEY_USER, KEY_LOGON, KEY_UNSUPPORTED };
enum key_flags { KEY_UID_RING=1U, KEY_REVOKED=2U, KEY_INVALIDATED=4U };
enum key_access { KEY_VIEW=1U, KEY_READ=2U, KEY_WRITE=4U, KEY_SEARCH=8U,
    KEY_LINK=16U, KEY_SETATTR=32U, KEY_ALL=63U };
enum key_special { KEY_THREAD=-1, KEY_PROCESS=-2, KEY_SESSION=-3,
    KEY_UID=-4, KEY_UID_SESSION=-5, KEY_GROUP=-6, KEY_AUTH=-7, KEY_REQUESTOR=-8 };
enum key_control { KEYCTL_GET_ID, KEYCTL_JOIN, KEYCTL_UPDATE, KEYCTL_REVOKE,
    KEYCTL_CHOWN, KEYCTL_SETPERM, KEYCTL_DESCRIBE, KEYCTL_CLEAR, KEYCTL_LINK,
    KEYCTL_UNLINK, KEYCTL_SEARCH, KEYCTL_READ, KEYCTL_INSTANTIATE, KEYCTL_NEGATE,
    KEYCTL_DEFAULT, KEYCTL_TIMEOUT, KEYCTL_AUTHORITY, KEYCTL_SECURITY,
    KEYCTL_SESSION_TO_PARENT, KEYCTL_REJECT, KEYCTL_INSTANTIATE_IOV,
    KEYCTL_INVALIDATE, KEYCTL_CAPABILITIES=31 };
enum lookup_flags { LOOKUP_CREATE=1U, LOOKUP_NEGATIVE=2U, LOOKUP_UNLINK=4U };

struct key_uid {
    struct kernel_avl_node uid;
    struct key_uid *next_anchor;
    struct kernel_heap *heap;
    struct kernel_key *user, *session;
    uint32_t refs, count;
    size_t bytes;
};

struct key_link {
    struct kernel_avl_node name;
    struct key_link *next, **previous;
    struct key_link *next_ring, **previous_ring;
    struct key_link *next_parent, **previous_parent;
    struct kernel_key *ring, *key;
};

struct kernel_key {
    struct kernel_avl_node serial, name, reap;
    struct kernel_key *dead_next;
    struct key_uid *account;
    struct kernel_heap *heap;
    struct kernel_avl_node *link_index;
    struct key_link *links, *rings, *parents;
    void *payload;
    size_t bytes, name_bytes, payload_bytes, link_count;
    uint64_t expires, visited;
    uint32_t references, uid, gid, perm;
    unsigned depth;
    enum key_type type;
    unsigned flags;
    int negative;
    char description[];
};

struct key_call {
    struct kernel_heap *heap;
    struct kernel_mm *mm;
    const struct kernel_credentials *cred;
    struct kernel_credentials *candidate;
    uint64_t now;
    int changed;
};

static const char *const key_type_names[] = {"keyring", "user", "logon"};
/* The service mutex serializes publication, reference retirement and graph
 * mutations. Allocations inside it cannot enter reclaim. User copies stay
 * outside, and serial-number lookup is independent of graph size. */
static struct kernel_mutex keys_lock;
static struct kernel_raw_lock init_lock = {
    .rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED,
};
static unsigned ready;
static struct kernel_avl_node *serials, *names, *quotas, *reap_queue;
static struct key_uid *uid_anchors;
static uint32_t next_serial;
static uint64_t walk_generation;

static void lock_keys(struct kernel_lock_guard *guard)
{
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        KERNEL_RAW_SCOPE(init, &init_lock);
        if (!__atomic_load_n(&ready, __ATOMIC_RELAXED)) {
            kernel_mutex_init(&keys_lock, 64, (uintptr_t)&keys_lock);
            __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
        }
    }
    kernel_mutex_lock(&keys_lock, guard);
}

static struct kernel_avl_node *find_first(struct kernel_avl_node *node, uint64_t key)
{
    struct kernel_avl_node *found = NULL;
    while (node) {
        if (node->key < key) {
            node = node->right;
        } else {
            if (node->key == key) found = node;
            node = node->left;
        }
    }
    return found;
}

static uint64_t name_hash(enum key_type type, const char *description)
{
    uint64_t hash = UINT64_C(14695981039346656037) ^ (unsigned)type;
    for (const unsigned char *p = (const unsigned char *)description; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static struct key_uid *find_uid(uint32_t uid)
{
    struct kernel_avl_node *node = find_first(quotas, uid);
    return node ? CONTAINER(node, struct key_uid, uid) : NULL;
}

static struct key_uid *uid_get(struct kernel_heap *heap, uint32_t uid)
{
    struct key_uid *account = find_uid(uid);
    if (!account) {
        if (kernel_heap_allocate_zeroed_nosleep(heap, 1, sizeof(*account), (void **)&account))
            return NULL;
        account->heap = heap;
        account->uid = (struct kernel_avl_node){ .key=uid, .height=1 };
        quotas = kernel_avl_insert_node(quotas, &account->uid);
    }
    if (account->refs == UINT32_MAX) __builtin_trap();
    ++account->refs;
    return account;
}

static void uid_put(struct key_uid *account)
{
    if (--account->refs) return;
    if (account->bytes || account->count || account->user || account->session) __builtin_trap();
    quotas = kernel_avl_remove_node(quotas, &account->uid);
    kernel_heap_release(account->heap, account);
}

static int quota_check(const struct key_uid *account, size_t added, unsigned count)
{
    unsigned max_keys = account->uid.key ? 200U : 1000000U;
    size_t max_bytes = account->uid.key ? 20000U : 25000000U;
    if ((count && account->count >= max_keys) || added > max_bytes ||
        account->bytes > max_bytes - added)
        return -KERNEL_EDQUOT;
    return 0;
}

static int key_resize(struct kernel_key *key, size_t bytes)
{
    if (bytes > key->bytes) {
        int error = quota_check(key->account, bytes - key->bytes, 0);
        if (error) return error;
    }
    key->account->bytes -= key->bytes;
    key->account->bytes += bytes;
    key->bytes = bytes;
    return 0;
}

static void wipe_free(struct kernel_heap *heap, void *buffer, size_t bytes)
{
    if (!buffer) return;
    for (volatile unsigned char *p = buffer; bytes; --bytes) *p++ = 0;
    kernel_heap_release(heap, buffer);
}

static void key_get(struct kernel_key *key)
{
    if (key) {
        uint32_t before = __atomic_fetch_add(&key->references, 1, __ATOMIC_RELAXED);
        if (!before || before == UINT32_MAX) __builtin_trap();
    }
}

static void key_drop(struct kernel_key *key, struct kernel_key **dead)
{
    uint32_t before = __atomic_fetch_sub(&key->references, 1, __ATOMIC_ACQ_REL);
    if (!before) __builtin_trap();
    if (before == 1) {
        key->dead_next = *dead;
        *dead = key;
    }
}

static void unlink_locked(struct key_link *link, struct kernel_key **dead)
{
    struct kernel_key *ring = link->ring, *key = link->key;
    *link->previous = link->next;
    if (link->next) link->next->previous = link->previous;
    if (link->previous_ring) {
        *link->previous_ring = link->next_ring;
        if (link->next_ring) link->next_ring->previous_ring = link->previous_ring;
    }
    *link->previous_parent = link->next_parent;
    if (link->next_parent) link->next_parent->previous_parent = link->previous_parent;
    ring->link_index = kernel_avl_remove_node(ring->link_index, &link->name);
    --ring->link_count;
    if (key_resize(ring, ring->bytes - KEY_LINK_CHARGE)) __builtin_trap();
    kernel_heap_release(ring->heap, link);
    key_drop(key, dead);
}

static void retire_keys(struct kernel_key *dead)
{
    while (dead) {
        struct kernel_key *key = dead;
        dead = key->dead_next;
        while (key->links) unlink_locked(key->links, &dead);
        if (key->parents) __builtin_trap();
        serials = kernel_avl_remove_node(serials, &key->serial);
        if (key->type == KEY_RING && !(key->flags & KEY_UID_RING))
            names = kernel_avl_remove_node(names, &key->name);
        if (key->reap.key) reap_queue = kernel_avl_remove_node(reap_queue, &key->reap);
        --key->account->count;
        key->account->bytes -= key->bytes;
        uid_put(key->account);
        wipe_free(key->heap, key->payload, key->payload_bytes);
        kernel_heap_release(key->heap, key);
    }
}

static void key_put_locked(struct kernel_key *key)
{
    struct kernel_key *dead = NULL;
    if (key) key_drop(key, &dead);
    retire_keys(dead);
}

static void key_put(struct kernel_key *key)
{
    if (!key) return;
    /* An independently held reference keeps the object discoverable even
     * when the last graph edge is being removed by another caller. */
    uint32_t refs = __atomic_load_n(&key->references, __ATOMIC_RELAXED);
    while (refs > 1) {
        if (__atomic_compare_exchange_n(&key->references, &refs, refs - 1, 0,
                __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    }
    KERNEL_LOCK_SCOPE(guard);
    lock_keys(&guard);
    key_put_locked(key);
}

static int new_key(struct kernel_heap *heap, uint32_t owner, uint32_t group, uint32_t charged_uid,
    enum key_type type, const char *description, uint32_t perm, unsigned flags, int overrun,
    void **payload, size_t payload_bytes, struct kernel_key **result)
{
    if (next_serial >= INT32_MAX) return -KERNEL_ENOSPC;
    struct key_uid *account = uid_get(heap, charged_uid);
    if (!account) return -KERNEL_ENOMEM;
    size_t desc_bytes = strlen(description) + 1;
    size_t bytes = desc_bytes + payload_bytes;
    int error = overrun ? 0 : quota_check(account, bytes, 1);
    struct kernel_key *key = NULL;
    if (!error && kernel_heap_allocate_zeroed_nosleep(heap, 1, sizeof(*key) + desc_bytes,
            (void **)&key)) error = -KERNEL_ENOMEM;
    if (error) {
        uid_put(account);
        return error;
    }
    key->serial = (struct kernel_avl_node){ .key=++next_serial, .height=1 };
    key->name = (struct kernel_avl_node){ .key=name_hash(type, description), .height=1 };
    key->account = account;
    key->heap = heap;
    key->uid = owner;
    key->gid = group;
    key->perm = perm;
    key->type = type;
    key->flags = flags;
    key->references = 1;
    key->bytes = bytes;
    key->name_bytes = desc_bytes;
    key->payload_bytes = payload_bytes;
    if (payload) {
        key->payload = *payload;
        *payload = NULL;
    }
    memcpy(key->description, description, desc_bytes);
    ++account->count;
    account->bytes += bytes;
    serials = kernel_avl_insert_node(serials, &key->serial);
    if (type == KEY_RING && !(flags & KEY_UID_RING))
        names = kernel_avl_insert_node(names, &key->name);
    *result = key;
    return 0;
}

static int new_ring(struct key_call *call, const char *description, uint32_t perm,
    int overrun, struct kernel_key **ring)
{
    return new_key(call->heap, call->cred->uid, call->cred->gid, call->cred->uid,
        KEY_RING, description, perm, 0, overrun, NULL, 0, ring);
}

void kernel_keys_credentials_get(struct kernel_credentials *cred)
{
    key_get(cred->thread_keyring);
    key_get(cred->process_keyring);
    key_get(cred->session_keyring);
}

void kernel_keys_credentials_put(const struct kernel_credentials *cred)
{
    key_put(cred->thread_keyring);
    key_put(cred->process_keyring);
    key_put(cred->session_keyring);
}

void kernel_keys_credentials_exec(struct kernel_credentials *cred)
{
    key_put(cred->thread_keyring);
    key_put(cred->process_keyring);
    cred->thread_keyring = NULL;
    cred->process_keyring = NULL;
}

void kernel_keys_credentials_fork(struct kernel_credentials *cred, int thread)
{
    if (!thread) {
        kernel_keys_credentials_exec(cred);
        return;
    }
    if (!cred->thread_keyring) return;
    key_put(cred->thread_keyring);
    cred->thread_keyring = NULL;
    struct key_call call = { .heap=cred->heap, .cred=cred };
    KERNEL_LOCK_SCOPE(guard);
    lock_keys(&guard);
    /* A failed eager allocation leaves the child's ring available lazily. */
    (void)new_ring(&call, "_tid", KEY_ALL << 24 | KEY_VIEW << 16, 1, &cred->thread_keyring);
}

void kernel_keys_credentials_change_ids(const struct kernel_credentials *old,
    const struct kernel_credentials *replacement)
{
    KERNEL_LOCK_SCOPE(guard);
    lock_keys(&guard);
    if (old->fsuid != replacement->fsuid) replacement->thread_keyring->uid = replacement->fsuid;
    if (old->fsgid != replacement->fsgid) replacement->thread_keyring->gid = replacement->fsgid;
}

static int key_status(const struct kernel_key *key, uint64_t now)
{
    if (key->flags & KEY_INVALIDATED) return -KERNEL_ENOKEY;
    if (key->flags & KEY_REVOKED) return -KERNEL_EKEYREVOKED;
    if (key->expires && now >= key->expires) return -KERNEL_EKEYEXPIRED;
    return 0;
}

static void schedule_reap(struct kernel_key *key, uint64_t deadline)
{
    if (key->reap.key) reap_queue = kernel_avl_remove_node(reap_queue, &key->reap);
    key->reap = (struct kernel_avl_node){ .key=deadline, .height=1 };
    if (deadline) reap_queue = kernel_avl_insert_node(reap_queue, &key->reap);
}

static void set_expiry(struct kernel_key *key, uint64_t expires)
{
    key->expires = expires;
    schedule_reap(key, expires ? expires + KEY_GC_DELAY_SECONDS : 0);
}

static void collect_keys(uint64_t now)
{
    for (unsigned i = 0; i < KEY_REAP_BATCH; ++i) {
        struct kernel_avl_node *first = kernel_avl_first(reap_queue);
        if (!first || first->key > now) break;
        struct kernel_key *key = CONTAINER(first, struct kernel_key, reap);
        if (!key->parents) {
            schedule_reap(key, 0);
            continue;
        }
        struct kernel_key *dead = NULL;
        unlink_locked(key->parents, &dead);
        retire_keys(dead);
    }
}

static struct key_link *find_link(const struct kernel_key *ring, enum key_type type,
    const char *description, uint64_t hash)
{
    for (struct kernel_avl_node *node = find_first(ring->link_index, hash);
            node && node->key == hash; node = kernel_avl_next(ring->link_index, node)) {
        struct key_link *link = CONTAINER(node, struct key_link, name);
        if (link->key->type == type && !strcmp(link->key->description, description))
            return link;
    }
    return NULL;
}

static int permitted(const struct kernel_key *key, const struct kernel_credentials *cred,
    unsigned need, int possessed)
{
    uint32_t perm = key->perm;
    unsigned access = perm;
    if (key->uid == cred->fsuid) access = perm >> 16;
    else if (key->gid != UINT32_MAX && (perm & (KEY_ALL << 8)) &&
        kernel_credentials_in_group(cred, key->gid)) access = perm >> 8;
    if (possessed) access |= perm >> 24;
    return (access & need) == need;
}

struct key_walk {
    const struct key_call *call;
    struct kernel_key *target, *found;
    const char *description;
    uint64_t hash, generation;
    enum key_type type;
    unsigned budget;
    int error, negative, searched, exceeded, possessed, topology;
};

static int walk_match(struct key_walk *walk, struct kernel_key *key)
{
    if (walk->target ? key != walk->target :
        key->type != walk->type || strcmp(key->description, walk->description)) return 0;
    if (!permitted(key, walk->call->cred, KEY_SEARCH, walk->possessed)) {
        walk->error = -KERNEL_EACCES;
        return 0;
    }
    if (!walk->target) {
        int error = key_status(key, walk->call->now);
        if (error) {
            walk->error = error;
            return 0;
        }
        if (key->negative) {
            walk->negative = key->negative;
            return 0;
        }
    }
    walk->found = key;
    return 1;
}

/* Walk only child keyrings after looking up the matching direct link. Visit
 * shared subtrees once per remaining depth, with a cap on examined edges. */
static int walk_ring(struct key_walk *walk, struct kernel_key *ring, unsigned depth)
{
    if (walk->topology && ring == walk->target) {
        walk->found = ring;
        return 1;
    }
    if (!walk->topology && (!permitted(ring, walk->call->cred, KEY_SEARCH, walk->possessed) ||
        (walk->target ? (ring->flags & (KEY_INVALIDATED | KEY_REVOKED)) != 0 :
                        key_status(ring, walk->call->now) != 0))) return 0;
    if (ring->visited == walk->generation && ring->depth <= depth) return 0;
    ring->visited = walk->generation;
    ring->depth = depth;
    if (!walk->topology) {
        walk->searched = 1;
        if (walk_match(walk, ring)) return 1;
        struct key_link *match = find_link(ring, walk->type, walk->description, walk->hash);
        if (match && walk_match(walk, match->key)) return 1;
    }
    for (struct key_link *link = ring->rings; link; link = link->next_ring) {
        if (!walk->budget) {
            walk->exceeded = 1;
            break;
        }
        --walk->budget;
        if (depth == KEY_SEARCH_DEPTH) {
            walk->exceeded = 1;
            break;
        }
        if (walk_ring(walk, link->key, depth + 1)) return 1;
    }
    return 0;
}

static int link_locked(struct kernel_key *ring, struct kernel_key *key)
{
    struct key_link *link = find_link(ring, key->type, key->description, key->name.key);
    if (link && link->key == key) return 0;
    if (key->type == KEY_RING) {
        struct key_walk walk = { .target=ring, .topology=1, .generation=++walk_generation,
            .budget=KEY_SEARCH_RINGS };
        if (walk_ring(&walk, key, 0)) return -KERNEL_EDEADLK;
        if (walk.exceeded) return -KERNEL_ELOOP;
    }
    if (!link) {
        int error = quota_check(ring->account, KEY_LINK_CHARGE, 0);
        if (error) return error;
    }
    struct key_link *new_link = NULL;
    if (kernel_heap_allocate_zeroed_nosleep(ring->heap, 1, sizeof(*new_link), (void **)&new_link))
        return -KERNEL_ENOMEM;
    key_get(key);
    if (link) {
        struct kernel_key *dead = NULL;
        unlink_locked(link, &dead);
        retire_keys(dead);
    }
    /* Replacing a same-name entry preserves its quota reservation. */
    ring->bytes += KEY_LINK_CHARGE;
    ring->account->bytes += KEY_LINK_CHARGE;
    ++ring->link_count;
    new_link->ring = ring;
    new_link->key = key;
    new_link->name = (struct kernel_avl_node){ .key=key->name.key, .height=1 };
    new_link->next = ring->links;
    new_link->previous = &ring->links;
    if (new_link->next) new_link->next->previous = &new_link->next;
    ring->links = new_link;
    if (key->type == KEY_RING) {
        new_link->next_ring = ring->rings;
        new_link->previous_ring = &ring->rings;
        if (new_link->next_ring) new_link->next_ring->previous_ring = &new_link->next_ring;
        ring->rings = new_link;
    }
    new_link->next_parent = key->parents;
    new_link->previous_parent = &key->parents;
    if (new_link->next_parent) new_link->next_parent->previous_parent = &new_link->next_parent;
    key->parents = new_link;
    ring->link_index = kernel_avl_insert_node(ring->link_index, &new_link->name);
    return 0;
}

static char *append_number(char *p, uint32_t number)
{
    char digits[10];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    while (n) *p++ = digits[--n];
    return p;
}

static int uid_rings(struct key_call *call, struct key_uid **out)
{
    struct key_uid *account = uid_get(call->heap, call->cred->uid);
    if (!account) return -KERNEL_ENOMEM;
    int error = 0;
    if (!account->user) {
        char user_name[sizeof("_uid.") + 10] = "_uid.";
        char session_name[sizeof("_uid_ses.") + 10] = "_uid_ses.";
        *append_number(user_name + sizeof("_uid.") - 1, call->cred->uid) = 0;
        *append_number(session_name + sizeof("_uid_ses.") - 1, call->cred->uid) = 0;
        unsigned perm = (KEY_ALL & ~KEY_SETATTR) << 24 | KEY_ALL << 16;
        struct kernel_key *user = NULL, *session = NULL;
        error = new_key(call->heap, call->cred->uid, UINT32_MAX, call->cred->uid,
            KEY_RING, user_name, perm, KEY_UID_RING, 0, NULL, 0, &user);
        if (!error) error = new_key(call->heap, call->cred->uid, UINT32_MAX, call->cred->uid,
            KEY_RING, session_name, perm, KEY_UID_RING, 0, NULL, 0, &session);
        if (!error) error = link_locked(session, user);
        if (!error) {
            account->user = user;
            account->session = session;
            account->next_anchor = uid_anchors;
            uid_anchors = account;
        } else {
            key_put_locked(session);
            key_put_locked(user);
        }
    }
    if (!error) *out = account;
    uid_put(account);
    return error;
}

int kernel_keys_shutdown(struct kernel_heap *heap)
{
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return 0;
    KERNEL_LOCK_SCOPE(guard);
    lock_keys(&guard);
    while (uid_anchors) {
        struct key_uid *account = uid_anchors;
        struct kernel_key *user = account->user, *session = account->session;
        if (account->heap != heap) return -KERNEL_EBUSY;
        uid_anchors = account->next_anchor;
        account->user = NULL;
        account->session = NULL;
        key_put_locked(session);
        key_put_locked(user);
    }
    return serials || quotas ? -KERNEL_EBUSY : 0;
}

static struct kernel_key *session_root(const struct kernel_credentials *cred)
{
    if (cred->session_keyring) return cred->session_keyring;
    struct key_uid *account = find_uid(cred->uid);
    return account ? account->session : NULL;
}

static int search_roots(struct key_walk *walk)
{
    const struct kernel_credentials *cred = walk->call->cred;
    struct kernel_key *roots[] = { cred->thread_keyring, cred->process_keyring,
        session_root(cred) };
    walk->possessed = 1;
    walk->generation = ++walk_generation;
    walk->budget = KEY_SEARCH_RINGS;
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (roots[i] && walk_ring(walk, roots[i], 0)) return 1;
    }
    return 0;
}

static int is_possessed(struct key_call *call, struct kernel_key *key)
{
    struct key_walk walk = { .call=call, .target=key, .description=key->description,
        .type=key->type, .hash=key->name.key };
    return search_roots(&walk);
}

static void replace_ring(struct key_call *call, struct kernel_key **slot, struct kernel_key *key)
{
    if (!call->candidate) __builtin_trap();
    key_put_locked(*slot);
    *slot = key;
    call->changed = 1;
}

static int special_ring(struct key_call *call, int32_t id, unsigned flags, struct kernel_key **out)
{
    const struct kernel_credentials *cred = call->cred;
    int create = (flags & LOOKUP_CREATE) != 0;
    struct kernel_key *key = NULL;
    struct key_uid *account;
    int error;
    switch (id) {
    case KEY_THREAD:
    case KEY_PROCESS:
        key = id == KEY_THREAD ? cred->thread_keyring : cred->process_keyring;
        if (key) break;
        if (!create) return -KERNEL_ENOKEY;
        error = new_ring(call, id == KEY_THREAD ? "_tid" : "_pid",
            KEY_ALL << 24 | KEY_VIEW << 16, 1, &key);
        if (error) return error;
        replace_ring(call, id == KEY_THREAD ? &call->candidate->thread_keyring :
            &call->candidate->process_keyring, key);
        break;
    case KEY_SESSION:
        key = cred->session_keyring;
        if (key && (!create || !(key->flags & KEY_UID_RING))) break;
        error = uid_rings(call, &account);
        if (error) return error;
        if (create) {
            error = new_ring(call, "_ses", KEY_ALL << 24 | (KEY_VIEW | KEY_READ) << 16,
                key == NULL, &key);
            if (error) return error;
        } else {
            key = account->session;
            key_get(key);
        }
        replace_ring(call, &call->candidate->session_keyring, key);
        break;
    case KEY_UID:
    case KEY_UID_SESSION:
        error = uid_rings(call, &account);
        if (error) return error;
        key = id == KEY_UID ? account->user : account->session;
        break;
    case KEY_AUTH:
    case KEY_REQUESTOR:
        return -KERNEL_ENOKEY;
    default:
        return -KERNEL_EINVAL;
    }
    *out = key;
    return 0;
}

struct key_ref { struct kernel_key *key; int possessed; };

static int lookup(struct key_call *call, int32_t id, unsigned flags, unsigned access,
    struct key_ref *out)
{
    struct kernel_key *key = NULL;
    int possessed = 1;
    if (id > 0) {
        struct kernel_avl_node *node = find_first(serials, (uint32_t)id);
        if (!node) return -KERNEL_ENOKEY;
        key = CONTAINER(node, struct kernel_key, serial);
        possessed = (flags & LOOKUP_UNLINK) ? 0 : is_possessed(call, key);
    } else {
        int error = special_ring(call, id, flags, &key);
        if (error) return error;
    }
    if (!(flags & LOOKUP_UNLINK)) {
        int error = key_status(key, call->now);
        if (error) return error;
        if (key->negative && !(flags & LOOKUP_NEGATIVE)) return key->negative;
        if (!permitted(key, call->cred, access, possessed)) return -KERNEL_EACCES;
    }
    *out = (struct key_ref){ .key=key, .possessed=possessed };
    return 0;
}

static int lookup_ring(struct key_call *call, int32_t id, unsigned flags, unsigned access,
    struct key_ref *out)
{
    int error = lookup(call, id, flags, access, out);
    if (!error && out->key->type != KEY_RING) error = -KERNEL_ENOTDIR;
    return error;
}

static int copy_string(struct key_call *call, uint64_t user, char *buffer, size_t capacity,
    int length_error)
{
    size_t copied;
    enum kernel_uaccess_status status =
        kernel_copy_string_from_user(call->mm, buffer, user, capacity, &copied);
    if (status == KERNEL_UACCESS_STATUS_TOO_LONG) return -length_error;
    return status ? -KERNEL_EFAULT : 0;
}

static int copy_text(struct key_call *call, uint64_t user, char *short_text, char **buffer)
{
    size_t copied;
    enum kernel_uaccess_status status =
        kernel_copy_string_from_user(call->mm, short_text, user, KEY_TEXT_INLINE, &copied);
    if (status == KERNEL_UACCESS_STATUS_OK) {
        *buffer = short_text;
        return 0;
    }
    if (status != KERNEL_UACCESS_STATUS_TOO_LONG) return -KERNEL_EFAULT;
    if (kernel_heap_allocate(call->heap, KEY_DESC_MAX, (void **)buffer))
        return -KERNEL_ENOMEM;
    return copy_string(call, user, *buffer, KEY_DESC_MAX, KERNEL_EINVAL);
}

static int copy_type(struct key_call *call, uint64_t user, enum key_type *type)
{
    char name[32];
    int error = copy_string(call, user, name, sizeof(name), KERNEL_EINVAL);
    if (error) return error;
    if (!name[0]) return -KERNEL_EINVAL;
    if (name[0] == '.') return -KERNEL_EPERM;
    for (unsigned i = 0; i < sizeof(key_type_names) / sizeof(key_type_names[0]); ++i) {
        if (!strcmp(name, key_type_names[i])) {
            *type = (enum key_type)i;
            return 0;
        }
    }
    *type = KEY_UNSUPPORTED;
    return 0;
}

static int copy_payload(struct key_call *call, uint64_t user, size_t bytes, void **out)
{
    if (!bytes) return 0;
    if (kernel_heap_allocate(call->heap, bytes, out)) return -KERNEL_ENOMEM;
    size_t copied;
    if (kernel_copy_from_user(call->mm, *out, user, bytes, &copied) || copied != bytes)
        return -KERNEL_EFAULT;
    return 0;
}

static int update_locked(struct kernel_key *key, void **payload, size_t bytes)
{
    if (key->type == KEY_RING) return -KERNEL_EOPNOTSUPP;
    if (!bytes || bytes > KEY_PAYLOAD_MAX) return -KERNEL_EINVAL;
    int error = key_resize(key, key->name_bytes + bytes);
    if (error) return error;
    wipe_free(key->heap, key->payload, key->payload_bytes);
    key->payload = *payload;
    *payload = NULL;
    key->payload_bytes = bytes;
    key->negative = 0;
    set_expiry(key, 0);
    return 0;
}

static unsigned new_permissions(enum key_type type)
{
    unsigned pos = type == KEY_LOGON ? KEY_ALL & ~KEY_READ : KEY_ALL;
    return pos << 24 | KEY_VIEW << 16;
}

static int description_valid(enum key_type type, const char *description)
{
    if (!description || !*description) return 0;
    if (type != KEY_LOGON) return 1;
    const char *colon = description;
    while (*colon && *colon != ':') ++colon;
    return *colon && colon != description;
}

static int64_t add_locked(struct key_call *call, enum key_type type, const char *description,
    void **payload, size_t bytes, int32_t id)
{
    struct key_ref ref;
    int error = lookup_ring(call, id, LOOKUP_CREATE, KEY_WRITE, &ref);
    if (error) return error;
    if (type == KEY_UNSUPPORTED) return -KERNEL_ENODEV;
    if (!description_valid(type, description)) return -KERNEL_EINVAL;
    if (type == KEY_RING ? bytes != 0 : !bytes || bytes > KEY_PAYLOAD_MAX)
        return -KERNEL_EINVAL;
    uint64_t hash = name_hash(type, description);
    struct key_link *existing = find_link(ref.key, type, description, hash);
    if (type != KEY_RING && existing && !key_status(existing->key, call->now)) {
        if (!permitted(existing->key, call->cred, KEY_WRITE, ref.possessed))
            return -KERNEL_EACCES;
        error = update_locked(existing->key, payload, bytes);
        return error ? (int64_t)error : (int64_t)existing->key->serial.key;
    }
    struct kernel_key *key;
    error = new_key(call->heap, call->cred->fsuid, call->cred->fsgid, call->cred->uid,
        type, description, new_permissions(type), 0, 0, payload, bytes, &key);
    if (error) return error;
    int64_t serial = (int64_t)key->serial.key;
    error = link_locked(ref.key, key);
    key_put_locked(key);
    return error ? (int64_t)error : serial;
}

static int default_ring(struct key_call *call, struct kernel_key **out)
{
    const struct kernel_credentials *cred = call->cred;
    unsigned selector = cred->request_key_default;
    if ((!selector || selector == 7 || selector == 1) && cred->thread_keyring)
        *out = cred->thread_keyring;
    else if (selector != 4 && selector != 5 && selector != 3 && cred->process_keyring)
        *out = cred->process_keyring;
    else if (selector != 4 && selector != 5 && cred->session_keyring)
        *out = cred->session_keyring;
    else {
        struct key_uid *account;
        int error = uid_rings(call, &account);
        if (error) return error;
        *out = selector == 4 ? account->user : account->session;
    }
    int error = key_status(*out, call->now);
    if (error) return error;
    return permitted(*out, cred, KEY_WRITE, 1) ? 0 : -KERNEL_EACCES;
}

static int64_t request_locked(struct key_call *call, enum key_type type, const char *description,
    int callout, int32_t id)
{
    struct key_ref dest = {0};
    int error;
    if (id) {
        error = lookup_ring(call, id, LOOKUP_CREATE, KEY_WRITE, &dest);
        if (error) return error;
    }
    if (type == KEY_UNSUPPORTED) return -KERNEL_ENODEV;
    struct key_walk walk = { .call=call, .description=description, .type=type,
        .hash=name_hash(type, description) };
    if (search_roots(&walk)) {
        if (dest.key) {
            if (!permitted(walk.found, call->cred, KEY_LINK, 1)) return -KERNEL_EACCES;
            error = link_locked(dest.key, walk.found);
            if (error) return error;
        }
        return (int64_t)walk.found->serial.key;
    }
    if (walk.negative) return walk.negative;
    if (walk.error && walk.error != -KERNEL_EACCES && walk.error != -KERNEL_EKEYEXPIRED)
        return walk.error;
    if (walk.exceeded) return -KERNEL_ELOOP;
    if (!callout) return -KERNEL_ENOKEY;
    if (type == KEY_RING) return -KERNEL_EPERM;
    if (!description_valid(type, description)) return -KERNEL_EINVAL;
    if (!dest.key) {
        error = default_ring(call, &dest.key);
        if (error) return error;
    }
    /* No userspace upcall is available. A failed construction is cached as a
     * real key, visible in its ring and updatable by an authorized publisher. */
    struct kernel_key *key;
    error = new_key(call->heap, call->cred->fsuid, call->cred->fsgid, call->cred->uid,
        type, description, new_permissions(type), 0, 0, NULL, 0, &key);
    if (error) return error;
    key->negative = -KERNEL_ENOKEY;
    set_expiry(key, call->now + KEY_NEGATIVE_SECONDS);
    error = link_locked(dest.key, key);
    key_put_locked(key);
    return error ? error : -KERNEL_ENOKEY;
}

static int64_t join_locked(struct key_call *call, const char *description)
{
    struct kernel_key *key = NULL;
    if (description) {
        if (!description[0]) return -KERNEL_EINVAL;
        if (description[0] == '.') return -KERNEL_EPERM;
        uint64_t hash = name_hash(KEY_RING, description);
        for (struct kernel_avl_node *node = find_first(names, hash);
                node && node->key == hash; node = kernel_avl_next(names, node)) {
            struct kernel_key *match = CONTAINER(node, struct kernel_key, name);
            if (!strcmp(match->description, description) && !key_status(match, call->now) &&
                permitted(match, call->cred, KEY_SEARCH, 0)) {
                if (match == call->cred->session_keyring) return 0;
                key = match;
                key_get(key);
                break;
            }
        }
    }
    if (!key) {
        unsigned perm = KEY_ALL << 24 | (KEY_VIEW | KEY_READ |
            (description ? KEY_LINK : 0)) << 16;
        int error = new_ring(call, description ? description : "_ses", perm,
            !description && !call->cred->session_keyring, &key);
        if (error) return error;
    }
    replace_ring(call, &call->candidate->session_keyring, key);
    return (int64_t)key->serial.key;
}

static int64_t chown_locked(struct key_call *call, struct kernel_key *key,
    uint32_t uid, uint32_t gid)
{
    int new_uid = uid != UINT32_MAX && uid != key->uid;
    int new_gid = gid != UINT32_MAX && gid != key->gid;
    if ((new_uid || (new_gid && !kernel_credentials_in_group(call->cred, gid))) &&
        !kernel_credentials_capable(call->cred, KERNEL_CAP_SYS_ADMIN)) return -KERNEL_EACCES;
    if (new_uid) {
        struct key_uid *account = uid_get(call->heap, uid);
        if (!account) return -KERNEL_ENOMEM;
        if (account != key->account) {
            int error = quota_check(account, key->bytes, 1);
            if (error) {
                uid_put(account);
                return error;
            }
            account->count++;
            account->bytes += key->bytes;
            key->account->count--;
            key->account->bytes -= key->bytes;
            uid_put(key->account);
            key->account = account;
        } else {
            uid_put(account);
        }
        key->uid = uid;
    }
    if (new_gid) key->gid = gid;
    return 0;
}

static void clear_ring(struct kernel_key *ring)
{
    struct kernel_key *dead = NULL;
    while (ring->links) unlink_locked(ring->links, &dead);
    retire_keys(dead);
}

static void revoke_locked(struct kernel_key *key, uint64_t now)
{
    key->flags |= KEY_REVOKED;
    if (key->type == KEY_RING) {
        clear_ring(key);
    } else {
        if (key_resize(key, key->name_bytes)) __builtin_trap();
        wipe_free(key->heap, key->payload, key->payload_bytes);
        key->payload = NULL;
        key->payload_bytes = 0;
    }
    schedule_reap(key, now + KEY_GC_DELAY_SECONDS);
}

struct key_output { void *buffer; uint64_t user; size_t bytes, sensitive; };

static int64_t copy_key_output(struct key_call *call, struct key_output *output)
{
    size_t copied;
    if (kernel_copy_to_user(call->mm, output->user, output->buffer, output->bytes, &copied) ||
        copied != output->bytes) return -KERNEL_EFAULT;
    return 0;
}

static int64_t read_locked(struct key_call *call, int32_t id, uint64_t address, size_t capacity,
    struct key_output *output)
{
    struct key_ref ref;
    int error = lookup(call, id, LOOKUP_NEGATIVE, 0, &ref);
    if (error) {
        /* Deferred-permission lookup maps a missing identifier to ENOKEY.
         * A live reference still reports expiration and revocation. */
        return error == -KERNEL_EINVAL ? -KERNEL_ENOKEY : error;
    }
    struct kernel_key *key = ref.key;
    if (key->negative) return key->negative;
    if (!ref.possessed && !permitted(key, call->cred, KEY_READ, 0)) return -KERNEL_EACCES;
    if (key->type == KEY_LOGON) return -KERNEL_EOPNOTSUPP;
    if (key->type == KEY_RING && address && (capacity % sizeof(int32_t)))
        return -KERNEL_EINVAL;
    size_t bytes = key->type == KEY_RING ? key->link_count * sizeof(int32_t) : key->payload_bytes;
    if (!address || !bytes || capacity < bytes) return (int64_t)bytes;
    if (kernel_heap_allocate_nosleep(call->heap, bytes, &output->buffer)) return -KERNEL_ENOMEM;
    output->user = address;
    output->bytes = bytes;
    if (key->type == KEY_RING) {
        int32_t *out = output->buffer;
        for (const struct key_link *link = key->links; link; link = link->next)
            *out++ = (int32_t)link->key->serial.key;
    } else {
        memcpy(output->buffer, key->payload, bytes);
        output->sensitive = bytes;
    }
    return (int64_t)bytes;
}

static int64_t describe_locked(struct key_call *call, struct kernel_key *key,
    uint64_t address, size_t capacity, struct key_output *output)
{
    char prefix[40];
    const char *type = key_type_names[key->type];
    size_t type_bytes = strlen(type);
    memcpy(prefix, type, type_bytes);
    char *p = prefix + type_bytes;
    *p++ = ';';
    p = append_number(p, key->uid);
    *p++ = ';';
    p = append_number(p, key->gid == UINT32_MAX ? 65534U : key->gid);
    *p++ = ';';
    for (int shift = 28; shift >= 0; shift -= 4) *p++ = "0123456789abcdef"[(key->perm >> shift) & 15U];
    *p++ = ';';
    size_t prefix_bytes = (size_t)(p - prefix), bytes = prefix_bytes + key->name_bytes;
    if (!address || capacity < bytes) return (int64_t)bytes;
    if (kernel_heap_allocate_nosleep(call->heap, bytes, &output->buffer)) return -KERNEL_ENOMEM;
    memcpy(output->buffer, prefix, prefix_bytes);
    memcpy((char *)output->buffer + prefix_bytes, key->description, key->name_bytes);
    output->user = address;
    output->bytes = bytes;
    return (int64_t)bytes;
}

static int64_t search_locked(struct key_call *call, const uint64_t *arg, enum key_type type,
    const char *description)
{
    struct key_ref source, dest = {0};
    int error = lookup_ring(call, (int32_t)arg[1], 0, KEY_SEARCH, &source);
    if (error) return error;
    if (arg[4]) {
        error = lookup_ring(call, (int32_t)arg[4], LOOKUP_CREATE, KEY_WRITE, &dest);
        if (error) return error;
    }
    if (type == KEY_UNSUPPORTED) return -KERNEL_ENODEV;
    struct key_walk walk = { .call=call, .description=description, .type=type,
        .hash=name_hash(type, description), .generation=++walk_generation,
        .budget=KEY_SEARCH_RINGS, .possessed=source.possessed };
    if (!walk_ring(&walk, source.key, 0)) {
        if (walk.negative) return walk.negative;
        if (walk.error) return walk.error;
        return -KERNEL_ENOKEY;
    }
    if (dest.key) {
        if (!permitted(walk.found, call->cred, KEY_LINK, walk.possessed)) return -KERNEL_EACCES;
        error = link_locked(dest.key, walk.found);
        if (error) return error;
    }
    return (int64_t)walk.found->serial.key;
}

static int64_t control_locked(struct key_call *call, const uint64_t *arg,
    enum key_type type, const char *description, void **payload, struct key_output *output)
{
    int32_t id = (int32_t)arg[1];
    struct key_ref ref, ring;
    int error;
    switch ((int32_t)arg[0]) {
    case KEYCTL_GET_ID:
        error = lookup(call, id, (int32_t)arg[2] ? LOOKUP_CREATE : 0, KEY_SEARCH, &ref);
        return error ? (int64_t)error : (int64_t)ref.key->serial.key;
    case KEYCTL_JOIN:
        return join_locked(call, description);
    case KEYCTL_UPDATE:
        error = lookup(call, id, LOOKUP_NEGATIVE, KEY_WRITE, &ref);
        return error ? error : update_locked(ref.key, payload, arg[3]);
    case KEYCTL_REVOKE:
        error = lookup(call, id, 0, KEY_WRITE, &ref);
        if (error == -KERNEL_EACCES) error = lookup(call, id, 0, KEY_SETATTR, &ref);
        if (error) return error;
        revoke_locked(ref.key, call->now);
        return 0;
    case KEYCTL_CHOWN:
        if ((uint32_t)arg[2] == UINT32_MAX && (uint32_t)arg[3] == UINT32_MAX) return 0;
        error = lookup(call, id, LOOKUP_CREATE | LOOKUP_NEGATIVE, KEY_SETATTR, &ref);
        return error ? error : chown_locked(call, ref.key, (uint32_t)arg[2], (uint32_t)arg[3]);
    case KEYCTL_SETPERM:
        if ((uint32_t)arg[2] & ~UINT32_C(0x3f3f3f3f)) return -KERNEL_EINVAL;
        error = lookup(call, id, LOOKUP_CREATE | LOOKUP_NEGATIVE, KEY_SETATTR, &ref);
        if (error) return error;
        if (ref.key->uid != call->cred->fsuid &&
            !kernel_credentials_capable(call->cred, KERNEL_CAP_SYS_ADMIN)) return -KERNEL_EACCES;
        ref.key->perm = (uint32_t)arg[2];
        return 0;
    case KEYCTL_DESCRIBE:
        error = lookup(call, id, LOOKUP_NEGATIVE, KEY_VIEW, &ref);
        return error ? error : describe_locked(call, ref.key, arg[2], arg[3], output);
    case KEYCTL_CLEAR:
        error = lookup_ring(call, id, LOOKUP_CREATE, KEY_WRITE, &ref);
        if (error) return error;
        clear_ring(ref.key);
        return 0;
    case KEYCTL_LINK:
        error = lookup_ring(call, (int32_t)arg[2], LOOKUP_CREATE, KEY_WRITE, &ring);
        if (!error) error = lookup(call, id, LOOKUP_CREATE, KEY_LINK, &ref);
        return error ? error : link_locked(ring.key, ref.key);
    case KEYCTL_UNLINK: {
        error = lookup_ring(call, (int32_t)arg[2], 0, KEY_WRITE, &ring);
        if (!error) error = lookup(call, id, LOOKUP_UNLINK, 0, &ref);
        if (error) return error;
        struct key_link *link = find_link(ring.key, ref.key->type,
            ref.key->description, ref.key->name.key);
        if (!link || link->key != ref.key) return -KERNEL_ENOENT;
        struct kernel_key *dead = NULL;
        unlink_locked(link, &dead);
        retire_keys(dead);
        return 0;
    }
    case KEYCTL_SEARCH:
        return search_locked(call, arg, type, description);
    case KEYCTL_READ:
        return read_locked(call, id, arg[2], arg[3], output);
    case KEYCTL_INSTANTIATE:
    case KEYCTL_NEGATE:
    case KEYCTL_REJECT:
    case KEYCTL_INSTANTIATE_IOV:
        return id <= 0 ? -KERNEL_EINVAL : -KERNEL_EPERM;
    case KEYCTL_DEFAULT: {
        unsigned old = call->cred->request_key_default;
        if (id == -1) return old;
        if (id < 0 || id == 6 || id > 7) return -KERNEL_EINVAL;
        if (id == 1 || id == 2) {
            error = special_ring(call, id == 1 ? KEY_THREAD : KEY_PROCESS, LOOKUP_CREATE, &ref.key);
            if (error) return error;
        }
        call->candidate->request_key_default = (uint8_t)id;
        call->changed = 1;
        return old;
    }
    case KEYCTL_TIMEOUT:
        error = lookup(call, id, LOOKUP_CREATE | LOOKUP_NEGATIVE, KEY_SETATTR, &ref);
        if (error) return error;
        set_expiry(ref.key, (uint32_t)arg[2] ? call->now + (uint32_t)arg[2] : 0);
        return 0;
    case KEYCTL_AUTHORITY:
        return id < 0 ? -KERNEL_EINVAL : id ? -KERNEL_ENOKEY : 0;
    case KEYCTL_SECURITY:
        error = lookup(call, id, LOOKUP_NEGATIVE, KEY_VIEW, &ref);
        if (error) return error;
        if (arg[2] && arg[3]) {
            if (kernel_heap_allocate_zeroed_nosleep(call->heap, 1, 1, &output->buffer))
                return -KERNEL_ENOMEM;
            output->user = arg[2];
            output->bytes = 1;
        }
        return 1;
    case KEYCTL_INVALIDATE:
        error = lookup(call, id, 0, KEY_SEARCH, &ref);
        if (error) return error;
        ref.key->flags |= KEY_INVALIDATED;
        schedule_reap(ref.key, call->now ? call->now : 1);
        return 0;
    default:
        return -KERNEL_EOPNOTSUPP;
    }
}

static int may_install(const struct kernel_credentials *cred, int32_t id, int create)
{
    switch (id) {
    case KEY_THREAD: return create && !cred->thread_keyring;
    case KEY_PROCESS: return create && !cred->process_keyring;
    case KEY_SESSION: return create || !cred->session_keyring;
    default: return 0;
    }
}

static int need_candidate(const struct key_call *call, uint64_t number, const uint64_t *arg)
{
    const struct kernel_credentials *cred = call->cred;
    if (number == 217U) return may_install(cred, (int32_t)arg[4], 1);
    if (number == 218U) return may_install(cred, (int32_t)arg[3], 1);
    int32_t id = (int32_t)arg[1];
    switch ((int32_t)arg[0]) {
    case KEYCTL_JOIN:
        return 1;
    case KEYCTL_DEFAULT:
        return id >= 0 && id <= 7 && id != 6;
    case KEYCTL_GET_ID:
        return may_install(cred, id, (int32_t)arg[2] != 0);
    case KEYCTL_CLEAR:
    case KEYCTL_CHOWN:
    case KEYCTL_SETPERM:
    case KEYCTL_TIMEOUT:
        return may_install(cred, id, 1);
    case KEYCTL_LINK:
        return may_install(cred, id, 1) || may_install(cred, (int32_t)arg[2], 1);
    case KEYCTL_UNLINK:
        return may_install(cred, id, 0) || may_install(cred, (int32_t)arg[2], 0);
    case KEYCTL_SEARCH:
        return may_install(cred, id, 0) || may_install(cred, (int32_t)arg[4], 1);
    case KEYCTL_UPDATE:
    case KEYCTL_REVOKE:
    case KEYCTL_DESCRIBE:
    case KEYCTL_READ:
    case KEYCTL_SECURITY:
    case KEYCTL_INVALIDATE:
        return may_install(cred, id, 0);
    default:
        return 0;
    }
}

static int64_t capabilities(struct key_call *call, uint64_t user, size_t bytes)
{
    /* The capabilities and invalidation control are available. */
    const unsigned char flags[] = { 0x11, 0 };
    unsigned char zeroes[64] = {0};
    size_t initial = bytes < sizeof(flags) ? bytes : sizeof(flags);
    size_t copied;
    if (bytes && kernel_user_range_check(user, bytes)) return -KERNEL_EFAULT;
    if (initial && (kernel_copy_to_user(call->mm, user, flags, initial, &copied) ||
        copied != initial)) return -KERNEL_EFAULT;
    for (size_t done = initial; done < bytes;) {
        size_t part = bytes - done;
        if (part > sizeof(zeroes)) part = sizeof(zeroes);
        if (kernel_copy_to_user(call->mm, user + done, zeroes, part, &copied) || copied != part)
            return -KERNEL_EFAULT;
        done += part;
    }
    return sizeof(flags);
}

enum kernel_syscall_status syscall_handle_keys(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *result)
{
    struct key_call call = { .heap=caller->files.heap, .cred=kernel_credentials_current() };
    if (kernel_task_mm_borrow_mutable(caller, &call.mm)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    result->action = KERNEL_SYSCALL_ACTION_RETURN;
    const uint64_t *arg = request->arguments;
    uint64_t number = request->number;
    int32_t command = (int32_t)arg[0];
    if (number == 219U && command == KEYCTL_CAPABILITIES) {
        result->value = capabilities(&call, arg[1], arg[2]);
        return KERNEL_SYSCALL_STATUS_OK;
    }
    enum key_type type = KEY_RING;
    char short_desc[KEY_TEXT_INLINE], short_callout[KEY_TEXT_INLINE];
    char *description = NULL, *callout = NULL;
    void *payload = NULL;
    size_t payload_bytes = 0;
    int64_t value = 0;
    if (number == 217U && arg[3] >= 1024U * 1024U) value = -KERNEL_EINVAL;
    if (!value && (number != 219U || command == KEYCTL_SEARCH))
        value = copy_type(&call, number == 219U ? arg[2] : arg[0], &type);
    if (!value) {
        if (number == 217U && arg[1]) value = copy_text(&call, arg[1], short_desc, &description);
        else if (number == 218U) value = copy_text(&call, arg[1], short_desc, &description);
        else if (number == 219U && command == KEYCTL_SEARCH)
            value = copy_text(&call, arg[3], short_desc, &description);
        else if (number == 219U && command == KEYCTL_JOIN && arg[1])
            value = copy_text(&call, arg[1], short_desc, &description);
    }
    if (!value && number == 217U && type == KEY_RING && description && description[0] == '.')
        value = -KERNEL_EPERM;
    if (!value && number == 218U && arg[2])
        value = copy_text(&call, arg[2], short_callout, &callout);
    if (!value && (number == 217U || (number == 219U && command == KEYCTL_UPDATE))) {
        payload_bytes = arg[3];
        if (number == 219U && payload_bytes > BOAROS_PAGE_SIZE) value = -KERNEL_EINVAL;
        else value = copy_payload(&call, arg[2], payload_bytes, &payload);
    }
    if (!value && need_candidate(&call, number, arg)) {
        call.candidate = kernel_credentials_clone(call.heap, call.cred);
        if (call.candidate) call.cred = call.candidate;
        else value = -KERNEL_ENOMEM;
    }
    struct key_output output = {0};
    if (!value) {
        KERNEL_LOCK_SCOPE(guard);
        lock_keys(&guard);
        call.now = kernel_time_monotonic_ns() / UINT64_C(1000000000);
        collect_keys(call.now);
        if (number == 217U)
            value = add_locked(&call, type, description, &payload, payload_bytes, (int32_t)arg[4]);
        else if (number == 218U)
            value = request_locked(&call, type, description, callout != NULL, (int32_t)arg[3]);
        else
            value = control_locked(&call, arg, type, description, &payload, &output);
    }
    if (call.changed) kernel_credentials_publish(caller, call.candidate);
    else if (call.candidate) kernel_credentials_put(call.candidate);
    if (output.buffer) {
        if (copy_key_output(&call, &output)) value = -KERNEL_EFAULT;
        wipe_free(call.heap, output.buffer, output.sensitive);
    }
    if (description && description != short_desc) kernel_heap_release(call.heap, description);
    if (callout && callout != short_callout) wipe_free(call.heap, callout, KEY_DESC_MAX);
    wipe_free(call.heap, payload, payload_bytes);
    result->value = value;
    return KERNEL_SYSCALL_STATUS_OK;
}
