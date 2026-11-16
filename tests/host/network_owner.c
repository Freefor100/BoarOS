/* Actual socket/ethernet/lwIP paths. Host heap, clock, callback wait queues and NIC
 * capacity are boundary models; TCP ownership and work queues are production. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <kernel/random.h>
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>
#include "lwip/inet_chksum.h"
#include "lwip/tcp.h"
static err_t copy_capacity_tcp_writev(struct tcp_pcb *,
    const struct kernel_socket_write_segment *,unsigned,u16_t,u8_t,u16_t *);
static err_t held_completion_tcp_output(struct tcp_pcb *, u8_t);
#define boaros_lwip_tcp_writev copy_capacity_tcp_writev
#define boaros_lwip_tcp_output_more(pcb, more) held_completion_tcp_output(pcb, more)
#include "../../net/socket.c"
#undef boaros_lwip_tcp_output_more
#undef boaros_lwip_tcp_writev
#include "../../net/ethernet.c"
#include "../../kernel/pid.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
static struct {unsigned references,calls;uint32_t keep;} raw_filter_test;
static pthread_mutex_t raw_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t raw_changed=PTHREAD_COND_INITIALIZER;
static unsigned raw_pause,raw_filter_entered,raw_close_waiting,raw_resume;
static _Thread_local unsigned raw_closing_actor;
static struct kernel_socket *accept_binding_probe;
static unsigned accept_binding_changed;
void __wrap_kernel_bpf_program_get(struct kernel_bpf_program *program)
{CHECK(program==(void *)&raw_filter_test);__atomic_fetch_add(&raw_filter_test.references,1,__ATOMIC_RELAXED);}
void __wrap_kernel_bpf_program_put(struct kernel_bpf_program *program)
{CHECK(program==(void *)&raw_filter_test && __atomic_fetch_sub(&raw_filter_test.references,1,__ATOMIC_RELAXED));}
uint32_t __wrap_kernel_bpf_filter(struct kernel_bpf_program *program,const struct kernel_bpf_packet *packet)
{
    CHECK(program==(void *)&raw_filter_test && raw_filter_test.references);
    CHECK(packet->protocol==0x0800 && packet->network_offset==0 && packet->payload_offset==28 && packet->length>=28);
    unsigned char header[20];CHECK(!packet->read(packet->object,0,header,sizeof(header)));
    CHECK(header[0]==0x45 && header[9]==1);
    raw_filter_test.calls++;
    CHECK(!pthread_mutex_lock(&raw_gate));
    if(raw_pause) {
        raw_filter_entered=1;CHECK(!pthread_cond_broadcast(&raw_changed));
        while(!raw_resume)CHECK(!pthread_cond_wait(&raw_changed,&raw_gate));
        raw_pause=0;
    }
    CHECK(!pthread_mutex_unlock(&raw_gate));return raw_filter_test.keep;
}
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
static struct kernel_heap test_heap;
static uint64_t model_pid_bitmap[KERNEL_PID_BITMAP_WORDS(1024)];
static struct kernel_pid_allocator model_pid_numbers;
static struct kernel_pid_registry model_pid_registry={.numbers=&model_pid_numbers,.next_generation=1};
static struct kernel_pid model_pid;
static struct kernel_uid_account model_uid_account={.references=1};
static struct kernel_credentials model_credentials={.effective=KERNEL_CAP_VALID_MASK,.account=&model_uid_account};
struct model_socket_file {
    struct model_socket_file *next;
    struct kernel_socket *socket;
    uint32_t references;
};
static struct model_socket_file *model_socket_files;
static unsigned regular_rights_no_graph, ordinary_control_no_graph, graph_acquisitions;
static uint64_t model_nofile_limit=KERNEL_RLIMIT_NOFILE_CAP;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct model_socket_file *target;
    unsigned fail, pause, entered, resume, attempts;
} control_release_probe={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
static _Thread_local struct kernel_io_context test_context;
_Thread_local uintptr_t network_host_irq = RISCV_SSTATUS_SIE;
_Thread_local unsigned network_host_irq_saves,network_host_irq_restores;
static unsigned heap_allocations,protocol_services,allow_ofd_release,heap_fail_after;
static size_t heap_last_allocation_size;
static unsigned space_callbacks;
static _Thread_local unsigned reservation_actor,state_acquires;
static struct kernel_socket *lookup_close_target;
static struct kernel_socket *reset_retry_socket;
static unsigned reset_retry_observations;
static unsigned lookup_close_done;
static struct {
    struct kernel_socket *target,*peer;
    const unsigned char *bytes;
    unsigned length,observed;
} unix_close_name_probe;
static pthread_mutex_t reservation_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reservation_changed=PTHREAD_COND_INITIALIZER;
static unsigned reservation_first, reservation_second, reservation_release;
static pthread_mutex_t remote_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t remote_changed=PTHREAD_COND_INITIALIZER;
static unsigned remote_work,remote_done,remote_mode,remote_testing,remote_parked;
static unsigned copy_waiting,copy_release,stop_waiting,stop_done,stop_borrows;
static unsigned batch_waiting,batch_release;
static struct {
    struct kernel_socket *socket;
    unsigned entered, release;
} completion_output;
static struct kernel_socket *remote_socket;
static struct kernel_wait_queue *stop_probe_queue;
static unsigned stop_probe_notified;
static unsigned notify_testing,notify_entered,notify_release,notify_stopping;
static struct kernel_wait_queue notify_queue;
static struct kernel_wait_queue model_work_queue;
static _Thread_local struct kernel_task *remote_task;
static _Thread_local struct remote_waiter {
    struct kernel_socket_write_request *request;
    unsigned notified;
} remote_waiter;
static void reservation_arrive(void)
{
    CHECK(!pthread_mutex_lock(&reservation_gate));
    if(reservation_actor==1) {
        reservation_first=1;CHECK(!pthread_cond_broadcast(&reservation_changed));
        while(!reservation_release)CHECK(!pthread_cond_wait(&reservation_changed,&reservation_gate));
    } else {reservation_second=1;CHECK(!pthread_cond_broadcast(&reservation_changed));}
    CHECK(!pthread_mutex_unlock(&reservation_gate));
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    if(lock==&socket_state_lock) {
        state_acquires++;
        /* unix_lookup owns its target reference before connect reacquires the
         * endpoint lock. Close exactly at that ownership boundary. */
        if(lookup_close_target && lookup_close_target->unix_references==2 && !kernel_cpu_current()->raw_locks) {
            struct kernel_socket *target=lookup_close_target;lookup_close_target=NULL;
            kernel_socket_destroy(target);lookup_close_done++;
        }
    }
    if(reservation_actor==2)reservation_arrive();
    __real_kernel_raw_lock_acquire(lock,guard);
    if(lock==&socket_state_lock && reset_retry_socket &&
       reset_retry_socket->work_links[receive_wait.slot].queued) {
        CHECK(reset_retry_socket->receive_retry);
        reset_retry_observations++;
    }
}
uint64_t kernel_cost_clock(void) {return 0;}
struct kernel_cost_tag kernel_cost_capture(void) {return (struct kernel_cost_tag){0};}
void kernel_cost_add_tag(struct kernel_cost_tag tag,enum kernel_cost_metric metric,uint64_t value)
{(void)tag;kernel_cost_add(metric,value);}
struct kernel_cost_scope kernel_cost_enter(enum kernel_cost_metric metric) {return (struct kernel_cost_scope){.metric=metric};}
void kernel_cost_leave(struct kernel_cost_scope *scope) {(void)scope;}
void kernel_cost_raw_lock(struct kernel_cost_tag tag,unsigned rank,uint64_t wait,uint64_t hold)
{(void)tag;(void)rank;(void)wait;(void)hold;}

void kernel_cost_add(enum kernel_cost_metric metric,uint64_t value)
{ if (metric==COST_NETWORK_SERVICE_CALLS) protocol_services+=(unsigned)value; }
static size_t user_remaining = SIZE_MAX;
static struct kernel_socket *close_during_copy;
static struct kernel_socket *shutdown_during_copy;
static struct kernel_socket *read_shutdown_during_copy;
static struct kernel_socket *unix_credit_copy_probe;
static unsigned unix_credit_copy_observed;
static unsigned heap_live, dma_eligible, nic_full, copy_attempts, copied_packets, segment_attempts, captured_size;
static void (*heap_release_once)(void *);
static uint64_t clock_ns = 1000000000ULL;
static unsigned char captured[1536];
static unsigned fixed_random;
static uint32_t random_value;

void kernel_assert_can_block(void) {CHECK(!kernel_cpu_current()->raw_locks);}
#if BOAROS_MAX_CPUS > 1
void kernel_assert_service_context(void) {CHECK(!kernel_cpu_current()->logical_id);}
void kernel_cpu_flush_notifications(void) {CHECK(!kernel_cpu_current()->outbound);}
#endif
int kernel_wait_cursor_prepare(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor)
{
    CHECK(queue->initialized);
    KERNEL_RAW_SCOPE(guard,&queue->lock);
    if(queue->head) {queue->borrows++;queue->head->references++;*cursor=(struct kernel_wait_cursor){.queue=queue,.node=queue->head};return 1;}
    return 0;
}
int kernel_wait_cursor_prepare_event(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor,uint64_t event,struct kernel_task *recipient)
{
    CHECK(!recipient);
    int prepared=kernel_wait_cursor_prepare(queue,cursor);
    if(prepared)cursor->event=event;
    return prepared;
}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *cursor,int all)
{
    CHECK(all && cursor->queue->borrows && !kernel_cpu_current()->raw_locks);
    if(cursor->queue==stop_probe_queue)stop_probe_notified++;
    else {
        struct kernel_wait_node *node=cursor->node;
        CHECK(node && node->references);
        while(node) {
            struct kernel_wait_node *next=node->next;
            if(next)next->references++;
            CHECK(node->callback);
            if(node->callback==unix_space_notify)space_callbacks++;
            node->callback(node,KERNEL_WAIT_WOKEN,cursor->event);
            CHECK(node->references);node->references--;node=next;
        }
    }
    if(cursor->queue==stop_probe_queue) {CHECK(cursor->node->references);cursor->node->references--;}
    if(notify_testing && cursor->queue==&notify_queue) {
        CHECK(!pthread_mutex_lock(&remote_gate));
        {KERNEL_RAW_SCOPE(guard,&cursor->queue->lock);cursor->queue->borrows--;}
        CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    } else {KERNEL_RAW_SCOPE(guard,&cursor->queue->lock);cursor->queue->borrows--;}
    *cursor=(struct kernel_wait_cursor){0};
}
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{ *lock=(struct kernel_rwlock){.rank=rank,.key=key}; }
static pthread_mutex_t model_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t model_rights_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t model_unix_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *model_lock(struct kernel_rwlock *lock)
{
    if(lock==&protocol_mutex.lock)return &model_mutex;
    if(lock==&rights_mutex.lock)return &model_rights_mutex;
    CHECK(lock==&unix_control.lock);return &model_unix_mutex;
}
static pthread_mutex_t handshake=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handshake_changed=PTHREAD_COND_INITIALIZER;
static unsigned competing, waiter_registered, acquired;
static struct kernel_socket *handoff_peer;
static unsigned handoff_bytes;
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    CHECK(!guard->lock);
    if(raw_closing_actor && lock==&protocol_mutex.lock) {
        CHECK(!pthread_mutex_lock(&raw_gate));raw_close_waiting=1;
        CHECK(!pthread_cond_broadcast(&raw_changed) && !pthread_mutex_unlock(&raw_gate));
    }
    if(lock==&rights_mutex.lock) {
        CHECK(!ordinary_control_no_graph);__atomic_fetch_add(&graph_acquisitions,1,__ATOMIC_RELAXED);
    }
    if(competing && lock==&protocol_mutex.lock) {
        CHECK(!pthread_mutex_lock(&handshake)); waiter_registered=1;
        CHECK(!pthread_cond_signal(&handshake_changed)); CHECK(!pthread_mutex_unlock(&handshake));
    }
    CHECK(!pthread_mutex_lock(model_lock(lock))); CHECK(!lock->writer);
    lock->writer=&test_context; *guard=(struct kernel_lock_guard){.lock=lock,.owner=&test_context};
    if(lock==&protocol_mutex.lock)acquired++;
}
int kernel_rwlock_try_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    CHECK(!guard->lock);
    if (pthread_mutex_trylock(model_lock(lock))) return 0;
    CHECK(!lock->writer);
    lock->writer=&test_context; *guard=(struct kernel_lock_guard){.lock=lock,.owner=&test_context};
    if(lock==&protocol_mutex.lock)acquired++;
    return 1;
}
void kernel_lock_release(struct kernel_lock_guard *guard)
{
    CHECK(guard->lock && guard->owner==&test_context && guard->lock->writer==&test_context);
    if (handoff_peer && guard->lock==&protocol_mutex.lock) { CHECK(handoff_peer->rx_bytes == handoff_bytes); handoff_peer=NULL; }
    pthread_mutex_t *mutex=model_lock(guard->lock);
    guard->lock->writer=0; guard->lock=0; CHECK(!pthread_mutex_unlock(mutex));
}
static int work_pending_snapshot(void)
{ struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope); int result=kernel_socket_work_pending(); kernel_socket_protocol_leave(&scope); return result; }
struct kernel_io_context *kernel_io_context_current(void) { return &test_context; }
struct kernel_task *kernel_task_current(void) { return remote_task; }
const struct kernel_credentials *kernel_credentials_current(void)
{
    return &model_credentials;
}
/* The host boundary has one published process identity and one UID account.
 * Ancillary snapshots borrow them with the same reference ownership as tasks. */
int kernel_task_identity_get(int32_t pid,struct kernel_pid **owner)
{
    if(!owner)return -KERNEL_EINVAL;
    *owner=NULL;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_pid *identity=kernel_pid_find(&model_pid_registry,pid?pid:model_pid.number);
    if(!identity)return -KERNEL_ESRCH;
    kernel_pid_get(identity);*owner=identity;return 0;
}
void kernel_task_identity_put(struct kernel_pid *identity)
{
    CHECK(identity==&model_pid && identity->references>1);
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_pid_put(identity);
}
struct kernel_uid_account *kernel_uid_account_get_locked(struct kernel_uid_account *account)
{
    CHECK(account==&model_uid_account && account->references && account->references<UINT64_MAX);
    account->references++;return account;
}
void kernel_uid_account_put(struct kernel_uid_account *account)
{
    CHECK(account==&model_uid_account);
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);CHECK(account->references>1);account->references--;
}
enum kernel_task_status kernel_task_get_rlimit(const struct kernel_task *task,uint32_t resource,
    struct kernel_rlimit64 *limit)
{
    if(task!=kernel_task_current() || resource!=KERNEL_RLIMIT_NOFILE || !limit)
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    *limit=(struct kernel_rlimit64){.current=model_nofile_limit,.maximum=KERNEL_RLIMIT_NOFILE_CAP};
    return KERNEL_TASK_STATUS_OK;
}
int kernel_credentials_capable(const struct kernel_credentials *credentials, unsigned capability)
{ return capability <= KERNEL_CAP_LAST_CAP && (credentials->effective & (UINT64_C(1)<<capability)) != 0; }
enum kernel_signal_status kernel_signal_send_task(struct kernel_task *task, uint32_t signal, kernel_pid_t sender)
{ (void)task; (void)signal; (void)sender; abort(); }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t n, size_t size, void **out)
{ CHECK(heap == &test_heap);
  if(accept_binding_probe && size==sizeof(struct kernel_socket)) {
      struct kernel_socket *listener=accept_binding_probe;accept_binding_probe=NULL;
      CHECK(!kernel_socket_set_bound_device(listener,"eth0"));accept_binding_changed++;
  }
  if(heap_fail_after && !--heap_fail_after) { *out=NULL;return KERNEL_HEAP_STATUS_EMPTY; } *out = calloc(n, size); CHECK(*out); heap_last_allocation_size=n*size; ++heap_live; ++heap_allocations; return KERNEL_HEAP_STATUS_OK; }
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *heap, size_t size, void **out)
{ return kernel_heap_allocate_zeroed(heap, 1, size, out); }
void kernel_heap_release(struct kernel_heap *heap, void *allocation)
{
    CHECK(heap == &test_heap && allocation && heap_live);
    if (heap_release_once) {
        void (*interleave)(void *)=heap_release_once;
        heap_release_once=0;
        interleave(allocation);
    }
    --heap_live;
    free(allocation);
}
void kernel_wait_node_init_callback(struct kernel_wait_node *node,
    kernel_wait_callback_fn callback,void *context)
{ CHECK(!node->queue && !node->references);*node=(struct kernel_wait_node){.callback=callback,.context=context}; }
void kernel_wait_queue_add(struct kernel_wait_queue *queue,struct kernel_wait_node *node)
{
    CHECK(queue->initialized && !queue->closed && !node->queue && !node->references);
    node->queue=queue;node->previous=queue->tail;node->sequence=++queue->sequence;
    if(queue->tail)queue->tail->next=node;else queue->head=node;
    queue->tail=node;queue->registrations++;
}
enum kernel_scheduler_status kernel_wait_node_remove_sync(struct kernel_wait_node *node)
{
    CHECK(!kernel_cpu_current()->raw_locks && !node->references);
    if(unix_close_name_probe.target && node==&unix_close_name_probe.target->unix_space) {
        /* Observe close after name removal from the namespace and before the
         * incoming edge transfers its fallback name to the live peer. */
        CHECK(unix_close_name_probe.target->destroying==1 &&
            unix_close_name_probe.peer->peer==unix_close_name_probe.target);
        struct kernel_socket_address address={0};
        CHECK(!kernel_socket_getpeer(unix_close_name_probe.peer,&address));
        CHECK(address.family==KERNEL_SOCKET_AF_UNIX && address.unix_length==unix_close_name_probe.length &&
            !memcmp(address.unix_path,unix_close_name_probe.bytes,address.unix_length));
        kernel_socket_address_release(&address);unix_close_name_probe.observed++;
        unix_close_name_probe.target=NULL;
    }
    struct kernel_wait_queue *queue=node->queue;
    if(queue) {
        CHECK(queue->registrations);
        if(node->previous)node->previous->next=node->next;else queue->head=node->next;
        if(node->next)node->next->previous=node->previous;else queue->tail=node->previous;
        queue->registrations--;node->queue=NULL;node->previous=node->next=NULL;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
/* Abstract endpoint cases must never fabricate a pathname identity. Real VFS
 * socket inode lookup, permissions and unlink are checked by the VFS suite. */
enum kernel_task_status kernel_task_tgid(const struct kernel_task *task,kernel_pid_t *pid)
{ CHECK(task);*pid=model_pid.number;return KERNEL_TASK_STATUS_OK; }
enum kernel_task_status kernel_task_fs_context_borrow(const struct kernel_task *task,
    const struct kernel_fs_context **fs)
{ (void)task;*fs=NULL;return KERNEL_TASK_STATUS_INVALID_ARGUMENT; }
struct kernel_fs_paths kernel_fs_context_snapshot(const struct kernel_fs_context *fs)
{ CHECK(!fs);return (struct kernel_fs_paths){0}; }
void kernel_fs_paths_release(struct kernel_fs_paths *paths)
{ CHECK(!paths->root && !paths->cwd); }
int kernel_vfs_socket_bind_at(struct kernel_vfs_path *start,struct kernel_vfs_path *root,
    const char *path,uint32_t mode,struct kernel_vfs_path **owner,uint32_t umask)
{ (void)start;(void)root;(void)path;(void)mode;(void)umask;CHECK(!*owner);return -KERNEL_ENOENT; }
int kernel_vfs_socket_lookup_at(struct kernel_vfs_path *start,struct kernel_vfs_path *root,
    const char *path,struct kernel_vfs_path **owner)
{ (void)start;(void)root;(void)path;CHECK(!*owner);return -KERNEL_ENOENT; }
struct kernel_vfs_mount *kernel_vfs_path_mount(const struct kernel_vfs_path *path)
{ CHECK(!path);return NULL; }
uint64_t kernel_vfs_path_inode(const struct kernel_vfs_path *path)
{ CHECK(!path);return 0; }
int kernel_vfs_path_release(struct kernel_vfs_path **owner)
{ CHECK(!*owner);return 0; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ *queue = (struct kernel_wait_queue){.initialized = 1};kernel_raw_lock_init(&queue->lock,KERNEL_RAW_RANK_WAIT_QUEUE); }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ CHECK(queue->initialized && !queue->head && !arch_interrupt_is_enabled()); return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *q)
{CHECK(q->initialized && !q->head && !q->registrations);q->closed=1;return KERNEL_SCHEDULER_STATUS_OK;}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *q)
{CHECK(q->closed && !q->head && !q->borrows);*q=(struct kernel_wait_queue){0};return KERNEL_SCHEDULER_STATUS_OK;}
void kernel_wait_backend_quiesce(void)
{
    CHECK(notify_testing && !kernel_cpu_current()->raw_locks);
    CHECK(!pthread_mutex_lock(&remote_gate));notify_stopping=1;
    CHECK(!pthread_cond_broadcast(&remote_changed));
    while(notify_queue.borrows)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    CHECK(!pthread_mutex_unlock(&remote_gate));
}
uint64_t kernel_time_monotonic_ns(void) { return clock_ns; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t time, uint64_t *out)
{ *out = time; return KERNEL_TIME_STATUS_OK; }
int kernel_random_available(void) { return fixed_random; }
enum kernel_random_status kernel_random_fill(void *buffer, size_t size)
{ CHECK(fixed_random && size==sizeof(random_value));memcpy(buffer,&random_value,size);return KERNEL_RANDOM_STATUS_OK; }

int arch_dma_image_address(uint64_t address, uint64_t size, uint64_t *physical)
{ (void)size; *physical = address; return dma_eligible; }
int virtio_net_send_segments(struct virtio_net_device *device,
    const struct virtio_net_tx_segment *segments, unsigned count, void *owner)
{ (void)device; CHECK(segments && count && owner); ++segment_attempts; return -KERNEL_ENOTSUP; }
int virtio_net_send_copy(struct virtio_net_device *device, uint32_t size,
    int (*copy)(const void *, void *, uint32_t), const void *context)
{
    (void)device; ++copy_attempts;
    if (nic_full) return -KERNEL_EAGAIN;
    CHECK(size <= sizeof(captured) && copy(context, captured, size));
    captured_size = size;
    ++copied_packets;
    return 0;
}

static struct model_socket_file *model_socket_file(const struct kernel_open_file_description *file)
{
    for(struct model_socket_file *entry=model_socket_files;entry;entry=entry->next)
        if((const void *)entry==(const void *)file)return entry;
    return NULL;
}
static struct kernel_open_file_description *model_socket_file_create(struct kernel_socket *socket)
{
    struct model_socket_file *file=calloc(1,sizeof(*file));CHECK(file);
    file->socket=socket;file->references=1;file->next=model_socket_files;model_socket_files=file;
    kernel_socket_attach_file(socket,(void *)file);return (void *)file;
}
struct kernel_socket *kernel_open_file_socket(struct kernel_open_file_description *f)
{ struct model_socket_file *file=model_socket_file(f);return file?file->socket:(struct kernel_socket *)(uintptr_t)f; }
uint32_t kernel_open_file_references(const struct kernel_open_file_description *description)
{ CHECK(!regular_rights_no_graph);struct model_socket_file *file=model_socket_file(description);CHECK(file);return __atomic_load_n(&file->references,__ATOMIC_ACQUIRE); }
enum kernel_open_file_status kernel_open_file_acquire(struct kernel_open_file_description *f)
{
    struct model_socket_file *file=model_socket_file(f);
    if(file) { uint32_t before=__atomic_fetch_add(&file->references,1,__ATOMIC_RELAXED);CHECK(before && before<UINT32_MAX);return KERNEL_OPEN_FILE_STATUS_OK; }
    CHECK(remote_testing && f==(void *)remote_socket);stop_borrows++;return KERNEL_OPEN_FILE_STATUS_OK;
}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **f)
{ CHECK(f && *f);
  struct model_socket_file *file=model_socket_file(*f);
  if(file) {
      if(file==control_release_probe.target) {
          CHECK(__atomic_load_n(&file->references,__ATOMIC_ACQUIRE)==1);
          CHECK(!kernel_cpu_current()->raw_locks);
          /* The production control owner must drop its graph mutex before
           * backend cleanup, including the injected failing last put. */
          CHECK(!pthread_mutex_trylock(&model_rights_mutex));
          CHECK(!pthread_mutex_unlock(&model_rights_mutex));
          CHECK(!pthread_mutex_lock(&control_release_probe.lock));
          control_release_probe.attempts++;
          if(control_release_probe.pause) {
              control_release_probe.entered=1;
              CHECK(!pthread_cond_broadcast(&control_release_probe.changed));
              while(!control_release_probe.resume)
                  CHECK(!pthread_cond_wait(&control_release_probe.changed,&control_release_probe.lock));
          }
          unsigned fail=control_release_probe.fail;
          CHECK(!pthread_mutex_unlock(&control_release_probe.lock));
          /* A failed cleanup retains this exact owner and its reference. */
          if(fail)return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
      }
      *f=NULL;uint32_t before=__atomic_fetch_sub(&file->references,1,__ATOMIC_ACQ_REL);CHECK(before);
      if(file->socket)kernel_socket_rights_changed();
      if(before==1) {
          struct model_socket_file **link=&model_socket_files;while(*link!=file) { CHECK(*link);link=&(*link)->next; }*link=file->next;
          if(file->socket)kernel_socket_destroy(file->socket);
          free(file);
      }
      return KERNEL_OPEN_FILE_STATUS_OK;
  }
  if(stop_borrows && *f==(void *)remote_socket)stop_borrows--;
  else {CHECK(allow_ofd_release);--allow_ofd_release;}
  *f=0;return KERNEL_OPEN_FILE_STATUS_OK; }
enum kernel_task_status kernel_task_socket_write_register(struct kernel_task *t, struct kernel_socket_write_request *r)
{CHECK(remote_testing && t==remote_task && !remote_waiter.request);remote_waiter.request=r;return KERNEL_TASK_STATUS_OK;}
enum kernel_task_status kernel_task_socket_write_clear(struct kernel_task *t, struct kernel_socket_write_request *r)
{CHECK(remote_testing && t==remote_task && remote_waiter.request==r);remote_waiter.request=NULL;return KERNEL_TASK_STATUS_OK;}
enum kernel_task_status kernel_task_socket_read_register(struct kernel_task *t, struct kernel_socket_read_request *r)
{ (void)t;(void)r;CHECK(reservation_actor);reservation_arrive();return KERNEL_TASK_STATUS_OK; }
enum kernel_task_status kernel_task_socket_read_clear(struct kernel_task *t, struct kernel_socket_read_request *r)
{ (void)t;(void)r;return KERNEL_TASK_STATUS_OK; }
void kernel_signal_note_syscall_restart(struct kernel_task *t) { (void)t; abort(); }
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t)
{
    (void)d;(void)i;CHECK(q || remote_waiter.request);
    remote_waiter.notified=0;*t=(struct kernel_wait_token){(void *)&remote_waiter,1};
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_notify(const struct kernel_wait_token *t,enum kernel_wait_wake_reason reason)
{
    struct remote_waiter *waiter=(void *)t->task;
    CHECK(reason==KERNEL_WAIT_WOKEN && waiter->request && t==&waiter->request->completion &&
          t->generation && !waiter->request->queued && !waiter->notified);
    waiter->notified=1;
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{
    (void)t;CHECK(remote_testing && !kernel_cpu_current()->raw_locks);
    CHECK(remote_mode!=10);
    CHECK(!pthread_mutex_lock(&remote_gate));
    int stopping_actor=!kernel_cpu_current()->logical_id;
    if(stopping_actor) {stop_waiting=1;CHECK(!pthread_cond_broadcast(&remote_changed));}
    else if(remote_mode==9) {remote_parked++;CHECK(!pthread_cond_broadcast(&remote_changed));}
    for(;;) {
        int pending;
        if (stopping_actor) { SOCKET_ENDPOINT(state,remote_socket);pending=remote_socket->active_writes!=NULL; }
        else { SOCKET_STATE(state);pending=!remote_waiter.notified; }
        if(!pending)break;
        CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    }
    CHECK(!pthread_mutex_unlock(&remote_gate));*r=KERNEL_WAIT_WOKEN;return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t)
{CHECK(remote_testing && t->generation);*t=(struct kernel_wait_token){0};return KERNEL_SCHEDULER_STATUS_OK;}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m,void *out,uint64_t in,size_t n,size_t *copied)
{
    (void)m;CHECK(!caller_owns_protocol() && !kernel_cpu_current()->raw_locks);*copied=n<user_remaining?n:user_remaining;
    if(unix_credit_copy_probe) {
        CHECK(unix_credit_copy_probe->tx_reserved &&
            !(kernel_socket_poll(unix_credit_copy_probe,NULL)&KERNEL_POLLOUT));
        unix_credit_copy_observed++;
    }
    memcpy(out,(void *)(uintptr_t)in,*copied);
    if(remote_testing && (remote_mode==2 || remote_mode==3)) {
        CHECK(boaros_lwip_parallel_enabled() && kernel_cpu_current()->logical_id==1 &&
            remote_waiter.request && remote_waiter.request->protocol_payload && !remote_waiter.request->operation);
    }
    if(remote_testing && remote_mode==3) {
        CHECK(!pthread_mutex_lock(&remote_gate));copy_waiting=1;
        CHECK(!pthread_cond_broadcast(&remote_changed));
        while(!copy_release)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        CHECK(!pthread_mutex_unlock(&remote_gate));
    }
    if(read_shutdown_during_copy) {
        struct kernel_socket *peer=read_shutdown_during_copy;read_shutdown_during_copy=NULL;
        CHECK(!kernel_socket_shutdown(peer,0));
    }
    if (shutdown_during_copy) {
        struct kernel_socket *peer=shutdown_during_copy;shutdown_during_copy=0;
        CHECK(!kernel_socket_shutdown(peer,1));
    }
    if (close_during_copy) {
        struct kernel_socket *peer=close_during_copy;close_during_copy=0;
        kernel_socket_destroy(peer);
    }
    if(user_remaining!=SIZE_MAX)user_remaining-=*copied;
    return *copied==n?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;
}

enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m,uint64_t out,const void *in,size_t n,size_t *copied)
{
    (void)m;CHECK(!caller_owns_protocol() && !kernel_cpu_current()->raw_locks);*copied=n<user_remaining?n:user_remaining;
    memcpy((void *)(uintptr_t)out,in,*copied);
    if(user_remaining!=SIZE_MAX)user_remaining-=*copied;
    return *copied==n?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;
}

enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(struct kernel_mm *mm,
    struct kernel_uaccess_write_cursor *pages,uint64_t out,const void *in,size_t count,size_t *copied)
{ (void)pages; return kernel_copy_to_user(mm,out,in,count,copied); }
void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *pages)
{ (void)pages; CHECK(!caller_owns_protocol() && !kernel_cpu_current()->raw_locks); }

/* Host OFDs are boundary objects; exercise the production request ownership path. */
static int write_buffer(struct kernel_socket *socket, const void *data, uint32_t size, uint32_t flags)
{
    struct kernel_open_file_description *pin = (void *)socket;
    if (!kernel_socket_is_tcp(socket)) return kernel_socket_write_packet_buffer(&pin, data, size, flags);
    struct kernel_socket_write_request request = {0};
    kernel_socket_stream_begin(&request, &pin);
    int result = kernel_socket_stream_reserve(&request, size, flags);
    if (result > 0) result = kernel_socket_stream_commit(&request, data, (uint32_t)result, flags);
    kernel_socket_stream_finish(&request);
    return result;
}

static struct kernel_socket_write_request *copy_capacity_output_request;
static unsigned copy_capacity_outputs;
static err_t physical_output(struct netif *interface, struct pbuf *packet, const ip4_addr_t *destination)
{
    (void)destination;
    if(copy_capacity_output_request) {
        CHECK(!kernel_cpu_current()->raw_locks && caller_owns_protocol());
        CHECK(copy_capacity_output_request->result==3 && !copy_capacity_output_request->reserved);
        struct kernel_socket *socket=copy_capacity_output_request->socket;
        SOCKET_ENDPOINT(state,socket);
        CHECK(!socket->tx_reserved && socket->tx_budget==tcp_protocol_budget(socket));
        copy_capacity_outputs++;
    }
    return link_output(interface, packet);
}
static err_t physical_init(struct netif *interface)
{ interface->name[0] = 't'; interface->name[1] = 'e'; interface->mtu = 1500; interface->output = physical_output; return ERR_OK; }

static unsigned char route_frame[128];
static unsigned route_frame_size;
static unsigned route_fragments,route_fragment_bytes,route_df_packets,route_validate_packets;
static err_t route_link_output(struct netif *interface, struct pbuf *packet)
{
    (void)interface;
    route_frame_size = packet->tot_len < sizeof(route_frame) ? packet->tot_len : sizeof(route_frame);
    CHECK(pbuf_copy_partial(packet, route_frame, route_frame_size, 0) == route_frame_size);
    if(route_validate_packets && route_frame_size>=34 && route_frame[12]==8 && !route_frame[13]) {
        unsigned offset=((unsigned)route_frame[20]<<8)|route_frame[21];
        unsigned total=((unsigned)route_frame[16]<<8)|route_frame[17];
        unsigned header=(route_frame[14]&15U)*4U;
        CHECK(header>=20 && header<=60 && route_frame_size>=14+header && !inet_chksum(route_frame+14,header));
        if(offset&0x4000)route_df_packets++;
        if(offset&0x3fff) {
            CHECK(!(offset&0x4000) && total>header);
            route_fragments++;route_fragment_bytes+=total-header;
        }
    }
    return ERR_OK;
}
static err_t route_interface_init(struct netif *interface)
{
    interface->name[0] = 'e'; interface->name[1] = 't'; interface->mtu = 1500;
    interface->hwaddr_len = 6; interface->hwaddr[0] = 2;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    interface->output = etharp_output; interface->linkoutput = route_link_output;
    return ERR_OK;
}
static void ipv4_route_output(void)
{
    kernel_socket_network_initialize();
    struct netif interface = {0};
    ip4_addr_t local, mask, empty, gateway, remote;
    IP4_ADDR(&local, 10, 77, 0, 2); IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 77, 0, 1); IP4_ADDR(&remote, 192, 0, 2, 1);
    ip4_addr_set_zero(&empty);
    CHECK(netif_add(&interface, &local, &mask, &empty, NULL, route_interface_init, ip_input));
    netif_set_up(&interface); netif_set_link_up(&interface);
    CHECK(!kernel_socket_default_route("eth0", gateway.addr, 0));
    CHECK(ip4_route(&remote) == &interface);
    struct pbuf *packet = pbuf_alloc(PBUF_IP, 20, PBUF_RAM); CHECK(packet);
    memset(packet->payload, 0, 20); ((unsigned char *)packet->payload)[0] = 0x45;
    memcpy((unsigned char *)packet->payload + 16, &remote.addr, 4);
    CHECK(etharp_output(&interface, packet, &remote) == ERR_OK);
    CHECK(route_frame_size >= 42 && route_frame[12] == 8 && route_frame[13] == 6);
    CHECK(!memcmp(route_frame + 38, &gateway.addr, 4));
    const unsigned char gateway_mac[6] = {2, 1, 2, 3, 4, 5};
    unsigned char reply[42] = {0};
    memcpy(reply, interface.hwaddr, 6); memcpy(reply + 6, gateway_mac, 6);
    reply[12] = 8; reply[13] = 6; reply[15] = 1; reply[16] = 8;
    reply[18] = 6; reply[19] = 4; reply[21] = 2;
    memcpy(reply + 22, gateway_mac, 6); memcpy(reply + 28, &gateway.addr, 4);
    memcpy(reply + 32, interface.hwaddr, 6); memcpy(reply + 38, &local.addr, 4);
    struct pbuf *arp = pbuf_alloc(PBUF_RAW, sizeof(reply), PBUF_RAM); CHECK(arp);
    CHECK(pbuf_take(arp, reply, sizeof(reply)) == ERR_OK);
    CHECK(ethernet_input(arp, &interface) == ERR_OK);
    CHECK(route_frame_size >= 34 && !memcmp(route_frame, gateway_mac, 6));
    CHECK(route_frame[12] == 8 && route_frame[13] == 0 && !memcmp(route_frame + 30, &remote.addr, 4));
    CHECK(!kernel_socket_default_route("eth0", gateway.addr, 1));
    CHECK(etharp_output(&interface, packet, &remote) == ERR_RTE);
    pbuf_free(packet); etharp_cleanup_netif(&interface); netif_remove(&interface);
    puts("PASS configured IPv4 gateway determines the ARP target for off-subnet traffic");
}
static void tcp_ephemeral_ports(void)
{
    static const struct {uint32_t seed;uint16_t occupied,want;} cases[]={
        {0xffff,0,49152},{0xfffe,0,65535},
        {0xffff,49152,49153},{0xfffe,65535,49152}
    };
    kernel_socket_network_initialize();
    for(unsigned connecting=0;connecting<2;connecting++)for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        struct kernel_socket *occupied=NULL,*listener=NULL,*endpoint=NULL,*accepted=NULL;
        struct kernel_socket_address local={.family=KERNEL_SOCKET_AF_INET,.bytes={127,0,0,1}};
        struct kernel_socket_address remote=local;remote.port=23000;
        /* tcp_init resets the PCB context, so seed it before publishing any
         * occupied endpoint or listener that this iteration must retain. */
        fixed_random=1;random_value=cases[i].seed;
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        tcp_init();kernel_socket_protocol_leave(&scope);
        if(cases[i].occupied) {
            CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM, 0,&occupied));
            local.port=cases[i].occupied;CHECK(!kernel_socket_bind(occupied,&local));local.port=0;
        }
        if(connecting) {
            CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM, 0,&listener));
            CHECK(!kernel_socket_bind(listener,&remote) && !kernel_socket_listen(listener,1));
        }
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM, 0,&endpoint));
        if(connecting) {
            int result=kernel_socket_connect(endpoint,&remote,0);
            CHECK(!result || result==-KERNEL_EINPROGRESS);
            for(unsigned turn=0;turn<4;turn++)
                (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
            CHECK(endpoint->connected && !kernel_socket_accept(listener,&accepted,NULL));
        } else CHECK(!kernel_socket_bind(endpoint,&local));
        struct kernel_socket_address actual={0};CHECK(!kernel_socket_getname(endpoint,&actual));
        CHECK(actual.port==cases[i].want && (!occupied || actual.port!=cases[i].occupied));
        fixed_random=0;
        kernel_socket_protocol_enter(&scope);
        if(connecting) {tcp_abort(endpoint->tcp);if(accepted->tcp)tcp_abort(accepted->tcp);}
        kernel_socket_protocol_leave(&scope);
        kernel_socket_destroy(endpoint);
        if(accepted)kernel_socket_destroy(accepted);
        if(listener)kernel_socket_destroy(listener);
        if(occupied)kernel_socket_destroy(occupied);
        for(unsigned turn=0;turn<4;turn++)
            (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
        CHECK(!heap_live && !tcp_bound_pcbs && !tcp_active_pcbs && !tcp_tw_pcbs && !tcp_listen_pcbs.pcbs);
    }
    puts("PASS TCP ephemeral ports: first bind/connect at randomized upper endpoint, last port before wrap, occupied-port skip and exact owner return");
}

static void ipv4_address_shutdown(void)
{
    kernel_socket_network_initialize();
    struct netif interface = {0};
    ip4_addr_t local, mask, empty, changed;
    IP4_ADDR(&local, 10, 77, 0, 2); IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&changed, 10, 77, 0, 3); ip4_addr_set_zero(&empty);
    CHECK(netif_add(&interface, &local, &mask, &empty, NULL, route_interface_init, ip_input));
    netif_set_up(&interface); netif_set_link_up(&interface);
    struct kernel_socket *socket = NULL;
    CHECK(!kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, &socket));
    socket->connected = 1; socket->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(socket->tcp->local_ip, local);
    IP_ADDR4(&socket->tcp->remote_ip, 10, 77, 0, 1);
    socket->tcp->local_port = 22000; socket->tcp->remote_port = 22001;
    socket->tcp->cwnd = TCP_MSS; socket->tcp->snd_wnd = socket->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(socket->tcp);
    const char name[16] = "eth0";
    CHECK(!kernel_socket_interface_set_ipv4(name, 0x8916, changed.addr));
    CHECK(socket->tcp && ip4_addr_eq(ip_2_ip4(&socket->tcp->local_ip), &local));
    struct kernel_socket *survivor = NULL;
    CHECK(!kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, &survivor));
    survivor->connected = 1; survivor->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(survivor->tcp->local_ip, changed);
    IP_ADDR4(&survivor->tcp->remote_ip, 10, 77, 0, 1);
    survivor->tcp->local_port = 22002; survivor->tcp->remote_port = 22001;
    survivor->tcp->cwnd = TCP_MSS; survivor->tcp->snd_wnd = survivor->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(survivor->tcp);
    kernel_socket_destroy(socket);
    CHECK(tcp_active_pcbs);
    kernel_socket_network_purge_orphans();
    CHECK(tcp_active_pcbs == survivor->tcp && survivor->tcp->callback_arg == survivor);
    kernel_socket_destroy(survivor);
    kernel_socket_network_purge_orphans();
    netif_set_down(&interface); netif_remove(&interface);
    CHECK(!tcp_active_pcbs);
    puts("PASS address change and device shutdown release prior TCP owners");
}

static void nodelay_pending(unsigned blocked)
{
    kernel_socket_network_initialize();
    struct virtio_net_device device = {0};
    struct kernel_network network = {.device = &device};
    ip4_addr_t local, mask, gateway;
    IP4_ADDR(&local, 10, 77, 0, 2); IP4_ADDR(&mask, 255, 255, 255, 0);
    ip4_addr_set_zero(&gateway);
    CHECK(netif_add(&network.interface, &local, &mask, &gateway, &network, physical_init, ip_input));
    netif_set_up(&network.interface); netif_set_link_up(&network.interface);
    struct kernel_socket *socket = NULL;
    CHECK(!kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, &socket));
    socket->connected = 1; socket->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(socket->tcp->local_ip, local);
    IP_ADDR4(&socket->tcp->remote_ip, 10, 77, 0, 1);
    socket->tcp->local_port = 22000; socket->tcp->remote_port = 22001;
    socket->tcp->cwnd = TCP_MSS; socket->tcp->snd_wnd = socket->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(socket->tcp); tcp_bind_netif(socket->tcp, &network.interface);
    {SOCKET_ENDPOINT(state,socket);tcp_publish_capacity_locked(socket);}
    uint64_t frozen = clock_ns;
    CHECK(write_buffer(socket, "a", 1, 0) == 1);
    CHECK(copied_packets == 1 && socket->tcp->unacked && !socket->tcp->unsent);
    CHECK(write_buffer(socket, "b", 1, 0) == 1);
    CHECK(copied_packets == 1 && socket->tcp->unsent);
    CHECK(!kernel_socket_set_option(socket, KERNEL_SOCKET_NODELAY, 0));
    CHECK(copied_packets == 1 && socket->tcp->unsent);
    nic_full = blocked;
    if (blocked==2) {
        struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
        struct tcp_pcb *pcb=socket->tcp;
        struct pbuf *packet=pbuf_alloc(PBUF_RAW,TCP_HLEN,PBUF_RAM);CHECK(packet);
        struct tcp_hdr *hdr=packet->payload;
        *hdr=(struct tcp_hdr){.src=lwip_htons(pcb->remote_port),.dest=lwip_htons(pcb->local_port),
            .seqno=lwip_htonl(pcb->rcv_nxt),.ackno=lwip_htonl(pcb->snd_nxt),.wnd=lwip_htons(TCP_WND)};
        TCPH_HDRLEN_FLAGS_SET(hdr,5,TCP_ACK);
        hdr->chksum=ip_chksum_pseudo(packet,IP_PROTO_TCP,TCP_HLEN,&pcb->remote_ip,&pcb->local_ip);
        struct ip_globals saved=ip_data;
        ip_data.current_iphdr_src=pcb->remote_ip;ip_data.current_iphdr_dest=pcb->local_ip;
        ip_data.current_netif=ip_data.current_input_netif=&network.interface;
        tcp_input(packet,&network.interface);
        ip_data=saved;
        CHECK(!pcb->unacked && !socket->work_links[ready_work.slot].queued);
        kernel_socket_protocol_leave(&scope);
    } else CHECK(!kernel_socket_set_option(socket, KERNEL_SOCKET_NODELAY, 1));
    if (blocked) {
        CHECK(copied_packets == 1 && socket->tcp->unsent);
        CHECK(socket->work_links[nic_wait.slot].queued);
        nic_full = 0; kernel_socket_network_capacity();
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    }
    CHECK(copied_packets == 2 && !socket->tcp->unsent && clock_ns == frozen);
    CHECK(!kernel_socket_set_option(socket, KERNEL_SOCKET_NODELAY, 1));
    CHECK(copied_packets == 2);
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    tcp_abort(socket->tcp); kernel_socket_destroy(socket);
    netif_remove(&network.interface);
    kernel_socket_protocol_leave(&irq);
    if (blocked==2) puts("PASS input output: NIC retry follows real failure without a speculative ready job or timer");
    else printf("PASS NODELAY pending output and %s without ACK or timer progress\n", blocked ? "NIC capacity retry" : "immediate transmission");
}

static struct {
    struct kernel_socket *socket;
    struct kernel_socket_write_request *request;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned stage,returned,writes;
    uint32_t available;
} copy_capacity_interleave={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};

static void copy_capacity_pause(unsigned stage)
{
    CHECK(!pthread_mutex_lock(&copy_capacity_interleave.lock));
    copy_capacity_interleave.stage=stage;
    CHECK(!pthread_cond_broadcast(&copy_capacity_interleave.changed));
    while(copy_capacity_interleave.returned!=stage)
        CHECK(!pthread_cond_wait(&copy_capacity_interleave.changed,&copy_capacity_interleave.lock));
    CHECK(!pthread_mutex_unlock(&copy_capacity_interleave.lock));
}
/* Another CPU can reserve both before COPY and after the prefix is in the
 * PCB but before the owner publishes its new budget. */
static err_t copy_capacity_tcp_writev(struct tcp_pcb *pcb,
    const struct kernel_socket_write_segment *segments,unsigned count,u16_t size,u8_t flags,u16_t *written)
{
    CHECK(!kernel_cpu_current()->raw_locks && caller_owns_protocol());
    int observe=copy_capacity_interleave.socket && pcb==copy_capacity_interleave.socket->tcp;
    if(observe) {
        CHECK(copy_capacity_interleave.request->reserved==3 && count==3);
        copy_capacity_interleave.writes++;
        copy_capacity_pause(1);
    }
    err_t result=boaros_lwip_tcp_writev(pcb,segments,count,size,flags,written);
    if(observe) {
        CHECK(result==ERR_OK && *written==3 && copy_capacity_interleave.request->reserved==3);
        copy_capacity_pause(2);
    }
    return result;
}
static void *copy_capacity_reserver(void *unused)
{
    (void)unused;kernel_cpu_current()->logical_id=1;
    struct kernel_open_file_description *owner=(void *)copy_capacity_interleave.socket;
    struct kernel_socket_write_request other={0};
    kernel_socket_stream_begin(&other,&owner);
    for(unsigned stage=1;stage<=2;stage++) {
        CHECK(!pthread_mutex_lock(&copy_capacity_interleave.lock));
        while(copy_capacity_interleave.stage!=stage)
            CHECK(!pthread_cond_wait(&copy_capacity_interleave.changed,&copy_capacity_interleave.lock));
        CHECK(!pthread_mutex_unlock(&copy_capacity_interleave.lock));
        if(stage==1) {
            CHECK(kernel_socket_stream_reserve(&other,copy_capacity_interleave.available,0)==(int)copy_capacity_interleave.available);
        }
        CHECK(!(kernel_socket_poll(copy_capacity_interleave.socket,NULL)&KERNEL_POLLOUT));
        {SOCKET_ENDPOINT(state,copy_capacity_interleave.socket);CHECK(!tcp_admission_capacity_locked(copy_capacity_interleave.socket));}
        if(stage==2) {
            kernel_socket_stream_cancel(&other);
            CHECK(kernel_socket_stream_reserve(&other,copy_capacity_interleave.available,0)==(int)copy_capacity_interleave.available);
            CHECK(!(kernel_socket_poll(copy_capacity_interleave.socket,NULL)&KERNEL_POLLOUT));
            kernel_socket_stream_finish(&other);
            CHECK(owner==(void *)copy_capacity_interleave.socket);
        }
        CHECK(!pthread_mutex_lock(&copy_capacity_interleave.lock));copy_capacity_interleave.returned=stage;
        CHECK(!pthread_cond_broadcast(&copy_capacity_interleave.changed) && !pthread_mutex_unlock(&copy_capacity_interleave.lock));
    }
    return NULL;
}
static void copy_capacity_batch(struct kernel_socket_write_request *request)
{
    CHECK(kernel_socket_stream_reserve(request,3,0)==3);
    unsigned before=state_acquires;
    CHECK(kernel_socket_stream_commit(request,"bcd",3,KERNEL_SOCKET_IO_MORE)==3);
    unsigned single=state_acquires-before;
    const struct kernel_socket_write_segment segments[]={{"b",1},{"c",1},{"d",1}};
    CHECK(kernel_socket_stream_reserve(request,3,0)==3);before=state_acquires;
    struct kernel_socket_statistics stats_before, stats_after;
    kernel_socket_get_statistics(&stats_before);
    CHECK(kernel_socket_stream_commitv(request,segments,3,KERNEL_SOCKET_IO_MORE)==3);
    kernel_socket_get_statistics(&stats_after);
    CHECK(stats_after.tcp_write_calls-stats_before.tcp_write_calls==1);
    CHECK(stats_after.tcp_written_bytes-stats_before.tcp_written_bytes==3);
    printf("TCP capacity locks: single=%u split=%u\n",single,state_acquires-before);
    CHECK(state_acquires-before==single);

    struct kernel_open_file_description *held_owner=(void *)request->socket;
    struct kernel_socket_write_request held={0};
    kernel_socket_stream_begin(&held,&held_owner);CHECK(kernel_socket_stream_reserve(&held,1,0)==1);
    CHECK(kernel_socket_stream_reserve(request,3,0)==3);
    copy_capacity_interleave.socket=request->socket;copy_capacity_interleave.request=request;
    {SOCKET_ENDPOINT(state,request->socket);copy_capacity_interleave.available=tcp_admission_capacity_locked(request->socket);}
    CHECK(copy_capacity_interleave.available);
    uint32_t budget=request->socket->tx_budget;
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,copy_capacity_reserver,NULL));
    CHECK(kernel_socket_stream_commitv(request,segments,3,KERNEL_SOCKET_IO_MORE)==3);
    CHECK(!pthread_join(thread,NULL));
    CHECK(copy_capacity_interleave.writes==1 && request->socket->tx_reserved==1);
    CHECK(request->socket->tx_budget==budget-3 && !request->reserved);
    copy_capacity_interleave.socket=NULL;copy_capacity_interleave.request=NULL;
    kernel_socket_stream_finish(&held);CHECK(held_owner==(void *)request->socket && !request->socket->tx_reserved);
}

static void copy_capacity(unsigned eligible, unsigned gather)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    kernel_socket_network_initialize();
    struct virtio_net_device device = {0};
    struct kernel_network network = {.device=&device};
    ip4_addr_t local, mask, gateway;
    IP4_ADDR(&local, 10, 77, 0, 2); IP4_ADDR(&mask, 255, 255, 255, 0); ip4_addr_set_zero(&gateway);
    CHECK(netif_add(&network.interface, &local, &mask, &gateway, &network, physical_init, ip_input));
    netif_set_up(&network.interface); netif_set_link_up(&network.interface);
    struct kernel_socket *socket = NULL;
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, &socket) == 0);
    /* Establish the transport fixture without a simulated remote handshake;
     * all send admission, protocol output and NIC wait routing remain real. */
    socket->connected = 1; socket->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(socket->tcp->local_ip, local);
    IP_ADDR4(&socket->tcp->remote_ip, 10, 77, 0, 1);
    socket->tcp->local_port = 22000; socket->tcp->remote_port = 22001;
    socket->tcp->cwnd = TCP_MSS; socket->tcp->snd_wnd = socket->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(socket->tcp); tcp_bind_netif(socket->tcp, &network.interface);
    {SOCKET_ENDPOINT(state,socket);tcp_publish_capacity_locked(socket);}
    dma_eligible = eligible; nic_full = 1; copy_attempts = copied_packets = segment_attempts = 0;
    kernel_socket_protocol_leave(&irq);protocol_services=0;
    if (gather) {
        struct kernel_open_file_description *owner=(void *)socket;
        struct kernel_socket_write_request request={0};
        kernel_socket_stream_begin(&request,&owner);
        if(gather==1)copy_capacity_batch(&request);
        unsigned prefix=gather==2?2:5;
        CHECK(kernel_socket_stream_reserve(&request,prefix,0)==(int)prefix);
        CHECK(kernel_socket_stream_commit(&request,"owner",prefix,KERNEL_SOCKET_IO_MORE)==(int)prefix);
        CHECK(!copy_attempts && !segment_attempts && !protocol_services);
        CHECK(!work_pending_snapshot());
        if (gather==2) {
            CHECK(kernel_socket_stream_reserve(&request,3,0)==3);
            copy_capacity_output_request=&request;
            CHECK(kernel_socket_stream_commit(&request,"ner",3,0)==3);
            copy_capacity_output_request=NULL;CHECK(copy_capacity_outputs==1);
            CHECK(copy_attempts==1 && protocol_services==1);
        }
        if (gather==4) {
            CHECK(kernel_socket_stream_reserve(&request,3,0)==3);
            socket->pending_error=-KERNEL_ECONNRESET;
            CHECK(kernel_socket_stream_commit(&request,"ner",3,0)==-KERNEL_ECONNRESET);
            CHECK(socket->pending_error==-KERNEL_ECONNRESET && !socket->tx_reserved);
        }
        if (gather==3) {
            allow_ofd_release=1;kernel_socket_abort_write(&request);
            CHECK(!owner && !allow_ofd_release && !request.pin);
        } else {
            kernel_socket_stream_finish(&request);
            CHECK(owner==(void *)socket && protocol_services==1);
            if (gather==4) {
                CHECK(socket->pending_error==-KERNEL_ECONNRESET);
                CHECK(write_buffer(socket,"next",4,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ECONNRESET);
                CHECK(!socket->pending_error && protocol_services==1);
            }
        }
    } else {
        CHECK(write_buffer(socket, "owner", 5, 0) == 5);
        CHECK(protocol_services==1);
    }
    kernel_socket_protocol_enter(&irq);
    CHECK(socket->tcp->unsent && copy_attempts == 1 && segment_attempts == eligible && !copied_packets);
    kernel_socket_protocol_leave(&irq);
    uint64_t generation = pool_generation, before_time = clock_ns;
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    CHECK(copy_attempts == 1 && !copied_packets);
    nic_full = 0; kernel_socket_network_capacity();
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    CHECK(copied_packets == 1 && !socket->tcp->unsent);
    CHECK(pool_generation == generation && clock_ns == before_time);
    CHECK(captured_size >= 5 && !memcmp(captured + captured_size - 5, "owner", 5));
    kernel_socket_protocol_enter(&irq);
    tcp_abort(socket->tcp); kernel_socket_destroy(socket);
    netif_remove(&network.interface);
    kernel_socket_protocol_leave(&irq);
    CHECK(!heap_live);
    if (gather) printf("PASS TCP batch %u: bounded output/service calls, retained prefix and NIC-only retry\n",gather);
    else printf("PASS copy fallback %s: NIC capacity alone retries real TCP output\n", eligible ? "no indirect" : "SG unsuitable");
}

static void udp_bind_ownership(void)
{
    struct kernel_socket *v6=NULL,*v4=NULL;
    struct kernel_socket_address any6={.family=KERNEL_SOCKET_AF_INET6,.port=23001};
    struct kernel_socket_address any4={.family=KERNEL_SOCKET_AF_INET,.port=23001};
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET6,SOCKET_DGRAM, 0,&v6));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&v4));
    CHECK(!kernel_socket_bind(v6,&any6));
    CHECK(kernel_socket_bind(v4,&any4)==-KERNEL_EADDRINUSE);
    CHECK(!kernel_socket_set_option(v4,KERNEL_SOCKET_REUSEADDR,1));
    CHECK(kernel_socket_bind(v4,&any4)==-KERNEL_EADDRINUSE);
    CHECK(!kernel_socket_set_option(v6,KERNEL_SOCKET_REUSEADDR,1));
    CHECK(!kernel_socket_bind(v4,&any4));
    kernel_socket_destroy(v6);kernel_socket_destroy(v4);
    v6=NULL;v4=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET6,SOCKET_DGRAM, 0,&v6));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&v4));
    CHECK(!kernel_socket_set_option(v6,KERNEL_SOCKET_V6ONLY,1));
    CHECK(!kernel_socket_bind(v6,&any6) && !kernel_socket_bind(v4,&any4));
    kernel_socket_destroy(v4);kernel_socket_destroy(v6);

    struct kernel_socket *sender=NULL,*receiver=NULL;
    struct kernel_socket_address old={0},unspec={0};
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&sender));
    CHECK(!kernel_socket_connect(sender,&any4,0) && !kernel_socket_getname(sender,&old) && old.port);
    CHECK(!kernel_socket_connect(sender,&unspec,0));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&receiver));
    CHECK(!kernel_socket_bind(receiver,&old));
    CHECK(!kernel_socket_connect(sender,&old,0));
    CHECK(write_buffer(sender,"x",1,0)==1 && receiver->rx_bytes==1 && !sender->rx_bytes);
    kernel_socket_destroy(sender);kernel_socket_destroy(receiver);
}

static void udp_final_payload(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *socket=0;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&socket));
    struct kernel_open_file_description *owner=(void *)socket;
    char first[]="alpha",second[]="beta";
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)first,5},{(uintptr_t)second,4}};
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=23001,.bytes={127,0,0,1}};
    unsigned before=heap_allocations;
    CHECK(kernel_socket_write_datagram(&owner,(void *)socket,iov,2,9,0,&address, NULL)==9);
    CHECK(owner==(void *)socket && heap_allocations==before);
    uint32_t memory=lwip_stats.mem.used;
    user_remaining=6;
    CHECK(kernel_socket_write_datagram(&owner,(void *)socket,iov,2,9,0,&address, NULL)==-KERNEL_EFAULT);
    CHECK(owner==(void *)socket && lwip_stats.mem.used==memory && !socket->tx_reserved);
    user_remaining=SIZE_MAX;
    kernel_socket_destroy(socket);
    udp_bind_ownership();
    puts("PASS UDP final payload and bind: gather, fault rollback, dual-stack reuse and V6ONLY port ownership");
}

static void unix_final_payload(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=0,*receiver=0;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    struct kernel_open_file_description *owner=(void *)sender;
    char first[]="alpha",second[]="beta";
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)first,5},{(uintptr_t)second,4}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    unsigned before=heap_allocations;
    shutdown_during_copy=receiver;
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0, NULL)==9);
    CHECK(!shutdown_during_copy && sender->peer_closed && !receiver->read_closed &&
        owner==(void *)sender && heap_allocations==before+1 && receiver->rx_bytes==9);
    CHECK(!memcmp(receiver->packets_head->data,"alphabeta",9) && !sender->tx_reserved);
    uint64_t charged=sender->unix_memory;
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};user_remaining=6;
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0, NULL)==6);
    CHECK(receiver->packets_tail->length==6 && !memcmp(receiver->packets_tail->data,"alphab",6));
    CHECK(sender->unix_memory>charged && !sender->tx_reserved);user_remaining=SIZE_MAX;
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};close_during_copy=receiver;
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0, NULL)==-KERNEL_EPIPE);
    CHECK(!close_during_copy && owner==(void *)sender && heap_live==1 &&
        !sender->tx_reserved && !sender->unix_memory && !sender->unix_packets && sender->pending_error==-KERNEL_ECONNRESET);
    kernel_socket_destroy(sender);
    puts("PASS UNIX final queue payload: half-close before/during send, gather fill, fault prefix and peer-close reservation rollback");
}

static void unix_stream_packet_boundary(void)
{
    kernel_socket_network_initialize();
    static unsigned char source[32768],received[32768];
    for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i*19U+7U);
    struct kernel_socket *sender=NULL,*receiver=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    const int sendbuf[]={106496,0,4<<20};
    for(unsigned i=0;i<sizeof(sendbuf)/sizeof(sendbuf[0]);i++) {
        CHECK(!kernel_socket_set_option(sender,KERNEL_SOCKET_SNDBUF,sendbuf[i]));
        uint32_t packet_limit=sender->tx_limit/2U;
        if(packet_limit>32768U)packet_limit=32768U;
        uint32_t payload_limit=packet_limit-(uint32_t)sizeof(struct socket_packet);
        const struct kernel_uaccess_iovec iov[]={{(uintptr_t)source,4093},
            {(uintptr_t)source+4093,sizeof(source)-4093}};
        struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
        struct kernel_open_file_description *pin=(void *)sender;
        unsigned before=heap_allocations;
        int result=kernel_socket_write_unix(&pin,NULL,&cursor,sizeof(source),KERNEL_SOCKET_MSG_DONTWAIT,NULL, NULL);
        CHECK(result==(int)payload_limit && pin==(void *)sender && heap_allocations==before+1);
        CHECK(heap_last_allocation_size==packet_limit && sender->unix_memory==packet_limit &&
            sender->unix_packets==1 && !sender->tx_reserved && receiver->rx_bytes==payload_limit);
        struct kernel_socket_read_request read={0};pin=(void *)receiver;
        CHECK(kernel_socket_reserve_read(receiver,NULL,&read,&pin,sizeof(received),1, NULL)==result && !pin);
        kernel_socket_copy_read(&read,0,received,payload_limit);
        CHECK(!memcmp(received,source,payload_limit));
        CHECK(kernel_socket_finish_read(&read,0)==payload_limit && pin==(void *)receiver &&
            !sender->unix_memory && !sender->unix_packets && !sender->tx_reserved && !receiver->rx_bytes);
    }
    kernel_socket_destroy(sender);kernel_socket_destroy(receiver);CHECK(!heap_live);
    puts("PASS UNIX stream packet boundary: metadata fits half-sndbuf/32KiB allocation, gather prefix content and sender accounting returned");
}

static void receive_payload(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=0,*receiver=0;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    CHECK(write_buffer(sender,"alphabeta",9,0)==9);
    struct kernel_open_file_description *owner=(void *)receiver;
    struct kernel_socket_read_request request={0};
    char first[2]={0},second[7]={0};
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)first,2},{(uintptr_t)second,7}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0, NULL)==9);
    user_remaining=6;
    CHECK(kernel_socket_read_user(&request,(void *)receiver,&cursor)==-KERNEL_EFAULT);
    kernel_socket_finish_read(&request,1);
    CHECK(receiver->rx_bytes==9 && owner==(void *)receiver);
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};user_remaining=SIZE_MAX;
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0, NULL)==9);
    CHECK(!kernel_socket_read_user(&request,(void *)receiver,&cursor));
    kernel_socket_finish_read(&request,0);
    CHECK(!memcmp(first,"al",2) && !memcmp(second,"phabeta",7) && !receiver->rx_bytes);
    kernel_socket_destroy(sender);kernel_socket_destroy(receiver);receiver=0;

    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&receiver));
    struct pbuf *head=pbuf_alloc(PBUF_RAW,3,PBUF_RAM),*tail=pbuf_alloc(PBUF_RAW,6,PBUF_RAM);
    CHECK(head && tail && pbuf_take(head,"abc",3)==ERR_OK && pbuf_take(tail,"defghi",6)==ERR_OK);
    pbuf_cat(head,tail);
    ip_addr_t peer;IP_ADDR4(&peer,127,0,0,1);
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);udp_received(receiver,receiver->udp,head,&peer,23001);kernel_socket_protocol_leave(&irq);
    owner=(void *)receiver;cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0, NULL)==9);
    user_remaining=6;
    CHECK(kernel_socket_read_user(&request,(void *)receiver,&cursor)==-KERNEL_EFAULT);
    kernel_socket_finish_read(&request,1);user_remaining=SIZE_MAX;
    CHECK(!receiver->rx_bytes && !receiver->packets_head);
    kernel_socket_destroy(receiver);
    puts("PASS direct receive: multi-iovec, chained pbuf, STREAM fault replay and UDP fault discard");
}

static void connected_pair(struct kernel_socket **listener, struct kernel_socket **client, struct kernel_socket **server)
{
    struct kernel_socket_address address = {.family = KERNEL_SOCKET_AF_INET, .bytes = {127, 0, 0, 1}};
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, listener) == 0);
    CHECK(kernel_socket_bind(*listener, &address) == 0 && kernel_socket_listen(*listener, 4) == 0);
    CHECK(kernel_socket_getname(*listener, &address) == 0);
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, 0, client) == 0);
    int result = kernel_socket_connect(*client, &address, 0);
    CHECK(result == 0 || result == -KERNEL_EINPROGRESS);
    for (unsigned i = 0; i < 8 && !(*client)->connected; ++i)
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 8, 0});
    CHECK((*client)->connected && kernel_socket_accept(*listener, server, NULL) == 0);
}

static void tcp_more_service(void)
{
    for(unsigned i=0;i<8;i++)
        kernel_socket_service_pending((struct kernel_socket_service_budget){16,16,1});
}

static void tcp_more_read(struct kernel_socket *socket, const char *bytes, unsigned size)
{
    struct kernel_open_file_description *pin=(void *)socket;
    struct kernel_socket_read_request read={0};
    char output[32768];
    CHECK(size<=sizeof(output));
    CHECK(kernel_socket_reserve_read(socket,0,&read,&pin,size,KERNEL_SOCKET_MSG_DONTWAIT,NULL)==(int)size);
    kernel_socket_copy_read(&read,0,output,size);
    CHECK(kernel_socket_finish_read(&read,0)==size && !memcmp(output,bytes,size));
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     tcp_ack_now(socket->tcp);CHECK(tcp_output(socket->tcp)==ERR_OK);kernel_socket_protocol_leave(&scope);}
    tcp_more_service();
}

static void tcp_more_contract(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    CHECK(write_buffer(client,"abc",3,KERNEL_SOCKET_MSG_MORE)==3);
    tcp_more_service();
    CHECK(!server->rx_bytes && client->tcp->unsent && client->tcp->boaros_cork_pending);
    uint32_t deadline=client->tcp->boaros_cork_deadline;
    CHECK(boaros_lwip_tcp_cork_delay(sys_now())==200);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     struct boaros_lwip_tcp_context moved;boaros_lwip_tcp_context_init(&moved);
     CHECK(boaros_lwip_tcp_context_move_active(&boaros_lwip_tcp_default_context,&moved,client->tcp));
     CHECK(boaros_lwip_tcp_cork_delay(sys_now())==UINT32_MAX && moved.active==client->tcp);
     CHECK(moved.active->boaros_cork_deadline==deadline);
     CHECK(boaros_lwip_tcp_context_move_active(&moved,&boaros_lwip_tcp_default_context,client->tcp));
     CHECK(!moved.active && boaros_lwip_tcp_cork_delay(sys_now())==200);
     kernel_socket_protocol_leave(&scope);}
    clock_ns+=100000000;
    CHECK(write_buffer(client,"def",3,KERNEL_SOCKET_MSG_MORE)==3);
    tcp_more_service();
    CHECK(!server->rx_bytes && client->tcp->boaros_cork_deadline==deadline);
    CHECK(write_buffer(client,"g",1,0)==1);
    tcp_more_service();tcp_more_read(server,"abcdefg",7);
    CHECK(!client->tcp->boaros_cork_pending);

    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    CHECK(write_buffer(client,"n",1,KERNEL_SOCKET_MSG_MORE)==1);
    tcp_more_service();CHECK(!server->rx_bytes);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    tcp_more_service();tcp_more_read(server,"n",1);

    CHECK(write_buffer(client,"timeout",7,KERNEL_SOCKET_MSG_MORE)==7);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     clock_ns+=199000000;CHECK(!boaros_lwip_tcp_cork_due(sys_now(),1) && !server->rx_bytes);
     clock_ns+=1000000;CHECK(boaros_lwip_tcp_cork_due(sys_now(),1)==1);
     kernel_socket_protocol_leave(&scope);}
    tcp_more_service();tcp_more_read(server,"timeout",7);
    CHECK(!client->tcp->boaros_cork_pending);

    /* MORE is not CORK: an unrelated ordinary output opportunity can push it. */
    CHECK(write_buffer(client,"ack",3,KERNEL_SOCKET_MSG_MORE)==3);
    tcp_more_service();CHECK(!server->rx_bytes);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     CHECK(tcp_output(client->tcp)==ERR_OK);kernel_socket_protocol_leave(&scope);}
    tcp_more_service();tcp_more_read(server,"ack",3);

    unsigned mss=tcp_mss(client->tcp);static char payload[32768];
    CHECK(mss+3<sizeof(payload));memset(payload,'m',mss+3);
    CHECK(write_buffer(client,payload,mss+3,KERNEL_SOCKET_MSG_MORE)==(int)mss+3);
    /* Output has published the full segment even before incoming ACK processing. */
    CHECK(TCP_SEQ_GEQ(client->tcp->snd_nxt,client->tcp->snd_lbb-3));
    tcp_more_service();CHECK(server->rx_bytes>=mss);
    clock_ns+=200000000;tcp_more_service();tcp_more_read(server,payload,mss+3);

    /* Absolute millisecond deadlines survive clock wraparound. */
    clock_ns=((uint64_t)UINT32_MAX-100)*1000000;
    CHECK(write_buffer(client,"wrap",4,KERNEL_SOCKET_MSG_MORE)==4);
    CHECK(boaros_lwip_tcp_cork_delay(sys_now())==200);
    clock_ns+=200000000;tcp_more_service();tcp_more_read(server,"wrap",4);
    CHECK(write_buffer(client,"fin",3,KERNEL_SOCKET_MSG_MORE)==3);
    CHECK(!kernel_socket_shutdown(client,1));tcp_more_service();tcp_more_read(server,"fin",3);
    CHECK(server->peer_closed);
    kernel_socket_destroy(client);kernel_socket_destroy(server);kernel_socket_destroy(listener);
    tcp_more_service();kernel_socket_network_purge_orphans();tcp_more_service();
    CHECK(!heap_live && boaros_lwip_tcp_cork_delay(sys_now())==UINT32_MAX);
    puts("PASS TCP MSG_MORE: syscall tail, ordinary ACK policy, full MSS, fixed deadline/wrap, NODELAY push, FIN and timer ownership");
}

static void tcp_cork_contract(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);int value;
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
    CHECK(!kernel_socket_get_option(client,KERNEL_SOCKET_CORK,&value) && value==1);
    CHECK(write_buffer(client,"abc",3,0)==3 && write_buffer(client,"d",1,KERNEL_SOCKET_MSG_MORE)==1);
    tcp_more_service();CHECK(!server->rx_bytes);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     CHECK(tcp_output(client->tcp)==ERR_OK);kernel_socket_protocol_leave(&scope);}
    tcp_more_service();CHECK(!server->rx_bytes);
    clock_ns+=199000000;tcp_more_service();CHECK(!server->rx_bytes);
    clock_ns+=1000000;tcp_more_service();tcp_more_read(server,"abcd",4);
    CHECK(!kernel_socket_get_option(client,KERNEL_SOCKET_CORK,&value) && value==1);
    CHECK(write_buffer(client,"e",1,0)==1);tcp_more_service();CHECK(!server->rx_bytes);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    tcp_more_service();tcp_more_read(server,"e",1);
    CHECK(write_buffer(client,"f",1,0)==1);tcp_more_service();CHECK(!server->rx_bytes);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,0));
    tcp_more_service();tcp_more_read(server,"f",1);

    /* Uncork without NODELAY does not erase an outstanding small segment. */
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,0));
    CHECK(write_buffer(client,"r",1,0)==1);tcp_more_service();CHECK(client->tcp->unacked);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
    CHECK(write_buffer(client,"s",1,0)==1 && !kernel_socket_set_option(client,KERNEL_SOCKET_CORK,0));
    tcp_more_service();CHECK(server->rx_bytes==1 && client->tcp->unsent);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     tcp_ack_now(server->tcp);CHECK(tcp_output(server->tcp)==ERR_OK);kernel_socket_protocol_leave(&scope);}
    tcp_more_service();tcp_more_read(server,"rs",2);

    unsigned mss=tcp_mss(client->tcp);static char payload[32768];memset(payload,'m',sizeof(payload));
    CHECK(mss+1<sizeof(payload) && !kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
    CHECK(write_buffer(client,payload,mss+1,0)==(int)mss+1);tcp_more_service();
    CHECK(server->rx_bytes==mss && client->tcp->unsent && client->tcp->unsent->len==1);
    tcp_more_read(server,payload,mss);CHECK(!server->rx_bytes);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,0));tcp_more_service();tcp_more_read(server,payload,1);

    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
    CHECK(write_buffer(client,"tail",4,0)==4 && write_buffer(server,"q",1,0)==1);
    tcp_more_service();tcp_more_read(client,"q",1);
    CHECK(!server->rx_bytes && !server->tcp->unacked && client->tcp->unsent);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,0));tcp_more_service();tcp_more_read(server,"tail",4);

    /* Retransmission is older sequence space, never a withheld new tail. */
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     CHECK(tcp_write(client->tcp,"r",1,TCP_WRITE_FLAG_COPY)==ERR_OK && tcp_output(client->tcp)==ERR_OK);
     CHECK(client->tcp->unacked && !client->tcp->unsent);
     CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
     CHECK(tcp_write(client->tcp,"tail",4,TCP_WRITE_FLAG_COPY)==ERR_OK);
     CHECK(tcp_rexmit(client->tcp)==ERR_OK && tcp_output(client->tcp)==ERR_OK);
     CHECK(client->tcp->unsent && client->tcp->unsent->len==4);
     kernel_socket_protocol_leave(&scope);}
    tcp_more_service();CHECK(server->rx_bytes==1);tcp_more_read(server,"r",1);
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,0));tcp_more_service();tcp_more_read(server,"tail",4);

    /* A push survives a closed window. MORE overrides it for the application
     * attempt, but ordinary window-open output can send the combined tail. */
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_CORK,1));
    tcpwnd_size_t window=client->tcp->snd_wnd;client->tcp->snd_wnd=0;
    CHECK(write_buffer(client,"a",1,0)==1 && !kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    CHECK(write_buffer(client,"b",1,KERNEL_SOCKET_MSG_MORE)==1);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     client->tcp->snd_wnd=window;CHECK(boaros_lwip_tcp_output_more(client->tcp,1)==ERR_OK && client->tcp->unsent);
     CHECK(tcp_output(client->tcp)==ERR_OK && !client->tcp->unsent);
     kernel_socket_protocol_leave(&scope);}
    tcp_more_service();tcp_more_read(server,"ab",2);

    /* A full application admission budget must push before waiting for room,
     * even when CORK and MSG_MORE are both active. */
    CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_SNDBUF,0));
    window=client->tcp->snd_wnd;client->tcp->snd_wnd=0;
    struct kernel_open_file_description *pin=(void *)client;
    struct kernel_socket_write_request write={0};kernel_socket_stream_begin(&write,&pin);
    CHECK(kernel_socket_stream_reserve(&write,4608,KERNEL_SOCKET_MSG_MORE)==4608);
    CHECK(kernel_socket_stream_commit(&write,payload,4608,KERNEL_SOCKET_MSG_MORE)==4608);
    CHECK(kernel_socket_stream_reserve(&write,1,KERNEL_SOCKET_MSG_MORE)==-KERNEL_EAGAIN);
    client->tcp->snd_wnd=window;tcp_more_service();
    kernel_socket_stream_finish(&write);CHECK(pin==(void *)client);
    tcp_more_read(server,payload,4608);

    CHECK(write_buffer(client,"abort",5,0)==5);tcp_more_service();CHECK(client->tcp->boaros_cork_pending);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
     tcp_abort(client->tcp);CHECK(!client->tcp);kernel_socket_protocol_leave(&scope);}
    tcp_more_service();CHECK(boaros_lwip_tcp_cork_delay(sys_now())==UINT32_MAX);
    kernel_socket_destroy(client);kernel_socket_destroy(server);kernel_socket_destroy(listener);
    tcp_more_service();kernel_socket_network_purge_orphans();tcp_more_service();CHECK(!heap_live);
    puts("PASS TCP_CORK: persistent tail, deadline, NODELAY/uncork Minshall, MSS, retransmission, window/push, pressure and abort cleanup");
}

static void tcp_detached_copy(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;
    kernel_socket_protocol_enter(&scope);
    struct tcp_pcb *pcb=client->tcp;
    u16_t mss=boaros_lwip_tcp_detached_mss(pcb);
    u32_t sequence=pcb->snd_lbb;
    tcpwnd_size_t room=pcb->snd_buf;
    CHECK(!pcb->unsent && !pcb->snd_queuelen && mss>16 && 2U*mss+7<=room);
    kernel_socket_protocol_leave(&scope);
    static unsigned char source[32768], output[32768];
    unsigned length=2U*mss+7;
    CHECK(length<=sizeof(source));
    for (unsigned i=0;i<length;i++) source[i]=(unsigned char)(31*i+3);
    const struct kernel_socket_write_segment vector[]={{source,17},{source+17,length-17}};
    unsigned pool=MEMP_STATS_GET(used,MEMP_TCP_SEG);
    struct tcp_seg *prepared=NULL;
    CHECK(boaros_lwip_tcp_preparev(vector,2,(u16_t)length,mss,&prepared)==ERR_OK);
    CHECK(prepared && prepared->next && prepared->next->next && !prepared->next->next->next);
    CHECK(MEMP_STATS_GET(used,MEMP_TCP_SEG)==pool+3);
    memset(source,0,length);

    kernel_socket_protocol_enter(&scope);
    u16_t written;
    CHECK(!pcb->unsent && pcb->snd_lbb==sequence && pcb->snd_buf==room);
    struct tcp_seg *first=prepared, *suffix=first->next;
    CHECK(boaros_lwip_tcp_publishv(pcb,&prepared,mss-1,0,&written)==ERR_WOULDBLOCK);
    CHECK(!written && prepared==first && !pcb->unsent && pcb->snd_lbb==sequence);
    CHECK(boaros_lwip_tcp_publishv(pcb,&prepared,mss,0,&written)==ERR_OK && written==mss);
    CHECK(prepared==suffix && pcb->unsent==first && !first->next && pcb->snd_queuelen==1);
    CHECK(pcb->snd_lbb==sequence+mss && pcb->snd_buf==room-mss);
    CHECK(!(TCPH_FLAGS(first->tcphdr)&TCP_PSH) && tcp_output(pcb)==ERR_OK);
    kernel_socket_protocol_leave(&scope);
    tcp_segs_free(prepared); prepared=NULL;
    for (unsigned i=0;i<8 && !server->rx_bytes;i++)
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    struct kernel_open_file_description *pin=(void *)server;
    struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(server,0,&read,&pin,mss,0,NULL)==mss);
    kernel_socket_copy_read(&read,0,output,mss);
    CHECK(kernel_socket_finish_read(&read,0)==mss && pin==(void *)server);
    for (unsigned i=0;i<mss;i++) CHECK(output[i]==(unsigned char)(31*i+3));

    /* Exhaust only the segment pool. A prepared prefix survives the second
     * allocation failure; both the unused pbuf and the prefix are returned. */
    void *held[MEMP_NUM_TCP_SEG]; unsigned count=0;
    while (count<MEMP_NUM_TCP_SEG && (held[count]=memp_malloc(MEMP_TCP_SEG))) count++;
    CHECK(count>1); memp_free(MEMP_TCP_SEG,held[--count]);
    mem_size_t heap=STATS_GET(mem.used);
    CHECK(boaros_lwip_tcp_preparev(vector,2,(u16_t)length,mss,&prepared)==ERR_MEM);
    CHECK(prepared && prepared->len==mss && !prepared->next);
    tcp_segs_free(prepared);
    CHECK(STATS_GET(mem.used)==heap);
    while (count) memp_free(MEMP_TCP_SEG,held[--count]);
    kernel_socket_protocol_enter(&scope);
    if (client->tcp) tcp_abort(client->tcp);
    if (server->tcp) tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    for (unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(MEMP_STATS_GET(used,MEMP_TCP_SEG)==pool && !heap_live);
    puts("PASS detached TCP COPY: immutable final pbuf, live prefix publication, untouched suffix and allocation rollback");
}

static void tcp_reset_receive_retry(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    tcp_nagle_disable(client->tcp);
    CHECK(write_buffer(client,"kept",4,0)==4);
    netif_poll_all();
    struct kernel_open_file_description *pin=(void *)server;
    struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(server,0,&read,&pin,4,0,NULL)==4);
    heap_fail_after=1;
    CHECK(write_buffer(client,"x",1,0)==1);
    netif_poll_all();
    CHECK(!heap_fail_after && server->receive_retry && server->tcp->refused_data &&
          server->work_links[receive_wait.slot].queued);
    reset_retry_socket=server;
    tcp_abort(server->tcp);
    reset_retry_socket=NULL;
    CHECK(reset_retry_observations && !server->tcp && !server->receive_retry &&
          !server->work_links[receive_wait.slot].queued && server->pending_error==-KERNEL_ECONNABORTED);
    kernel_socket_protocol_leave(&scope);
    char bytes[4];kernel_socket_copy_read(&read,0,bytes,sizeof(bytes));
    CHECK(kernel_socket_finish_read(&read,0)==4 && !memcmp(bytes,"kept",4) && pin==(void *)server);
    kernel_socket_protocol_enter(&scope);
    if (client->tcp) tcp_abort(client->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!heap_live && !receive_wait.head);
    puts("PASS TCP abort: retry state and list end together, reserved receive prefix survives, resources returned");
}

static void tcp_loopback_checksum(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *loop=loopback_netif();
    struct netif ethernet={0};
    ip4_addr_t local,mask,empty;
    IP4_ADDR(&local,10,77,0,2);IP4_ADDR(&mask,255,255,255,0);ip4_addr_set_zero(&empty);
    CHECK(loop && netif_add(&ethernet,&local,&mask,&empty,NULL,route_interface_init,ip_input));
#if LWIP_CHECKSUM_CTRL_PER_NETIF
    CHECK(ethernet.chksum_flags==NETIF_CHECKSUM_ENABLE_ALL &&
          loop->chksum_flags==(NETIF_CHECKSUM_ENABLE_ALL &
              ~(NETIF_CHECKSUM_GEN_TCP|NETIF_CHECKSUM_CHECK_TCP)));
#endif
    const struct ip_globals saved=ip_data;
    ip_data.current_netif=loop;
    ip_addr_copy(ip_data.current_iphdr_src,server->tcp->remote_ip);
    ip_addr_copy(ip_data.current_iphdr_dest,server->tcp->local_ip);
    struct pbuf *wire=pbuf_alloc(PBUF_RAW,TCP_HLEN+1,PBUF_RAM);CHECK(wire);
    struct tcp_hdr *header=wire->payload;
    memset(header,0,TCP_HLEN);
    header->src=lwip_htons(server->tcp->remote_port);
    header->dest=lwip_htons(server->tcp->local_port);
    header->seqno=lwip_htonl(server->tcp->rcv_nxt);
    header->ackno=lwip_htonl(server->tcp->lastack);
    header->wnd=lwip_htons(8192);
    TCPH_HDRLEN_FLAGS_SET(header,5,TCP_ACK);
    ((char *)header)[TCP_HLEN]='x';
    header->chksum=ip_chksum_pseudo(wire,IP_PROTO_TCP,wire->tot_len,
        ip_current_src_addr(),ip_current_dest_addr())^lwip_htons(1);
    CHECK(ip_chksum_pseudo(wire,IP_PROTO_TCP,wire->tot_len,ip_current_src_addr(),ip_current_dest_addr()));
    struct pbuf *local_packet=pbuf_clone(PBUF_RAW,PBUF_RAM,wire);CHECK(local_packet);
    unsigned errors=STATS_GET(tcp.chkerr);
    /* The destination interface is lo in both calls; ingress is what matters. */
    ip_data.current_input_netif=&ethernet;
    tcp_input(wire,&ethernet);
    CHECK(STATS_GET(tcp.chkerr)==errors+1 && !server->rx_bytes);
    ip_data.current_input_netif=loop;
    tcp_input(local_packet,loop);
    char received=0;
    CHECK(STATS_GET(tcp.chkerr)==errors+1 && server->rx_bytes==1 &&
          pbuf_copy_partial(server->packets_head->payload,&received,1,0)==1 && received=='x');
    ip_data=saved;
    netif_remove(&ethernet);
    if(client->tcp)tcp_abandon(client->tcp,0);
    if(server->tcp)tcp_abandon(server->tcp,0);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,0});
    puts("PASS TCP checksum: the same bad checksum is rejected on Ethernet ingress and ignored on internal lo");
}

static struct tcp_pcb *close_to_timewait(struct kernel_socket *client, struct kernel_socket *server)
{
    /* Only protocol packets advance. No budget is given to deferred socket work. */
    struct tcp_pcb *pcb=client->tcp;
    CHECK(kernel_socket_shutdown(client, 1) == 0);
    netif_poll_all();
    CHECK(kernel_socket_shutdown(server, 1) == 0);
    netif_poll_all();
    CHECK(pcb->state==TIME_WAIT && !pcb->callback_arg && !pcb->errf);
    CHECK(!client->tcp && !server->tcp && client->peer_closed);
    return pcb;
}

static void timewait_owner(unsigned mode)
{
    struct kernel_socket *listener = NULL, *client = NULL, *server = NULL;
    connected_pair(&listener, &client, &server);
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    struct tcp_pcb *borrowed = close_to_timewait(client, server);
    unsigned owned_before = heap_live;
    if (mode == 0) {
        void *held[MEMP_NUM_TCP_PCB]; unsigned count = 0;
        while (count < MEMP_NUM_TCP_PCB && (held[count] = memp_malloc(MEMP_TCP_PCB))) ++count;
        CHECK(count && lwip_stats.memp[MEMP_TCP_PCB]->used == MEMP_NUM_TCP_PCB);
        struct tcp_pcb *next = tcp_new(); CHECK(next);
        /* Stop at the owner boundary; do not access the released old PCB. */
        CHECK(client->tcp == NULL && client->peer_closed && heap_live == owned_before);
        tcp_abort(next);
        for (unsigned i = 0; i < count; ++i) memp_free(MEMP_TCP_PCB, held[i]);
    } else if (mode == 1) {
        for (unsigned i = 0; i <= 2U * TCP_MSL / TCP_SLOW_INTERVAL + 1; ++i) tcp_slowtmr();
        CHECK(client->tcp == NULL && client->peer_closed && heap_live == owned_before);
    } else {
        kernel_socket_destroy(client); client = NULL;
        CHECK(borrowed->callback_arg == NULL && borrowed->errf == NULL);
        tcp_abort(borrowed);
        CHECK(heap_live + 1 == owned_before);
    }
    if (client) kernel_socket_destroy(client);
    kernel_socket_destroy(server); kernel_socket_destroy(listener);
    kernel_socket_protocol_leave(&irq);
    CHECK(!heap_live && !tcp_tw_pcbs);
    printf("PASS TIME_WAIT owner %s without deferred socket service\n", mode == 0 ? "capacity reclaim" : mode == 1 ? "timer expiry" : "already destroyed");
}

static void raw_error(void *context, err_t error) { (void)context; (void)error; abort(); }
static void raw_owner_control(void)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    unsigned marker = 17;
    for (unsigned with_arg = 0; with_arg < 2; ++with_arg) {
        struct tcp_pcb *pcb = tcp_new(); CHECK(pcb);
        tcp_arg(pcb, with_arg ? &marker : NULL); tcp_err(pcb, raw_error);
        pcb->state = TIME_WAIT; TCP_REG(&tcp_tw_pcbs, pcb);
        tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(&irq);
    CHECK(marker == 17 && !heap_live);
    puts("PASS unowned raw TIME_WAIT PCBs do not invoke a socket owner");
}

struct raw_tcp_observation {
    struct tcp_pcb *accepted;
    const char *payload;
    unsigned bytes, closed;
};
static err_t raw_tcp_received(void *context,struct tcp_pcb *pcb,struct pbuf *p,err_t error)
{
    struct raw_tcp_observation *observation=context;
    CHECK(error==ERR_OK);
    if (!p) { observation->closed++; return ERR_OK; }
    char bytes[3];
    CHECK(p->tot_len==sizeof(bytes) && pbuf_copy_partial(p,bytes,sizeof(bytes),0)==sizeof(bytes));
    CHECK(!memcmp(bytes,observation->payload,sizeof(bytes)));
    observation->bytes+=sizeof(bytes);tcp_recved(pcb,p->tot_len);pbuf_free(p);
    return ERR_OK;
}
static void raw_tcp_error(void *context,err_t error)
{
    struct raw_tcp_observation *observation=context;
    CHECK(error==ERR_CLSD);observation->accepted=NULL;
}
static err_t raw_tcp_accepted(void *context,struct tcp_pcb *pcb,err_t error)
{
    struct raw_tcp_observation *observation=context;
    CHECK(error==ERR_OK && !observation->accepted);
    observation->accepted=pcb;
    tcp_arg(pcb,context);tcp_recv(pcb,raw_tcp_received);tcp_err(pcb,raw_tcp_error);
    return ERR_OK;
}
static void raw_tcp_ingress(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    for (unsigned ipv6=0;ipv6<2;ipv6++) {
        struct raw_tcp_observation observation={.payload="raw"};
        ip_addr_t address;
        if (ipv6) IP_ADDR6_HOST(&address,0,0,0,1);
        else IP_ADDR4(&address,127,0,0,1);
        struct tcp_pcb *listener=tcp_new_ip_type(ipv6?IPADDR_TYPE_V6:IPADDR_TYPE_V4);
        struct tcp_pcb *client=tcp_new_ip_type(ipv6?IPADDR_TYPE_V6:IPADDR_TYPE_V4);
        CHECK(listener && client && tcp_bind(listener,&address,0)==ERR_OK);
        listener=tcp_listen(listener);CHECK(listener);
        tcp_arg(listener,&observation);tcp_accept(listener,raw_tcp_accepted);
        tcp_arg(client,&observation);tcp_recv(client,raw_tcp_received);tcp_err(client,raw_error);
        CHECK(tcp_connect(client,&address,listener->local_port,NULL)==ERR_OK);
        netif_poll_all();
        CHECK(observation.accepted && client->state==ESTABLISHED);
        CHECK(tcp_write(client,observation.payload,3,TCP_WRITE_FLAG_COPY)==ERR_OK && tcp_output(client)==ERR_OK);
        netif_poll_all();CHECK(observation.bytes==3);
        CHECK(tcp_shutdown(client,0,1)==ERR_OK);netif_poll_all();
        CHECK(observation.closed==1);
        CHECK(tcp_shutdown(observation.accepted,0,1)==ERR_OK);netif_poll_all();
        CHECK(observation.closed==2 && !observation.accepted && client->state==TIME_WAIT);
        /* A late segment traverses the TIME_WAIT ingress path with the raw
         * callback argument still owned by this caller. RST leaves it alive. */
        tcp_rst(client,client->rcv_nxt,client->snd_nxt,&client->remote_ip,&client->local_ip,
                client->remote_port,client->local_port);
        netif_poll_all();CHECK(client->state==TIME_WAIT && client->callback_arg==&observation);
        tcp_abort(client);CHECK(tcp_close(listener)==ERR_OK);netif_poll_all();
        CHECK(!tcp_active_pcbs && !tcp_tw_pcbs && !lwip_stats.mem.used && !heap_live);
        printf("PASS IPv%u raw TCP callback context survives listen, handshake, data and TIME_WAIT ingress\n",ipv6?6:4);
    }
    kernel_socket_protocol_leave(&scope);
}

static struct {
    struct kernel_socket *sender;
    struct socket_packet *incoming;
    struct kernel_socket_read_request *read;
    unsigned live;
} unix_destroy_race;

static void finish_during_unix_destroy(void *allocation)
{
    CHECK(allocation==unix_destroy_race.incoming && arch_interrupt_is_enabled());
    CHECK(unix_destroy_race.sender->destroying==1 && unix_destroy_race.sender->unix_packets==1);
    CHECK(kernel_socket_finish_read(unix_destroy_race.read,0)==4);
    /* The closer still uses sender after this callback. Returning its last
     * packet must not transfer final release until that cleanup is complete. */
    CHECK(unix_destroy_race.sender->destroying==1 && !unix_destroy_race.sender->unix_packets &&
        !unix_destroy_race.sender->unix_memory && heap_live+1==unix_destroy_race.live);
}

static void unix_destroy_interleaving(void)
{
    unsigned before=heap_live;
    struct kernel_socket *sender=0,*receiver=0;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    CHECK(write_buffer(sender,"sent",4,0)==4);
    CHECK(write_buffer(receiver,"back",4,0)==4);
    struct kernel_open_file_description *owner=(void *)receiver;
    struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(receiver,0,&read,&owner,4,0, NULL)==4 && !owner);
    char bytes[4];
    kernel_socket_copy_read(&read,0,bytes,sizeof(bytes));
    CHECK(!memcmp(bytes,"sent",sizeof(bytes)) && heap_live==before+4);
    unix_destroy_race.sender=sender;
    unix_destroy_race.incoming=sender->packets_head;
    unix_destroy_race.read=&read;
    unix_destroy_race.live=heap_live;
    heap_release_once=finish_during_unix_destroy;
    kernel_socket_destroy(sender);
    CHECK(!heap_release_once && !read.socket && owner==(void *)receiver && !receiver->rx_bytes);
    CHECK(heap_live==before+1);
    kernel_socket_destroy(receiver);
    CHECK(heap_live==before);
    memset(&unix_destroy_race,0,sizeof(unix_destroy_race));
    puts("PASS UNIX destroy preemption: last sent packet returned during incoming cleanup, closer retains final release");
}

static void unix_sender_owner(void)
{
    kernel_socket_network_initialize();
    static unsigned char bytes[65537];
    for(unsigned reverse=0;reverse<2;reverse++) {
        struct kernel_socket *sender=0,*receiver=0;
        CHECK(kernel_socket_pair(&test_heap,SOCKET_DGRAM,&sender,&receiver)==0);
        CHECK(kernel_socket_set_option(receiver,KERNEL_SOCKET_RCVBUF,0)==0);
        CHECK(write_buffer(sender,bytes,sizeof(bytes),0)==sizeof(bytes));
        if(reverse) {
            kernel_socket_destroy(receiver);
            CHECK(write_buffer(sender,bytes,1,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ECONNREFUSED);
            CHECK(write_buffer(sender,bytes,1,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ENOTCONN);
            kernel_socket_destroy(sender);
        } else {
            kernel_socket_destroy(sender);
            /* Receiver still owns message and dead sender accounting until final drain. */
            kernel_socket_destroy(receiver);
        }
        CHECK(!heap_live);
    }
    unix_destroy_interleaving();
    puts("PASS UNIX queued sender survives endpoint close and returns accounting once in either close order");
}

struct loopback_observation {
    struct udp_pcb *sender;
    ip_addr_t address;
    struct pbuf *payload;
    u16_t port;
    unsigned wakes, received;
    err_t error;
};

static void loopback_wake(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event;CHECK(reason==KERNEL_WAIT_WOKEN);((struct loopback_observation *)node->context)->wakes++;}

static void loopback_receive(void *context, struct udp_pcb *pcb, struct pbuf *packet,
    const ip_addr_t *address, u16_t port)
{
    (void)pcb; (void)address; (void)port;
    char data[3];
    CHECK(packet->tot_len == 3 && pbuf_copy_partial(packet, data, 3, 0) == 3);
    CHECK(!memcmp(data, "new", 3));
    ((struct loopback_observation *)context)->received++;
    pbuf_free(packet);
}

static void loopback_emit(void *context)
{
    struct loopback_observation *observation = context;
    observation->error = udp_sendto(observation->sender, observation->payload,
        &observation->address, observation->port);
    pbuf_free(observation->payload);
    observation->payload = NULL;
}

static void loopback_prepare(struct loopback_observation *observation)
{
    CHECK(!observation->payload);
    observation->payload = pbuf_alloc(PBUF_TRANSPORT, 3, PBUF_RAM);
    CHECK(observation->payload && pbuf_take(observation->payload, "new", 3) == ERR_OK);
}

static void loopback_work(void)
{
    kernel_socket_network_initialize();
    uint64_t frozen = clock_ns;
    u32_t initial_memory = lwip_stats.mem.used;
    u32_t initial_udp = lwip_stats.memp[MEMP_UDP_PCB]->used;
    for (unsigned ipv6 = 0; ipv6 < 2; ++ipv6) {
        struct loopback_observation observation = {0};
        if (ipv6) IP_ADDR6_HOST(&observation.address, 0, 0, 0, 1);
        else IP_ADDR4(&observation.address, 127, 0, 0, 1);
        struct udp_pcb *receiver = udp_new_ip_type(ipv6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        observation.sender = udp_new_ip_type(ipv6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        CHECK(receiver && observation.sender);
        CHECK(udp_bind(receiver, &observation.address, 0) == ERR_OK);
        observation.port = receiver->local_port;
        udp_recv(receiver, loopback_receive, &observation);
        loopback_prepare(&observation);
        struct kernel_wait_node work_node={.callback=loopback_wake,.context=&observation};
        kernel_wait_queue_init(&model_work_queue);model_work_queue.head=model_work_queue.tail=&work_node;
        kernel_socket_network_hooks(NULL, &model_work_queue, NULL, &observation);
        struct kernel_socket_service_result service = kernel_socket_service_pending(
            (struct kernel_socket_service_budget){0, 8, 0});
        CHECK(!service.runnable && !work_pending_snapshot());

        /* 不运行 timer 或 inline service；真实入队必须发布可运行工作并请求唤醒。 */
        struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
        loopback_emit(&observation);
        loopback_prepare(&observation);loopback_emit(&observation);
        kernel_socket_protocol_leave(&irq);
        CHECK(observation.error == ERR_OK && !observation.received);
        CHECK(work_pending_snapshot() && observation.wakes==1);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 1 && service.runnable);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && observation.received == 2 && !service.runnable);

        /* 模拟 protocol timer 在本批 loopback 阶段之后入队；返回值仍须报告新工作。 */
        unsigned before_wakes = observation.wakes;
        loopback_prepare(&observation);
        sys_timeout(0, loopback_emit, &observation);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 1});
        CHECK(observation.error == ERR_OK && service.packets == 0 && service.timers == 1);
        CHECK(service.runnable && work_pending_snapshot() && observation.wakes > before_wakes);
        CHECK(observation.received == 2);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 3);
        CHECK(!service.runnable && !work_pending_snapshot());

        void *held[MEM_SIZE / 1024 + 128];
        unsigned count = 0;
        void *allocation;
        u32_t before_memory = lwip_stats.mem.used;
        loopback_prepare(&observation);
        while ((allocation = mem_malloc(1024)) != NULL) {
            CHECK(count < sizeof(held) / sizeof(held[0])); held[count++] = allocation;
        }
        while ((allocation = mem_malloc(1)) != NULL) {
            CHECK(count < sizeof(held) / sizeof(held[0])); held[count++] = allocation;
        }
        before_wakes = observation.wakes;
        kernel_socket_protocol_enter(&irq);
        loopback_emit(&observation);
        kernel_socket_protocol_leave(&irq);
        CHECK(observation.error == ERR_MEM && observation.wakes == before_wakes);
        CHECK(!work_pending_snapshot() && observation.received == 3);
        while (count) mem_free(held[--count]);
        CHECK(lwip_stats.mem.used == before_memory);
        loopback_prepare(&observation);
        kernel_socket_protocol_enter(&irq);
        loopback_emit(&observation);
        kernel_socket_protocol_leave(&irq);
        CHECK(observation.error == ERR_OK && work_pending_snapshot());
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 4);
        udp_remove(observation.sender); udp_remove(receiver);
        kernel_socket_network_hooks(NULL, NULL, NULL, NULL);
        CHECK(clock_ns == frozen && lwip_stats.mem.used == initial_memory);
        CHECK(lwip_stats.memp[MEMP_UDP_PCB]->used == initial_udp);
        printf("PASS IPv%u loopback work publication, late enqueue and OOM rollback with frozen clock\n", ipv6 ? 6 : 4);
    }
}

static void *competing_owner(void *unused)
{
    (void)unused;
    struct kernel_socket_protocol_scope scope;
    kernel_socket_protocol_enter(&scope);
    CHECK(arch_interrupt_is_enabled() && protocol_owner==&test_context && protocol_depth==1);
    CHECK(test_context.allocation_depth==1);
    kernel_socket_protocol_leave(&scope);
    CHECK(arch_interrupt_is_enabled() && !test_context.allocation_depth);
    return NULL;
}
static void protocol_ownership(void)
{
    network_host_irq=0;
    struct kernel_socket_protocol_scope outer,nested;
    kernel_socket_protocol_enter(&outer);
    CHECK(arch_interrupt_is_enabled() && protocol_depth==1 && acquired==1);
    CHECK(protocol_owner==&test_context && test_context.allocation_depth==1);
    uintptr_t enabled=arch_interrupt_save();
    for(unsigned on=0;on<2;on++) {
        if(on) arch_interrupt_restore(enabled);
        unsigned saves=network_host_irq_saves,restores=network_host_irq_restores;
        kernel_socket_protocol_enter(&nested);
        CHECK(arch_interrupt_is_enabled()==(int)on && protocol_depth==2 && acquired==1);
        kernel_socket_protocol_leave(&nested);
        CHECK(arch_interrupt_is_enabled()==(int)on && protocol_depth==1);
        CHECK(network_host_irq_saves==saves && network_host_irq_restores==restores);
        CHECK(acquired==1 && test_context.allocation_depth==1);
    }
    competing=1;
    pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,competing_owner,NULL));
    CHECK(!pthread_mutex_lock(&handshake));
    while(!waiter_registered) CHECK(!pthread_cond_wait(&handshake_changed,&handshake));
    CHECK(acquired==1 && protocol_owner==&test_context);
    CHECK(!pthread_mutex_unlock(&handshake));
    kernel_socket_protocol_leave(&outer);
    CHECK(!arch_interrupt_is_enabled() && !test_context.allocation_depth);
    CHECK(!pthread_join(thread,NULL));
    CHECK(acquired==2 && !protocol_depth && !protocol_owner);
    puts("PASS protocol owner: outer IRQ enabled, nested preserves IRQ on/off without IRQ operations, competing owner waits, entry IRQ and allocation depth restored");
}

static void protocol_transaction(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=23100,.bytes={127,0,0,1}};
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM, 0,&listener));
    CHECK(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM, 0,&client));
    int connected=kernel_socket_connect(client,&address,0);
    CHECK(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<4;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!kernel_socket_accept(listener,&server, NULL));
    struct kernel_open_file_description *pin=(void *)client;
    struct kernel_socket_write_request write={0};
    kernel_socket_stream_begin(&write,&pin);
    CHECK(kernel_socket_stream_reserve(&write,1,0)==1);
    unsigned locks=acquired,services=protocol_services;
    /* 模拟outer归还时获得资格的下一调用者，必须已经看到本批服务结果。 */
    handoff_peer=server;handoff_bytes=1;
    CHECK(kernel_socket_stream_commit(&write,"x",1,0)==1);
    CHECK(!handoff_peer && acquired==locks+1 && protocol_services==services+1);
    locks=acquired;
    kernel_socket_stream_finish(&write);
    CHECK(pin==(void *)client && acquired==locks);
    struct kernel_open_file_description *reader=(void *)server;
    struct kernel_socket_read_request read={0};
    locks=acquired;services=protocol_services;
    CHECK(kernel_socket_reserve_read(server,NULL,&read,&reader,1,0, NULL)==1);
    CHECK(acquired==locks && protocol_services==services);
    kernel_socket_finish_read(&read,0);
    CHECK(acquired==locks && protocol_services==services);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    locks=acquired;services=protocol_services;
    CHECK(write_buffer(client,"y",1,0)==1);
    CHECK(acquired==locks && protocol_services==services);
    kernel_socket_protocol_leave(&scope);
    locks=acquired;
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(acquired==locks+1);
    kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    for(unsigned i=0;i<4;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    puts("PASS socket transaction: commit service before handoff, one owner for receive/service, no empty finish lock or nested service");
}

static void receive_prefix(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=NULL,*receiver=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    CHECK(write_buffer(sender,"abc",3,0)==3);
    CHECK(write_buffer(sender,"defg",4,0)==4);
    CHECK(write_buffer(sender,"hijkl",5,0)==5);
    struct kernel_open_file_description *pin=(void *)receiver;
    struct kernel_socket_read_request request={0};
    unsigned locks=acquired;
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,10,0, NULL)==10);
    char a[2]={0},b[8]={0};
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)a,2},{(uintptr_t)b,8}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    CHECK(!kernel_socket_read_user(&request,(void *)receiver,&cursor));
    CHECK(acquired==locks && !memcmp(a,"ab",2) && !memcmp(b,"cdefghij",8));
    kernel_socket_finish_read(&request,0);
    CHECK(acquired==locks && receiver->rx_bytes==2);
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0, NULL)==2);
    struct kernel_open_file_description *other=(void *)receiver;
    struct kernel_socket_read_request second={0};
    CHECK(kernel_socket_reserve_read(receiver,NULL,&second,&other,32,1, NULL)==-KERNEL_EAGAIN && !second.socket);
    CHECK(write_buffer(sender,"late",4,0)==4);
    kernel_socket_destroy(sender);sender=NULL;
    char tail[8]={0};kernel_socket_copy_read(&request,0,tail,2);
    CHECK(!memcmp(tail,"kl",2));kernel_socket_finish_read(&request,0);
    CHECK(receiver->rx_bytes==4 && receiver->peer_closed);
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0, NULL)==4);
    allow_ofd_release=1;kernel_socket_abort_read(&request);
    CHECK(!request.socket && !allow_ofd_release && receiver->rx_bytes==4 && !receiver->read_request);
    pin=(void *)receiver;
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0, NULL)==4);
    kernel_socket_copy_read(&request,1,tail,3);CHECK(!memcmp(tail,"ate",3));
    kernel_socket_finish_read(&request,0);CHECK(!receiver->rx_bytes);
    kernel_socket_destroy(receiver);
    puts("PASS receive prefix: cross-packet readv uses reserve+finish owners, late append excluded, peer close, competing reader and abort replay");
}

static void receive_packet_chains(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    const char *contents[]={"abcd","efghi","jkl"};
    const unsigned lengths[]={4,5,3};
    for(unsigned i=0;i<3;i++) {
        unsigned first=lengths[i]/2;
        struct pbuf *head=pbuf_alloc(PBUF_RAW,(u16_t)first,PBUF_RAM);
        struct pbuf *tail=pbuf_alloc(PBUF_RAW,(u16_t)(lengths[i]-first),PBUF_RAM);
        CHECK(head && tail && pbuf_take(head,contents[i],(u16_t)first)==ERR_OK &&
            pbuf_take(tail,contents[i]+first,(u16_t)(lengths[i]-first))==ERR_OK);
        pbuf_cat(head,tail);
        /* raw接收边界前TCP已经扣除本packet的接收窗口。 */
        server->tcp->rcv_wnd-=lengths[i];
        CHECK(tcp_data_received(server,server->tcp,head,ERR_OK)==ERR_OK);
    }
    kernel_socket_protocol_leave(&scope);
    struct kernel_open_file_description *pin=(void *)server;
    struct kernel_socket_read_request request={0};
    char a[3]={0},b[9]={0};struct kernel_uaccess_iovec iov[]={{(uintptr_t)a,3},{(uintptr_t)b,9}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    CHECK(kernel_socket_reserve_read(server,NULL,&request,&pin,12,0, NULL)==12);
    user_remaining=7;
    CHECK(kernel_socket_read_user(&request,(void *)server,&cursor)==-KERNEL_EFAULT);
    CHECK(request.completed_bytes==4 && kernel_socket_finish_read(&request,1)==4 && server->rx_bytes==8);
    user_remaining=SIZE_MAX;
    char remaining[8]={0};struct kernel_uaccess_iovec tail_iov={(uintptr_t)remaining,8};
    cursor=(struct kernel_uaccess_iov_cursor){&tail_iov,1,0,0};
    CHECK(kernel_socket_reserve_read(server,NULL,&request,&pin,8,0, NULL)==8);
    CHECK(!kernel_socket_read_user(&request,(void *)server,&cursor));
    CHECK(kernel_socket_finish_read(&request,0)==8 && !memcmp(remaining,"efghijkl",8) && !server->rx_bytes);
    kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    puts("PASS chained packet prefix: readv spans pbuf chains, fault commits preceding whole packet and replays fault packet");
}

static unsigned retired_payloads;
static void retired_payload_free(struct pbuf *payload)
{
    CHECK(payload && protocol_owner == &test_context && protocol_depth && arch_interrupt_is_enabled());
    retired_payloads++;
}
/* Queue more records than the cache can keep. The payload callback is the
 * ownership observation: empty nodes must not retain payload references. */
static void receive_node_reuse_udp(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *socket=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,0,&socket));
    unsigned base=heap_live,returned=retired_payloads;
    struct pbuf_custom custom[24];unsigned char data[24][4];
    ip_addr_t peer;IP_ADDR4(&peer,127,0,0,1);
    struct kernel_socket_protocol_scope scope;
    struct kernel_open_file_description *pin=(void *)socket;
    struct kernel_socket_read_request read={0};
    for(unsigned round=0;round<3;round++) {
        unsigned allocations=heap_allocations;
        kernel_socket_protocol_enter(&scope);
        for(unsigned i=0;i<24;i++) {
            unsigned length=i%4;
            memset(data[i],(int)(i+round),sizeof(data[i]));
            custom[i]=(struct pbuf_custom){.custom_free_function=retired_payload_free};
            struct pbuf *p=pbuf_alloced_custom(PBUF_RAW,length,PBUF_REF,&custom[i],data[i],4);
            CHECK(p);udp_received(socket,socket->udp,p,&peer,(u16_t)(24000+i+round));
            CHECK(p->ref==1);
        }
        kernel_socket_protocol_leave(&scope);
        if(round)CHECK(heap_allocations-allocations==24-SOCKET_RECEIVE_SPARES);
        for(unsigned i=0;i<24;i++) {
            unsigned length=i%4,bytes=length>1?1:length;
            struct kernel_socket_message message={.flags=KERNEL_SOCKET_MSG_PEEK};
            unsigned charge=socket->rx_bytes;
            CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,1,0,&message)==(int)bytes);
            CHECK(kernel_socket_finish_read(&read,0)==bytes && socket->rx_bytes==charge);
            CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,1,0,NULL)==(int)bytes);
            struct kernel_socket_address address;uint32_t actual;
            kernel_socket_read_info(&read,&address,&actual);
            CHECK(actual==length && address.port==24000+i+round && address.family==KERNEL_SOCKET_AF_INET);
            unsigned char out=255;kernel_socket_copy_read(&read,0,&out,bytes);
            CHECK(!bytes || out==(unsigned char)(i+round));
            CHECK(kernel_socket_finish_read(&read,0)==bytes);
        }
        CHECK(!socket->rx_bytes && !socket->packets_head && retired_payloads==returned);
        tcp_more_service();
        CHECK(retired_payloads==returned+24 && !socket->retired_head &&
              heap_live-base==SOCKET_RECEIVE_SPARES);
        returned=retired_payloads;
    }
    /* A warm endpoint accepts under heap failure, but its UDP byte limit still
     * drops a complete record and returns its payload, not a truncated record. */
    kernel_socket_protocol_enter(&scope);
    socket->rx_limit=1;heap_fail_after=1;
    for(unsigned i=0;i<3;i++) {
        custom[i]=(struct pbuf_custom){.custom_free_function=retired_payload_free};
        struct pbuf *p=pbuf_alloced_custom(PBUF_RAW,1,PBUF_REF,&custom[i],data[i],4);CHECK(p);
        udp_received(socket,socket->udp,p,&peer,25000+i);
    }
    CHECK(heap_fail_after==1 && socket->rx_bytes==1 && retired_payloads==returned+2);
    heap_fail_after=0;
    kernel_socket_protocol_leave(&scope);
    CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,1,0,NULL)==1);
    CHECK(kernel_socket_finish_read(&read,0)==1);
    kernel_socket_protocol_enter(&scope);
    custom[3]=(struct pbuf_custom){.custom_free_function=retired_payload_free};
    struct pbuf *p=pbuf_alloced_custom(PBUF_RAW,1,PBUF_REF,&custom[3],data[3],4);CHECK(p);
    udp_received(socket,socket->udp,p,&peer,25003);
    kernel_socket_protocol_leave(&scope);
    /* Final close sees a live queue, retirement and empty nodes together. */
    CHECK(socket->packets_head && socket->retired_head && heap_live>base);
    kernel_socket_destroy(socket);
    CHECK(!heap_live && retired_payloads==returned+4);
    socket=NULL;CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,0,&socket));
    kernel_socket_protocol_enter(&scope);
    custom[0]=(struct pbuf_custom){.custom_free_function=retired_payload_free};
    p=pbuf_alloced_custom(PBUF_RAW,1,PBUF_REF,&custom[0],data[0],4);CHECK(p);
    heap_fail_after=1;udp_received(socket,socket->udp,p,&peer,25004);
    CHECK(!heap_fail_after && !socket->packets_head && retired_payloads==returned+5);
    kernel_socket_protocol_leave(&scope);kernel_socket_destroy(socket);CHECK(!heap_live);
    puts("PASS receive node reuse UDP: bounded burst cache, no pbuf refs, zero/truncated/peek records, source reset, heap failure, byte limit and mixed-owner close");
}

static void receive_node_reuse_tcp(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    tcp_nagle_disable(client->tcp);
    CHECK(write_buffer(client,"abcd",4,0)==4);netif_poll_all();
    struct kernel_open_file_description *pin=(void *)server;
    struct kernel_socket_read_request read={0};
    char data[4]={0};
    CHECK(kernel_socket_reserve_read(server,NULL,&read,&pin,2,0,NULL)==2);
    kernel_socket_copy_read(&read,0,data,2);CHECK(!memcmp(data,"ab",2));
    CHECK(kernel_socket_finish_read(&read,0)==2);
    retire_received(server);
    /* Partial consumption returns credit, but cannot recycle a live node. */
    heap_fail_after=1;CHECK(write_buffer(client,"ef",2,0)==2);netif_poll_all();
    CHECK(!heap_fail_after && server->receive_retry && server->tcp->refused_data && server->rx_bytes==2);
    struct pbuf *refused=server->tcp->refused_data;
    CHECK(refused->ref==1);
    CHECK(kernel_socket_reserve_read(server,NULL,&read,&pin,2,0,NULL)==2);
    kernel_socket_copy_read(&read,0,data,2);CHECK(!memcmp(data,"cd",2));
    CHECK(kernel_socket_finish_read(&read,0)==2);
    /* The scheduled REAP precedes retry and supplies the node without a timer
     * or a successful heap allocation. A zero service budget does no work. */
    kernel_socket_protocol_leave(&scope);
    heap_fail_after=1;
    kernel_socket_service_pending((struct kernel_socket_service_budget){0,0,0});
    CHECK(server->tcp->refused_data==refused && server->rx_bytes==0);
    for(unsigned i=0;i<8 && server->receive_retry;i++)
        kernel_socket_service_pending((struct kernel_socket_service_budget){1,0,0});
    CHECK(heap_fail_after==1 && !server->receive_retry && !server->tcp->refused_data && server->rx_bytes==2);
    heap_fail_after=0;
    CHECK(kernel_socket_reserve_read(server,NULL,&read,&pin,2,0,NULL)==2);
    kernel_socket_copy_read(&read,0,data,2);CHECK(!memcmp(data,"ef",2));
    CHECK(kernel_socket_finish_read(&read,0)==2);
    tcp_more_service();
    /* Repeated actual TCP input consumes no new kernel receive nodes once the
     * retired node is available. Stream offsets were cleared after partial RX. */
    unsigned allocations=heap_allocations;
    for(unsigned i=0;i<16;i++) {
        CHECK(write_buffer(client,"next",4,0)==4);tcp_more_service();
        CHECK(kernel_socket_reserve_read(server,NULL,&read,&pin,4,0,NULL)==4);
        kernel_socket_copy_read(&read,0,data,4);CHECK(!memcmp(data,"next",4));
        CHECK(kernel_socket_finish_read(&read,0)==4);tcp_more_service();
    }
    CHECK(heap_allocations==allocations);
    kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    tcp_more_service();CHECK(!heap_live && !receive_wait.head);
    puts("PASS receive node reuse TCP: partial prefix, refused_data ownership, zero budget, retirement-driven OOM retry, reset offsets and allocation-free steady input");
}

static void endpoint_ownership(void)
{
    kernel_socket_network_initialize();
    /* A consumed custom pbuf still owns its DMA-like payload until the core
     * worker, or final close, actually uses and returns it. */
    for (unsigned final_close = 0; final_close < 2; final_close++) {
        struct kernel_socket *socket = NULL;
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&socket));
        struct pbuf_custom custom = {.custom_free_function=retired_payload_free};
        unsigned char bytes[4] = {'l','o','a','n'}, out[4] = {0};
        struct pbuf *payload = pbuf_alloced_custom(PBUF_RAW,4,PBUF_REF,&custom,bytes,4);
        CHECK(payload);
        ip_addr_t peer;IP_ADDR4(&peer,127,0,0,1);
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        udp_received(socket,socket->udp,payload,&peer,24001);
        kernel_socket_protocol_leave(&scope);
        unsigned locks=acquired, returned=retired_payloads;
        struct kernel_open_file_description *pin=(void *)socket;
        struct kernel_socket_read_request read={0};
        CHECK(kernel_socket_receive_ready(socket) && (kernel_socket_poll(socket,NULL)&KERNEL_POLLIN));
        CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,4,0, NULL)==4);
        kernel_socket_copy_read(&read,0,out,4);
        CHECK(kernel_socket_finish_read(&read,0)==4 && !memcmp(out,bytes,4));
        CHECK(acquired==locks && retired_payloads==returned && socket->retired_head && !socket->packets_head);
        if (!final_close) {
            (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
            CHECK(!socket->retired_head && retired_payloads==returned+1);
        }
        kernel_socket_destroy(socket);
        CHECK(retired_payloads==returned+1);
    }
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    /* The accepted side sends first: no incoming payload or timer may be
     * needed to create its transmit admission. Two writers share one ledger. */
    struct kernel_open_file_description *a=(void *)server,*b=(void *)server;
    struct kernel_socket_write_request first={0},second={0};
    kernel_socket_stream_begin(&first,&a);kernel_socket_stream_begin(&second,&b);
    unsigned locks=acquired;
    uint32_t available;{SOCKET_ENDPOINT(state,server);available=tcp_admission_capacity_locked(server);}
    CHECK(available && kernel_socket_stream_reserve(&first,available,0)==(int)available);
    CHECK(kernel_socket_stream_reserve(&second,1,0)==-KERNEL_EAGAIN);
    CHECK(!(kernel_socket_poll(server,NULL)&KERNEL_POLLOUT));
    kernel_socket_stream_cancel(&first);
    CHECK(kernel_socket_stream_reserve(&second,1,0)==1 && acquired==locks);
    CHECK(kernel_socket_stream_commit(&second,"g",1,0)==1 && acquired==locks+1);
    kernel_socket_stream_finish(&first);kernel_socket_stream_finish(&second);
    CHECK(server->tx_reserved==0 && kernel_socket_receive_ready(client));
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(client);kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    puts("PASS endpoint ownership: no core lock for RX/poll/admission, accepted writer sends first, reservations cannot oversell, payload retired once by worker or close");
}



static void pool_release_owner(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct pbuf *payload=pbuf_alloc(PBUF_RAW,64,PBUF_RAM);CHECK(payload && !pool_wait.head);
    unsigned before=state_acquires;uint64_t generation=pool_generation;
    pbuf_free(payload);
    CHECK(state_acquires==before && pool_generation>generation);
    kernel_socket_protocol_leave(&scope);
    puts("PASS protocol pool release: actual pbuf ownership returned and capacity generation published without endpoint contention");
}

static void tcp_quota_wakeup(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event; CHECK(reason==KERNEL_WAIT_WOKEN);(*(unsigned *)node->context)++; }
static void tcp_quota_notifications(void)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    struct kernel_open_file_description *a=(void *)client,*b=(void *)client;
    struct kernel_socket_write_request first={0},second={0};
    kernel_socket_stream_begin(&first,&a);kernel_socket_stream_begin(&second,&b);
    uint32_t capacity;{SOCKET_ENDPOINT(state,client);capacity=tcp_admission_capacity_locked(client);}
    CHECK(capacity && kernel_socket_stream_reserve(&first,capacity,0)==(int)capacity);
    CHECK(kernel_socket_stream_reserve(&second,1,0)==-KERNEL_EAGAIN);
    /* All protocol capacity is still held by a caller, rather than by TCP.
     * A competing admission cannot make progress by running the worker. */
    CHECK(!work_pending_snapshot());
    unsigned notifications=0;
    struct kernel_wait_node observer={0};
    kernel_wait_node_init_callback(&observer,tcp_quota_wakeup,&notifications);
    kernel_wait_queue_add(&client->wait,&observer);
    kernel_socket_stream_cancel(&first);
    CHECK(notifications==1 && (kernel_socket_poll(client,NULL)&KERNEL_POLLOUT));
    CHECK(kernel_socket_stream_reserve(&second,capacity,0)==(int)capacity);
    notifications=0;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    /* Only one byte is prepared: the unused reservation becomes genuinely
     * admissible again, and must notify competing writers exactly once. */
    CHECK(kernel_socket_stream_commit(&second,"q",1,KERNEL_SOCKET_IO_MORE)==1);
    CHECK(notifications==1);
    notifications=0;
    kernel_socket_stream_finish(&first);kernel_socket_stream_finish(&second);
    CHECK(!notifications);
    kernel_socket_protocol_leave(&scope);
    CHECK(kernel_wait_node_remove_sync(&observer)==KERNEL_SCHEDULER_STATUS_OK);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(server->rx_bytes==1 && !client->tx_reserved && !client->active_writes);
    kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(client);kernel_socket_destroy(server);kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    puts("PASS TCP quota: caller-held admission does not schedule CPU0; returned capacity notifies once, completed request removal does not notify");
}

static void minshall_send(void)
{
    for(unsigned wrap=0;wrap<2;wrap++) {
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    unsigned mss=tcp_mss(client->tcp);
    unsigned char bytes[mss];memset(bytes,'m',sizeof(bytes));
    client->tcp->cwnd=tcp_sndbuf_limit(client->tcp);
    if(wrap) {
        uint32_t sequence=UINT32_MAX-255U;
        CHECK(!client->tcp->unacked && !client->tcp->unsent);
        client->tcp->lastack=client->tcp->snd_nxt=client->tcp->snd_lbb=sequence;
        client->tcp->snd_sml=client->tcp->snd_wl2=sequence;
        server->tcp->rcv_nxt=sequence;
        server->tcp->rcv_ann_right_edge=sequence+server->tcp->rcv_ann_wnd;
        server->tcp->snd_wl1=sequence-1;
    }
    uint32_t start=client->tcp->snd_nxt;
    CHECK(write_buffer(client,bytes,sizeof(bytes),0)==(int)sizeof(bytes));
    CHECK(client->tcp->unacked && !client->tcp->unsent && client->tcp->snd_nxt==start+mss);
    CHECK(write_buffer(client,"x",1,0)==1);
    CHECK(!client->tcp->unsent && client->tcp->snd_nxt==start+mss+1);
    CHECK(write_buffer(client,"y",1,0)==1);
    CHECK(client->tcp->unsent && client->tcp->snd_nxt==start+mss+1);
    CHECK(write_buffer(client,bytes,mss-1,0)==(int)mss-1);
    CHECK(!client->tcp->unsent && client->tcp->snd_nxt==start+2*mss+1);
    CHECK(write_buffer(client,"z",1,0)==1 && client->tcp->unsent);
    netif_poll_all();
    CHECK(!client->tcp->unsent && server->rx_bytes==2*mss+2);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(client);kernel_socket_destroy(server);kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    }
    puts("PASS Minshall: full packet permits first small packet, another small waits for ACK, full packet does not erase small-packet debt, no timer");
}

static void remote_work_ready(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event;
    CHECK(reason==KERNEL_WAIT_WOKEN && node->context==&remote_work && !kernel_cpu_current()->raw_locks);
    CHECK(!pthread_mutex_lock(&remote_gate));remote_work=1;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
}
static err_t held_completion_tcp_output(struct tcp_pcb *pcb, u8_t more)
{
    if (completion_output.socket && pcb==completion_output.socket->tcp) {
        CHECK(caller_owns_protocol() && !kernel_cpu_current()->raw_locks);
        CHECK(!pthread_mutex_lock(&remote_gate));
        completion_output.entered=1;
        CHECK(!pthread_cond_broadcast(&remote_changed));
        while (!completion_output.release)
            CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        completion_output.socket=NULL;
        CHECK(!pthread_mutex_unlock(&remote_gate));
    }
    return boaros_lwip_tcp_output_more(pcb, more);
}
struct completed_output_write {
    struct kernel_socket *socket;
    struct kernel_open_file_description *pin;
};
static void *completed_output_writer(void *argument)
{
    struct completed_output_write *write=argument;
    kernel_cpu_current()->logical_id=1;remote_task=(void *)&test_context;
    struct kernel_socket_write_request request={0};
    kernel_socket_stream_begin(&request,&write->pin);
    CHECK(kernel_socket_stream_reserve(&request,3,0)==3);
    CHECK(kernel_socket_stream_commit(&request,"out",3,0)==3);
    kernel_socket_stream_finish(&request);
    CHECK(write->pin && kernel_open_file_socket(write->pin)==write->socket &&
        !request.pin && !remote_waiter.request);
    remote_task=NULL;
    CHECK(!pthread_mutex_lock(&remote_gate));remote_done=1;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    return NULL;
}
static void *completed_output_service(void *unused)
{
    (void)unused;
    struct kernel_socket_service_result result=kernel_socket_service_pending(
        (struct kernel_socket_service_budget){1,0,0});
    CHECK(result.sockets==1);
    return NULL;
}
static void *completed_output_close(void *argument)
{
    struct completed_output_write *write=argument;
    CHECK(kernel_open_file_release(&write->pin)==KERNEL_OPEN_FILE_STATUS_OK && !write->pin);
    return NULL;
}
static void queued_completion_before_output(void)
{
    remote_testing=1;remote_mode=11;
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    connected_pair(&listener,&client,&server);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    struct completed_output_write write={client,model_socket_file_create(client)};
    struct kernel_wait_node work_node={.callback=remote_work_ready,.context=&remote_work};
    kernel_wait_queue_init(&model_work_queue);model_work_queue.head=model_work_queue.tail=&work_node;
    kernel_socket_network_hooks(NULL,&model_work_queue,NULL,&remote_work);
    remote_socket=client;
    struct kernel_socket_protocol_scope held;kernel_socket_protocol_enter(&held);
    pthread_t writer,service,closer;
    CHECK(!pthread_create(&writer,NULL,completed_output_writer,&write));
    CHECK(!pthread_mutex_lock(&remote_gate));
    while(!remote_work)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    {SOCKET_STATE(state);CHECK(client->writes_head && client->writes_head->pin &&
        kernel_open_file_socket(client->writes_head->pin)==client);}
    CHECK(!pthread_mutex_unlock(&remote_gate));
    kernel_socket_protocol_leave(&held);

    completion_output.socket=client;
    CHECK(!pthread_create(&service,NULL,completed_output_service,NULL));
    CHECK(!pthread_mutex_lock(&remote_gate));
    while(!completion_output.entered)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    /* The real output call is stopped after COPY accepted the bytes. Its
     * caller must already be allowed to finish and retire the request stack. */
    CHECK(!pthread_cond_broadcast(&remote_changed));
    while(!remote_done)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    CHECK(!pthread_mutex_unlock(&remote_gate) && !pthread_join(writer,NULL));
    CHECK(!client->active_writes && !client->writes_head &&
        !client->tx_reserved && !server->rx_bytes);
    CHECK(client->tcp && client->tcp->unsent && write.pin);

    /* Last close is a different task. It reaches the actual protocol mutex
     * while output holds it, so the worker's socket borrow stays valid. */
    unsigned live=heap_live;
    competing=1;
    CHECK(!pthread_create(&closer,NULL,completed_output_close,&write));
    CHECK(!pthread_mutex_lock(&handshake));
    while(!waiter_registered)CHECK(!pthread_cond_wait(&handshake_changed,&handshake));
    CHECK(heap_live==live && protocol_current_owner() && client->tcp);
    CHECK(!pthread_mutex_unlock(&handshake));
    CHECK(!pthread_mutex_lock(&remote_gate));completion_output.release=1;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    CHECK(!pthread_join(service,NULL) && !pthread_join(closer,NULL));competing=0;
    CHECK(!write.pin);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(server->rx_bytes==3);
    char bytes[3];
    CHECK(pbuf_copy_partial(server->packets_head->payload,bytes,3,0)==3 && !memcmp(bytes,"out",3));
    kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_destroy(server);kernel_socket_destroy(listener);
    kernel_socket_network_purge_orphans();
    CHECK(!heap_live && !model_socket_files);
    puts("PASS queued TCP completion: caller and OFD pin return before blocked output; last close waits for protocol owner and delivered COPY remains intact");
}
struct remote_write_args { struct kernel_socket *socket; const char *data; unsigned size; };
static void *remote_writer(void *argument)
{
    const struct remote_write_args *args=argument;
    struct kernel_socket *socket=args->socket;kernel_cpu_current()->logical_id=1;
    struct kernel_open_file_description *pin=(void *)socket;
    remote_task=(void *)&test_context;
    if(!remote_mode || remote_mode>=4) {
        struct kernel_socket_write_request request={0};
        kernel_socket_stream_begin(&request,&pin);
        CHECK(kernel_socket_stream_reserve(&request,args->size,0)==(int)args->size);
        CHECK(kernel_socket_stream_commit(&request,args->data,args->size,
            (remote_mode==5 || remote_mode==7)?KERNEL_SOCKET_IO_MORE:0)==(remote_mode==4?-KERNEL_EIO:(int)args->size));
        if(remote_mode==5 || remote_mode==7) {
            CHECK(!pthread_mutex_lock(&remote_gate));batch_waiting=1;
            CHECK(!pthread_cond_broadcast(&remote_changed));
            while(!batch_release)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
            CHECK(!pthread_mutex_unlock(&remote_gate));
        }
        kernel_socket_stream_finish(&request);
    } else {
        const struct kernel_uaccess_iovec iov={(uintptr_t)"r",1};
        int result=kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,1,0,NULL, NULL);
        CHECK(result==(remote_mode==2?-KERNEL_EFAULT:remote_mode==3?-KERNEL_EIO:1));
    }
    CHECK(pin==(void *)socket && !remote_waiter.request);remote_task=NULL;
    CHECK(!pthread_mutex_lock(&remote_gate));remote_done++;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    return NULL;
}
static void *remote_stopper(void *unused)
{
    (void)unused;kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    CHECK(!pthread_mutex_lock(&remote_gate));stop_done=1;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));return NULL;
}
static void remote_protocol_request(unsigned mode)
{
    remote_testing=1;remote_mode=mode;
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    if(!mode || mode>=4)connected_pair(&listener,&client,&server);
    else {
        struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24017,.bytes={127,0,0,1}};
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&server));
        CHECK(!kernel_socket_bind(server,&address));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&client));
        CHECK(!kernel_socket_connect(client,&address,0));
        if(mode==2)user_remaining=0;
        if(mode==2 || mode==3) boaros_lwip_ip_init(2);
    }
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    struct kernel_wait_node work_node={.callback=remote_work_ready,.context=&remote_work};
    kernel_wait_queue_init(&model_work_queue);model_work_queue.head=model_work_queue.tail=&work_node;
    kernel_socket_network_hooks(NULL,&model_work_queue,NULL,&remote_work);
    remote_socket=client;
    struct kernel_open_file_description *quota_pin=(void *)client;
    struct kernel_socket_write_request quota={0};
    struct kernel_wait_node quota_observer={0};unsigned quota_notifications=0;
    if(mode==8 || mode==9) {
        kernel_socket_stream_begin(&quota,&quota_pin);
        uint32_t capacity;{SOCKET_ENDPOINT(state,client);capacity=tcp_admission_capacity_locked(client);}
        unsigned send_bytes=mode==9?3:1;
        CHECK(capacity>send_bytes && kernel_socket_stream_reserve(&quota,capacity-send_bytes,0)==(int)(capacity-send_bytes));
        kernel_wait_node_init_callback(&quota_observer,tcp_quota_wakeup,&quota_notifications);
        kernel_wait_queue_add(&client->wait,&quota_observer);
    }
    if(mode==9)CHECK(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    unsigned initial_xmit=lwip_stats.tcp.xmit;
    pthread_t thread,second,stopper;unsigned stop_started=0,turns=0;
    const struct remote_write_args first_args={client,"r",1}, second_args={client,"st",2};
    struct kernel_socket_protocol_scope held;
    int force_queue=!mode || (mode>=4 && mode<10);
    if(force_queue)kernel_socket_protocol_enter(&held);
    CHECK(!pthread_create(&thread,NULL,remote_writer,(void *)&first_args));
    if(force_queue || mode==10) {
        CHECK(!pthread_mutex_lock(&remote_gate));
        while(force_queue?!remote_work:!remote_done)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        if(mode==10)CHECK(client->tcp->unsent && !server->rx_bytes && remote_work);
        CHECK(!pthread_mutex_unlock(&remote_gate));
        if(force_queue)kernel_socket_protocol_leave(&held);
    }
    if(mode==10) {
        CHECK(write_buffer(client,"s",1,KERNEL_SOCKET_MSG_MORE)==1);
        CHECK(server->rx_bytes==1 && client->tcp->unsent);
    }
    if(mode==9) {
        CHECK(!pthread_mutex_lock(&remote_gate));
        while(remote_parked!=1)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        CHECK(!pthread_create(&second,NULL,remote_writer,(void *)&second_args));
        while(remote_parked!=2)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        CHECK(!remote_done && !pthread_mutex_unlock(&remote_gate));
        struct kernel_socket_service_result service=kernel_socket_service_pending((struct kernel_socket_service_budget){2,0,0});
        CHECK(service.sockets==2 && lwip_stats.tcp.xmit==initial_xmit+1);
        CHECK(!quota_notifications && !(kernel_socket_poll(client,NULL)&KERNEL_POLLOUT));
        CHECK(kernel_wait_node_remove_sync(&quota_observer)==KERNEL_SCHEDULER_STATUS_OK);
        kernel_socket_stream_finish(&quota);
        CHECK(!pthread_mutex_lock(&remote_gate));
        CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
        CHECK(!pthread_join(second,NULL));
    }
    for(;;) {
        CHECK(!pthread_mutex_lock(&remote_gate));
        while(!remote_work && remote_done<(mode==9?2U:1U) && !(mode==3 && copy_waiting && !stop_started) && !((mode==5 || mode==7) && batch_waiting && !batch_release))
            CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        int done=remote_done==(mode==9?2U:1U);
        int inspect_batch=(mode==5 || mode==7) && batch_waiting && !batch_release;
        int start_stop=!stop_started && ((mode==3 && copy_waiting) || (mode==4 && remote_work));
        remote_work=0;CHECK(!pthread_mutex_unlock(&remote_gate));
        if(done)break;
        if(inspect_batch) {
            struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
            CHECK(client->tcp->unsent && lwip_stats.tcp.xmit==initial_xmit && !server->rx_bytes);
            kernel_socket_protocol_leave(&scope);
            CHECK(!pthread_mutex_lock(&remote_gate));batch_release=1;
            CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
        }
        if(start_stop) {
            stop_started=1;CHECK(!pthread_create(&stopper,NULL,remote_stopper,NULL));
            CHECK(!pthread_mutex_lock(&remote_gate));
            while(!stop_waiting)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
            CHECK(!stop_done && stop_borrows==1);
            {SOCKET_STATE(state);CHECK(client->active_writes && client->active_writes->pin==(void *)client);
             if(mode==3)CHECK(client->active_writes->protocol_payload && !client->active_writes->queued);
             else CHECK(client->active_writes->queued && client->active_writes->segments);}
            copy_release=1;CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
        }
        if(mode==7 && !turns) {
            /* A deferred OUTPUT preceding the queued COPY must not flush its
             * new IO_MORE prefix. Actual output work, not every input, does this. */
            socket_schedule(client,SOCKET_WORK_OUTPUT);
        }
        struct kernel_socket_service_result service=kernel_socket_service_pending(
            mode==8?(struct kernel_socket_service_budget){1,0,0}:
            mode==6?(struct kernel_socket_service_budget){1,8,0}:(struct kernel_socket_service_budget){8,8,1});
        if(mode==6 && !turns) CHECK(service.sockets==1 && service.packets && server->rx_bytes==1);
        turns++;
        CHECK(!pthread_mutex_lock(&remote_gate));
        CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    }
    CHECK(!pthread_join(thread,NULL));
    if(mode==8) {
        CHECK(!quota_notifications && !(kernel_socket_poll(client,NULL)&KERNEL_POLLOUT));
        kernel_socket_stream_cancel(&quota);
        CHECK(quota_notifications==1 && (kernel_socket_poll(client,NULL)&KERNEL_POLLOUT));
        kernel_socket_stream_finish(&quota);
        CHECK(quota_notifications==1);
        CHECK(kernel_wait_node_remove_sync(&quota_observer)==KERNEL_SCHEDULER_STATUS_OK);
    }
    if(stop_started) {CHECK(!pthread_join(stopper,NULL));CHECK(stop_done && !stop_borrows);}
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(server->rx_bytes==(mode==9?3U:(mode>=2 && mode<=4)?0U:1U) &&
          !client->tx_reserved && !client->writes_head && !client->active_writes);
    if(mode==9 || mode==10) {
        char received[3];
        unsigned size=mode==9?sizeof(received):1;
        CHECK(pbuf_copy_partial(server->packets_head->payload,received,size,0)==size &&
              !memcmp(received,"rst",size));
    }
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    kernel_socket_destroy(client);kernel_socket_destroy(server);if(listener)kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    if(mode==9)
        puts("PASS remote TCP batch: 2 waiting writers, 2 service units, 1 NODELAY packet, 3 FIFO bytes, quota and owners returned");
    else if(mode==10)
        puts("PASS remote TCP direct: CPU1 accepts COPY before worker runs; CPU0 outputs, receiver verifies and owners return");
    else
        printf("PASS remote protocol %u: CPU1 copies/waits, CPU0 executes, request/OFD/payload returned\n",mode);
}

static void held_work_notification(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event;
    CHECK(reason==KERNEL_WAIT_WOKEN && node->context==&notify_queue && !kernel_cpu_current()->raw_locks);
    CHECK(!pthread_mutex_lock(&remote_gate));notify_entered=1;
    CHECK(!pthread_cond_broadcast(&remote_changed));
    while(!notify_release)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    CHECK(!pthread_mutex_unlock(&remote_gate));
}
static void *late_work_producer(void *unused)
{(void)unused;kernel_cpu_current()->logical_id=1;protocol_work_ready();return NULL;}
static void notification_stop_lifetime(void)
{
    kernel_socket_network_initialize();kernel_wait_queue_init(&notify_queue);notify_testing=1;
    struct kernel_wait_node node={.callback=held_work_notification,.context=&notify_queue};
    notify_queue.head=notify_queue.tail=&node;
    kernel_socket_network_hooks(NULL,&notify_queue,NULL,NULL);
    pthread_t producer,stopper;CHECK(!pthread_create(&producer,NULL,late_work_producer,NULL));
    CHECK(!pthread_mutex_lock(&remote_gate));
    while(!notify_entered)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    CHECK(!pthread_mutex_unlock(&remote_gate));
    CHECK(!pthread_create(&stopper,NULL,remote_stopper,NULL));
    CHECK(!pthread_mutex_lock(&remote_gate));
    while(!notify_stopping && !stop_done)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
    CHECK(notify_stopping && !stop_done);
    notify_release=1;CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    CHECK(!pthread_join(producer,NULL) && !pthread_join(stopper,NULL));
    CHECK(stop_done && !notify_queue.borrows);notify_testing=0;
    puts("PASS captured worker notification retains its queue until stop drain completes");
}

static void stopped_wait_notification(void)
{
    struct kernel_socket *socket=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM, 0,&socket));
    struct kernel_wait_node marker={0};stop_probe_queue=&socket->wait;
    socket->wait.head=socket->wait.tail=&marker;
    kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    CHECK(stop_probe_notified && !socket->wait.borrows && kernel_socket_receive_ready(socket));
    CHECK(kernel_socket_poll(socket,NULL)&KERNEL_POLLERR);
    socket->wait.head=socket->wait.tail=NULL;stop_probe_queue=NULL;
    kernel_socket_destroy(socket);
    puts("PASS service stop publishes error and wakes an empty receive waiter outside endpoint lock");
}

struct reservation_attempt {struct kernel_socket *socket;struct kernel_socket_read_request read;struct kernel_open_file_description *pin;int result;unsigned actor;};
static void *reserve_concurrently(void *argument)
{
    struct reservation_attempt *a=argument;reservation_actor=a->actor;
    a->pin=(void *)a->socket;
    a->result=kernel_socket_reserve_read(a->socket,(void *)a,&a->read,&a->pin,4,0, NULL);
    return NULL;
}
static void concurrent_reservation(void)
{
    struct kernel_socket socket={.heap=&test_heap,.type=SOCKET_STREAM,.domain=KERNEL_SOCKET_DOMAIN_UNIX,.connected=1,.rx_bytes=4};
    struct socket_packet packet={.length=4};socket.packets_head=socket.packets_tail=&packet;
    kernel_wait_queue_init(&socket.wait);
    struct reservation_attempt attempts[2]={{.socket=&socket,.actor=1},{.socket=&socket,.actor=2}};
    pthread_t threads[2];CHECK(!pthread_create(&threads[0],NULL,reserve_concurrently,&attempts[0]));
    CHECK(!pthread_mutex_lock(&reservation_gate));
    while(!reservation_first)CHECK(!pthread_cond_wait(&reservation_changed,&reservation_gate));
    CHECK(!pthread_mutex_unlock(&reservation_gate));
    CHECK(!pthread_create(&threads[1],NULL,reserve_concurrently,&attempts[1]));
    CHECK(!pthread_mutex_lock(&reservation_gate));
    while(!reservation_second)CHECK(!pthread_cond_wait(&reservation_changed,&reservation_gate));
    reservation_release=1;CHECK(!pthread_cond_broadcast(&reservation_changed));
    CHECK(!pthread_mutex_unlock(&reservation_gate));
    CHECK(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));
    CHECK(attempts[0].result==4 && attempts[1].result==-KERNEL_EAGAIN);
    CHECK(socket.read_request==&attempts[0].read && !attempts[0].pin && attempts[1].pin==(void *)&socket);
    puts("PASS concurrent receive: one reservation, loser retains its OFD pin");
}

static struct kernel_socket *named_unix_socket(unsigned type,
    const unsigned char *bytes,unsigned length)
{
    struct kernel_socket *socket=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,type, 0,&socket));
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_UNIX,
        .unix_path=bytes,.unix_length=length};
    CHECK(!kernel_socket_bind(socket,&address));return socket;
}
static struct kernel_socket_address abstract_address(const unsigned char *bytes,unsigned length)
{ return (struct kernel_socket_address){.family=KERNEL_SOCKET_AF_UNIX,.unix_path=bytes,.unix_length=length}; }
static void expect_unix_name(const struct kernel_socket_address *address,
    const unsigned char *bytes,unsigned length)
{ CHECK(address->family==KERNEL_SOCKET_AF_UNIX && address->unix_length==length &&
        (!length || !memcmp(address->unix_path,bytes,length))); }
static struct kernel_socket_address read_named_packet(struct kernel_socket *socket,const char *expected)
{
    struct kernel_open_file_description *pin=(void *)socket;
    struct kernel_socket_read_request read={0};
    unsigned length=(unsigned)strlen(expected);char bytes[16]={0};
    CHECK(length<=sizeof(bytes));
    CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,sizeof(bytes),1, NULL)==(int)length && !pin);
    struct kernel_socket_address address={0};uint32_t actual=0;
    kernel_socket_read_info(&read,&address,&actual);CHECK(actual==length);
    kernel_socket_copy_read(&read,0,bytes,length);CHECK(!memcmp(bytes,expected,length));
    CHECK(kernel_socket_finish_read(&read,0)==length && pin==(void *)socket);
    return address;
}
static void unix_abstract_names(void)
{
    unsigned char full[108]={0};memset(full+1,'n',107);
    const unsigned char binary[]={0,'n',0,'x'};
    struct kernel_socket *a=named_unix_socket(SOCKET_DGRAM,binary,sizeof(binary));
    struct kernel_socket *b=named_unix_socket(SOCKET_DGRAM,binary,sizeof(binary)-1);
    struct kernel_socket *c=named_unix_socket(SOCKET_DGRAM,full,sizeof(full));
    struct kernel_socket *duplicate=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&duplicate));
    struct kernel_socket_address address=abstract_address(binary,sizeof(binary));
    CHECK(kernel_socket_bind(duplicate,&address)==-KERNEL_EADDRINUSE);
    address.unix_length=109;CHECK(kernel_socket_bind(duplicate,&address)==-KERNEL_EINVAL);
    address.unix_length=0;CHECK(!kernel_socket_bind(duplicate,&address));
    struct kernel_socket_address saved={0};CHECK(!kernel_socket_getname(a,&saved));
    expect_unix_name(&saved,binary,sizeof(binary));
    kernel_socket_destroy(a);expect_unix_name(&saved,binary,sizeof(binary));
    a=named_unix_socket(SOCKET_DGRAM,binary,sizeof(binary));
    CHECK(a->unix_name!=saved.unix_owner);
    kernel_socket_address_release(&saved);kernel_socket_destroy(a);
    kernel_socket_destroy(b);kernel_socket_destroy(c);kernel_socket_destroy(duplicate);
    CHECK(!heap_live);
    puts("PASS UNIX abstract names: binary length, full 108 bytes, collision, autobind, exported name retains its owner after close/rebind");
}
static void unix_named_stream(void)
{
    const unsigned char name[]={0,'s','t','r','e','a','m'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    struct kernel_socket *listener=named_unix_socket(SOCKET_STREAM,name,sizeof(name));
    struct kernel_socket *client=NULL,*accepted=NULL;
    CHECK(!kernel_socket_listen(listener,1));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&client));
    CHECK(!kernel_socket_connect(client,&address,1) && listener->pending_live==1);
    CHECK(kernel_socket_poll(listener,NULL)&KERNEL_POLLIN);
    CHECK(!kernel_socket_accept(listener,&accepted, NULL) && !listener->pending_live);
    CHECK(client->peer==accepted && accepted->peer==client && client->unix_references==2 && accepted->unix_references==2);
    struct kernel_socket_address peer={0};CHECK(!kernel_socket_getpeer(client,&peer));
    expect_unix_name(&peer,name,sizeof(name));kernel_socket_address_release(&peer);
    CHECK(write_buffer(client,"stream",6,0)==6);
    peer=read_named_packet(accepted,"stream");kernel_socket_address_release(&peer);
    kernel_socket_destroy(listener);
    CHECK(write_buffer(client,"live",4,0)==4);
    peer=read_named_packet(accepted,"live");kernel_socket_address_release(&peer);
    kernel_socket_destroy(client);CHECK(!accepted->peer && accepted->peer_closed && accepted->unix_references==1);
    kernel_socket_destroy(accepted);CHECK(!heap_live);

    listener=named_unix_socket(SOCKET_STREAM,name,sizeof(name));CHECK(!kernel_socket_listen(listener,0));
    client=NULL;accepted=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&client));
    CHECK(!kernel_socket_connect(client,&address,1));kernel_socket_destroy(client);
    CHECK(!kernel_socket_accept(listener,&accepted, NULL) && accepted->peer_closed && !accepted->peer);
    CHECK(kernel_socket_receive_ready(accepted));kernel_socket_destroy(accepted);
    client=NULL;CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&client));
    CHECK(!kernel_socket_connect(client,&address,1));kernel_socket_destroy(listener);
    CHECK(!client->peer && client->peer_closed && client->unix_references==1);
    CHECK(write_buffer(client,"x",1,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_EPIPE);
    kernel_socket_destroy(client);CHECK(!heap_live);
    puts("PASS UNIX named stream: accept, transferred child/name ownership, accepted connection survives listener close, queued client/listener close");
}
static void unix_named_datagram(void)
{
    const unsigned char source[]={0,'s','o','u','r','c','e'},first[]={0,'d','s','t','1'},second[]={0,'d','s','t','2'};
    struct kernel_socket_address one=abstract_address(first,sizeof(first)),two=abstract_address(second,sizeof(second));
    struct kernel_socket *sender=named_unix_socket(SOCKET_DGRAM,source,sizeof(source));
    struct kernel_socket *a=named_unix_socket(SOCKET_DGRAM,first,sizeof(first));
    struct kernel_socket *b=named_unix_socket(SOCKET_DGRAM,second,sizeof(second));
    CHECK(!kernel_socket_connect(sender,&one,1));
    CHECK(sender->peer==a && !a->peer && sender->unix_references==1 && a->unix_references==2);
    CHECK(a->wait.registrations==1 && sender->unix_space.queue==&a->wait);
    CHECK(write_buffer(sender,"first",5,0)==5);
    CHECK(!kernel_socket_connect(sender,&two,1));
    CHECK(sender->peer==b && !a->wait.registrations && b->wait.registrations==1 && a->unix_references==1);
    struct kernel_socket_address saved=read_named_packet(a,"first");
    expect_unix_name(&saved,source,sizeof(source));kernel_socket_address_release(&saved);
    CHECK(!sender->unix_packets && !sender->unix_memory);
    CHECK(write_buffer(sender,"billed",6,0)==6 && sender->unix_packets==1);
    struct kernel_socket_name *original=sender->unix_name;
    kernel_socket_destroy(sender);
    CHECK(sender->destroying==2 && sender->unix_references==1 && sender->unix_packets==1 && sender->unix_memory);
    CHECK(!b->wait.registrations && b->unix_references==1);
    sender=named_unix_socket(SOCKET_DGRAM,source,sizeof(source));
    CHECK(sender->unix_name!=original);
    saved=read_named_packet(b,"billed");CHECK(saved.unix_owner==original);
    expect_unix_name(&saved,source,sizeof(source));kernel_socket_address_release(&saved);
    CHECK(!kernel_socket_connect(sender,&two,1));
    /* Fill the receiver using actual packets, then drain one to dispatch the
     * connected writer callback outside the endpoint lock. */
    for(unsigned i=0;i<11;i++)CHECK(write_buffer(sender,"q",1,KERNEL_SOCKET_MSG_DONTWAIT)==1);
    CHECK(write_buffer(sender,"q",1,KERNEL_SOCKET_MSG_DONTWAIT)==-KERNEL_EAGAIN);
    CHECK(!(kernel_socket_poll(sender,NULL)&KERNEL_POLLOUT) && sender->unix_space_wait);
    saved=read_named_packet(b,"q");kernel_socket_address_release(&saved);
    CHECK(!sender->unix_space_wait && (kernel_socket_poll(sender,NULL)&KERNEL_POLLOUT));
    for(unsigned i=0;i<10;i++) {saved=read_named_packet(b,"q");kernel_socket_address_release(&saved);}
    CHECK(!sender->unix_memory && !sender->unix_packets && !sender->tx_reserved);
    struct kernel_socket_address disconnect={0};CHECK(!kernel_socket_connect(sender,&disconnect,1));
    CHECK(!sender->peer && b->unix_references==1 && !b->wait.registrations);
    struct kernel_socket_address self=abstract_address(source,sizeof(source));
    CHECK(!kernel_socket_connect(sender,&self,1) && sender->peer==sender && sender->unix_references==2);
    CHECK(write_buffer(sender,"self",4,0)==4);saved=read_named_packet(sender,"self");
    expect_unix_name(&saved,source,sizeof(source));kernel_socket_address_release(&saved);
    CHECK(!kernel_socket_connect(sender,&two,1) && sender->unix_references==1 && b->unix_references==2);
    char bytes[]="held";struct kernel_uaccess_iovec iov={(uintptr_t)bytes,4};
    struct kernel_open_file_description *pin=(void *)sender;close_during_copy=b;
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,4,KERNEL_SOCKET_MSG_DONTWAIT,NULL, NULL)==-KERNEL_ECONNREFUSED);
    CHECK(!close_during_copy && !sender->peer && sender->peer_closed && sender->unix_references==1 &&
        !sender->tx_reserved && !sender->unix_memory && !sender->unix_packets && !sender->unix_space.queue);
    b=named_unix_socket(SOCKET_DGRAM,second,sizeof(second));
    close_during_copy=b;pin=(void *)sender;
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,4,KERNEL_SOCKET_MSG_DONTWAIT,&two, NULL)==-KERNEL_ECONNREFUSED);
    CHECK(!close_during_copy && pin==(void *)sender && !sender->tx_reserved && !sender->unix_memory && !sender->unix_packets);
    kernel_socket_destroy(a);kernel_socket_destroy(sender);CHECK(!heap_live);
    puts("PASS UNIX named datagram: one-way and self connections, reconnect, queue-space callback, sender-close packet billing/name retention, connected/sendto destination close during user copy");
}
static void unix_stream_peer_after_close(unsigned queued)
{
    kernel_socket_network_initialize();
    const unsigned char name[]={0,'p','e','e','r','s'},client_name[]={0,'c','l','i','e','n','t'};
    struct kernel_socket_address destination=abstract_address(name,sizeof(name)),peer={0};
    struct kernel_socket *listener=named_unix_socket(SOCKET_STREAM,name,sizeof(name));
    struct kernel_socket *client=named_unix_socket(SOCKET_STREAM,client_name,sizeof(client_name)),*child=NULL;
    CHECK(!kernel_socket_listen(listener,1));CHECK(!kernel_socket_connect(client,&destination,1));
    if(!queued)CHECK(!kernel_socket_accept(listener,&child, NULL));
    unix_close_name_probe.target=client;unix_close_name_probe.peer=client->peer;
    unix_close_name_probe.bytes=client_name;unix_close_name_probe.length=sizeof(client_name);
    unix_close_name_probe.observed=0;
    kernel_socket_destroy(client);
    CHECK(unix_close_name_probe.observed==1 && !unix_close_name_probe.target);
    unix_close_name_probe.peer=NULL;unix_close_name_probe.bytes=NULL;
    if(queued)CHECK(!kernel_socket_accept(listener,&child, NULL));
    CHECK(!kernel_socket_getpeer(child,&peer));expect_unix_name(&peer,client_name,sizeof(client_name));
    kernel_socket_address_release(&peer);kernel_socket_destroy(child);kernel_socket_destroy(listener);CHECK(!heap_live);
    printf("PASS UNIX %s stream preserves bound peer name during and after peer close\n",queued?"queued":"accepted");
}
static void unix_autobind_collision(void)
{
    kernel_socket_network_initialize();
    const unsigned char collision[]={0,'0','0','0','0','1'};
    struct kernel_socket *reserved=named_unix_socket(SOCKET_DGRAM,collision,sizeof(collision));
    struct kernel_socket *automatic=NULL;CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&automatic));
    struct kernel_socket_address bind={.family=KERNEL_SOCKET_AF_UNIX},actual={0};
    CHECK(!kernel_socket_bind(automatic,&bind));CHECK(!kernel_socket_getname(automatic,&actual));
    CHECK(actual.unix_length==6 && actual.unix_path[0]==0 && memcmp(actual.unix_path,collision,sizeof(collision)));
    kernel_socket_address_release(&actual);kernel_socket_destroy(automatic);kernel_socket_destroy(reserved);CHECK(!heap_live);
    puts("PASS UNIX autobind skips explicitly reserved generated name");
}
static void unix_listener_lookup_close(void)
{
    kernel_socket_network_initialize();
    const unsigned char name[]={0,'l','o','o','k','u','p'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    struct kernel_socket *listener=named_unix_socket(SOCKET_STREAM,name,sizeof(name)),*client=NULL;
    CHECK(!kernel_socket_listen(listener,1));CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&client));
    lookup_close_target=listener;
    CHECK(kernel_socket_connect(client,&address,1)==-KERNEL_ECONNREFUSED);
    CHECK(lookup_close_done==1 && !lookup_close_target && !client->peer && !client->connected && client->unix_references==1);
    kernel_socket_destroy(client);CHECK(!heap_live);
    puts("PASS UNIX stream listener close after lookup reference prevents connection and releases child, queue entry and target");
}
static void unix_shutdown_states(void)
{
    kernel_socket_network_initialize();
    const int types[]={SOCKET_STREAM,SOCKET_DGRAM,SOCKET_SEQPACKET};
    const unsigned char name[]={0,'s','h','u','t','-','s','t','a','t','e'};
    const uint32_t mask=KERNEL_POLLIN|KERNEL_POLLRDNORM|KERNEL_POLLRDHUP|
        KERNEL_POLLHUP|KERNEL_POLLOUT|KERNEL_POLLERR;
    for(unsigned t=0;t<3;t++)for(unsigned listener=0;listener<2;listener++) {
        if(listener && types[t]==SOCKET_DGRAM)continue;
        for(int how=-1;how<=3;how++) {
            struct kernel_socket *socket=NULL;
            CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,types[t], 0,&socket));
            if(listener) {
                struct kernel_socket_address address=abstract_address(name,sizeof(name));
                CHECK(!kernel_socket_bind(socket,&address) && !kernel_socket_listen(socket,1));
            }
            uint32_t initial=listener?0:KERNEL_POLLOUT;
            if(!listener && types[t]!=SOCKET_DGRAM)initial|=KERNEL_POLLHUP;
            CHECK((kernel_socket_poll(socket,NULL)&mask)==initial);
            int valid=how>=0 && how<=2;
            CHECK(kernel_socket_shutdown(socket,how)==(valid?0:-KERNEL_EINVAL));
            unsigned read_closed=valid && how!=1,write_closed=valid && how!=0;
            CHECK(socket->read_closed==read_closed && socket->write_closed==write_closed);
            CHECK(!socket->connected && socket->listening==listener && !socket->peer && !socket->pending_error);
            uint32_t expected=initial;
            if(read_closed)expected|=KERNEL_POLLIN|KERNEL_POLLRDNORM|KERNEL_POLLRDHUP;
            if(read_closed && write_closed)expected|=KERNEL_POLLHUP;
            CHECK((kernel_socket_poll(socket,NULL)&mask)==expected);
            /* Valid shutdown is idempotent; invalid how cannot alter state. */
            CHECK(kernel_socket_shutdown(socket,how)==(valid?0:-KERNEL_EINVAL));
            CHECK(socket->read_closed==read_closed && socket->write_closed==write_closed);
            CHECK((kernel_socket_poll(socket,NULL)&mask)==expected);
            kernel_socket_destroy(socket);CHECK(!heap_live);
        }
    }
    puts("PASS UNIX shutdown states: STREAM/DGRAM/SEQ new and STREAM/SEQ listeners, 15 valid and 10 invalid how cases, idempotence, state and poll, complete storage return");
}
static void unix_datagram_read_shutdown(void)
{
    kernel_socket_network_initialize();
    const unsigned char name[]={0,'s','h','u','t'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    struct kernel_socket *target=NULL,*sender=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&target));
    CHECK(!kernel_socket_shutdown(target,0) && target->read_closed);
    CHECK(!kernel_socket_bind(target,&address));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&sender));
    CHECK(!kernel_socket_connect(sender,&address,1));
    CHECK(write_buffer(sender,"x",1,KERNEL_SOCKET_MSG_DONTWAIT)==-KERNEL_EPIPE);
    struct kernel_open_file_description *pin=(void *)sender;char bytes[]="x";
    struct kernel_uaccess_iovec iov={(uintptr_t)bytes,1};
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,1,KERNEL_SOCKET_MSG_DONTWAIT,&address, NULL)==-KERNEL_EPIPE);
    CHECK(!target->rx_bytes && !target->unix_reserved && !sender->unix_packets && !sender->tx_reserved);
    kernel_socket_destroy(target);kernel_socket_destroy(sender);CHECK(!heap_live);
    target=named_unix_socket(SOCKET_DGRAM,name,sizeof(name));sender=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&sender));
    CHECK(!kernel_socket_connect(sender,&address,1));pin=(void *)sender;read_shutdown_during_copy=target;
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,1,KERNEL_SOCKET_MSG_DONTWAIT,NULL, NULL)==-KERNEL_EPIPE);
    CHECK(!read_shutdown_during_copy && pin==(void *)sender && !sender->tx_reserved && !sender->unix_packets &&
        !target->unix_reserved && !target->rx_bytes);
    kernel_socket_destroy(target);kernel_socket_destroy(sender);CHECK(!heap_live);
    puts("PASS UNIX DGRAM SHUT_RD: bind and connect succeed; send/sendto and shutdown during copy return EPIPE with reservations returned");
}
static void unix_datagram_reconnect_receive(void)
{
    kernel_socket_network_initialize();
    const unsigned char an[]={0,'r','x'},bn[]={0,'o','l','d'},cn[]={0,'n','e','w'};
    struct kernel_socket_address aa=abstract_address(an,sizeof(an)),ba=abstract_address(bn,sizeof(bn)),ca=abstract_address(cn,sizeof(cn));
    struct kernel_socket *a=named_unix_socket(SOCKET_DGRAM,an,sizeof(an)),*b=named_unix_socket(SOCKET_DGRAM,bn,sizeof(bn)),
        *c=named_unix_socket(SOCKET_DGRAM,cn,sizeof(cn));
    CHECK(!kernel_socket_connect(a,&ba,1) && !kernel_socket_connect(b,&aa,1));
    CHECK(write_buffer(b,"old",3,0)==3 && write_buffer(b,"tail",4,0)==4);
    CHECK(a->unix_queued==2 && b->unix_packets==2);
    CHECK(!kernel_socket_connect(a,&ca,1));
    CHECK(!a->packets_head && !a->rx_bytes && !a->unix_queued && !b->unix_packets && !b->unix_memory);
    CHECK(kernel_socket_unix_wait_error(b)==-KERNEL_ECONNRESET);
    CHECK(!kernel_socket_connect(a,&ba,1));
    CHECK(write_buffer(b,"head",4,0)==4 && write_buffer(b,"tail",4,0)==4);
    struct kernel_open_file_description *pin=(void *)a;struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(a,NULL,&read,&pin,2,1, NULL)==2 && !pin);
    CHECK(!kernel_socket_connect(a,&ca,1));
    CHECK(a->packets_head==read.packet && a->packets_tail==read.packet && !a->packets_head->next &&
        a->unix_queued==1 && a->rx_bytes==4 && b->unix_packets==1 && b->unix_memory==packet_charge(4));
    char bytes[2];kernel_socket_copy_read(&read,0,bytes,2);CHECK(!memcmp(bytes,"he",2));
    CHECK(kernel_socket_finish_read(&read,0)==2 && pin==(void *)a);
    CHECK(!a->rx_bytes && !a->unix_queued && !b->unix_packets && !b->unix_memory);
    CHECK(kernel_socket_unix_wait_error(b)==-KERNEL_ECONNRESET);
    CHECK(write_buffer(b,"x",1,KERNEL_SOCKET_MSG_DONTWAIT)==-KERNEL_EPERM);
    kernel_socket_destroy(a);kernel_socket_destroy(b);kernel_socket_destroy(c);CHECK(!heap_live);
    puts("PASS UNIX DGRAM reconnect: discard unreserved messages, preserve active head/prefix, return old sender packet billing once");
}
static void unix_named_endpoints(void)
{
    kernel_socket_network_initialize();
    unix_abstract_names();unix_named_stream();unix_named_datagram();
}

static void unix_stream_receive_state(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *socket=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&socket));
    struct kernel_open_file_description *pin=(void *)socket;
    struct kernel_socket_read_request request={0};
    for(int nonblocking=0;nonblocking<2;nonblocking++) {
        CHECK(kernel_socket_receive_ready(socket));
        CHECK(kernel_socket_reserve_read(socket,NULL,&request,&pin,1,nonblocking, NULL)==-KERNEL_EINVAL);
        CHECK(!request.socket && pin==(void *)socket);
    }
    const unsigned char name[]={0,'r','e','c','v','-','s','t','a','t','e'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    CHECK(!kernel_socket_bind(socket,&address) && !kernel_socket_listen(socket,1));
    CHECK(kernel_socket_reserve_read(socket,NULL,&request,&pin,1,1, NULL)==-KERNEL_EINVAL);
    kernel_socket_destroy(socket);
    struct kernel_socket *a=NULL,*b=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&a,&b));
    kernel_socket_destroy(b);pin=(void *)a;
    CHECK(kernel_socket_receive_ready(a));
    CHECK(!kernel_socket_reserve_read(a,NULL,&request,&pin,1,0, NULL));
    kernel_socket_destroy(a);CHECK(!heap_live);
    puts("PASS UNIX stream receive state: unconnected/listener EINVAL before waiting, established peer close remains EOF");
}

static void count_space_wakeup(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event; CHECK(reason==KERNEL_WAIT_WOKEN);(*(unsigned *)node->context)++; }

static void unix_space_topology(void)
{
    kernel_socket_network_initialize();
    const unsigned char an[]={0,'n','o','t','i','f','y','-','a'},bn[]={0,'n','o','t','i','f','y','-','b'};
    struct kernel_socket_address aa=abstract_address(an,sizeof(an)),ba=abstract_address(bn,sizeof(bn)),disconnect={0};
    struct kernel_socket *a=named_unix_socket(SOCKET_DGRAM,an,sizeof(an)),*b=named_unix_socket(SOCKET_DGRAM,bn,sizeof(bn)),*c=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_DGRAM, 0,&c));
    CHECK(!kernel_socket_connect(a,&ba,1) && !kernel_socket_connect(b,&aa,1));
    unsigned before=space_callbacks;
    CHECK(write_buffer(a,"pair",4,0)==4);
    struct kernel_socket_address source=read_named_packet(b,"pair");kernel_socket_address_release(&source);
    CHECK(space_callbacks==before);
    CHECK(!kernel_socket_connect(b,&disconnect,1));
    char byte='q';struct kernel_uaccess_iovec iov={(uintptr_t)&byte,1};
    for(unsigned i=0;i<11;i++) {
        struct kernel_open_file_description *pin=(void *)c;
        CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,1,KERNEL_SOCKET_MSG_DONTWAIT,&ba, NULL)==1);
    }
    CHECK(!(kernel_socket_poll(a,NULL)&KERNEL_POLLOUT));
    unsigned notifications=0;struct kernel_wait_node watcher={0};
    kernel_wait_node_init_callback(&watcher,count_space_wakeup,&notifications);kernel_wait_queue_add(&a->wait,&watcher);
    source=read_named_packet(b,"q");kernel_socket_address_release(&source);
    CHECK(notifications && (kernel_socket_poll(a,NULL)&KERNEL_POLLOUT));
    struct kernel_open_file_description *pin=(void *)c;
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,1,KERNEL_SOCKET_MSG_DONTWAIT,&ba, NULL)==1);
    CHECK(!(kernel_socket_poll(a,NULL)&KERNEL_POLLOUT));
    before=notifications;
    CHECK(!kernel_socket_connect(b,&aa,1));
    CHECK(notifications>before && (kernel_socket_poll(a,NULL)&KERNEL_POLLOUT));
    CHECK(kernel_wait_node_remove_sync(&watcher)==KERNEL_SCHEDULER_STATUS_OK);
    before=space_callbacks;
    for(unsigned i=0;i<11;i++) {source=read_named_packet(b,"q");kernel_socket_address_release(&source);}
    CHECK(space_callbacks==before);
    kernel_socket_destroy(a);kernel_socket_destroy(b);kernel_socket_destroy(c);CHECK(!heap_live);
    puts("PASS UNIX space notification: reciprocal traffic skips callbacks; disconnect restores queue-space wake, reciprocal reconnect wakes existing writer");
}

static void unix_write_completion_notifications(void)
{
    kernel_socket_network_initialize();
    const int types[]={SOCKET_STREAM,SOCKET_DGRAM,SOCKET_SEQPACKET};
    for(unsigned t=0;t<sizeof(types)/sizeof(types[0]);t++) {
        int type=types[t];
        struct kernel_socket *sender=NULL,*receiver=NULL;
        CHECK(!kernel_socket_pair(&test_heap,type,&sender,&receiver));
        unsigned notifications=0;
        struct kernel_wait_node observer={0};
        kernel_wait_node_init_callback(&observer,count_space_wakeup,&notifications);
        kernel_wait_queue_add(&sender->wait,&observer);
        CHECK(write_buffer(sender,"credit",6,0)==6);
        CHECK(!notifications);
        struct kernel_socket_address source=read_named_packet(receiver,"credit");
        kernel_socket_address_release(&source);
        CHECK(notifications && !sender->unix_memory && !sender->unix_packets);
        CHECK(!kernel_socket_set_option(sender,KERNEL_SOCKET_SNDBUF,4096));
        char bytes[3000]={0};const unsigned sizes[]={sizeof(bytes),5};
        for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
            CHECK(kernel_socket_poll(sender,NULL)&KERNEL_POLLOUT);
            unsigned before=notifications;
            const struct kernel_uaccess_iovec iov={(uintptr_t)bytes,sizes[i]};
            struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};
            struct kernel_open_file_description *pin=(void *)sender;
            unix_credit_copy_probe=i?NULL:sender;unix_credit_copy_observed=0;user_remaining=0;
            int fault=0;
            int result=type==SOCKET_DGRAM
                ?kernel_socket_write_datagram(&pin,NULL,&iov,1,sizes[i],0,NULL,NULL)
                :kernel_socket_write_unix(&pin,NULL,&cursor,sizes[i],0,&fault,NULL);
            unix_credit_copy_probe=NULL;user_remaining=SIZE_MAX;
            CHECK(result==-KERNEL_EFAULT && pin==(void *)sender);
            CHECK(i || unix_credit_copy_observed);
            CHECK(notifications>before && (kernel_socket_poll(sender,NULL)&KERNEL_POLLOUT));
            CHECK(!sender->tx_reserved && !sender->unix_memory && !receiver->rx_bytes && !receiver->unix_reserved);
        }
        CHECK(kernel_wait_node_remove_sync(&observer)==KERNEL_SCHEDULER_STATUS_OK);
        kernel_socket_destroy(sender);kernel_socket_destroy(receiver);CHECK(!heap_live);
    }
    puts("PASS UNIX write completion: no unchanged-state notification, packet credit and failed-copy cancellation still wake sender");
}

/* These OFD boundary objects have real reference counts and final socket puts;
 * the graph walk, queue ownership, PID snapshots and packet disposal are production. */
static void model_send_right(struct kernel_open_file_description *sender,
    struct kernel_open_file_description *right)
{
    struct kernel_socket_message message={0};
    CHECK(!kernel_socket_control_allocate(&test_heap,1,&message.control));
    CHECK(kernel_open_file_acquire(right)==KERNEL_OPEN_FILE_STATUS_OK);
    message.control->files[0]=right;message.control->count=1;
    char byte='G';struct kernel_uaccess_iovec iov={(uintptr_t)&byte,1};
    struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};
    struct kernel_open_file_description *pin=sender;
    CHECK(kernel_socket_write_unix(&pin,NULL,&cursor,1,KERNEL_SOCKET_MSG_DONTWAIT,NULL,&message)==1 && pin==sender && !message.control);
    kernel_socket_control_release(&message.credentials);
}
static void model_owned_pair(struct kernel_open_file_description *files[2],struct kernel_socket *sockets[2])
{
    sockets[0]=sockets[1]=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_DGRAM,&sockets[0],&sockets[1]));
    for(unsigned side=0;side<2;side++)files[side]=model_socket_file_create(sockets[side]);
    CHECK(!kernel_socket_set_option(sockets[1],KERNEL_SOCKET_PASSCRED,1));
}
static void model_put(struct kernel_open_file_description **file)
{ CHECK(kernel_open_file_release(file)==KERNEL_OPEN_FILE_STATUS_OK && !*file); }
static void model_no_control_owners(void)
{
    CHECK(!heap_live && !model_socket_files && !unix_sockets && !rights_retired && !rights_releasing && !rights_unix_inflight && !rights_queued &&
        !model_uid_account.unix_inflight && model_uid_account.references==1 && model_pid.references==1);
}
struct root_cycle_gate {
    pthread_mutex_t lock;pthread_cond_t changed;unsigned phase;
    struct kernel_open_file_description *owner;
};
static void *root_cycle_releaser(void *argument)
{
    struct root_cycle_gate *gate=argument;CHECK(!pthread_mutex_lock(&gate->lock));
    gate->phase=1;CHECK(!pthread_cond_broadcast(&gate->changed));
    while(gate->phase!=2)CHECK(!pthread_cond_wait(&gate->changed,&gate->lock));
    CHECK(!pthread_mutex_unlock(&gate->lock));model_put(&gate->owner);
    CHECK(!pthread_mutex_lock(&gate->lock));gate->phase=3;
    CHECK(!pthread_cond_broadcast(&gate->changed) && !pthread_mutex_unlock(&gate->lock));return NULL;
}
static void *control_owner_releaser(void *argument)
{
    kernel_socket_control_release(argument);return NULL;
}
static void unix_regular_right_owners(void)
{
    struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];
    model_owned_pair(file,socket);
    /* A NULL socket is an ordinary OFD boundary object, with the same actual
     * reference and cleanup ownership as the socket-backed model objects. */
    struct kernel_open_file_description *regular=model_socket_file_create(NULL);
    struct model_socket_file *held=model_socket_file(regular);
    ordinary_control_no_graph=1;
    model_send_right(file[0],regular);model_put(&regular);
    struct kernel_socket_control *control=socket[1]->packets_head->control;
    unsigned retained=heap_live;
    CHECK(control && !socket[1]->rights_head && !control->receiver && rights_queued==1 &&
        !rights_unix_inflight && !rights_active && !rights_dirty && held->references==1 &&
        model_uid_account.unix_inflight==1 && model_uid_account.references==2 && model_pid.references==2);
    regular_rights_no_graph=1;
    kernel_socket_rights_service();
    CHECK(kernel_socket_rights_drain()==-KERNEL_EBUSY);
    regular_rights_no_graph=0;
    CHECK(heap_live==retained && held->references==1 && socket[1]->packets_head->control==control &&
        socket[1]->packets_head->data[0]=='G' && !rights_unix_inflight && !rights_dirty);
    ordinary_control_no_graph=0;
    model_put(&file[0]);model_put(&file[1]);
    CHECK(!kernel_socket_rights_drain());model_no_control_owners();

    model_owned_pair(file,socket);regular=model_socket_file_create(NULL);
    held=model_socket_file(regular);ordinary_control_no_graph=1;
    model_send_right(file[0],regular);model_put(&regular);
    control_release_probe.target=held;control_release_probe.fail=1;control_release_probe.attempts=0;
    struct kernel_open_file_description *pin=file[1];struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(socket[1],NULL,&read,&pin,1,0,NULL)==1);
    char byte=0;kernel_socket_copy_read(&read,0,&byte,1);CHECK(byte=='G');
    CHECK(kernel_socket_finish_read(&read,0)==1 && pin==file[1]);
    CHECK(rights_retired && !rights_queued && !rights_unix_inflight && rights_active &&
        held->references==1 && model_uid_account.references==2 && model_pid.references==2 &&
        !model_uid_account.unix_inflight && control_release_probe.attempts==1);
    retained=heap_live;regular_rights_no_graph=1;
    CHECK(kernel_socket_rights_drain()==-KERNEL_EBUSY);
    CHECK(heap_live==retained && held->references==1 && control_release_probe.attempts==2);
    control_release_probe.fail=0;
    CHECK(!kernel_socket_rights_drain() && control_release_probe.attempts==3);
    regular_rights_no_graph=0;control_release_probe.target=NULL;
    CHECK(!rights_retired && !rights_releasing && !rights_active &&
        model_uid_account.references==1 && model_pid.references==1);
    ordinary_control_no_graph=0;
    model_put(&file[0]);model_put(&file[1]);model_no_control_owners();
    model_owned_pair(file,socket);regular=model_socket_file_create(NULL);
    held=model_socket_file(regular);model_send_right(file[0],regular);model_put(&regular);
    struct kernel_socket_message received={0};pin=file[1];read=(struct kernel_socket_read_request){0};
    CHECK(kernel_socket_reserve_read(socket[1],NULL,&read,&pin,1,0,&received)==1);
    CHECK(kernel_socket_finish_read(&read,0)==1 && received.control && !rights_queued);
    model_put(&file[0]);model_put(&file[1]);
    control_release_probe.target=held;control_release_probe.attempts=0;
    control_release_probe.pause=1;control_release_probe.entered=control_release_probe.resume=0;
    ordinary_control_no_graph=1;
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,control_owner_releaser,&received.control));
    CHECK(!pthread_mutex_lock(&control_release_probe.lock));
    while(!control_release_probe.entered)
        CHECK(!pthread_cond_wait(&control_release_probe.changed,&control_release_probe.lock));
    CHECK(!received.control && !rights_queued && !rights_retired && rights_releasing &&
        control_release_probe.attempts==1 && held->references==1);
    CHECK(kernel_socket_rights_drain()==-KERNEL_EBUSY);
    control_release_probe.resume=1;CHECK(!pthread_cond_broadcast(&control_release_probe.changed));
    CHECK(!pthread_mutex_unlock(&control_release_probe.lock) && !pthread_join(thread,NULL));
    control_release_probe.target=NULL;control_release_probe.pause=0;ordinary_control_no_graph=0;
    CHECK(!kernel_socket_rights_drain());model_no_control_owners();
    puts("PASS UNIX ordinary FD rights: publish/reserve/consume/release/retry never acquire graph mutex; UID and queued owners keep drain busy; close and failed final put return all owners");
}
static void unix_right_quota(void)
{
    struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];model_owned_pair(file,socket);
    struct kernel_open_file_description *regular=model_socket_file_create(NULL);
    model_nofile_limit=1;model_credentials.effective=0;ordinary_control_no_graph=1;
    model_send_right(file[0],regular);model_send_right(file[0],regular);
    CHECK(model_uid_account.unix_inflight==2 && rights_queued==2);
    struct kernel_socket_message rejected={0};
    CHECK(!kernel_socket_control_allocate(&test_heap,1,&rejected.control));
    CHECK(kernel_open_file_acquire(regular)==KERNEL_OPEN_FILE_STATUS_OK);
    rejected.control->files[0]=regular;rejected.control->count=1;
    char byte='Q';struct kernel_uaccess_iovec iov={(uintptr_t)&byte,1};
    struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};struct kernel_open_file_description *pin=file[0];
    CHECK(kernel_socket_write_unix(&pin,NULL,&cursor,1,KERNEL_SOCKET_MSG_DONTWAIT,NULL,&rejected)==-KERNEL_ETOOMANYREFS);
    CHECK(rejected.control && !rejected.control->inflight && rights_queued==2 &&
        model_uid_account.unix_inflight==2 && !socket[0]->tx_reserved && !socket[0]->active_writes);
    kernel_socket_control_release(&rejected.control);kernel_socket_control_release(&rejected.credentials);
    CHECK(kernel_open_file_references(regular)==3);
    ordinary_control_no_graph=0;model_nofile_limit=KERNEL_RLIMIT_NOFILE_CAP;model_credentials.effective=KERNEL_CAP_VALID_MASK;
    model_put(&regular);model_put(&file[0]);model_put(&file[1]);model_no_control_owners();
    puts("PASS UNIX ordinary rights quota: count equal to limit accepts one more descriptor; strictly greater returns ETOOMANYREFS, retains rejected control and releases reservation without changing queued charge");
}
static void unix_mixed_right_owners(void)
{
    for(unsigned cycle=0;cycle<2;cycle++) {
        struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];
        model_owned_pair(file,socket);
        struct kernel_open_file_description *regular=model_socket_file_create(NULL);
        struct kernel_socket_message sent={0};
        CHECK(!kernel_socket_control_allocate(&test_heap,2,&sent.control));
        CHECK(kernel_open_file_acquire(regular)==KERNEL_OPEN_FILE_STATUS_OK);
        CHECK(kernel_open_file_acquire(file[1])==KERNEL_OPEN_FILE_STATUS_OK);
        sent.control->files[0]=regular;sent.control->files[1]=file[1];sent.control->count=2;
        char byte='M';struct kernel_uaccess_iovec iov={(uintptr_t)&byte,1};
        struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};
        struct kernel_open_file_description *pin=file[0];
        unsigned before=graph_acquisitions;
        CHECK(kernel_socket_write_unix(&pin,NULL,&cursor,1,KERNEL_SOCKET_MSG_DONTWAIT,NULL,&sent)==1);
        CHECK(graph_acquisitions>before && !sent.control);
        kernel_socket_control_release(&sent.credentials);
        CHECK(rights_queued==1 && rights_unix_inflight==1 && model_uid_account.unix_inflight==2 &&
            socket[1]->rights_head && socket[1]->rights_head->unix_count==1);
        if(cycle) {
            /* A second ordinary packet is retained by the same unreachable
             * receiver, although only the mixed control is on the graph. */
            ordinary_control_no_graph=1;model_send_right(file[0],regular);ordinary_control_no_graph=0;
            CHECK(rights_queued==2 && model_uid_account.unix_inflight==3);
        } else for(unsigned peek=0;peek<2;peek++) {
            struct kernel_socket_message received={.flags=peek?0:KERNEL_SOCKET_MSG_PEEK};
            struct kernel_socket_read_request read={0};pin=file[1];before=graph_acquisitions;
            CHECK(kernel_socket_reserve_read(socket[1],NULL,&read,&pin,1,0,&received)==1);
            CHECK(read.rights && graph_acquisitions>before);before=graph_acquisitions;
            CHECK(kernel_socket_finish_read(&read,0)==1 && graph_acquisitions>before);
            CHECK(received.control && received.control->count==2 &&
                received.control->files[0]==regular && received.control->files[1]==file[1]);
            CHECK(model_uid_account.unix_inflight==(peek?0:2));
            kernel_socket_control_release(&received.control);
        }
        model_put(&regular);model_put(&file[0]);model_put(&file[1]);
        kernel_socket_rights_service();model_no_control_owners();
    }
    puts("PASS UNIX mixed rights: publication and PEEK/consume acquire graph mutex, both OFDs survive; unreachable mixed cycle also destroys ordinary-only packet and returns every owner");
}
static void unix_control_owners(void)
{
    kernel_socket_network_initialize();
    unix_regular_right_owners();
    unix_right_quota();
    unix_mixed_right_owners();
    for(unsigned round=0;round<16;round++) {
        struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];model_owned_pair(file,socket);
        model_send_right(file[0],file[1]);CHECK(model_uid_account.unix_inflight==1 && model_pid.references==2);
        model_put(&file[0]);model_put(&file[1]);CHECK(heap_live && model_socket_files && rights_queued);
        kernel_socket_rights_service();model_no_control_owners();
    }
    puts("PASS UNIX SCM self cycles: 16 queued rights/credential snapshots collected, socket/OFD/PID/UID/heap owners restored");
    for(unsigned round=0;round<16;round++) {
        struct kernel_open_file_description *a[2],*b[2];struct kernel_socket *sa[2],*sb[2];model_owned_pair(a,sa);model_owned_pair(b,sb);
        model_send_right(a[0],b[1]);model_send_right(b[0],a[1]);CHECK(model_uid_account.unix_inflight==2);
        model_put(&a[0]);model_put(&a[1]);model_put(&b[0]);model_put(&b[1]);
        kernel_socket_rights_service();model_no_control_owners();
    }
    puts("PASS UNIX SCM mutual cycles: 16 reciprocal graphs collected, all queue and final owners restored");
    struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];model_owned_pair(file,socket);
    model_send_right(file[0],file[1]);model_put(&file[0]);
    struct root_cycle_gate gate={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.owner=file[1]};file[1]=NULL;
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,root_cycle_releaser,&gate));
    CHECK(!pthread_mutex_lock(&gate.lock));while(gate.phase!=1)CHECK(!pthread_cond_wait(&gate.changed,&gate.lock));
    unsigned retained=heap_live;kernel_socket_rights_service();
    CHECK(heap_live==retained && model_uid_account.unix_inflight==1 && socket[1]->packets_head &&
        socket[1]->packets_head->data[0]=='G' && kernel_open_file_references(gate.owner)==2);
    gate.phase=2;CHECK(!pthread_cond_broadcast(&gate.changed));
    while(gate.phase!=3)CHECK(!pthread_cond_wait(&gate.changed,&gate.lock));
    CHECK(!gate.owner);
    CHECK(!pthread_mutex_unlock(&gate.lock) && !pthread_join(thread,NULL));
    kernel_socket_rights_service();model_no_control_owners();
    CHECK(!pthread_cond_destroy(&gate.changed) && !pthread_mutex_destroy(&gate.lock));
    puts("PASS UNIX SCM external root: handshake retains queued content during collection, remote final external put enables collection");
    const unsigned char name[]={0,'g','c','-','p','e','n','d','i','n','g'};
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_UNIX,.unix_path=name,.unix_length=sizeof(name)};
    struct kernel_socket *listener=NULL,*client=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&listener) &&
        !kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,1));
    struct kernel_open_file_description *listener_file=model_socket_file_create(listener);
    CHECK(!kernel_socket_set_option(listener,KERNEL_SOCKET_PASSCRED,1));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM, 0,&client) && !kernel_socket_connect(client,&address,1));
    struct kernel_open_file_description *client_file=model_socket_file_create(client);
    model_send_right(client_file,client_file);model_put(&client_file);retained=heap_live;
    kernel_socket_rights_service();CHECK(heap_live==retained && listener->pending_head && listener->pending_head->child->rights_head && model_uid_account.unix_inflight==1);
    model_put(&listener_file);kernel_socket_rights_service();model_no_control_owners();
    puts("PASS UNIX SCM unaccepted child: listener root retains queued child rights, closing listener returns all socket/OFD/PID/UID/heap owners");
}

static struct kernel_socket_control *model_unreachable_self_right(void)
{
    struct kernel_open_file_description *file[2];struct kernel_socket *socket[2];
    model_owned_pair(file,socket);model_send_right(file[0],file[1]);
    struct kernel_socket_control *control=socket[1]->rights_head;
    CHECK(control && control->count==1 && control->files[0]==file[1] &&
        control->pid==&model_pid && control->account==&model_uid_account);
    struct model_socket_file *target=model_socket_file(file[1]);CHECK(target);
    model_put(&file[0]);model_put(&file[1]);
    CHECK(kernel_open_file_references(control->files[0])==1 &&
        model_uid_account.unix_inflight==1 && model_uid_account.references==2 && model_pid.references==2);
    control_release_probe.target=target;
    return control;
}
static void *control_gc_releaser(void *unused)
{
    (void)unused;kernel_socket_rights_service();return NULL;
}
static void unix_control_cleanup_failure(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket_control *control=model_unreachable_self_right();
    struct kernel_open_file_description *file=control->files[0];
    unsigned retained=heap_live;
    control_release_probe.fail=1;
    kernel_socket_rights_service();
    CHECK(control_release_probe.attempts==1 && rights_retired==control && !rights_queued &&
        !rights_releasing && !control->inflight && !control->receiver_previous &&
        control->files[0]==file && kernel_open_file_references(file)==1 && heap_live==retained &&
        !model_uid_account.unix_inflight && model_uid_account.references==2 && model_pid.references==2);
    CHECK(kernel_socket_rights_drain()==-KERNEL_EBUSY);
    CHECK(control_release_probe.attempts==2 && rights_retired==control && !rights_queued &&
        !rights_releasing && control->files[0]==file && kernel_open_file_references(file)==1 &&
        heap_live==retained && model_uid_account.references==2 && model_pid.references==2);
    control_release_probe.fail=0;
    CHECK(!kernel_socket_rights_drain() && control_release_probe.attempts==3);
    control_release_probe.target=NULL;
    model_no_control_owners();
    CHECK(!rights_retry_pending && !rights_active);
    puts("PASS UNIX SCM cleanup failure: two injected last-put failures retain exact OFD/control/PID/UID/heap owners; immediate drain retry returns EBUSY, recovery returns every owner");

    control_release_probe.attempts=0;control_release_probe.pause=1;
    control=model_unreachable_self_right();file=control->files[0];retained=heap_live;
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,control_gc_releaser,NULL));
    CHECK(!pthread_mutex_lock(&control_release_probe.lock));
    while(!control_release_probe.entered)
        CHECK(!pthread_cond_wait(&control_release_probe.changed,&control_release_probe.lock));
    /* The worker has detached the graph and is stopped at the actual last
     * put. Empty linked lists alone are insufficient to permit shutdown. */
    CHECK(!rights_queued && !rights_retired && rights_releasing &&
        control_release_probe.attempts==1 && heap_live==retained && control->files[0]==file &&
        kernel_open_file_references(file)==1 && model_uid_account.references==2 && model_pid.references==2 &&
        !model_uid_account.unix_inflight);
    CHECK(kernel_socket_rights_drain()==-KERNEL_EBUSY);
    CHECK(!rights_queued && !rights_retired && rights_releasing &&
        control_release_probe.attempts==1 && heap_live==retained);
    control_release_probe.resume=1;
    CHECK(!pthread_cond_broadcast(&control_release_probe.changed));
    CHECK(!pthread_mutex_unlock(&control_release_probe.lock) && !pthread_join(thread,NULL));
    control_release_probe.target=NULL;control_release_probe.pause=0;
    CHECK(!kernel_socket_rights_drain() && control_release_probe.attempts==1);
    model_no_control_owners();
    CHECK(!rights_retry_pending && !rights_active);
    puts("PASS UNIX SCM cleanup interleave: condition-variable handshake pauses lock-free last put, concurrent drain rejects empty-list false success, completed put returns all owners");
}

static void unix_seqpacket_records(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *a=NULL,*b=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_SEQPACKET, 0,&a));
    CHECK(kernel_socket_prepare_send(a)==-KERNEL_ENOTCONN);
    struct kernel_open_file_description *pin=(void *)a;struct kernel_socket_read_request read={0};
    struct kernel_socket_message message={0};
    CHECK(kernel_socket_reserve_read(a,NULL,&read,&pin,4,1,&message)==-KERNEL_ENOTCONN);
    CHECK((kernel_socket_poll(a,NULL)&(KERNEL_POLLHUP|KERNEL_POLLOUT|KERNEL_POLLERR))==(KERNEL_POLLHUP|KERNEL_POLLOUT));
    const unsigned char listener_name[]={0,'s','e','q','-','s','t','a','t','e'};
    struct kernel_socket_address listen_address=abstract_address(listener_name,sizeof(listener_name));
    CHECK(!kernel_socket_bind(a,&listen_address) && !kernel_socket_listen(a,1));
    CHECK(kernel_socket_reserve_read(a,NULL,&read,&pin,4,1,&message)==-KERNEL_ENOTCONN);
    CHECK(!(kernel_socket_poll(a,NULL)&(KERNEL_POLLHUP|KERNEL_POLLOUT|KERNEL_POLLERR)));
    kernel_socket_destroy(a);a=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_SEQPACKET,&a,&b));
    CHECK(write_buffer(a,"first",5,0)==5 && write_buffer(a,"tail",4,0)==4 && write_buffer(a,"",0,0)==0);
    uint64_t billed=a->unix_memory;unsigned live=heap_live;
    for(unsigned repeat=0;repeat<2;repeat++) {
        pin=(void *)b;message=(struct kernel_socket_message){.flags=KERNEL_SOCKET_MSG_PEEK};
        CHECK(kernel_socket_reserve_read(b,NULL,&read,&pin,2,1,&message)==2);
        uint32_t size=0;kernel_socket_read_info(&read,NULL,&size);CHECK(size==5);
        char bytes[2];kernel_socket_copy_read(&read,0,bytes,2);CHECK(!memcmp(bytes,"fi",2));
        CHECK(kernel_socket_finish_read(&read,0)==2 && message.receive_state==KERNEL_SOCKET_RECEIVE_RECORD);
        CHECK(a->unix_memory==billed && heap_live==live && b->unix_queued==3);
    }
    message=(struct kernel_socket_message){0};pin=(void *)b;
    CHECK(kernel_socket_reserve_read(b,NULL,&read,&pin,2,1,&message)==2);
    CHECK(kernel_socket_finish_read(&read,0)==2 && b->unix_queued==2);
    struct kernel_socket_address source=read_named_packet(b,"tail");kernel_socket_address_release(&source);
    pin=(void *)b;message=(struct kernel_socket_message){0};
    CHECK(!kernel_socket_reserve_read(b,NULL,&read,&pin,8,1,&message) && read.socket==b);
    CHECK(message.receive_state==KERNEL_SOCKET_RECEIVE_RECORD && !kernel_socket_finish_read(&read,0));
    CHECK(!b->rx_bytes && !a->unix_packets && !a->unix_memory);
    char bytes[]="fault";struct kernel_uaccess_iovec iov={(uintptr_t)bytes,5};
    pin=(void *)a;user_remaining=2;
    CHECK(kernel_socket_write_datagram(&pin,NULL,&iov,1,5,0,NULL,NULL)==-KERNEL_EFAULT);
    user_remaining=SIZE_MAX;CHECK(!a->active_writes && !a->tx_reserved && !b->unix_reserved && !b->packets_head);
    CHECK(write_buffer(a,"fault",5,0)==5);pin=(void *)b;message=(struct kernel_socket_message){0};
    CHECK(kernel_socket_reserve_read(b,NULL,&read,&pin,5,1,&message)==5);
    user_remaining=2;struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};
    CHECK(kernel_socket_read_user(&read,NULL,&cursor)==-KERNEL_EFAULT);
    CHECK(!kernel_socket_finish_read(&read,1));user_remaining=SIZE_MAX;
    CHECK(!b->rx_bytes && !a->unix_packets && !a->unix_memory);
    heap_fail_after=1;CHECK(write_buffer(a,"x",1,0)==-KERNEL_ENOMEM);CHECK(!heap_fail_after);
    CHECK(!a->active_writes && !a->tx_reserved && !b->unix_reserved && !b->packets_head);
    kernel_socket_destroy(a);pin=(void *)b;message=(struct kernel_socket_message){0};
    CHECK(!kernel_socket_reserve_read(b,NULL,&read,&pin,8,1,&message) && !read.socket);
    CHECK(message.receive_state==KERNEL_SOCKET_RECEIVE_EOF);
    CHECK((kernel_socket_poll(b,NULL)&(KERNEL_POLLHUP|KERNEL_POLLRDHUP|KERNEL_POLLERR))==(KERNEL_POLLHUP|KERNEL_POLLRDHUP));
    CHECK(write_buffer(b,"x",1,0)==-KERNEL_EPIPE);
    kernel_socket_destroy(b);CHECK(!heap_live);
    a=b=NULL;CHECK(!kernel_socket_pair(&test_heap,SOCKET_SEQPACKET,&a,&b));
    CHECK(!kernel_socket_shutdown(a,1));
    CHECK((kernel_socket_poll(b,NULL)&(KERNEL_POLLIN|KERNEL_POLLRDHUP|KERNEL_POLLHUP))==(KERNEL_POLLIN|KERNEL_POLLRDHUP));
    pin=(void *)b;message=(struct kernel_socket_message){0};
    CHECK(!kernel_socket_reserve_read(b,NULL,&read,&pin,1,1,&message) && message.receive_state==KERNEL_SOCKET_RECEIVE_EOF);
    CHECK(write_buffer(b,"reply",5,0)==5);
    source=read_named_packet(a,"reply");kernel_socket_address_release(&source);
    CHECK(!kernel_socket_shutdown(a,0) && write_buffer(b,"x",1,0)==-KERNEL_EPIPE);
    CHECK(kernel_socket_poll(b,NULL)&KERNEL_POLLHUP);
    kernel_socket_destroy(a);kernel_socket_destroy(b);CHECK(!heap_live);
    puts("PASS UNIX SEQ record: initial ENOTCONN/poll, two PEEKs, truncation, zero record/EOF, send/receive fault and allocation failure return request/credit/storage");
}
static void unix_late_source(void)
{
    kernel_socket_network_initialize();
    const unsigned char name[]={0,'l','a','t','e'};
    const int types[]={SOCKET_DGRAM,SOCKET_SEQPACKET,SOCKET_STREAM};
    for(unsigned t=0;t<3;t++) {
        struct kernel_socket *a=NULL,*b=NULL;CHECK(!kernel_socket_pair(&test_heap,types[t],&a,&b));
        CHECK(write_buffer(a,"early",5,0)==5);
        struct kernel_socket_address address=abstract_address(name,sizeof(name));
        CHECK(!kernel_socket_bind(a,&address));kernel_socket_destroy(a);
        struct kernel_socket *replacement=named_unix_socket(types[t],name,sizeof(name));
        struct kernel_socket_address source=read_named_packet(b,"early");
        expect_unix_name(&source,name,sizeof(name));CHECK(source.unix_owner!=replacement->unix_name);
        kernel_socket_address_release(&source);kernel_socket_destroy(b);kernel_socket_destroy(replacement);CHECK(!heap_live);
    }
    puts("PASS UNIX source name: send before bind, close and rebind before receive retain original current name for DGRAM/SEQ/STREAM");
}
static void unix_pending_reset(void)
{
    kernel_socket_network_initialize();
    const int types[]={SOCKET_STREAM,SOCKET_SEQPACKET};
    const unsigned char name[]={0,'p','e','n','d','i','n','g'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    for(unsigned t=0;t<2;t++) {
        struct kernel_socket *listener=named_unix_socket(types[t],name,sizeof(name)),*client=NULL;
        CHECK(!kernel_socket_listen(listener,0));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,types[t], 0,&client));
        CHECK(!kernel_socket_connect(client,&address,1));
        CHECK(listener->pending_head->child->type==types[t]);
        CHECK(!listener->pending_head->child->packets_head);
        kernel_socket_destroy(listener);
        CHECK(kernel_socket_poll(client,NULL)&KERNEL_POLLERR);
        CHECK(kernel_socket_unix_wait_error(client)==-KERNEL_ECONNRESET);
        CHECK(!kernel_socket_unix_wait_error(client));
        CHECK(write_buffer(client,"x",1,0)==-KERNEL_EPIPE);
        kernel_socket_destroy(client);CHECK(!heap_live);
    }
    puts("PASS UNIX empty pending child: listener close resets STREAM/SEQ client without needing queued payload, all objects returned");
}
static void unix_seqpacket_control(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *socket[2]={0};struct kernel_open_file_description *file[2];
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_SEQPACKET,&socket[0],&socket[1]));
    for(unsigned i=0;i<2;i++)file[i]=model_socket_file_create(socket[i]);
    CHECK(!kernel_socket_set_option(socket[1],KERNEL_SOCKET_PASSCRED,1));
    struct kernel_open_file_description *regular=model_socket_file_create(NULL);
    ordinary_control_no_graph=1;
    model_send_right(file[0],regular);
    unsigned references=kernel_open_file_references(regular);
    struct kernel_socket_message failed={.flags=KERNEL_SOCKET_MSG_PEEK};
    struct kernel_socket_read_request failed_read={0};struct kernel_open_file_description *failed_pin=file[1];
    heap_fail_after=1;
    CHECK(kernel_socket_reserve_read(socket[1],NULL,&failed_read,&failed_pin,1,1,&failed)==-KERNEL_ENOMEM);
    CHECK(!heap_fail_after && !failed_read.socket && failed_pin==file[1] && !socket[1]->read_request);
    CHECK(kernel_open_file_references(regular)==references && model_uid_account.unix_inflight==1);
    for(unsigned peek=0;peek<3;peek++) {
        struct kernel_socket_message message={.flags=peek<2?KERNEL_SOCKET_MSG_PEEK:0};
        struct kernel_socket_read_request read={0};struct kernel_open_file_description *pin=file[1];
        CHECK(kernel_socket_reserve_read(socket[1],NULL,&read,&pin,1,1,&message)==1);
        CHECK(kernel_socket_finish_read(&read,0)==1 && message.control && message.control->count==1);
        CHECK(message.control->files[0]==regular && message.control->has_credentials);
        CHECK(kernel_open_file_references(regular)==references+(peek<2));
        kernel_socket_control_release(&message.control);
    }
    CHECK(kernel_open_file_references(regular)==1 && !model_uid_account.unix_inflight);
    ordinary_control_no_graph=0;
    model_put(&regular);model_put(&file[0]);model_put(&file[1]);model_no_control_owners();
    for(unsigned round=0;round<4;round++) {
        socket[0]=socket[1]=NULL;CHECK(!kernel_socket_pair(&test_heap,SOCKET_SEQPACKET,&socket[0],&socket[1]));
        for(unsigned i=0;i<2;i++)file[i]=model_socket_file_create(socket[i]);
        CHECK(!kernel_socket_set_option(socket[1],KERNEL_SOCKET_PASSCRED,1));
        model_send_right(file[0],file[1]);model_put(&file[0]);
        struct root_cycle_gate gate={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.owner=file[1]};file[1]=NULL;
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,root_cycle_releaser,&gate));
        CHECK(!pthread_mutex_lock(&gate.lock));while(gate.phase!=1)CHECK(!pthread_cond_wait(&gate.changed,&gate.lock));
        unsigned live=heap_live;kernel_socket_rights_service();CHECK(heap_live==live && socket[1]->rights_head);
        gate.phase=2;CHECK(!pthread_cond_broadcast(&gate.changed));
        while(gate.phase!=3)CHECK(!pthread_cond_wait(&gate.changed,&gate.lock));
        CHECK(!pthread_mutex_unlock(&gate.lock) && !pthread_join(thread,NULL));
        kernel_socket_rights_service();model_no_control_owners();
        CHECK(!pthread_cond_destroy(&gate.changed) && !pthread_mutex_destroy(&gate.lock));
    }
    puts("PASS UNIX SEQ SCM: repeated PEEK duplicates real OFD references, consume returns UID/PID owners; remote final-root handshake enables GC in four rounds");
}
static void unix_seqpacket_reservation(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=NULL,*receiver=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_SEQPACKET,&sender,&receiver));
    CHECK(write_buffer(sender,"head",4,0)==4 && write_buffer(sender,"tail",4,0)==4);
    struct reservation_attempt attempts[2]={{.socket=receiver,.actor=1},{.socket=receiver,.actor=2}};
    pthread_t threads[2];CHECK(!pthread_create(&threads[0],NULL,reserve_concurrently,&attempts[0]));
    CHECK(!pthread_mutex_lock(&reservation_gate));
    while(!reservation_first)CHECK(!pthread_cond_wait(&reservation_changed,&reservation_gate));
    CHECK(!pthread_mutex_unlock(&reservation_gate));
    CHECK(!pthread_create(&threads[1],NULL,reserve_concurrently,&attempts[1]));
    CHECK(!pthread_mutex_lock(&reservation_gate));
    while(!reservation_second)CHECK(!pthread_cond_wait(&reservation_changed,&reservation_gate));
    reservation_release=1;CHECK(!pthread_cond_broadcast(&reservation_changed));
    CHECK(!pthread_mutex_unlock(&reservation_gate));
    CHECK(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));
    CHECK(attempts[0].result==4 && attempts[1].result==-KERNEL_EAGAIN);
    CHECK(!attempts[0].pin && attempts[1].pin==(void *)receiver);
    kernel_socket_destroy(sender);
    CHECK(sender->destroying==2 && sender->unix_packets==2);
    char bytes[4];kernel_socket_copy_read(&attempts[0].read,0,bytes,4);CHECK(!memcmp(bytes,"head",4));
    CHECK(kernel_socket_finish_read(&attempts[0].read,0)==4 && attempts[0].pin==(void *)receiver);
    CHECK(sender->unix_packets==1 && !receiver->read_request && receiver->unix_queued==1);
    struct kernel_socket_address source=read_named_packet(receiver,"tail");kernel_socket_address_release(&source);
    kernel_socket_destroy(receiver);CHECK(!heap_live);
    puts("PASS UNIX SEQ concurrent receive: handshake selects one head owner, loser retains pin, sender close preserves reserved and trailing records until exact credit/storage return");
}
static void unix_seqpacket_pending_control(void)
{
    kernel_socket_network_initialize();
    const unsigned char name[]={0,'s','e','q','-','g','c'};
    struct kernel_socket_address address=abstract_address(name,sizeof(name));
    for(unsigned accept=0;accept<2;accept++) {
        struct kernel_socket *listener=named_unix_socket(SOCKET_SEQPACKET,name,sizeof(name)),*client=NULL,*child=NULL;
        struct kernel_open_file_description *listener_file=model_socket_file_create(listener);
        CHECK(!kernel_socket_listen(listener,1) && !kernel_socket_set_option(listener,KERNEL_SOCKET_PASSCRED,1));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_UNIX,SOCKET_SEQPACKET, 0,&client));
        CHECK(!kernel_socket_connect(client,&address,1));
        struct kernel_open_file_description *client_file=model_socket_file_create(client);
        model_send_right(client_file,client_file);model_put(&client_file);
        unsigned live=heap_live;kernel_socket_rights_service();
        CHECK(heap_live==live && listener->pending_head->child->rights_head && model_uid_account.unix_inflight==1);
        if(accept) {
            struct kernel_open_file_description *accepted=model_socket_file_create(NULL);
            CHECK(!kernel_socket_accept(listener,&child,accepted));model_socket_file(accepted)->socket=child;
            CHECK(child->gc_file==accepted && !child->listener);
            model_put(&listener_file);live=heap_live;kernel_socket_rights_service();
            CHECK(heap_live==live && child->rights_head && model_uid_account.unix_inflight==1);
            model_put(&accepted);
        } else model_put(&listener_file);
        kernel_socket_rights_service();model_no_control_owners();
    }
    puts("PASS UNIX SEQ pending SCM: listener roots unaccepted rights, accept transfers root to OFD, final close returns socket/OFD/PID/UID/heap owners");
}

static unsigned async_reasons[7];
static void async_observer(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{(void)node;CHECK(reason==KERNEL_WAIT_WOKEN&&event>=1&&event<=6);async_reasons[event]++;}
static void async_events(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=NULL,*receiver=NULL;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_DGRAM,&sender,&receiver));
    struct kernel_wait_node observer={0};kernel_wait_node_init_callback(&observer,async_observer,NULL);
    kernel_wait_queue_add(&receiver->async_events,&observer);
    CHECK(write_buffer(sender,"a",1,0)==1&&write_buffer(sender,"b",1,0)==1);
    CHECK(async_reasons[KERNEL_ASYNC_POLL_IN]==2);
    struct kernel_socket_address source=read_named_packet(receiver,"a");kernel_socket_address_release(&source);
    source=read_named_packet(receiver,"b");kernel_socket_address_release(&source);
    CHECK(async_reasons[KERNEL_ASYNC_POLL_IN]==2);
    {
        SOCKET_STATE(state);
        socket_state_notify(state.owner,&receiver->wait);socket_state_notify(state.owner,&receiver->wait);
        socket_async(receiver,KERNEL_ASYNC_POLL_IN);socket_async(receiver,KERNEL_ASYNC_POLL_IN);
    }
    CHECK(async_reasons[KERNEL_ASYNC_POLL_IN]==4);
    CHECK(!kernel_socket_shutdown(sender,0)&&!kernel_socket_shutdown(sender,1)&&!kernel_socket_shutdown(sender,2));
    CHECK(async_reasons[KERNEL_ASYNC_POLL_IN]==4&&!async_reasons[KERNEL_ASYNC_POLL_OUT]&&!async_reasons[KERNEL_ASYNC_POLL_HUP]);
    kernel_socket_destroy(sender);CHECK(!async_reasons[KERNEL_ASYNC_POLL_HUP]);
    CHECK(!kernel_wait_node_remove_sync(&observer));kernel_socket_destroy(receiver);CHECK(!heap_live);
    const int types[]={SOCKET_STREAM,SOCKET_SEQPACKET};
    for(unsigned i=0;i<2;i++) {
        memset(async_reasons,0,sizeof(async_reasons));observer=(struct kernel_wait_node){0};sender=receiver=NULL;
        CHECK(!kernel_socket_pair(&test_heap,types[i],&sender,&receiver));
        kernel_wait_node_init_callback(&observer,async_observer,NULL);kernel_wait_queue_add(&receiver->async_events,&observer);
        CHECK(!kernel_socket_shutdown(sender,0));CHECK(!async_reasons[KERNEL_ASYNC_POLL_IN]&&!async_reasons[KERNEL_ASYNC_POLL_HUP]);
        CHECK(!kernel_socket_shutdown(sender,1));CHECK(async_reasons[KERNEL_ASYNC_POLL_IN]==1&&!async_reasons[KERNEL_ASYNC_POLL_HUP]);
        CHECK(!kernel_socket_shutdown(sender,2));CHECK(async_reasons[KERNEL_ASYNC_POLL_HUP]==1);
        kernel_socket_destroy(sender);CHECK(async_reasons[KERNEL_ASYNC_POLL_HUP]==2);
        CHECK(!kernel_wait_node_remove_sync(&observer));kernel_socket_destroy(receiver);CHECK(!heap_live);
    }
    puts("PASS socket async events: accepted datagrams, independent cursors, and Linux UNIX shutdown/close cadence");
}

static void raw_echo_bytes(unsigned char bytes[64])
{
    memset(bytes,0,64);bytes[0]=8;bytes[4]=0x12;bytes[5]=0x34;bytes[7]=1;
    for(unsigned i=8;i<64;i++)bytes[i]=(unsigned char)i;
    uint16_t checksum=inet_chksum(bytes,64);memcpy(bytes+2,&checksum,2);
}
static void raw_drain(struct kernel_socket *socket)
{
    while(socket->packets_head) {
        struct kernel_open_file_description *pin=(void *)socket;
        struct kernel_socket_read_request read={0};
        CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,65535,1,NULL)>0);
        kernel_socket_finish_read(&read,0);CHECK(pin==(void *)socket);
    }
}
static void raw_icmp_owner(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *a=NULL,*b=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_RAW,1,&a));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_RAW,1,&b));
    raw_filter_test.references=1;raw_filter_test.keep=0;
    CHECK(!kernel_socket_filter_set(b,(void *)&raw_filter_test));
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=1,.bytes={127,0,0,1}};
    CHECK(!kernel_socket_connect(a,&address,0));
    unsigned char echo[64];raw_echo_bytes(echo);
    CHECK(write_buffer(a,echo,sizeof(echo),0)==64);
    for(unsigned i=0;i<4;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(raw_filter_test.calls==2 && raw_filter_test.references==1 && !b->rx_bytes);
    CHECK(a->packets_head && a->packets_tail!=a->packets_head && a->rx_bytes==168);
    unsigned char request[84],reply[84];
    CHECK(pbuf_copy_partial(a->packets_head->payload,request,84,0)==84);
    CHECK(pbuf_copy_partial(a->packets_tail->payload,reply,84,0)==84);
    CHECK(request[20]==8 && reply[20]==0 && !memcmp(request+20,echo,64));
    CHECK(request[6]==0x40 && request[7]==0 && request[8]==64 && reply[8]==64);
    CHECK(!inet_chksum(request,20) && !inet_chksum(reply,20) && !inet_chksum(reply+20,64));
    struct kernel_open_file_description *pin=(void *)a;
    struct kernel_socket_read_request read={0};
    struct kernel_socket_message message={.flags=KERNEL_SOCKET_MSG_PEEK};
    CHECK(kernel_socket_reserve_read(a,NULL,&read,&pin,4,1,&message)==4);
    uint32_t length=0;struct kernel_socket_address peer={0};
    kernel_socket_read_info(&read,&peer,&length);CHECK(length==84 && !peer.port && peer.family==KERNEL_SOCKET_AF_INET);
    kernel_socket_finish_read(&read,0);CHECK(a->rx_bytes==168 && pin==(void *)a);
    unsigned char output[84];struct kernel_uaccess_iovec iov={(uintptr_t)output,sizeof(output)};
    struct kernel_uaccess_iov_cursor cursor={&iov,1,0,0};
    CHECK(kernel_socket_reserve_read(a,NULL,&read,&pin,84,1,NULL)==84);
    user_remaining=7;CHECK(kernel_socket_read_user(&read,(void *)a,&cursor)==-KERNEL_EFAULT);
    kernel_socket_finish_read(&read,1);user_remaining=SIZE_MAX;
    CHECK(a->rx_bytes==84 && pin==(void *)a);raw_drain(a);
    heap_fail_after=1;
    CHECK(write_buffer(a,echo,sizeof(echo),0)==64);
    for(unsigned i=0;i<4;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!heap_fail_after && a->rx_bytes==84 && !b->rx_bytes);raw_drain(a);
    /* A queue with no room drops its own version without preventing the
     * other raw endpoint or ICMP's automatic Echo Reply. */
    raw_filter_test.keep=UINT32_MAX;a->rx_limit=1;
    CHECK(write_buffer(a,echo,sizeof(echo),0)==64);
    for(unsigned i=0;i<4;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!a->rx_bytes && b->rx_bytes==168);raw_drain(b);
    kernel_socket_destroy(a);kernel_socket_destroy(b);
    CHECK(!raw_filter_test.references && !lwip_stats.memp[MEMP_RAW_PCB]->used);
    puts("PASS raw ICMP: immutable IPv4 snapshots, independent BPF/queue/OOM drops, PEEK, fault consumption, responder and owners");
}
static void *raw_sender_thread(void *argument)
{
    unsigned char echo[64];raw_echo_bytes(echo);
    CHECK(write_buffer(argument,echo,sizeof(echo),0)==64);return NULL;
}
static void *raw_closer_thread(void *argument)
{raw_closing_actor=1;kernel_socket_destroy(argument);return NULL;}
static void raw_icmp_close(void)
{
    kernel_socket_network_initialize();struct kernel_socket *a=NULL,*b=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_RAW,1,&a));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_RAW,1,&b));
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=1,.bytes={127,0,0,1}};
    CHECK(!kernel_socket_connect(a,&address,0));
    raw_filter_test.references=1;raw_filter_test.keep=UINT32_MAX;
    CHECK(!kernel_socket_filter_set(b,(void *)&raw_filter_test));raw_pause=1;
    pthread_t sender,closer;CHECK(!pthread_create(&sender,NULL,raw_sender_thread,a));
    CHECK(!pthread_mutex_lock(&raw_gate));
    while(!raw_filter_entered)CHECK(!pthread_cond_wait(&raw_changed,&raw_gate));
    CHECK(!pthread_mutex_unlock(&raw_gate));
    CHECK(!pthread_create(&closer,NULL,raw_closer_thread,b));
    CHECK(!pthread_mutex_lock(&raw_gate));
    while(!raw_close_waiting)CHECK(!pthread_cond_wait(&raw_changed,&raw_gate));
    CHECK(raw_filter_test.references==1 && !b->destroying);
    raw_resume=1;CHECK(!pthread_cond_broadcast(&raw_changed) && !pthread_mutex_unlock(&raw_gate));
    CHECK(!pthread_join(sender,NULL) && !pthread_join(closer,NULL));
    CHECK(!raw_filter_test.references);kernel_socket_destroy(a);
    puts("PASS raw close: stopped in real filter callback, last close waits for protocol and returns packet/program owners");
}
static void ipv4_options(void)
{
    kernel_socket_network_initialize();struct netif interface={0};
    route_validate_packets=1;
    ip4_addr_t local,mask,empty;IP4_ADDR(&local,10,77,0,2);IP4_ADDR(&mask,255,255,255,0);ip4_addr_set_zero(&empty);
    CHECK(netif_add(&interface,&local,&mask,&empty,NULL,route_interface_init,ip_input));
    netif_set_up(&interface);netif_set_link_up(&interface);
    struct kernel_socket *socket=NULL;CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,0,&socket));
    struct kernel_open_file_description *pin=(void *)socket;
    char bytes[]="broadcast";struct kernel_uaccess_iovec iov={(uintptr_t)bytes,sizeof(bytes)};
    struct kernel_socket_address broadcast={.family=KERNEL_SOCKET_AF_INET,.port=23000,.bytes={255,255,255,255}};
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&broadcast,NULL)==-KERNEL_EACCES);
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_BROADCAST,1));
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_IPV4_TTL,37));
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&broadcast,NULL)==sizeof(bytes));
    CHECK(route_frame_size>=42 && route_frame[22]==37 && !memcmp(route_frame,"\xff\xff\xff\xff\xff\xff",6));
    CHECK(route_frame[20]==0x40 && !route_frame[21] && route_df_packets==1);
    char name[16]={0};uint32_t length=sizeof(name);
    CHECK(!kernel_socket_set_bound_device(socket,"eth0") && !kernel_socket_get_bound_device(socket,name,&length));
    CHECK(length==5 && !strcmp(name,"eth0"));length=5;
    CHECK(kernel_socket_get_bound_device(socket,name,&length)==-KERNEL_EINVAL);
    model_credentials.effective=0;
    CHECK(kernel_socket_set_bound_device(socket,"lo")==-KERNEL_EPERM);
    CHECK(kernel_socket_set_bound_device(socket,"")==-KERNEL_EPERM);
    model_credentials.effective=KERNEL_CAP_VALID_MASK;
    CHECK(!kernel_socket_set_multicast_interface(socket,netif_get_index(&interface),local.addr));
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_MULTICAST_TTL,7));
    struct kernel_socket_address multicast={.family=KERNEL_SOCKET_AF_INET,.port=23000,.bytes={239,1,2,3}};
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&multicast,NULL)==sizeof(bytes));
    CHECK(route_frame[22]==7 && !memcmp(route_frame+26,&local.addr,4));
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_MULTICAST_TTL,0));
    route_frame_size=0;
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&multicast,NULL)==sizeof(bytes));
    CHECK(!route_frame_size);
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_MULTICAST_TTL,7));
    ip4_addr_t nonlocal;IP4_ADDR(&nonlocal,192,0,2,123);
    CHECK(!kernel_socket_set_multicast_interface(socket,netif_get_index(&interface),nonlocal.addr));
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&multicast,NULL)==-KERNEL_ENETUNREACH);
    CHECK(!kernel_socket_set_multicast_interface(socket,netif_get_index(&interface),local.addr));
    struct kernel_socket *raw=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_RAW,1,&raw));
    CHECK(!kernel_socket_set_bound_device(raw,"eth0") && !kernel_socket_set_option(raw,KERNEL_SOCKET_BROADCAST,1));
    CHECK(!kernel_socket_connect(raw,&broadcast,0));
    unsigned char large[4096]={8};large[4]=1;
    uint16_t checksum=inet_chksum(large,sizeof(large));memcpy(large+2,&checksum,2);
    CHECK(write_buffer(raw,large,sizeof(large),0)==sizeof(large));
    CHECK(route_fragments==3 && route_fragment_bytes==sizeof(large));
    kernel_socket_destroy(raw);
    CHECK(!kernel_socket_set_bound_device(socket,"lo"));
    CHECK(kernel_socket_set_multicast_interface(socket,netif_get_index(&interface),local.addr)==-KERNEL_EINVAL);
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_BROADCAST,0));
    broadcast.bytes[0]=127;broadcast.bytes[1]=broadcast.bytes[2]=broadcast.bytes[3]=255;
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&broadcast,NULL)==-KERNEL_EACCES);
    struct kernel_socket *broadcast_receiver=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,0,&broadcast_receiver));
    struct kernel_socket_address any={.family=KERNEL_SOCKET_AF_INET,.port=23000};
    CHECK(!kernel_socket_set_bound_device(broadcast_receiver,"lo") && !kernel_socket_bind(broadcast_receiver,&any));
    CHECK(!kernel_socket_set_option(socket,KERNEL_SOCKET_BROADCAST,1));
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&broadcast,NULL)==sizeof(bytes));
    for(unsigned i=0;i<4;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(broadcast_receiver->rx_bytes==sizeof(bytes));kernel_socket_destroy(broadcast_receiver);
    /* A selected 127/8 alias is a real source choice, not just a successful
     * option query. The source stays owned while the packet is emitted. */
    ip4_addr_t alias;IP4_ADDR(&alias,127,0,0,2);
    CHECK(!kernel_socket_set_multicast_interface(socket,0,alias.addr));
    CHECK(kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,sizeof(bytes),0,&multicast,NULL)==sizeof(bytes));
    for(unsigned i=0;i<4;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!kernel_socket_set_bound_device(socket,""));
    model_credentials.effective=0;CHECK(!kernel_socket_set_bound_device(socket,"eth0"));
    model_credentials.effective=KERNEL_CAP_VALID_MASK;
    for(int type=SOCKET_STREAM;type<=SOCKET_DGRAM;type++) {
        struct kernel_socket *a=NULL,*b=NULL,*c=NULL;
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,type,0,&a));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,type,0,&b));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,type,0,&c));
        any.port=(uint16_t)(24000+type);
        CHECK(!kernel_socket_set_bound_device(a,"lo") && !kernel_socket_set_bound_device(b,"eth0"));
        CHECK(!kernel_socket_bind(a,&any) && !kernel_socket_bind(b,&any));
        CHECK(kernel_socket_bind(c,&any)==-KERNEL_EADDRINUSE);
        CHECK(!kernel_socket_set_bound_device(c,"eth0") && kernel_socket_bind(c,&any)==-KERNEL_EADDRINUSE);
        if(type==SOCKET_STREAM)CHECK(!kernel_socket_listen(a,1) && !kernel_socket_listen(b,1));
        kernel_socket_destroy(c);kernel_socket_destroy(b);kernel_socket_destroy(a);
    }
    struct kernel_socket *listener=NULL,*client=NULL,*accepted=NULL;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM,0,&listener));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM,0,&client));
    any=(struct kernel_socket_address){.family=KERNEL_SOCKET_AF_INET,.port=24100,.bytes={127,0,0,1}};
    CHECK(!kernel_socket_set_bound_device(listener,"lo") && !kernel_socket_bind(listener,&any) && !kernel_socket_listen(listener,1));
    accept_binding_probe=listener;
    int connected=kernel_socket_connect(client,&any,0);
    CHECK(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(accept_binding_changed==1 && !accept_binding_probe && !kernel_socket_accept(listener,&accepted,NULL));
    length=16;CHECK(!kernel_socket_get_bound_device(accepted,name,&length) && !strcmp(name,"lo"));
    CHECK(accepted->bound_device==accepted->tcp->netif_idx && listener->bound_device!=accepted->bound_device);
    {struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
      if(client->tcp)tcp_abort(client->tcp);
      if(accepted->tcp)tcp_abort(accepted->tcp);
      kernel_socket_protocol_leave(&scope);}
    kernel_socket_destroy(client);kernel_socket_destroy(accepted);kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    kernel_socket_destroy(socket);etharp_cleanup_netif(&interface);netif_remove(&interface);
    puts("PASS IPv4 options: physical and lo broadcast permission, TTL on wire, multicast source, stable binding and capability changes");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(kernel_pid_allocator_init(&model_pid_numbers,model_pid_bitmap,1024)==KERNEL_PID_STATUS_OK);
    CHECK(kernel_pid_publish(&model_pid_registry,&model_pid)==KERNEL_PID_STATUS_OK);
    if (!strcmp(argv[1], "raw-icmp-owner")) raw_icmp_owner();
    else if (!strcmp(argv[1], "raw-icmp-close")) raw_icmp_close();
    else if (!strcmp(argv[1], "ipv4-options")) ipv4_options();
    else if (!strcmp(argv[1], "async-events")) async_events();
    else if (!strcmp(argv[1], "unix-seq-records")) unix_seqpacket_records();
    else if (!strcmp(argv[1], "ipv4-route")) ipv4_route_output();
    else if (!strcmp(argv[1], "tcp-loopback-checksum")) tcp_loopback_checksum();
    else if (!strcmp(argv[1], "tcp-cork")) tcp_cork_contract();
    else if (!strcmp(argv[1], "tcp-more")) tcp_more_contract();
    else if (!strcmp(argv[1], "tcp-detached")) tcp_detached_copy();
    else if (!strcmp(argv[1], "tcp-reset-receive-retry")) tcp_reset_receive_retry();
    else if (!strcmp(argv[1], "ipv4-address-shutdown")) ipv4_address_shutdown();
    else if (!strcmp(argv[1], "unix-seq-control")) unix_seqpacket_control();
    else if (!strcmp(argv[1], "unix-seq-reservation")) unix_seqpacket_reservation();
    else if (!strcmp(argv[1], "unix-seq-pending-control")) unix_seqpacket_pending_control();
    else if (!strcmp(argv[1], "unix-late-source")) unix_late_source();
    else if (!strcmp(argv[1], "unix-pending-reset")) unix_pending_reset();
    else if (!strcmp(argv[1], "unix-control-cleanup")) unix_control_cleanup_failure();
    else if (!strcmp(argv[1], "unix-control-owner")) unix_control_owners();
    else if (!strcmp(argv[1], "unix-write-notify")) unix_write_completion_notifications();
    else if (!strcmp(argv[1], "unix-stream-state")) unix_stream_receive_state();
    else if (!strcmp(argv[1], "unix-space-topology")) unix_space_topology();
    else if (!strcmp(argv[1], "unix-shutdown-state")) unix_shutdown_states();
    else if (!strcmp(argv[1], "unix-dgram-shutdown")) unix_datagram_read_shutdown();
    else if (!strcmp(argv[1], "unix-dgram-reconnect")) unix_datagram_reconnect_receive();
    else if (!strcmp(argv[1], "unix-listener-lookup-close")) unix_listener_lookup_close();
    else if (!strcmp(argv[1], "unix-peer-queued")) unix_stream_peer_after_close(1);
    else if (!strcmp(argv[1], "unix-peer-accepted")) unix_stream_peer_after_close(0);
    else if (!strcmp(argv[1], "unix-autobind-collision")) unix_autobind_collision();
    else if (!strcmp(argv[1], "unix-names")) unix_named_endpoints();
    else if (!strcmp(argv[1], "notify-stop")) notification_stop_lifetime();
    else if (!strcmp(argv[1], "stop-wait")) stopped_wait_notification();
    else if (!strcmp(argv[1], "remote-protocol")) remote_protocol_request(0);
    else if (!strcmp(argv[1], "queued-completion")) queued_completion_before_output();
    else if (!strcmp(argv[1], "remote-direct")) remote_protocol_request(10);
    else if (!strcmp(argv[1], "remote-batch")) remote_protocol_request(5);
    else if (!strcmp(argv[1], "remote-batch-ack")) remote_protocol_request(7);
    else if (!strcmp(argv[1], "remote-batch-writers")) remote_protocol_request(9);
    else if (!strcmp(argv[1], "remote-turn")) remote_protocol_request(6);
    else if (!strcmp(argv[1], "remote-quota-notify")) remote_protocol_request(8);
    else if (!strcmp(argv[1], "remote-udp")) remote_protocol_request(1);
    else if (!strcmp(argv[1], "remote-udp-fault")) remote_protocol_request(2);
    else if (!strcmp(argv[1], "remote-udp-stop")) remote_protocol_request(3);
    else if (!strcmp(argv[1], "remote-copy-stop")) remote_protocol_request(4);
    else if (!strcmp(argv[1], "concurrent-reservation")) {concurrent_reservation();return 0;}
    else if (!strcmp(argv[1], "protocol-owner")) { protocol_ownership(); return 0; }
    else if (!strcmp(argv[1], "endpoint-owner")) endpoint_ownership();
    else if (!strcmp(argv[1], "receive-reuse-udp")) receive_node_reuse_udp();
    else if (!strcmp(argv[1], "receive-reuse-tcp")) receive_node_reuse_tcp();
    else if (!strcmp(argv[1], "receive-chains")) receive_packet_chains();
    else if (!strcmp(argv[1], "receive-prefix")) receive_prefix();
    else if (!strcmp(argv[1], "protocol-transaction")) protocol_transaction();
    else if (!strcmp(argv[1], "nodelay")) nodelay_pending(0);
    else if (!strcmp(argv[1], "minshall")) minshall_send();
    else if (!strcmp(argv[1], "nodelay-capacity")) nodelay_pending(1);
    else if (!strcmp(argv[1], "tcp-input-output")) nodelay_pending(2);
    else if (!strcmp(argv[1], "unix-payload")) unix_final_payload();
    else if (!strcmp(argv[1], "unix-packet-boundary")) unix_stream_packet_boundary();
    else if (!strcmp(argv[1], "receive-payload")) receive_payload();
    else if (!strcmp(argv[1], "udp-payload")) udp_final_payload();
    else if (!strcmp(argv[1], "loopback-work")) loopback_work();
    else if (!strcmp(argv[1], "raw-tcp-ingress")) raw_tcp_ingress();
    else if (!strcmp(argv[1], "unix-sender")) unix_sender_owner();
    else if (!strcmp(argv[1], "tcp-batch")) copy_capacity(0,1);
    else if (!strcmp(argv[1], "tcp-quota-notify")) tcp_quota_notifications();
    else if (!strcmp(argv[1], "pool-release-owner")) pool_release_owner();
    else if (!strcmp(argv[1], "tcp-batch-last")) copy_capacity(0,2);
    else if (!strcmp(argv[1], "tcp-batch-abort")) copy_capacity(0,3);
    else if (!strcmp(argv[1], "tcp-batch-error")) copy_capacity(0,4);
    else if (!strcmp(argv[1], "copy-indirect")) copy_capacity(1,0);
    else if (!strcmp(argv[1], "copy-ineligible")) copy_capacity(0,0);
    else if (!strcmp(argv[1], "timewait-capacity")) timewait_owner(0);
    else if (!strcmp(argv[1], "timewait-timer")) timewait_owner(1);
    else if (!strcmp(argv[1], "controls")) { tcp_ephemeral_ports(); timewait_owner(2); raw_owner_control(); }
    else return 2;
    CHECK(!test_context.allocation_depth && !protocol_depth);
    CHECK(!heap_live && !tcp_active_pcbs && !tcp_tw_pcbs && !tcp_listen_pcbs.pcbs);
    CHECK(!lwip_stats.memp[MEMP_TCP_PCB]->used && !lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used &&
          !lwip_stats.memp[MEMP_TCP_SEG]->used && !lwip_stats.mem.used);
    CHECK(model_pid.references==1 && model_uid_account.references==1 && !model_uid_account.unix_inflight);
    kernel_pid_put(&model_pid);CHECK(kernel_pid_take_retired(&model_pid_registry)==&model_pid && !model_pid_numbers.allocated);
    return 0;
}
