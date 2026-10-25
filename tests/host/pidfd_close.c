/* Actual OFD release/poll dispatch; PID observers have their own real tests. */
#include "../../fs/open_file_internal.h"
#include <kernel/credentials.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/pidfd.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED,.logical_id=1};
static struct kernel_task task;
static struct kernel_heap heap;
static struct kernel_wait_queue queue;
static unsigned services,closed,freed,epoll_notifications;
void *sync_test_cpu(void){return &cpu;}
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
void kernel_block_file_close(struct kernel_open_file_description *file)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD);}
void kernel_credentials_put(const struct kernel_credentials *credentials)
{assert(!credentials);}
void kernel_heap_release(struct kernel_heap *h,void *pointer)
{assert(h==&heap && pointer);freed++;free(pointer);}
void kernel_pidfd_close(struct kernel_open_file_description *file)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD);closed++;}
uint32_t kernel_pidfd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **out)
{assert(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD);*out=&queue;return KERNEL_POLLIN;}
void kernel_epoll_notify_file_release(struct kernel_open_file_description *file)
{assert(file->ep_items && services);epoll_notifications++;file->ep_items=0;}
int main(void)
{
    for(unsigned observed=0;observed<2;observed++){
        struct kernel_open_file_description *file=calloc(1,sizeof(*file));assert(file);
        file->heap=&heap;file->references=1;file->kind=KERNEL_OPEN_FILE_KIND_PIDFD;
        if(observed)file->ep_items=(void *)(uintptr_t)1;
        struct kernel_wait_queue *source=0;services=0;
        assert(kernel_open_file_poll(file,KERNEL_POLLIN,&source)==KERNEL_POLLIN && source==&queue && !services);
        assert(!kernel_open_file_release(&file) && !file);
        assert(services==observed && "unobserved pidfd last close unnecessarily entered CPU0 service");
    }
    assert(closed==2 && freed==2 && epoll_notifications==1 && cpu.logical_id==1);
    puts("PASS pidfd OFD poll/last close on caller CPU, epoll close retains control path");
}
