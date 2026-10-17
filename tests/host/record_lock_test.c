#define BOAROS_PAGE_SHIFT 12
#define BOAROS_RECORD_LOCK_MEASURE 1
#include "../../fs/record_lock.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static size_t live_nodes;
static int fail_alloc;
static int allocations_left = -1;
_Thread_local uintptr_t sync_test_irq = 1;

enum kernel_heap_status kernel_heap_allocate_zeroed(
    struct kernel_heap *heap, size_t count, size_t size, void **pointer)
{
    (void)heap;
    if (fail_alloc || allocations_left == 0) {
        *pointer = 0;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    *pointer = calloc(count, size);
    if (!*pointer) return KERNEL_HEAP_STATUS_EMPTY;
    if (allocations_left > 0) allocations_left--;
    live_nodes++;
    return KERNEL_HEAP_STATUS_OK;
}

void kernel_heap_release(struct kernel_heap *heap,
                         void *pointer)
{
    (void)heap;
    assert(pointer && live_nodes);
    live_nodes--;
    free(pointer);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    *queue = (struct kernel_wait_queue){ .initialized =
                                         KERNEL_WAIT_QUEUE_INITIALIZED };
}

enum kernel_scheduler_status kernel_wait_queue_wake_all(
    struct kernel_wait_queue *queue)
{
    assert(queue->initialized == KERNEL_WAIT_QUEUE_INITIALIZED);
    return KERNEL_SCHEDULER_STATUS_OK;
}

static uint32_t random_state = 0x583afb27U;

static uint32_t random_number(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static int verify_tree(struct kernel_record_lock *node,
                       struct kernel_record_lock *lower,
                       struct kernel_record_lock *upper,
                       size_t *count)
{
    if (!node) return 0;
    assert(!lower || before(lower, node));
    assert(!upper || before(node, upper));
    int left = verify_tree(node->left, lower, node, count);
    int right = verify_tree(node->right, node, upper, count);
    assert(left - right >= -1 && left - right <= 1);
    assert(node->height == 1 + (left > right ? left : right));
    int64_t max_end = node->end;
    if (node->left) max_end = maximum(max_end, node->left->max_end);
    if (node->right) max_end = maximum(max_end, node->right->max_end);
    assert(node->max_end == max_end);
    assert(node->start >= 0 && node->start <= node->end);
    assert(node->owner_previous && *node->owner_previous == node);
    assert(node->state);
    ++*count;
    return node->height;
}

static void verify_model(struct kernel_record_lock_state *state,
                         struct kernel_record_lock **heads,
                         int values[4][256], int *owners)
{
    size_t tree_count = 0, owner_count = 0;
    int actual[4][256] = {{0}};
    verify_tree(state->root, 0, 0, &tree_count);
    for (int owner = 0; owner < 4; ++owner) {
        for (struct kernel_record_lock *node = heads[owner]; node;
             node = node->owner_next) {
            assert(node->owner == &owners[owner]);
            assert(node->kind == (owner >= 2));
            assert(node->state == state);
            for (int pos = 0; pos < 256; ++pos)
                if (node->start <= pos && node->end >= pos) {
                    assert(!actual[owner][pos]);
                    actual[owner][pos] = node->type + 1;
                }
            if (node->owner_next)
                assert(node->owner_next->owner_previous == &node->owner_next);
            owner_count++;
        }
    }
    assert(tree_count == owner_count && tree_count == live_nodes);
    for (int owner = 0; owner < 4; ++owner)
        for (int pos = 0; pos < 256; ++pos)
            assert(actual[owner][pos] == values[owner][pos]);
    for (int owner = 0; owner < 4; ++owner) {
        for (int pos = 0; pos < 256; ++pos) {
            for (int type = 0; type < 2; ++type) {
                int expected = 0;
                for (int other = 0; other < 4; ++other)
                    if (other != owner && values[other][pos] &&
                        (type == 1 || values[other][pos] == 2))
                        expected = 1;
                struct kernel_record_lock_conflict result;
                assert(kernel_record_lock_get(state, &owners[owner],
                    owner >= 2, pos, pos, type, &result) == 0);
                assert((result.type != 2) == expected);
                if (expected) {
                    assert(result.start <= pos && result.end >= pos);
                    assert(result.type == 0 || result.type == 1);
                    if (type == 0) assert(result.type == 1);
                }
            }
        }
    }
}

static void expect_interval(struct kernel_record_lock_state *state,
                            const void *observer, int64_t position,
                            int16_t type, int64_t start, int64_t end)
{
    struct kernel_record_lock_conflict found;
    assert(kernel_record_lock_get(state, observer, 0, position, position,
                                   1, &found) == 0);
    assert(found.type == type);
    if (type != 2) assert(found.start == start && found.end == end);
}

static void test_edit_boundaries(struct kernel_heap *heap)
{
    struct kernel_record_lock_state state;
    struct kernel_record_lock *head = 0;
    int owner, observer;
    kernel_record_lock_state_init(&state);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   0, INT64_MAX, 0, heap) == 0);

    /* Failure after reserving the replacement must also return its node. */
    allocations_left = 1;
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   10, 20, 1, heap) == -KERNEL_ENOLCK);
    assert(live_nodes == 1);
    expect_interval(&state, &observer, 15, 0, 0, INT64_MAX);
    allocations_left = -1;
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   10, 20, 1, heap) == 0);
    expect_interval(&state, &observer, 0, 0, 0, 9);
    expect_interval(&state, &observer, 15, 1, 10, 20);
    expect_interval(&state, &observer, INT64_MAX, 0, 21, INT64_MAX);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   0, 0, 2, heap) == 0);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   INT64_MAX, INT64_MAX, 2, heap) == 0);
    expect_interval(&state, &observer, 0, 2, 0, 0);
    expect_interval(&state, &observer, INT64_MAX, 2, 0, 0);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   10, 20, 0, heap) == 0);
    expect_interval(&state, &observer, 15, 0, 1, INT64_MAX - 1);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   0, 0, 0, heap) == 0);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   INT64_MAX, INT64_MAX, 0, heap) == 0);
    expect_interval(&state, &observer, 15, 0, 0, INT64_MAX);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   1, INT64_MAX - 1, 2, heap) == 0);
    expect_interval(&state, &observer, 0, 0, 0, 0);
    expect_interval(&state, &observer, INT64_MAX, 0, INT64_MAX, INT64_MAX);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 7,
                                   1, INT64_MAX - 1, 0, heap) == 0);
    expect_interval(&state, &observer, 15, 0, 0, INT64_MAX);
    size_t count = 0;
    verify_tree(state.root, 0, 0, &count);
    assert(count == 1 && live_nodes == 1);
    kernel_record_lock_release(&state, &head, heap);
    assert(!head && !live_nodes && kernel_record_lock_state_empty(&state));
}

static void test_unrelated_inodes(struct kernel_heap *heap)
{
    enum { INODES = 1024 };
    struct kernel_record_lock_state state, unrelated[INODES];
    struct kernel_record_lock *head = 0;
    int owner, observer;
    kernel_record_lock_state_init(&state);
    for (int index = 0; index < INODES; ++index) {
        kernel_record_lock_state_init(&unrelated[index]);
        assert(kernel_record_lock_set(&unrelated[index], &head, &owner,
                                       0, 3, 0, 0, 0, heap) == 0);
    }
    record_lock_edit_visits = 0;
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 3,
                                   10, 19, 0, heap) == 0);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 3,
                                   30, 39, 0, heap) == 0);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 3,
                                   20, 29, 0, heap) == 0);
    assert(record_lock_edit_visits < 64);
    expect_interval(&state, &observer, 25, 0, 10, 39);
    assert(kernel_record_lock_set(&state, &head, &owner, 0, 3,
                                   20, 29, 1, heap) == 0);
    expect_interval(&state, &observer, 10, 0, 10, 19);
    expect_interval(&state, &observer, 25, 1, 20, 29);
    expect_interval(&state, &observer, 39, 0, 30, 39);
    kernel_record_lock_release(&state, &head, heap);
    assert(kernel_record_lock_state_empty(&state) && live_nodes == INODES);
    for (int index = 0; index < INODES; ++index) {
        size_t count = 0;
        verify_tree(unrelated[index].root, 0, 0, &count);
        assert(count == 1);
        expect_interval(&unrelated[index], &observer, 0, 0, 0, 0);
        kernel_record_lock_release(&unrelated[index], &head, heap);
        assert(kernel_record_lock_state_empty(&unrelated[index]));
    }
    assert(!head && !live_nodes);
}

static void test_owner_kinds(struct kernel_heap *heap)
{
    struct kernel_record_lock_state state;
    struct kernel_record_lock *heads[2] = {0};
    int owner;
    kernel_record_lock_state_init(&state);
    for (int kind = 0; kind < 2; ++kind)
        assert(kernel_record_lock_set(&state, &heads[kind], &owner, kind,
                                       11, 10, 20, 0, heap) == 0);
    assert(kernel_record_lock_set(&state, &heads[0], &owner, 0, 11,
                                   10, 20, 1, heap) == -KERNEL_EAGAIN);
    assert(kernel_record_lock_set(&state, &heads[0], &owner, 0, 11,
                                   0, INT64_MAX, 2, heap) == 0);
    assert(!heads[0] && heads[1] && live_nodes == 1);
    struct kernel_record_lock_conflict found;
    kernel_record_lock_get(&state, &owner, 0, 15, 15, 1, &found);
    assert(found.type == 0 && found.kind == 1 && found.pid == -1);
    kernel_record_lock_get(&state, &owner, 1, 15, 15, 1, &found);
    assert(found.type == 2);
    kernel_record_lock_release(&state, &heads[1], heap);
    assert(!heads[1] && !live_nodes && kernel_record_lock_state_empty(&state));
}

static void test_bulk_edits(struct kernel_heap *heap)
{
    enum { INTERVALS = 1024, LAST = (INTERVALS - 1) * 4 };
    struct kernel_record_lock_state state;
    struct kernel_record_lock *heads[2] = {0};
    int owners[2];
    kernel_record_lock_state_init(&state);
    for (int index = 0; index < INTERVALS; ++index) {
        assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 3,
                                       index * 4, index * 4, 0, heap) == 0);
        assert(kernel_record_lock_set(&state, &heads[1], &owners[1], 0, 4,
                                       index * 4 + 2, index * 4 + 2,
                                       0, heap) == 0);
    }
    /* Remove a broad range while preserving interleaved other-owner locks. */
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 3,
                                   1, LAST - 1, 2, heap) == 0);
    size_t count = 0;
    verify_tree(state.root, 0, 0, &count);
    assert(count == INTERVALS + 2 && live_nodes == count);
    expect_interval(&state, &owners[1], 0, 0, 0, 0);
    expect_interval(&state, &owners[1], 512, 2, 0, 0);
    expect_interval(&state, &owners[1], LAST, 0, LAST, LAST);
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 3,
                                   1, LAST - 1, 0, heap) == 0);
    count = 0;
    verify_tree(state.root, 0, 0, &count);
    assert(count == INTERVALS + 1 && live_nodes == count);
    expect_interval(&state, &owners[1], 512, 0, 0, LAST);
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 3,
                                   1000, 2000, 2, heap) == 0);
    count = 0;
    verify_tree(state.root, 0, 0, &count);
    assert(count == INTERVALS + 2 && live_nodes == count);
    expect_interval(&state, &owners[1], 999, 0, 0, 999);
    expect_interval(&state, &owners[1], 1500, 2, 0, 0);
    expect_interval(&state, &owners[1], 2001, 0, 2001, LAST);
    kernel_record_lock_release(&state, &heads[0], heap);
    assert(!heads[0] && live_nodes == INTERVALS);
    count = 0;
    verify_tree(state.root, 0, 0, &count);
    assert(count == INTERVALS);
    for (int index = 0; index < INTERVALS; ++index)
        expect_interval(&state, &owners[0], index * 4 + 2, 0,
                         index * 4 + 2, index * 4 + 2);
    kernel_record_lock_release(&state, &heads[1], heap);
    assert(!heads[1] && !live_nodes && kernel_record_lock_state_empty(&state));
}

int main(void)
{
    struct kernel_record_lock_state state;
    struct kernel_record_lock *heads[4] = {0};
    struct kernel_heap heap = {0};
    int owners[4] = {0};
    int values[4][256] = {{0}};
    kernel_record_lock_state_init(&state);
    for (int iteration = 0; iteration < 3000; ++iteration) {
        int owner = random_number() % 4;
        int a = random_number() % 256, b = random_number() % 256;
        if (a > b) { int temporary = a; a = b; b = temporary; }
        int type = random_number() % 3;
        int blocked = 0;
        for (int other = 0; other < 4 && !blocked; ++other) {
            if (other == owner || type == 2) continue;
            for (int pos = a; pos <= b; ++pos)
                if (values[other][pos] &&
                    (type == 1 || values[other][pos] == 2)) {
                    blocked = 1;
                    break;
                }
        }
        int result = kernel_record_lock_set(&state, &heads[owner],
            &owners[owner], owner >= 2, owner, a, b, type, &heap);
        assert(result == (blocked ? -KERNEL_EAGAIN : 0));
        if (!blocked)
            for (int pos = a; pos <= b; ++pos)
                values[owner][pos] = type == 2 ? 0 : type + 1;
        if (iteration % 31 == 0)
            verify_model(&state, heads, values, owners);
    }
    verify_model(&state, heads, values, owners);
    for (int owner = 0; owner < 4; ++owner) {
        kernel_record_lock_release(&state, &heads[owner], &heap);
        assert(!heads[owner]);
    }
    assert(kernel_record_lock_state_empty(&state) && !live_nodes);

    /* Both a new lock and a split unlock must be atomic on allocation failure. */
    fail_alloc = 1;
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
                                  10, 30, 1, &heap) == -KERNEL_ENOLCK);
    assert(kernel_record_lock_state_empty(&state));
    fail_alloc = 0;
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
                                  10, 30, 1, &heap) == 0);
    fail_alloc = 1;
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
                                  15, 20, 2, &heap) == -KERNEL_ENOLCK);
    struct kernel_record_lock_conflict result;
    kernel_record_lock_get(&state, &owners[1], 0, 16, 16, 1, &result);
    assert(result.type == 1 && result.start == 10 && result.end == 30);
    kernel_record_lock_release(&state, &heads[0], &heap);
    assert(kernel_record_lock_state_empty(&state) && !live_nodes);
    fail_alloc = 0;

    test_edit_boundaries(&heap);
    test_unrelated_inodes(&heap);
    test_owner_kinds(&heap);
    test_bulk_edits(&heap);

    /* Sparse keys must use the augmented maximum to avoid a full scan. */
    enum { SPARSE = 4096, OVERLAP = 512 };
    record_lock_edit_visits = 0;
    for (int index = 0; index < SPARSE; ++index)
        assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
            index * 3, index * 3, 0, &heap) == 0);
    assert(live_nodes == SPARSE);
    size_t sparse_edit_visits = record_lock_edit_visits;
    assert(sparse_edit_visits < SPARSE * 64);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[1], 0,
                           INT64_MAX, INT64_MAX, 1, &result);
    assert(result.type == 2 && record_lock_conflict_visits == 1);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[1], 0, 3072, 3072, 1, &result);
    assert(result.type == 0 && record_lock_conflict_visits < 32);
    record_lock_edit_visits = 0;
    assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
                                   3072, 3072, 1, &heap) == 0);
    assert(record_lock_edit_visits < 80);
    expect_interval(&state, &owners[1], 3072, 1, 3072, 3072);
    record_lock_remove_visits = 0;
    kernel_record_lock_release(&state, &heads[0], &heap);
    assert(!live_nodes && record_lock_remove_visits < SPARSE * 40);
    printf("record lock sparse: %d nodes, %zu bytes, %zu edit visits, "
           "%zu remove visits\n",
           SPARSE, SPARSE * sizeof(struct kernel_record_lock),
           sparse_edit_visits,
           record_lock_remove_visits);

    struct kernel_record_lock *overlap_heads[OVERLAP] = {0};
    int overlap_owners[OVERLAP] = {0};
    for (int index = 0; index < OVERLAP; ++index)
        assert(kernel_record_lock_set(&state, &overlap_heads[index],
            &overlap_owners[index], 0, index,
            0, INT64_MAX, 0, &heap) == 0);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[0], 0, 0, 0, 1, &result);
    assert(result.type == 0 && record_lock_conflict_visits < 32);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[0], 0, 0, 0, 0, &result);
    assert(result.type == 2 && record_lock_conflict_visits >= OVERLAP);
    for (int index = 0; index < OVERLAP; ++index)
        kernel_record_lock_release(&state, &overlap_heads[index], &heap);
    assert(kernel_record_lock_state_empty(&state) && !live_nodes);
    printf("record lock overlap: %d read owners, %zu no-conflict visits\n",
           OVERLAP, record_lock_conflict_visits);
    puts("record lock AVL/model/OOM passed");
}
