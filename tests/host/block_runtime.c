#define _XOPEN_SOURCE 700
#include <kernel/sync.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
static _Thread_local struct kernel_io_context model_io;
struct kernel_io_context *kernel_io_context_current(void) { return &model_io; }
/* Production calls retain rank/owner nesting; pthread rwlocks implement the
 * standalone fixtures' sleeping execution model. */
struct model_lock { struct kernel_rwlock *object; pthread_rwlock_t lock; struct model_lock *next; };
static struct model_lock *objects;
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
void kernel_rwlock_init(struct kernel_rwlock *lock,uint32_t rank,uintptr_t key)
{
    assert(!pthread_mutex_lock(&registry));
    struct model_lock *object=objects;
    while (object && object->object!=lock)object=object->next;
    if (!object) {
        object=calloc(1,sizeof(*object));assert(object);
        object->object=lock;assert(!pthread_rwlock_init(&object->lock,0));
        object->next=objects;objects=object;
    }
    *lock=(struct kernel_rwlock){.rank=rank,.key=key};
    assert(!pthread_mutex_unlock(&registry));
}
static struct model_lock *lookup(struct kernel_rwlock *lock)
{
    assert(!pthread_mutex_lock(&registry));
    struct model_lock *object=objects;
    while (object && object->object!=lock)object=object->next;
    assert(object);assert(!pthread_mutex_unlock(&registry));return object;
}
extern void block_model_before_lock(struct kernel_rwlock *) __attribute__((weak));
extern void block_model_lock_waiting(struct kernel_rwlock *) __attribute__((weak));
static void acquire(struct kernel_rwlock *lock,struct kernel_lock_guard *guard,int write)
{
    assert(!guard->lock);
    struct kernel_lock_guard *previous=model_io.locks;
    assert(!previous || previous->lock->rank<lock->rank ||
        (previous->lock->rank==lock->rank && previous->lock->key<lock->key));
    if (block_model_before_lock)block_model_before_lock(lock);
    struct model_lock *object=lookup(lock);
    int result=write?pthread_rwlock_trywrlock(&object->lock):pthread_rwlock_tryrdlock(&object->lock);
    if (result) {
        assert(result==EBUSY);
        if (block_model_lock_waiting)block_model_lock_waiting(lock);
        assert(!(write?pthread_rwlock_wrlock(&object->lock):pthread_rwlock_rdlock(&object->lock)));
    }
    *guard=(struct kernel_lock_guard){.lock=lock,.owner=&model_io,.previous=previous,.write=write};
    model_io.locks=guard;
}
void kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{ acquire(lock,guard,1); }
void kernel_rwlock_read(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{ acquire(lock,guard,0); }
void kernel_lock_release(struct kernel_lock_guard *guard)
{
    assert(guard->lock && guard->owner==&model_io && model_io.locks==guard);
    struct model_lock *object=lookup(guard->lock);model_io.locks=guard->previous;
    *guard=(struct kernel_lock_guard){0};assert(!pthread_rwlock_unlock(&object->lock));
}
_Thread_local uint64_t block_test_read_bytes,block_test_write_bytes;
void kernel_task_account_read(uint64_t bytes) { block_test_read_bytes+=bytes; }
void kernel_task_account_write(uint64_t bytes) { block_test_write_bytes+=bytes; }
static _Thread_local struct kernel_cpu model_cpu={.initialized=KERNEL_CPU_INITIALIZED};
void *block_model_cpu(void) { return &model_cpu; }
