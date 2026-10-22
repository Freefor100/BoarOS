/* Actual socket/ethernet/lwIP paths. Host heap, clock, empty wait queues and NIC
 * capacity are boundary models; TCP ownership and work queues are production. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <kernel/random.h>
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>
#include "../../net/socket.c"
#include "../../net/ethernet.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
static struct kernel_heap test_heap;
static _Thread_local struct kernel_io_context test_context;
_Thread_local uintptr_t network_host_irq = RISCV_SSTATUS_SIE;
_Thread_local unsigned network_host_irq_saves,network_host_irq_restores;
static unsigned heap_allocations,protocol_services,allow_ofd_release;
static _Thread_local unsigned reservation_actor;
static pthread_mutex_t reservation_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reservation_changed=PTHREAD_COND_INITIALIZER;
static unsigned reservation_first, reservation_second, reservation_release;
static struct kernel_socket_write_request *remote_write;
static pthread_mutex_t remote_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t remote_changed=PTHREAD_COND_INITIALIZER;
static unsigned remote_work,remote_done,remote_mode,remote_testing;
static unsigned copy_waiting,copy_release,stop_waiting,stop_done,stop_borrows;
static struct kernel_socket *remote_socket;
static _Thread_local struct kernel_task *remote_task;
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
    if(reservation_actor==2)reservation_arrive();
    __real_kernel_raw_lock_acquire(lock,guard);
}
uint64_t kernel_cost_clock(void) {return 0;}
struct kernel_cost_tag kernel_cost_capture(void) {return (struct kernel_cost_tag){0};}
void kernel_cost_raw_lock(struct kernel_cost_tag tag,uint64_t wait,uint64_t hold)
{(void)tag;(void)wait;(void)hold;}

void kernel_cost_add(enum kernel_cost_metric metric,uint64_t value)
{ if (metric==COST_NETWORK_SERVICE_CALLS) protocol_services+=(unsigned)value; }
static size_t user_remaining = SIZE_MAX;
static struct kernel_socket *close_during_copy;
static struct kernel_socket *shutdown_during_copy;
static unsigned heap_live, dma_eligible, nic_full, copy_attempts, copied_packets, segment_attempts, captured_size;
static void (*heap_release_once)(void *);
static uint64_t clock_ns = 1000000000ULL;
static unsigned char captured[1536];

void kernel_assert_can_block(void) {CHECK(!kernel_cpu_current()->raw_locks);}
#if BOAROS_MAX_CPUS > 1
void kernel_assert_service_context(void) {CHECK(!kernel_cpu_current()->logical_id);}
void kernel_cpu_flush_notifications(void) {CHECK(!kernel_cpu_current()->outbound);}
#endif
int kernel_wait_cursor_prepare(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor)
{(void)cursor;CHECK(queue->initialized && !queue->head);return 0;}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *cursor,int all)
{(void)cursor;(void)all;abort();}
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{ *lock=(struct kernel_rwlock){.rank=rank,.key=key}; }
static pthread_mutex_t model_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t handshake=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handshake_changed=PTHREAD_COND_INITIALIZER;
static unsigned competing, waiter_registered, acquired;
static struct kernel_socket *handoff_peer;
static unsigned handoff_bytes;
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    CHECK(!guard->lock);
    if(competing) {
        CHECK(!pthread_mutex_lock(&handshake)); waiter_registered=1;
        CHECK(!pthread_cond_signal(&handshake_changed)); CHECK(!pthread_mutex_unlock(&handshake));
    }
    CHECK(!pthread_mutex_lock(&model_mutex)); CHECK(!lock->writer);
    lock->writer=&test_context; *guard=(struct kernel_lock_guard){.lock=lock,.owner=&test_context}; acquired++;
}
int kernel_rwlock_try_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    CHECK(!guard->lock);
    if (pthread_mutex_trylock(&model_mutex)) return 0;
    CHECK(!lock->writer);
    lock->writer=&test_context; *guard=(struct kernel_lock_guard){.lock=lock,.owner=&test_context}; acquired++;
    return 1;
}
void kernel_lock_release(struct kernel_lock_guard *guard)
{
    CHECK(guard->lock && guard->owner==&test_context && guard->lock->writer==&test_context);
    if (handoff_peer) { CHECK(handoff_peer->rx_bytes == handoff_bytes); handoff_peer=NULL; }
    guard->lock->writer=0; guard->lock=0; CHECK(!pthread_mutex_unlock(&model_mutex));
}
static int work_pending_snapshot(void)
{ struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope); int result=kernel_socket_work_pending(); kernel_socket_protocol_leave(&scope); return result; }
struct kernel_io_context *kernel_io_context_current(void) { return &test_context; }
struct kernel_task *kernel_task_current(void) { return remote_task; }
enum kernel_signal_status kernel_signal_send_task(struct kernel_task *task, uint32_t signal, kernel_pid_t sender)
{ (void)task; (void)signal; (void)sender; abort(); }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t n, size_t size, void **out)
{ CHECK(heap == &test_heap); *out = calloc(n, size); CHECK(*out); ++heap_live; ++heap_allocations; return KERNEL_HEAP_STATUS_OK; }
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
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ *queue = (struct kernel_wait_queue){.initialized = 1}; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ CHECK(queue->initialized && !queue->head && !arch_interrupt_is_enabled()); return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *q)
{CHECK(q->initialized && !q->head);q->closed=1;return KERNEL_SCHEDULER_STATUS_OK;}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *q)
{CHECK(q->closed && !q->head && !q->borrows);*q=(struct kernel_wait_queue){0};return KERNEL_SCHEDULER_STATUS_OK;}
void kernel_wait_backend_quiesce(void) {abort();}
uint64_t kernel_time_monotonic_ns(void) { return clock_ns; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t time, uint64_t *out)
{ *out = time; return KERNEL_TIME_STATUS_OK; }
int kernel_random_available(void) { return 0; }
enum kernel_random_status kernel_random_fill(void *buffer, size_t size)
{ (void)buffer; (void)size; abort(); }

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

struct kernel_socket *kernel_open_file_socket(struct kernel_open_file_description *f)
{ return (struct kernel_socket *)(uintptr_t)f; }
enum kernel_open_file_status kernel_open_file_acquire(struct kernel_open_file_description *f)
{CHECK(remote_testing && f==(void *)remote_socket);stop_borrows++;return KERNEL_OPEN_FILE_STATUS_OK;}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **f)
{ CHECK(f && *f);
  if(stop_borrows && *f==(void *)remote_socket)stop_borrows--;
  else {CHECK(allow_ofd_release);--allow_ofd_release;}
  *f=0;return KERNEL_OPEN_FILE_STATUS_OK; }
enum kernel_task_status kernel_task_socket_write_register(struct kernel_task *t, struct kernel_socket_write_request *r)
{CHECK(remote_testing && t==remote_task && (!remote_write || remote_write==r));remote_write=r;return KERNEL_TASK_STATUS_OK;}
enum kernel_task_status kernel_task_socket_write_clear(struct kernel_task *t, struct kernel_socket_write_request *r)
{CHECK(remote_testing && t==remote_task && remote_write==r);remote_write=NULL;return KERNEL_TASK_STATUS_OK;}
enum kernel_task_status kernel_task_socket_read_register(struct kernel_task *t, struct kernel_socket_read_request *r)
{ (void)t;(void)r;CHECK(reservation_actor);reservation_arrive();return KERNEL_TASK_STATUS_OK; }
enum kernel_task_status kernel_task_socket_read_clear(struct kernel_task *t, struct kernel_socket_read_request *r)
{ (void)t;(void)r;return KERNEL_TASK_STATUS_OK; }
void kernel_signal_note_syscall_restart(struct kernel_task *t) { (void)t; abort(); }
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t)
{ (void)q;(void)d;(void)i;CHECK(remote_write);*t=(struct kernel_wait_token){(void *)remote_write,1};return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{
    (void)t;CHECK(remote_testing && !kernel_cpu_current()->raw_locks);
    CHECK(!pthread_mutex_lock(&remote_gate));
    int stopping_actor=!kernel_cpu_current()->logical_id;
    if(stopping_actor) {stop_waiting=1;CHECK(!pthread_cond_broadcast(&remote_changed));}
    for(;;) {
        int pending;{SOCKET_STATE(state);pending=stopping_actor?remote_socket->active_writes!=NULL:remote_write->queued;}
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
    memcpy(out,(void *)(uintptr_t)in,*copied);
    if(remote_testing && remote_mode==3) {
        CHECK(!pthread_mutex_lock(&remote_gate));copy_waiting=1;
        CHECK(!pthread_cond_broadcast(&remote_changed));
        while(!copy_release)CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        CHECK(!pthread_mutex_unlock(&remote_gate));
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

static err_t physical_output(struct netif *interface, struct pbuf *packet, const ip4_addr_t *destination)
{ (void)destination; return link_output(interface, packet); }
static err_t physical_init(struct netif *interface)
{ interface->name[0] = 't'; interface->name[1] = 'e'; interface->mtu = 1500; interface->output = physical_output; return ERR_OK; }

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
    CHECK(!kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, &socket));
    socket->connected = 1; socket->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(socket->tcp->local_ip, local);
    IP_ADDR4(&socket->tcp->remote_ip, 10, 77, 0, 1);
    socket->tcp->local_port = 22000; socket->tcp->remote_port = 22001;
    socket->tcp->cwnd = TCP_MSS; socket->tcp->snd_wnd = socket->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(socket->tcp); tcp_bind_netif(socket->tcp, &network.interface);
    tcp_publish_capacity(socket);
    uint64_t frozen = clock_ns;
    CHECK(kernel_socket_write_buffer(socket, "a", 1, 0) == 1);
    CHECK(copied_packets == 1 && socket->tcp->unacked && !socket->tcp->unsent);
    CHECK(kernel_socket_write_buffer(socket, "b", 1, 0) == 1);
    CHECK(copied_packets == 1 && socket->tcp->unsent);
    CHECK(!kernel_socket_set_option(socket, KERNEL_SOCKET_NODELAY, 0));
    CHECK(copied_packets == 1 && socket->tcp->unsent);
    nic_full = blocked;
    CHECK(!kernel_socket_set_option(socket, KERNEL_SOCKET_NODELAY, 1));
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
    printf("PASS NODELAY pending output and %s without ACK or timer progress\n", blocked ? "NIC capacity retry" : "immediate transmission");
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
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, &socket) == 0);
    /* Establish the transport fixture without a simulated remote handshake;
     * all send admission, protocol output and NIC wait routing remain real. */
    socket->connected = 1; socket->tcp->state = ESTABLISHED;
    ip_addr_copy_from_ip4(socket->tcp->local_ip, local);
    IP_ADDR4(&socket->tcp->remote_ip, 10, 77, 0, 1);
    socket->tcp->local_port = 22000; socket->tcp->remote_port = 22001;
    socket->tcp->cwnd = TCP_MSS; socket->tcp->snd_wnd = socket->tcp->snd_wnd_max = TCP_WND;
    TCP_REG_ACTIVE(socket->tcp); tcp_bind_netif(socket->tcp, &network.interface);
    tcp_publish_capacity(socket);
    dma_eligible = eligible; nic_full = 1; copy_attempts = copied_packets = segment_attempts = 0;
    kernel_socket_protocol_leave(&irq);protocol_services=0;
    if (gather) {
        struct kernel_open_file_description *owner=(void *)socket;
        struct kernel_socket_write_request request={0};
        kernel_socket_stream_begin(&request,&owner);
        unsigned prefix=gather==2?2:5;
        CHECK(kernel_socket_stream_reserve(&request,prefix,0)==(int)prefix);
        CHECK(kernel_socket_stream_commit(&request,"owner",prefix,KERNEL_SOCKET_IO_MORE)==(int)prefix);
        CHECK(!copy_attempts && !segment_attempts && !protocol_services);
        if (gather==2) {
            CHECK(kernel_socket_stream_reserve(&request,3,0)==3);
            CHECK(kernel_socket_stream_commit(&request,"ner",3,0)==3);
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
                CHECK(kernel_socket_write_buffer(socket,"next",4,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ECONNRESET);
                CHECK(!socket->pending_error && protocol_services==1);
            }
        }
    } else {
        CHECK(kernel_socket_write_buffer(socket, "owner", 5, 0) == 5);
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

static void udp_final_payload(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *socket=0;
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,&socket));
    struct kernel_open_file_description *owner=(void *)socket;
    char first[]="alpha",second[]="beta";
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)first,5},{(uintptr_t)second,4}};
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=23001,.bytes={127,0,0,1}};
    unsigned before=heap_allocations;
    CHECK(kernel_socket_write_datagram(&owner,(void *)socket,iov,2,9,0,&address)==9);
    CHECK(owner==(void *)socket && heap_allocations==before);
    uint32_t memory=lwip_stats.mem.used;
    user_remaining=6;
    CHECK(kernel_socket_write_datagram(&owner,(void *)socket,iov,2,9,0,&address)==-KERNEL_EFAULT);
    CHECK(owner==(void *)socket && lwip_stats.mem.used==memory && !socket->tx_reserved);
    user_remaining=SIZE_MAX;
    kernel_socket_destroy(socket);
    puts("PASS UDP final payload: no transient kernel heap objects, gather usercopy and fault rollback");
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
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0)==9);
    CHECK(!shutdown_during_copy && sender->peer_closed && !receiver->read_closed &&
        owner==(void *)sender && heap_allocations==before+1 && receiver->rx_bytes==9);
    CHECK(!memcmp(receiver->packets_head->data,"alphabeta",9) && !sender->tx_reserved);
    uint64_t charged=sender->unix_memory;
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};user_remaining=6;
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0)==6);
    CHECK(receiver->packets_tail->length==6 && !memcmp(receiver->packets_tail->data,"alphab",6));
    CHECK(sender->unix_memory>charged && !sender->tx_reserved);user_remaining=SIZE_MAX;
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};close_during_copy=receiver;
    CHECK(kernel_socket_write_unix(&owner,(void *)sender,&cursor,9,KERNEL_SOCKET_MSG_DONTWAIT,0)==-KERNEL_EPIPE);
    CHECK(!close_during_copy && owner==(void *)sender && heap_live==1 &&
        !sender->tx_reserved && !sender->unix_memory && !sender->unix_packets && sender->pending_error==-KERNEL_ECONNRESET);
    kernel_socket_destroy(sender);
    puts("PASS UNIX final queue payload: half-close before/during send, gather fill, fault prefix and peer-close reservation rollback");
}

static void receive_payload(void)
{
    kernel_socket_network_initialize();
    struct kernel_socket *sender=0,*receiver=0;
    CHECK(!kernel_socket_pair(&test_heap,SOCKET_STREAM,&sender,&receiver));
    CHECK(kernel_socket_write_buffer(sender,"alphabeta",9,0)==9);
    struct kernel_open_file_description *owner=(void *)receiver;
    struct kernel_socket_read_request request={0};
    char first[2]={0},second[7]={0};
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)first,2},{(uintptr_t)second,7}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0)==9);
    user_remaining=6;
    CHECK(kernel_socket_read_user(&request,(void *)receiver,&cursor)==-KERNEL_EFAULT);
    kernel_socket_finish_read(&request,1);
    CHECK(receiver->rx_bytes==9 && owner==(void *)receiver);
    cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};user_remaining=SIZE_MAX;
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0)==9);
    CHECK(!kernel_socket_read_user(&request,(void *)receiver,&cursor));
    kernel_socket_finish_read(&request,0);
    CHECK(!memcmp(first,"al",2) && !memcmp(second,"phabeta",7) && !receiver->rx_bytes);
    kernel_socket_destroy(sender);kernel_socket_destroy(receiver);receiver=0;

    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,&receiver));
    struct pbuf *head=pbuf_alloc(PBUF_RAW,3,PBUF_RAM),*tail=pbuf_alloc(PBUF_RAW,6,PBUF_RAM);
    CHECK(head && tail && pbuf_take(head,"abc",3)==ERR_OK && pbuf_take(tail,"defghi",6)==ERR_OK);
    pbuf_cat(head,tail);
    ip_addr_t peer;IP_ADDR4(&peer,127,0,0,1);
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);udp_received(receiver,receiver->udp,head,&peer,23001);kernel_socket_protocol_leave(&irq);
    owner=(void *)receiver;cursor=(struct kernel_uaccess_iov_cursor){iov,2,0,0};
    CHECK(kernel_socket_reserve_read(receiver,0,&request,&owner,9,0)==9);
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
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, listener) == 0);
    CHECK(kernel_socket_bind(*listener, &address) == 0 && kernel_socket_listen(*listener, 4) == 0);
    CHECK(kernel_socket_getname(*listener, &address) == 0);
    CHECK(kernel_socket_create(&test_heap, KERNEL_SOCKET_AF_INET, SOCKET_STREAM, client) == 0);
    int result = kernel_socket_connect(*client, &address, 0);
    CHECK(result == 0 || result == -KERNEL_EINPROGRESS);
    for (unsigned i = 0; i < 8 && !(*client)->connected; ++i)
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 8, 0});
    CHECK((*client)->connected && kernel_socket_accept(*listener, server) == 0);
}

static struct tcp_pcb *close_to_timewait(struct kernel_socket *client, struct kernel_socket *server)
{
    /* Only protocol packets advance. No budget is given to deferred socket work. */
    CHECK(kernel_socket_shutdown(client, 1) == 0);
    netif_poll_all();
    CHECK(kernel_socket_shutdown(server, 1) == 0);
    netif_poll_all();
    CHECK(client->tcp && client->tcp->state == TIME_WAIT && client->work_links[ready_work.slot].queued);
    CHECK(!server->tcp && client->peer_closed);
    return client->tcp;
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
    CHECK(kernel_socket_write_buffer(sender,"sent",4,0)==4);
    CHECK(kernel_socket_write_buffer(receiver,"back",4,0)==4);
    struct kernel_open_file_description *owner=(void *)receiver;
    struct kernel_socket_read_request read={0};
    CHECK(kernel_socket_reserve_read(receiver,0,&read,&owner,4,0)==4 && !owner);
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
        CHECK(kernel_socket_write_buffer(sender,bytes,sizeof(bytes),0)==sizeof(bytes));
        if(reverse) {
            kernel_socket_destroy(receiver);
            CHECK(kernel_socket_write_buffer(sender,bytes,1,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ECONNREFUSED);
            CHECK(kernel_socket_write_buffer(sender,bytes,1,KERNEL_SOCKET_MSG_NOSIGNAL)==-KERNEL_ENOTCONN);
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

static void loopback_wake(void *context)
{ ((struct loopback_observation *)context)->wakes++; }

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
        kernel_socket_network_hooks(NULL, loopback_wake, NULL, &observation);
        struct kernel_socket_service_result service = kernel_socket_service_pending(
            (struct kernel_socket_service_budget){0, 8, 0});
        CHECK(!service.runnable && !work_pending_snapshot());

        /* 不运行 timer 或 inline service；真实入队必须发布可运行工作并请求唤醒。 */
        struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
        loopback_emit(&observation);
        kernel_socket_protocol_leave(&irq);
        CHECK(observation.error == ERR_OK && !observation.received);
        CHECK(work_pending_snapshot() && observation.wakes);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 1);

        /* 模拟 protocol timer 在本批 loopback 阶段之后入队；返回值仍须报告新工作。 */
        unsigned before_wakes = observation.wakes;
        loopback_prepare(&observation);
        sys_timeout(0, loopback_emit, &observation);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 1});
        CHECK(observation.error == ERR_OK && service.packets == 0 && service.timers == 1);
        CHECK(service.runnable && work_pending_snapshot() && observation.wakes > before_wakes);
        CHECK(observation.received == 1);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 2);
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
        CHECK(!work_pending_snapshot() && observation.received == 2);
        while (count) mem_free(held[--count]);
        CHECK(lwip_stats.mem.used == before_memory);
        loopback_prepare(&observation);
        kernel_socket_protocol_enter(&irq);
        loopback_emit(&observation);
        kernel_socket_protocol_leave(&irq);
        CHECK(observation.error == ERR_OK && work_pending_snapshot());
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 3);
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
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM,&listener));
    CHECK(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_STREAM,&client));
    int connected=kernel_socket_connect(client,&address,0);
    CHECK(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<4;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(!kernel_socket_accept(listener,&server));
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
    CHECK(kernel_socket_reserve_read(server,NULL,&read,&reader,1,0)==1);
    CHECK(acquired==locks && protocol_services==services);
    kernel_socket_finish_read(&read,0);
    CHECK(acquired==locks && protocol_services==services);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    locks=acquired;services=protocol_services;
    CHECK(kernel_socket_write_buffer(client,"y",1,0)==1);
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
    CHECK(kernel_socket_write_buffer(sender,"abc",3,0)==3);
    CHECK(kernel_socket_write_buffer(sender,"defg",4,0)==4);
    CHECK(kernel_socket_write_buffer(sender,"hijkl",5,0)==5);
    struct kernel_open_file_description *pin=(void *)receiver;
    struct kernel_socket_read_request request={0};
    unsigned locks=acquired;
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,10,0)==10);
    char a[2]={0},b[8]={0};
    struct kernel_uaccess_iovec iov[]={{(uintptr_t)a,2},{(uintptr_t)b,8}};
    struct kernel_uaccess_iov_cursor cursor={iov,2,0,0};
    CHECK(!kernel_socket_read_user(&request,(void *)receiver,&cursor));
    CHECK(acquired==locks && !memcmp(a,"ab",2) && !memcmp(b,"cdefghij",8));
    kernel_socket_finish_read(&request,0);
    CHECK(acquired==locks && receiver->rx_bytes==2);
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0)==2);
    struct kernel_open_file_description *other=(void *)receiver;
    struct kernel_socket_read_request second={0};
    CHECK(kernel_socket_reserve_read(receiver,NULL,&second,&other,32,1)==-KERNEL_EAGAIN && !second.socket);
    CHECK(kernel_socket_write_buffer(sender,"late",4,0)==4);
    kernel_socket_destroy(sender);sender=NULL;
    char tail[8]={0};kernel_socket_copy_read(&request,0,tail,2);
    CHECK(!memcmp(tail,"kl",2));kernel_socket_finish_read(&request,0);
    CHECK(receiver->rx_bytes==4 && receiver->peer_closed);
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0)==4);
    allow_ofd_release=1;kernel_socket_abort_read(&request);
    CHECK(!request.socket && !allow_ofd_release && receiver->rx_bytes==4 && !receiver->read_request);
    pin=(void *)receiver;
    CHECK(kernel_socket_reserve_read(receiver,NULL,&request,&pin,32,0)==4);
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
    CHECK(kernel_socket_reserve_read(server,NULL,&request,&pin,12,0)==12);
    user_remaining=7;
    CHECK(kernel_socket_read_user(&request,(void *)server,&cursor)==-KERNEL_EFAULT);
    CHECK(request.completed_bytes==4 && kernel_socket_finish_read(&request,1)==4 && server->rx_bytes==8);
    user_remaining=SIZE_MAX;
    char remaining[8]={0};struct kernel_uaccess_iovec tail_iov={(uintptr_t)remaining,8};
    cursor=(struct kernel_uaccess_iov_cursor){&tail_iov,1,0,0};
    CHECK(kernel_socket_reserve_read(server,NULL,&request,&pin,8,0)==8);
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
static void endpoint_ownership(void)
{
    kernel_socket_network_initialize();
    /* A consumed custom pbuf still owns its DMA-like payload until the core
     * worker, or final close, actually uses and returns it. */
    for (unsigned final_close = 0; final_close < 2; final_close++) {
        struct kernel_socket *socket = NULL;
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,&socket));
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
        CHECK(kernel_socket_reserve_read(socket,NULL,&read,&pin,4,0)==4);
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
    uint32_t available=tcp_admission_capacity(server);
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



static void remote_work_ready(void *context)
{
    CHECK(context==&remote_work && !kernel_cpu_current()->raw_locks);
    CHECK(!pthread_mutex_lock(&remote_gate));remote_work=1;
    CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
}
static void *remote_writer(void *argument)
{
    struct kernel_socket *socket=argument;kernel_cpu_current()->logical_id=1;
    struct kernel_open_file_description *pin=(void *)socket;
    remote_task=(void *)&test_context;
    if(!remote_mode || remote_mode==4) {
        struct kernel_socket_write_request request={0};
        kernel_socket_stream_begin(&request,&pin);
        CHECK(kernel_socket_stream_reserve(&request,1,0)==1);
        CHECK(kernel_socket_stream_commit(&request,"r",1,0)==(remote_mode==4?-KERNEL_EIO:1));
        kernel_socket_stream_finish(&request);
    } else {
        const struct kernel_uaccess_iovec iov={(uintptr_t)"r",1};
        int result=kernel_socket_write_datagram(&pin,(void *)socket,&iov,1,1,0,NULL);
        CHECK(result==(remote_mode==2?-KERNEL_EFAULT:remote_mode==3?-KERNEL_EIO:1));
    }
    CHECK(pin==(void *)socket && !remote_write);remote_task=NULL;
    CHECK(!pthread_mutex_lock(&remote_gate));remote_done=1;
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
    if(!mode || mode==4)connected_pair(&listener,&client,&server);
    else {
        struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24017,.bytes={127,0,0,1}};
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,&server));
        CHECK(!kernel_socket_bind(server,&address));
        CHECK(!kernel_socket_create(&test_heap,KERNEL_SOCKET_AF_INET,SOCKET_DGRAM,&client));
        CHECK(!kernel_socket_connect(client,&address,0));
        if(mode==2)user_remaining=0;
    }
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    kernel_socket_network_hooks(NULL,remote_work_ready,NULL,&remote_work);
    remote_socket=client;
    pthread_t thread,stopper;unsigned stop_started=0;
    CHECK(!pthread_create(&thread,NULL,remote_writer,client));
    for(;;) {
        CHECK(!pthread_mutex_lock(&remote_gate));
        while(!remote_work && !remote_done && !(mode==3 && copy_waiting && !stop_started))
            CHECK(!pthread_cond_wait(&remote_changed,&remote_gate));
        int done=remote_done;
        int start_stop=!stop_started && ((mode==3 && copy_waiting) || (mode==4 && remote_work));
        remote_work=0;CHECK(!pthread_mutex_unlock(&remote_gate));
        if(done)break;
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
        kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
        CHECK(!pthread_mutex_lock(&remote_gate));
        CHECK(!pthread_cond_broadcast(&remote_changed) && !pthread_mutex_unlock(&remote_gate));
    }
    CHECK(!pthread_join(thread,NULL));remote_write=NULL;
    if(stop_started) {CHECK(!pthread_join(stopper,NULL));CHECK(stop_done && !stop_borrows);}
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    CHECK(server->rx_bytes==(mode>=2?0:1) && !client->tx_reserved && !client->writes_head && !client->active_writes);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    if(client->tcp)tcp_abort(client->tcp);
    if(server->tcp)tcp_abort(server->tcp);
    kernel_socket_protocol_leave(&scope);
    kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    kernel_socket_destroy(client);kernel_socket_destroy(server);if(listener)kernel_socket_destroy(listener);
    for(unsigned i=0;i<8;i++)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    printf("PASS remote protocol %u: CPU1 copies/waits, CPU0 executes, request/OFD/payload returned\n",mode);
}

struct reservation_attempt {struct kernel_socket *socket;struct kernel_socket_read_request read;struct kernel_open_file_description *pin;int result;unsigned actor;};
static void *reserve_concurrently(void *argument)
{
    struct reservation_attempt *a=argument;reservation_actor=a->actor;
    a->pin=(void *)a->socket;
    a->result=kernel_socket_reserve_read(a->socket,(void *)a,&a->read,&a->pin,4,0);
    return NULL;
}
static void concurrent_reservation(void)
{
    struct kernel_socket socket={.heap=&test_heap,.type=SOCKET_STREAM,.domain=KERNEL_SOCKET_DOMAIN_UNIX,.rx_bytes=4};
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

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "remote-protocol")) remote_protocol_request(0);
    else if (!strcmp(argv[1], "remote-udp")) remote_protocol_request(1);
    else if (!strcmp(argv[1], "remote-udp-fault")) remote_protocol_request(2);
    else if (!strcmp(argv[1], "remote-udp-stop")) remote_protocol_request(3);
    else if (!strcmp(argv[1], "remote-copy-stop")) remote_protocol_request(4);
    else if (!strcmp(argv[1], "concurrent-reservation")) {concurrent_reservation();return 0;}
    else if (!strcmp(argv[1], "protocol-owner")) { protocol_ownership(); return 0; }
    else if (!strcmp(argv[1], "endpoint-owner")) endpoint_ownership();
    else if (!strcmp(argv[1], "receive-chains")) receive_packet_chains();
    else if (!strcmp(argv[1], "receive-prefix")) receive_prefix();
    else if (!strcmp(argv[1], "protocol-transaction")) protocol_transaction();
    else if (!strcmp(argv[1], "nodelay")) nodelay_pending(0);
    else if (!strcmp(argv[1], "nodelay-capacity")) nodelay_pending(1);
    else if (!strcmp(argv[1], "unix-payload")) unix_final_payload();
    else if (!strcmp(argv[1], "receive-payload")) receive_payload();
    else if (!strcmp(argv[1], "udp-payload")) udp_final_payload();
    else if (!strcmp(argv[1], "loopback-work")) loopback_work();
    else if (!strcmp(argv[1], "unix-sender")) unix_sender_owner();
    else if (!strcmp(argv[1], "tcp-batch")) copy_capacity(0,1);
    else if (!strcmp(argv[1], "tcp-batch-last")) copy_capacity(0,2);
    else if (!strcmp(argv[1], "tcp-batch-abort")) copy_capacity(0,3);
    else if (!strcmp(argv[1], "tcp-batch-error")) copy_capacity(0,4);
    else if (!strcmp(argv[1], "copy-indirect")) copy_capacity(1,0);
    else if (!strcmp(argv[1], "copy-ineligible")) copy_capacity(0,0);
    else if (!strcmp(argv[1], "timewait-capacity")) timewait_owner(0);
    else if (!strcmp(argv[1], "timewait-timer")) timewait_owner(1);
    else if (!strcmp(argv[1], "controls")) { timewait_owner(2); raw_owner_control(); }
    else return 2;
    CHECK(!test_context.allocation_depth && !protocol_depth);
    CHECK(!heap_live && !tcp_active_pcbs && !tcp_tw_pcbs && !tcp_listen_pcbs.pcbs);
    CHECK(!lwip_stats.memp[MEMP_TCP_PCB]->used && !lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used &&
          !lwip_stats.memp[MEMP_TCP_SEG]->used && !lwip_stats.mem.used);
    return 0;
}
