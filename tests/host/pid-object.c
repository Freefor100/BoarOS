#include <kernel/pid.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void encoding_boundaries(void)
{
    /* Protect the agreed allocation class, rather than member offsets. */
    assert(sizeof(struct kernel_pid) <= 64);
    const uint64_t generations[] = {1, UINT64_C(1) << 31, UINT64_C(1) << 33,
                                    UINT64_MAX >> 30};
    for (unsigned i = 0; i < sizeof(generations)/sizeof(generations[0]); i++) {
        uint64_t bitmap[1];
        struct kernel_pid_allocator numbers = {0};
        struct kernel_pid_registry registry = {.numbers = &numbers, .next_generation = generations[i]};
        struct kernel_pid id = {0};
        assert(kernel_pid_allocator_init(&numbers, bitmap, 1) == KERNEL_PID_STATUS_OK);
        assert(kernel_pid_reserve(&registry, &id) == KERNEL_PID_STATUS_OK);
        assert(id.generation == generations[i] && !id.published);
        assert(kernel_pid_publish_reserved(&id) == KERNEL_PID_STATUS_OK);
        assert(id.generation == generations[i] && id.published == 1);
        /* The full reference range remains independent of the state word. */
        id.references = UINT32_MAX - 1;
        kernel_pid_get(&id);
        assert(id.references == UINT32_MAX && id.generation == generations[i] && id.published == 1);
        for (unsigned bad = 0; bad < 2; bad++) {
            pid_t child = fork(); assert(child >= 0);
            if (!child) {
                if (bad) { id.references = 1; id.published = 3; }
                kernel_pid_get(&id);
                _exit(1);
            }
            int status; assert(waitpid(child, &status, 0) == child);
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
        }
        id.references = 1;
        kernel_pid_put(&id);
        assert(id.generation == generations[i] && id.published == 3 && !numbers.allocated);
        assert(kernel_pid_take_retired(&registry) == &id);
        assert(id.generation == generations[i] && !id.published && !id.references);
    }
}

static void published_iteration(void)
{
    enum { COUNT = 300 };
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(COUNT)];
    struct kernel_pid_allocator numbers = {0};
    struct kernel_pid_registry registry = {.numbers = &numbers, .next_generation = 1};
    struct kernel_pid ids[COUNT] = {0}, replacement = {0};
    assert(kernel_pid_allocator_init(&numbers, bitmap, COUNT) == KERNEL_PID_STATUS_OK);
    const unsigned published[] = {0, 62, 63, 64, 129, 256, 299};
    for (unsigned i = 0; i < COUNT; i++)
        assert(kernel_pid_reserve(&registry, &ids[i]) == KERNEL_PID_STATUS_OK);
    /* Private reservations fill the gaps, including whole bitmap words. */
    for (unsigned i = 0; i < sizeof(published)/sizeof(published[0]); i++)
        assert(kernel_pid_publish_reserved(&ids[published[i]]) == KERNEL_PID_STATUS_OK);
    kernel_pid_put(&ids[63]); /* Retired storage is still pending disposal. */
    uint64_t old_generation = ids[63].generation;
    assert(kernel_pid_publish(&registry, &replacement) == KERNEL_PID_STATUS_OK);
    assert(replacement.number == 64 && replacement.generation > old_generation);
    /* Published identities without a TID/TGID are returned for role filtering. */
    struct kernel_pid_allocator saved_numbers = numbers;
    uint32_t references[COUNT];
    for (unsigned i = 0; i < COUNT; i++) references[i] = ids[i].references;
    kernel_pid_t after = 0;
    for (unsigned i = 0; i < sizeof(published)/sizeof(published[0]); i++) {
        struct kernel_pid *expected = published[i] == 63 ? &replacement : &ids[published[i]];
        struct kernel_pid *next = kernel_pid_next_published(&registry, after);
        assert(next == expected);
        after = next->number;
    }
    assert(!kernel_pid_next_published(&registry, after));
    assert(!kernel_pid_next_published(&registry, INT32_MAX));
    assert(!kernel_pid_next_published(&registry, -1));
    assert(kernel_pid_next_published(&registry, 63) == &replacement);
    assert(kernel_pid_next_published(&registry, 64) == &ids[64]);
    assert(kernel_pid_next_published(&registry, 257) == &ids[299]);
    assert(numbers.cursor == saved_numbers.cursor && numbers.allocated == saved_numbers.allocated);
    for (unsigned i = 0; i < COUNT; i++) assert(references[i] == ids[i].references);
    for (unsigned i = 0; i < COUNT; i++)
        if (ids[i].published == 0 && ids[i].registry) kernel_pid_put(&ids[i]);
    /* Now the same continuation crosses true allocator holes and an empty word. */
    after = 0;
    for (unsigned i = 0; i < sizeof(published)/sizeof(published[0]); i++) {
        struct kernel_pid *next = kernel_pid_next_published(&registry, after);
        assert(next && next->number == (kernel_pid_t)(published[i] + 1U));
        after = next->number;
    }
    assert(!kernel_pid_next_published(&registry, after));
    kernel_pid_put(&replacement);
    for (unsigned i = 0; i < COUNT; i++) if (ids[i].registry) kernel_pid_put(&ids[i]);
    unsigned retired = 0;
    while (kernel_pid_take_retired(&registry)) retired++;
    assert(retired == COUNT + 1 && !numbers.allocated);
    assert(!kernel_pid_next_published(&registry, 0));
    puts("pid published iteration PASS: reservations, word boundaries, hash collision, retirement/reuse and read-only borrow");
}

/* Releasing a task must not recycle numbers owned by another role or a pin. */
int main(void)
{
    struct rlimit limit = {0}; assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    encoding_boundaries();
    published_iteration();
    uint64_t bitmap[1];
    struct kernel_pid_allocator numbers = {0};
    struct kernel_pid_registry registry = {.numbers = &numbers, .next_generation = 1};
    struct kernel_pid first = {0}, second = {0};
    struct kernel_pid_member tid = {0}, tgid = {0}, group = {0}, adopted = {0};
    struct kernel_pid_member session = {0}, peer = {0};
    int leader, survivor;
    assert(kernel_pid_allocator_init(&numbers, bitmap, 1) == KERNEL_PID_STATUS_OK);
    assert(kernel_pid_publish(&registry, &first) == KERNEL_PID_STATUS_OK);
    uint64_t generation = first.generation;
    kernel_pid_attach(&tid, &first, KERNEL_PID_TID, &leader);
    kernel_pid_attach(&group, &first, KERNEL_PID_PGID, &survivor);
    kernel_pid_attach(&tgid, &first, KERNEL_PID_TGID, &leader);
    kernel_pid_attach(&session, &first, KERNEL_PID_SID, &survivor);
    kernel_pid_attach(&peer, &first, KERNEL_PID_PGID, &leader);
    kernel_pid_detach(&tid);
    assert(!first.members[KERNEL_PID_TID]);
    assert(kernel_pid_find(&registry, 1) == &first);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_EXHAUSTED);
    assert(!second.registry && numbers.allocated == 1);
    kernel_pid_detach(&peer);
    kernel_pid_transfer(&group, &adopted, &survivor);
    assert(first.members[KERNEL_PID_PGID]->task == &survivor);
    kernel_pid_detach(&adopted);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_EXHAUSTED);
    kernel_pid_detach(&tgid);
    kernel_pid_detach(&session);
    kernel_pid_put(&first);
    assert(!kernel_pid_find(&registry, 1));
    assert(kernel_pid_take_retired(&registry) == &first);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_OK);
    assert(second.number == 1 && second.generation > generation);
    kernel_pid_put(&second);
    assert(numbers.allocated == 0);
    assert(kernel_pid_take_retired(&registry) == &second);
    assert(!kernel_pid_take_retired(&registry));
    registry.next_generation = (UINT64_MAX >> 30U) + 1;
    struct kernel_pid overflow = {0};
    assert(kernel_pid_publish(&registry, &overflow) == KERNEL_PID_STATUS_EXHAUSTED);
    assert(numbers.allocated == 0 && !overflow.registry);
    registry.next_generation = 100;
    struct kernel_pid reserved = {0};
    assert(kernel_pid_reserve(&registry, &reserved) == KERNEL_PID_STATUS_OK);
    assert(numbers.allocated == 1 && !kernel_pid_find(&registry, reserved.number));
    kernel_pid_attach(&tid, &reserved, KERNEL_PID_TID, &leader);
    assert(kernel_pid_publish_reserved(&reserved) == KERNEL_PID_STATUS_OK);
    assert(kernel_pid_find(&registry, reserved.number) == &reserved);
    assert(kernel_pid_publish_reserved(&reserved) == KERNEL_PID_STATUS_STATE);
    kernel_pid_detach(&tid);
    kernel_pid_put(&reserved);
    assert(kernel_pid_reserve(&registry, &reserved) == KERNEL_PID_STATUS_STATE);
    assert(kernel_pid_take_retired(&registry) == &reserved);
    struct kernel_pid aborted = {0};
    assert(kernel_pid_reserve(&registry, &aborted) == KERNEL_PID_STATUS_OK);
    kernel_pid_put(&aborted);
    assert(!numbers.allocated && !kernel_pid_find(&registry, aborted.number));
    assert(kernel_pid_take_retired(&registry) == &aborted);
    puts("pid object lifetime PASS");
}
