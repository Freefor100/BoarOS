#include <kernel/sync.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
/* Explicit host execution model for standalone block/lwext4 fixtures. Keep
 * the production lock calls intact; each device gets a real blocking mutex. */
struct model_mutex { struct kernel_rwlock *object; pthread_mutex_t mutex; struct model_mutex *next; };
static struct model_mutex *objects;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{
    assert(!pthread_mutex_lock(&registry));
    struct model_mutex *object = objects;
    while (object && object->object != lock) object = object->next;
    if (!object) {
        object = calloc(1, sizeof(*object)); assert(object);
        object->object = lock; assert(!pthread_mutex_init(&object->mutex, 0));
        object->next = objects; objects = object;
    }
    *lock = (struct kernel_rwlock){.rank = rank, .key = key};
    assert(!pthread_mutex_unlock(&registry));
}
extern void block_model_before_lock(struct kernel_rwlock *) __attribute__((weak));
static _Thread_local unsigned lock_depth;
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    assert(!guard->lock && !lock_depth++);
    if (block_model_before_lock) block_model_before_lock(lock);
    assert(!pthread_mutex_lock(&registry));
    struct model_mutex *object = objects;
    while (object && object->object != lock) object = object->next;
    assert(object); assert(!pthread_mutex_unlock(&registry));
    assert(!pthread_mutex_lock(&object->mutex));
    guard->lock = lock; guard->owner = (void *)object; guard->write = 1;
}
void kernel_lock_release(struct kernel_lock_guard *guard)
{
    assert(guard->lock && guard->owner && lock_depth-- == 1);
    struct model_mutex *object = (void *)guard->owner;
    guard->lock = 0; guard->owner = 0;
    assert(!pthread_mutex_unlock(&object->mutex));
}

/* 此块层模型没有用户task；保留真实提交钩子的字节观察。 */
_Thread_local uint64_t block_test_read_bytes, block_test_write_bytes;
void kernel_task_account_read(uint64_t bytes) { block_test_read_bytes += bytes; }
void kernel_task_account_write(uint64_t bytes) { block_test_write_bytes += bytes; }

static _Thread_local struct kernel_cpu model_cpu = {.initialized=KERNEL_CPU_INITIALIZED};
void *block_model_cpu(void) { return &model_cpu; }
