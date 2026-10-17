#include "record_lock.h"

#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/irq.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_record_lock {
    struct kernel_record_lock *left, *right;
    struct kernel_record_lock *owner_next;
    struct kernel_record_lock **owner_previous;
    struct kernel_record_lock_state *state;
    const void *owner;
    int64_t start, end, max_end;
    int32_t pid;
    int16_t type;
    int8_t height;
    uint8_t kind;
};

#ifdef BOAROS_RECORD_LOCK_MEASURE
static size_t record_lock_conflict_visits;
static size_t record_lock_remove_visits;
static size_t record_lock_edit_visits;
#endif

static int height(const struct kernel_record_lock *lock)
{
    return lock ? lock->height : 0;
}

static int64_t maximum(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static void update(struct kernel_record_lock *lock)
{
    int left = height(lock->left), right = height(lock->right);
    lock->height = (int8_t)(1 + (left > right ? left : right));
    lock->max_end = lock->end;
    if (lock->left)
        lock->max_end = maximum(lock->max_end, lock->left->max_end);
    if (lock->right)
        lock->max_end = maximum(lock->max_end, lock->right->max_end);
}

static struct kernel_record_lock *rotate_left(struct kernel_record_lock *root)
{
    struct kernel_record_lock *next = root->right;
    root->right = next->left;
    next->left = root;
    update(root);
    update(next);
    return next;
}

static struct kernel_record_lock *rotate_right(struct kernel_record_lock *root)
{
    struct kernel_record_lock *next = root->left;
    root->left = next->right;
    next->right = root;
    update(root);
    update(next);
    return next;
}

static struct kernel_record_lock *balance(struct kernel_record_lock *root)
{
    update(root);
    if (height(root->left) - height(root->right) > 1) {
        if (height(root->left->left) < height(root->left->right))
            root->left = rotate_left(root->left);
        return rotate_right(root);
    }
    if (height(root->right) - height(root->left) > 1) {
        if (height(root->right->right) < height(root->right->left))
            root->right = rotate_right(root->right);
        return rotate_left(root);
    }
    return root;
}

static int before(const struct kernel_record_lock *a,
                  const struct kernel_record_lock *b)
{
    return a->start < b->start ||
           (a->start == b->start && (uintptr_t)a < (uintptr_t)b);
}

static struct kernel_record_lock *tree_insert(struct kernel_record_lock *root,
                                               struct kernel_record_lock *lock)
{
    if (!root) return lock;
    if (before(lock, root)) root->left = tree_insert(root->left, lock);
    else root->right = tree_insert(root->right, lock);
    return balance(root);
}

static struct kernel_record_lock *extract_min(struct kernel_record_lock *root,
                                               struct kernel_record_lock **minimum)
{
    if (!root->left) {
        *minimum = root;
        return root->right;
    }
    root->left = extract_min(root->left, minimum);
    return balance(root);
}

static struct kernel_record_lock *tree_remove(struct kernel_record_lock *root,
                                               const struct kernel_record_lock *lock)
{
#ifdef BOAROS_RECORD_LOCK_MEASURE
    record_lock_remove_visits++;
#endif
    if (!root) __builtin_trap();
    if (root != lock) {
        if (before(lock, root)) root->left = tree_remove(root->left, lock);
        else root->right = tree_remove(root->right, lock);
        return balance(root);
    }
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    struct kernel_record_lock *next;
    struct kernel_record_lock *right = extract_min(root->right, &next);
    next->left = root->left;
    next->right = right;
    return balance(next);
}

/* A range removal can shrink either subtree by more than one level. */
static struct kernel_record_lock *join_root(struct kernel_record_lock *left,
                                             struct kernel_record_lock *root,
                                             struct kernel_record_lock *right)
{
    if (height(left) > height(right) + 1) {
        left->right = join_root(left->right, root, right);
        return balance(left);
    }
    if (height(right) > height(left) + 1) {
        right->left = join_root(left, root, right->left);
        return balance(right);
    }
    root->left = left;
    root->right = right;
    update(root);
    return root;
}

static struct kernel_record_lock *join_trees(struct kernel_record_lock *left,
                                              struct kernel_record_lock *right)
{
    if (!left) return right;
    if (!right) return left;
    struct kernel_record_lock *root;
    right = extract_min(right, &root);
    return join_root(left, root, right);
}

static void owner_insert(struct kernel_record_lock **head,
                         struct kernel_record_lock *lock)
{
    lock->owner_next = *head;
    lock->owner_previous = head;
    if (*head) (*head)->owner_previous = &lock->owner_next;
    __atomic_store_n(head,lock,__ATOMIC_RELEASE);
}

static void owner_remove(struct kernel_record_lock *lock)
{
    if (!lock->owner_previous) __builtin_trap();
    __atomic_store_n(lock->owner_previous,lock->owner_next,__ATOMIC_RELEASE);
    if (lock->owner_next)
        lock->owner_next->owner_previous = lock->owner_previous;
    lock->owner_next = 0;
    lock->owner_previous = 0;
}

static void publish(struct kernel_record_lock_state *state,
                    struct kernel_record_lock **owner_head,
                    struct kernel_record_lock *lock)
{
    lock->left = lock->right = 0;
    lock->height = 1;
    lock->max_end = lock->end;
    lock->state = state;
    owner_insert(owner_head, lock);
    state->root = tree_insert(state->root, lock);
}

static struct kernel_record_lock *conflict(struct kernel_record_lock *root,
                                            const void *owner, uint8_t kind,
                                            int64_t start, int64_t end,
                                            int16_t type)
{
    struct kernel_record_lock *found;
#ifdef BOAROS_RECORD_LOCK_MEASURE
    record_lock_conflict_visits++;
#endif
    if (!root || root->max_end < start) return 0;
    if (root->left && root->left->max_end >= start) {
        found = conflict(root->left, owner, kind, start, end, type);
        if (found) return found;
    }
    if (root->start <= end && root->end >= start &&
        (root->owner != owner || root->kind != kind) &&
        (type == 1 || root->type == 1)) return root;
    return root->start <= end
               ? conflict(root->right, owner, kind, start, end, type) : 0;
}

void kernel_record_lock_state_init(struct kernel_record_lock_state *state)
{
    state->root = 0;
    kernel_wait_queue_init(&state->waiters);
}

int kernel_record_lock_state_empty(const struct kernel_record_lock_state *state)
{
    return state->root == 0 && state->waiters.head == 0;
}

int kernel_record_lock_get(struct kernel_record_lock_state *state,
                           const void *owner, uint8_t kind,
                           int64_t start, int64_t end, int16_t type,
                           struct kernel_record_lock_conflict *found)
{
    struct kernel_record_lock *lock = conflict(state->root, owner, kind,
                                                 start, end, type);
    found->type = lock ? lock->type : 2;
    found->owner = lock ? lock->owner : 0;
    found->kind = lock ? lock->kind : 0;
    if (lock) {
        found->start = lock->start;
        found->end = lock->end;
        found->pid = lock->kind ? -1 : lock->pid;
    }
    return 0;
}

static struct kernel_record_lock *allocate(struct kernel_heap *heap)
{
    struct kernel_record_lock *lock = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        heap, 1, sizeof(*lock), (void **)&lock);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return 0;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return lock;
}

static void release(struct kernel_heap *heap, struct kernel_record_lock *lock)
{
    kernel_heap_release(heap, lock);
}

static int touching(const struct kernel_record_lock *lock,
                    int64_t start, int64_t end)
{
    return lock->start <= end && lock->end >= start;
}

static int adjacent(const struct kernel_record_lock *lock,
                    int64_t start, int64_t end)
{
    return (lock->end != INT64_MAX && lock->end + 1 == start) ||
           (end != INT64_MAX && end + 1 == lock->start);
}

/* Owner intervals are disjoint and equal types already coalesce. A left
 * extension cannot expose another predecessor; right extensions are still
 * ahead of an in-order walk. A containing different type cannot merge. */
static void prepare_edit(struct kernel_record_lock *root,
                         const void *owner, uint8_t kind, int16_t type,
                         int64_t *start, int64_t *end, size_t *needed)
{
    if (!root) return;
#ifdef BOAROS_RECORD_LOCK_MEASURE
    record_lock_edit_visits++;
#endif
    int64_t first = *start;
    if (type != 2 && first != INT64_MIN) first--;
    if (root->max_end < first) return;
    prepare_edit(root->left, owner, kind, type, start, end, needed);
    int64_t last = *end;
    if (type != 2 && last != INT64_MAX) last++;
    if (root->start > last) return;
    if (root->owner == owner && root->kind == kind) {
        if (type != 2 && root->type == type &&
            (touching(root, *start, *end) || adjacent(root, *start, *end))) {
            if (root->start < *start) *start = root->start;
            if (root->end > *end) *end = root->end;
        } else if (root->start < *start && root->end > *end) {
            ++*needed;
        }
    }
    prepare_edit(root->right, owner, kind, type, start, end, needed);
}

static struct kernel_record_lock *extract_owned(
    struct kernel_record_lock *root, const void *owner, uint8_t kind,
    int64_t start, int64_t end, struct kernel_record_lock **removed)
{
    if (!root) return 0;
#ifdef BOAROS_RECORD_LOCK_MEASURE
    record_lock_edit_visits++;
#endif
    if (root->max_end < start) return root;
    struct kernel_record_lock *left = extract_owned(root->left, owner, kind,
                                                      start, end, removed);
    if (root->start > end) return join_root(left, root, root->right);
    struct kernel_record_lock *right = extract_owned(root->right, owner, kind,
                                                       start, end, removed);
    if (root->owner != owner || root->kind != kind || root->end < start)
        return join_root(left, root, right);
    /* Keep owner membership intact until replacement. Detached tree links are
     * available for the edit list without adding another index or allocation. */
    root->left = *removed;
    *removed = root;
    return join_trees(left, right);
}

int kernel_record_lock_set(struct kernel_record_lock_state *state,
                           struct kernel_record_lock **owner_head,
                           const void *owner, uint8_t kind, int32_t pid,
                           int64_t start, int64_t end, int16_t type,
                           struct kernel_heap *heap)
{
    struct kernel_record_lock *spares = 0;
    struct kernel_record_lock *old;
    size_t needed = type == 2 ? 0U : 1U;
    if (type != 2 && conflict(state->root, owner, kind,
                              start, end, type)) return -KERNEL_EAGAIN;

    prepare_edit(state->root, owner, kind, type, &start, &end, &needed);
    for (size_t i = 0; i < needed; ++i) {
        struct kernel_record_lock *fresh = allocate(heap);
        if (!fresh) {
            while (spares) {
                fresh = spares;
                spares = spares->owner_next;
                release(heap, fresh);
            }
            return -KERNEL_ENOLCK;
        }
        fresh->owner_next = spares;
        spares = fresh;
    }

    old = 0;
    state->root = extract_owned(state->root, owner, kind, start, end, &old);
    while (old) {
        struct kernel_record_lock *next = old->left;
        int left = old->start < start, right = old->end > end;
        int64_t old_end = old->end;
        owner_remove(old);
        if (left) {
            old->end = start - 1;
            publish(state, owner_head, old);
        }
        if (right) {
            struct kernel_record_lock *tail;
            if (left) {
                if (!spares) __builtin_trap();
                tail = spares;
                spares = spares->owner_next;
                tail->owner = owner;
                tail->kind = kind;
                tail->pid = old->pid;
                tail->type = old->type;
            } else tail = old;
            tail->start = end + 1;
            tail->end = old_end;
            publish(state, owner_head, tail);
        } else if (!left) release(heap, old);
        old = next;
    }
    if (type != 2) {
        struct kernel_record_lock *fresh = spares;
        if (!fresh) __builtin_trap();
        spares = fresh->owner_next;
        fresh->owner = owner;
        fresh->kind = kind;
        fresh->pid = pid;
        fresh->type = type;
        fresh->start = start;
        fresh->end = end;
        publish(state, owner_head, fresh);
    }
    if (spares) __builtin_trap();
    if (kernel_wait_queue_wake_all(&state->waiters) !=
        KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    return 0;
}

void kernel_record_lock_release(struct kernel_record_lock_state *state,
                                struct kernel_record_lock **owner_head,
                                struct kernel_heap *heap)
{
    /* CPU0 worker可以开中断执行；摘除树/owner并通知的短操作自行排除抢占。 */
    KERNEL_IRQ_SCOPE(irq);
    struct kernel_record_lock *lock = *owner_head;
    int changed = 0;
    while (lock) {
        struct kernel_record_lock *next = lock->owner_next;
        if (lock->state == state) {
            state->root = tree_remove(state->root, lock);
            owner_remove(lock);
            release(heap, lock);
            changed = 1;
        }
        lock = next;
    }
    if (changed && kernel_wait_queue_wake_all(&state->waiters) !=
                       KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
