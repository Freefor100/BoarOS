/* Production OFD initialization/generation/release and PID registry/view owners.
 * The VFS copy/close, heap and executor edges are controlled boundaries. */
#include "../../kernel/sched/private.h"
#include "../../fs/open_file.c"
#include <kernel/credentials.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/pidfd.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void kernel_open_file_async_close(struct kernel_open_file_description *file)
{ assert(file && !file->async_io); }

_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED,.logical_id=1};
static struct kernel_task task;
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_namespace ipc={.references=1,.type=KERNEL_NS_IPC};
static struct kernel_pid_registry view;
static unsigned copy_failure,close_failure,snapshots;
static struct kernel_heap heap;
static struct kernel_wait_queue queue;
static unsigned services,closed,freed,epoll_notifications;
void *sync_test_cpu(void){return &cpu;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound); }
void *sync_test_current_task(void){return &task;}
struct kernel_task *kernel_task_current(void){return &task;}
struct kernel_io_context *kernel_io_context_current(void){return &task.io_context;}
int kernel_task_has_user_context(const struct kernel_task *t){return t==&task;}
int kernel_scheduler_user_service_active(void){return 0;}
void kernel_service_scope_begin(struct kernel_service_scope *scope)
{assert(!scope->entered);scope->entered=1;services++;}
void kernel_service_scope_end(struct kernel_service_scope *scope){scope->entered=0;}
void kernel_assert_service_context(void){assert(services);}
int kernel_socket_is_unix(const struct kernel_socket *socket){(void)socket;return 0;}
enum kernel_task_status kernel_task_mm_borrow(const struct kernel_task *t,const struct kernel_mm **mm)
{(void)t;(void)mm;return KERNEL_TASK_STATUS_STATE;}
struct kernel_page_cache *kernel_vfs_file_page_cache(const struct kernel_vfs_file *file)
{(void)file;return 0;}
int kernel_vfs_namespace_requires_cpu0(const struct kernel_vfs_mount *mount)
{(void)mount;return 0;}
struct kernel_vfs_node *kernel_vfs_file_node(const struct kernel_vfs_file *file)
{(void)file;return 0;}
int kernel_page_cache_needs_cpu0(const struct kernel_page_cache *cache)
{(void)cache;return 0;}
void kernel_vfs_lease_release(struct kernel_vfs_file *file)
{ assert(!file->lease_owner); }
void kernel_vfs_notify_close(const struct kernel_vfs_file *file, uint32_t mask, struct kernel_vfs_path **backing)
{ (void)file; (void)backing; (void)mask; abort(); }
void kernel_block_file_close(struct kernel_open_file_description *file)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_GENERATED);}
void kernel_flock_release(struct kernel_flock_owner *owner)
{assert(!owner->context);}
void kernel_flock_destroy(struct kernel_flock_identity *identity)
{assert(!identity->context);}
void kernel_credentials_put(const struct kernel_credentials *credentials)
{assert(!credentials);}
void kernel_heap_release(struct kernel_heap *h,void *pointer)
{assert((h==&heap || h==&scheduler.identity_heap) && pointer);freed++;free(pointer);}
void kernel_pidfd_close(struct kernel_open_file_description *file)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD);closed++;}
uint32_t kernel_pidfd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **out)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD);*out=&queue;return KERNEL_POLLIN;}
void kernel_epoll_notify_file_release(struct kernel_open_file_description *file)
{assert(file->ep_items && services);epoll_notifications++;file->ep_items=0;}

const struct kernel_credentials *kernel_credentials_current(void){return NULL;}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *cred){assert(!cred);return NULL;}
struct kernel_namespace *kernel_task_ipc_namespace(void){return &ipc;}
int kernel_procfs_ipc_file(const struct kernel_vfs_file *f){(void)f;return 1;}
int kernel_procfs_shm_limits_open(const struct kernel_vfs_file *f, struct kernel_shm_limits **owner)
{ (void)f; assert(!*owner); return 0; }
void kernel_ipc_shm_limits_put(struct kernel_shm_limits *limits) { assert(!limits); }
int kernel_procfs_kpageflags_file(const struct kernel_vfs_file *f){(void)f;return 0;}
int kernel_procfs_pagemap_open(const struct kernel_vfs_file *f, struct kernel_heap *heap,
    struct kernel_mm_pagemap **view)
{ (void)f; (void)heap; (void)view; return 0; }
void kernel_mm_pagemap_put(struct kernel_mm_pagemap *view)
{ (void)view; assert(!"pagemap is outside this fixture"); }
int kernel_vfs_file_generated(const struct kernel_vfs_file *f){(void)f;return 1;}
uint64_t kernel_vfs_mount_error_sequence(const struct kernel_vfs_mount *m){(void)m;return 0;}
uint64_t kernel_vfs_error_sequence(const struct kernel_vfs_file *f){(void)f;return 0;}
int kernel_vfs_close(struct kernel_vfs_file *f){(void)f;if(close_failure){close_failure=0;return -1;}return 0;}
void kernel_rwlock_init(struct kernel_rwlock *lock,uint32_t rank,uintptr_t key)
{memset(lock,0,sizeof(*lock));lock->rank=rank;lock->key=key;}
void exited_append_locked(struct kernel_task *t){(void)t;abort();}
void kernel_scheduler_notify_cleanup(void){abort();}
int kernel_vfs_file_snapshot(const struct kernel_vfs_file *file,struct kernel_heap *h,
    struct kernel_namespace *ns,struct kernel_pid_registry *pid_view,char **buffer,size_t *length)
{
    (void)file;assert(h==&heap&&ns==&ipc&&pid_view==&view);snapshots++;
    if(copy_failure)return -KERNEL_EIO;
    *buffer=calloc(1,8);assert(*buffer);*length=7;return 0;
}
int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    uint64_t root_bits[KERNEL_PID_BITMAP_WORDS(64)]={0},bits[KERNEL_PID_BITMAP_WORDS(64)]={0};
    struct kernel_pid_allocator numbers={0};
    assert(!kernel_pid_allocator_init(&scheduler.pid_allocator,root_bits,64));
    assert(!kernel_pid_registry_init(&scheduler.identities,&scheduler.pid_allocator,NULL));
    assert(!kernel_pid_allocator_init(&numbers,bits,64));
    assert(!kernel_pid_registry_init(&view,&numbers,&scheduler.identities));
    struct kernel_pid *id=calloc(1,kernel_pid_storage_size(&view));assert(id);task.group_leader=&task;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     assert(!kernel_pid_reserve_numbers(&view,id,kernel_pid_storage_size(&view),NULL,0));
     assert(!kernel_pid_publish_reserved(id));kernel_pid_attach(&task.identities[KERNEL_PID_TID],id,KERNEL_PID_TID,&task);}
    struct kernel_open_file_description *file=calloc(1,sizeof(*file));assert(file);
    assert(!finish_path_open(file,&heap));
    assert(file->ipc_proc_namespace==&ipc&&file->ipc_proc_pid_view==&view&&ipc.references==2&&view.references==3);
    struct kernel_open_file_description *alias=file;assert(!kernel_open_file_acquire(alias));
    assert(view.references==3&&!kernel_open_file_release(&alias)&&!alias&&view.references==3);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_detach(&task.identities[KERNEL_PID_TID]);kernel_pid_put(id);kernel_pid_registry_put(&view);}
    process_identity_collect();assert(view.references==1&&!numbers.allocated);
    copy_failure=1;assert(kernel_open_file_generate(file)==-KERNEL_EIO&&!file->generated_ready&&view.references==1);
    copy_failure=0;assert(!kernel_open_file_generate(file)&&snapshots==2&&view.references==1);
    close_failure=1;assert(kernel_open_file_release(&file)==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED&&file&&view.references==1&&ipc.references==2);
    assert(!kernel_open_file_release(&file)&&!file&&!view.references&&ipc.references==1);
    assert(!scheduler.identities.retired&&!scheduler.identities.retired_namespaces);
    puts("PASS real IPC proc OFD opener PID view, alias/exit retention, snapshot failure and retry close owners");return 0;
}
