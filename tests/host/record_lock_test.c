#define BOAROS_PAGE_SHIFT 12
#define BOAROS_RECORD_LOCK_MEASURE 1
#include "../../fs/record_lock.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static size_t live_nodes;
static int fail_alloc;

enum kernel_heap_status kernel_heap_allocate_zeroed(
    struct kernel_heap *heap, size_t count, size_t size, void **pointer)
{
    (void)heap;
    if (fail_alloc) {
        *pointer = 0;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    *pointer = calloc(count, size);
    if (!*pointer) return KERNEL_HEAP_STATUS_EMPTY;
    live_nodes++;
    return KERNEL_HEAP_STATUS_OK;
}

enum kernel_heap_status kernel_heap_release(struct kernel_heap *heap,
                                            void *pointer)
{
    (void)heap;
    assert(pointer && live_nodes);
    live_nodes--;
    free(pointer);
    return KERNEL_HEAP_STATUS_OK;
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

    /* Sparse keys must use the augmented maximum to avoid a full scan. */
    enum { SPARSE = 4096, OVERLAP = 512 };
    for (int index = 0; index < SPARSE; ++index)
        assert(kernel_record_lock_set(&state, &heads[0], &owners[0], 0, 0,
            index * 3, index * 3, 0, &heap) == 0);
    assert(live_nodes == SPARSE);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[1], 0,
                           INT64_MAX, INT64_MAX, 1, &result);
    assert(result.type == 2 && record_lock_conflict_visits == 1);
    record_lock_conflict_visits = 0;
    kernel_record_lock_get(&state, &owners[1], 0, 3072, 3072, 1, &result);
    assert(result.type == 0 && record_lock_conflict_visits < 32);
    record_lock_remove_visits = 0;
    kernel_record_lock_release(&state, &heads[0], &heap);
    assert(!live_nodes && record_lock_remove_visits < SPARSE * 40);
    printf("record lock sparse: %d nodes, %zu bytes, %zu remove visits\n",
           SPARSE, SPARSE * sizeof(struct kernel_record_lock),
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
