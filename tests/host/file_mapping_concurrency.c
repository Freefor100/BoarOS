#include "../../fs/vfs_objects.h"
#include <kernel/file_mapping.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <kernel/errno.h>

void kernel_console_putc(char byte) { putchar(byte); }
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static struct kernel_vfs_node node;
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_vfs_instance instance;
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p, uint64_t *out) { *out=(uintptr_t)p; return 1; }
static struct kernel_file_mapping mapping;
static struct kernel_file_mapping tail[31];
static unsigned entered, resume, removed, attempted;
static void callback(void *owner, struct kernel_vfs_node *n, uint64_t size)
{
    assert(owner == &mapping && n == &node && size == 123);
    assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&resume, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!__atomic_load_n(&removed, __ATOMIC_ACQUIRE));
}
static void *notify(void *arg)
{
    (void)arg; mm_test_attach(1);
    kernel_file_mapping_notify(&node, 123);
    return 0;
}
static void *remove_mapping(void *arg)
{
    (void)arg; mm_test_attach(2);
    __atomic_store_n(&attempted, 1, __ATOMIC_RELEASE);
    kernel_file_mapping_unregister(&mapping);
    __atomic_store_n(&removed, 1, __ATOMIC_RELEASE);
    return 0;
}
static void *drain_tail(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    mm_test_attach(id);
    kernel_file_mapping_drain(&tail[id - 3]);
    return 0;
}
static void lifetime(unsigned records, unsigned actors)
{
    kernel_file_mapping_destroy(&node);
    memset(&node, 0, sizeof(node));
    node.instance=&instance; node.references=1;
    memset(&mapping, 0, sizeof(mapping));
    memset(tail, 0, sizeof(tail));
    entered = resume = removed = attempted = 0;
    kernel_rwlock_init(&node.io_lock, 30, (uintptr_t)&node);
    kernel_file_mapping_init(&node);
    assert(!kernel_file_mapping_prepare(&node));
    { KERNEL_TASK_LOCK_SCOPE(identity, &kernel_vfs_state_lock);
      assert(kernel_file_mapping_empty(&node)); }

    for (unsigned i = records - 1; i--;) {
        tail[i] = (struct kernel_file_mapping){.node=&node, .owner=&tail[i], .truncate=callback};
        assert(!kernel_file_mapping_register(&tail[i]));
    }
    mapping.node = &node; mapping.owner = &mapping; mapping.truncate = callback;
    kernel_file_mapping_register(&mapping);
    pthread_t notifier, remover;
    assert(!pthread_create(&notifier, 0, notify, 0));
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE)) sched_yield();
    unsigned parked = mm_test_parked(2);
    assert(!pthread_create(&remover, 0, remove_mapping, 0));
    while (!__atomic_load_n(&attempted, __ATOMIC_ACQUIRE)) sched_yield();
    /* 等待真实park或旧实现错误地提前完成，不能用sleep推测阻塞。 */
    while (mm_test_parked(2) == parked && !__atomic_load_n(&removed, __ATOMIC_ACQUIRE)) sched_yield();
    /* stop 后每个退休后继有真实借用；释放链不得递归或一次锁住整链。 */
    for (unsigned i = 0; i + 1 < records; i++) kernel_file_mapping_stop(&tail[i]);
    assert(mapping.borrows && (records == 1 || tail[records-2].borrows));
    pthread_t extras[2]; unsigned active = actors == 4 && records > 2 ? 2 : 0;
    for (unsigned i = 0; i < active; i++) {
        unsigned before = mm_test_parked(i + 3);
        assert(!pthread_create(&extras[i], 0, drain_tail, (void *)(uintptr_t)(i + 3)));
        while (mm_test_parked(i + 3) == before) sched_yield();
    }
    assert(!__atomic_load_n(&removed, __ATOMIC_ACQUIRE));
    __atomic_store_n(&resume, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(notifier, 0) && !pthread_join(remover, 0));
    for (unsigned i = 0; i < active; i++) assert(!pthread_join(extras[i], 0));
    assert(removed && kernel_file_mapping_empty(&node));
    for (unsigned i = 0; i + 1 < records; i++) {
        assert(!tail[i].borrows && !tail[i].retired_next && !tail[i].next);
        kernel_file_mapping_drain(&tail[i]);
        assert(!kernel_file_mapping_register(&tail[i]));
        kernel_file_mapping_unregister(&tail[i]);
    }
    assert(kernel_file_mapping_empty(&node));
}
static void *wait_epoch(void *arg)
{
    (void)arg; mm_test_attach(2);
    kernel_file_mapping_wait(&node);
    return 0;
}
static void epoch(void)
{
    KERNEL_LOCK_SCOPE(guard);
    kernel_rwlock_write(&node.io_lock, &guard);
    uint64_t before, after;
    assert(!kernel_file_mapping_epoch(&node, &before));
    kernel_file_mapping_close(&node);
    kernel_lock_release(&guard);
    assert(kernel_file_mapping_register(&mapping) == -KERNEL_EBUSY && kernel_file_mapping_empty(&node));
    pthread_t waiter; unsigned parked = mm_test_parked(2);
    assert(!pthread_create(&waiter, 0, wait_epoch, 0));
    while (mm_test_parked(2) == parked) sched_yield();
    kernel_rwlock_write(&node.io_lock, &guard);
    kernel_file_mapping_open(&node);
    kernel_lock_release(&guard);
    assert(!pthread_join(waiter, 0));
    assert(!kernel_file_mapping_epoch(&node, &after) && after == before + 1);
    assert(!kernel_file_mapping_register(&mapping));
    kernel_file_mapping_unregister(&mapping);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) &&
           !physical_page_allocator_bind_access(&pages,access) && !physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of)); instance.heap=&heap;
    uint64_t baseline=physical_page_available(&pages);
    const unsigned records[] = {1,8,32};
    for (unsigned actors = 2; actors <= 4; actors += 2)
        for (unsigned i = 0; i < 3; i++) lifetime(records[i], actors);
    epoch();
    kernel_file_mapping_destroy(&node);
    assert(physical_page_available(&pages)==baseline && !physical_page_allocator_audit(&pages));
    printf("file mapping %lu KiB: cursor/drain/epoch owners OK\n", (unsigned long)BOAROS_PAGE_SIZE / 1024U);
    return 0;
}
