/* 实际换栈由客户机验证；本测试直接使用生产资格索引及执行owner交接。 */
#define main controlled_smp_main
#define SMP_TEST_USE_LIFETIME
#include "smp_sched.c"
#undef main
#include <arch/timer.h>
#include <kernel/time.h>
/* This scheduler/standard-signal model has no active timerfd. */
uint64_t kernel_timer_fd_next;

#ifdef SOCKET_PROTOCOL_CASES
#include "../../net/socket.c"
unsigned boaros_lwip_parallel;
void boaros_lwip_ip_init(unsigned cpus) { assert(cpus==1); }
void boaros_lwip_ip_lock_shared(struct kernel_lock_guard *guard)
{ (void)guard; assert(!"COPY must not acquire IP qualification"); }
void boaros_lwip_ip_unlock(struct kernel_lock_guard *guard)
{ (void)guard; assert(!"unexpected IP qualification"); }
/* Unrelated protocol branches are linked through the same dispatcher. */
#define UNUSED_SOCKET_API(result, name, ...) result name(__VA_ARGS__) { __builtin_trap(); }
UNUSED_SOCKET_API(struct kernel_vfs_mount *, kernel_vfs_path_mount, const struct kernel_vfs_path *)
UNUSED_SOCKET_API(uint64_t, kernel_vfs_path_inode, const struct kernel_vfs_path *)
UNUSED_SOCKET_API(int, kernel_vfs_path_release, struct kernel_vfs_path **)
UNUSED_SOCKET_API(struct kernel_socket *, kernel_open_file_socket, struct kernel_open_file_description *)
UNUSED_SOCKET_API(enum kernel_open_file_status, kernel_open_file_release, struct kernel_open_file_description **)
UNUSED_SOCKET_API(void, tcp_recved, struct tcp_pcb *, u16_t)
UNUSED_SOCKET_API(void, tcp_set_receive_limit, struct tcp_pcb *, u32_t)
UNUSED_SOCKET_API(err_t, boaros_lwip_tcp_preparev, const struct kernel_socket_write_segment *,
    unsigned, u16_t, u16_t, struct tcp_seg **)
UNUSED_SOCKET_API(err_t, boaros_lwip_tcp_publishv, struct tcp_pcb *, struct tcp_seg **,
    u16_t, u8_t, u16_t *)
UNUSED_SOCKET_API(void, tcp_segs_free, struct tcp_seg *)
UNUSED_SOCKET_API(err_t, tcp_process_refused_data, struct tcp_pcb *)
UNUSED_SOCKET_API(void, tcp_abort, struct tcp_pcb *)
UNUSED_SOCKET_API(err_t, tcp_close, struct tcp_pcb *)
UNUSED_SOCKET_API(void, tcp_arg, struct tcp_pcb *, void *)
UNUSED_SOCKET_API(void, tcp_accept, struct tcp_pcb *, tcp_accept_fn)
UNUSED_SOCKET_API(void, tcp_err, struct tcp_pcb *, tcp_err_fn)
UNUSED_SOCKET_API(void, tcp_recv, struct tcp_pcb *, tcp_recv_fn)
UNUSED_SOCKET_API(void, tcp_sent, struct tcp_pcb *, tcp_sent_fn)
UNUSED_SOCKET_API(void, tcp_poll, struct tcp_pcb *, tcp_poll_fn, u8_t)
UNUSED_SOCKET_API(void, udp_remove, struct udp_pcb *)
UNUSED_SOCKET_API(err_t, udp_send, struct udp_pcb *, struct pbuf *)
UNUSED_SOCKET_API(err_t, udp_sendto, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t)
UNUSED_SOCKET_API(struct pbuf *, pbuf_alloc, pbuf_layer, u16_t, pbuf_type)
UNUSED_SOCKET_API(u32_t, sys_now, void)
UNUSED_SOCKET_API(u32_t, sys_timeouts_sleeptime, void)
UNUSED_SOCKET_API(unsigned, sys_check_timeouts_budget, unsigned)
UNUSED_SOCKET_API(unsigned, netif_poll_budget, struct netif *, unsigned)
#undef UNUSED_SOCKET_API
struct netif *netif_list;
void kernel_assert_service_context(void)
{ assert(!active->logical_id && (!active->current || !active->current->controlled)); }
static struct kernel_socket_protocol_scope shard_worker_scope;
static struct kernel_socket_write_request *shard_queued_writer;
static unsigned shard_copy_cpu, shard_copies, shard_drains, shard_outputs;
static char shard_order[4];
static struct tcp_seg shard_unsent;
static struct pbuf *shard_loop_source;
static struct pbuf shard_loop_clone;
static unsigned shard_forbid_route, shard_revoke_loop, shard_clone_freed;
u16_t boaros_lwip_tcp_detached_mss(const struct tcp_pcb *pcb) { return pcb->mss; }
u16_t lwip_htons(u16_t x) { return (u16_t)((x<<8)|(x>>8)); }
u8_t pbuf_free(struct pbuf *p)
{
    assert(p==&shard_loop_clone && shard_revoke_loop && !active->raw_locks);
    shard_clone_freed++;
    return 1;
}
struct pbuf *pbuf_clone_shared(pbuf_layer layer, struct pbuf *p, u16_t head)
{
    assert(layer==PBUF_RAW && p==shard_loop_source && head==TCP_HLEN && !active->raw_locks);
    if (shard_revoke_loop) {
        struct kernel_cpu *source=active;
        active=&cpus[0];
        fast_loopback_revoke(fast_loopback);
        active=source;
    }
    return &shard_loop_clone;
}

err_t boaros_lwip_tcp_writev(struct tcp_pcb *pcb, const struct kernel_socket_write_segment *segments,
    unsigned count, u16_t length, u8_t flags, u16_t *accepted)
{
    assert(active->logical_id==shard_copy_cpu && current_tcp_owner()==tcp_owner_at(1));
    assert(!protocol_current_owner() && !active->raw_locks && !tasks[0].user_service);
    assert(count==1 && segments[0].size==1 && length==1 && (flags&TCP_WRITE_FLAG_COPY));
    assert(shard_copies<sizeof(shard_order) && pcb->snd_buf);
    shard_order[shard_copies++]=*(const char *)segments[0].data;
    pcb->snd_buf--; pcb->unsent=&shard_unsent; *accepted=length;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *pcb)
{
    assert(active==&cpus[1] && current_tcp_owner()==tcp_owner_at(1));
    assert(!active->raw_locks && pcb->unsent==&shard_unsent);
    pcb->unsent=0; shard_outputs++;
    return ERR_OK;
}

static void shard_drain_queued_writer(void)
{
    struct kernel_socket_write_request *writer=shard_queued_writer;
    struct socket_tcp_owner *owner=tcp_owner_at(1);
    int held=shard_worker_scope.outer;
    uintptr_t irq=sync_test_irq;
    shard_queued_writer=0;
    assert(active==&cpus[0] && active->current==&tasks[0]);
    assert(!active->raw_locks && !current_tcp_owner() && !tasks[0].io_context.locks);
    assert(kernel_wait_record_phase(&tasks[0].wait)==KERNEL_WAIT_PREPARED);
    /* Release the endpoint guard before letting the delayed shard owner run. */
    active=&cpus[1]; sync_test_irq=1; shard_copy_cpu=1;
    if (!held) assert(tcp_owner_lock(&shard_worker_scope,owner,1));
    assert(service_ready_socket(2,&owner->ready,&shard_worker_scope)==(held?1U:2U));
    assert(!writer->queued);
    kernel_socket_protocol_leave(&shard_worker_scope);
    shard_worker_scope.outer=0;
    assert(!current_tcp_owner() && !active->raw_locks);
    active=&cpus[0]; sync_test_irq=irq;
    shard_drains++;
}

static void socket_shard_copy_cases(void)
{
    tasks[0].cpu_mask=1;
    assert(!kernel_scheduler_yield_current() && active==&cpus[0]);
    assert(cpus[1].current==&idles[1]);
    scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
    struct socket_tcp_owner *owner=tcp_owner_at(1);
    owner->index=1;
    kernel_raw_lock_init(&owner->ingress,KERNEL_RAW_RANK_OBJECT+1);
    kernel_mutex_init(&owner->mutex,26,(uintptr_t)&owner->mutex);
    kernel_wait_queue_init(&owner->work);
    tcp_owner_count=1; boaros_lwip_parallel=1;
    struct kernel_wait_queue work;
    kernel_wait_queue_init(&work); network_work_queue=&work;
    struct tcp_pcb pcb={.state=ESTABLISHED,.snd_buf=TCP_SND_BUF};
    struct kernel_socket socket={.tcp=&pcb,.domain=KERNEL_SOCKET_DOMAIN_INET,.type=SOCKET_STREAM,
        .connected=1,.tx_limit=TCP_SND_BUF,.tx_budget=TCP_SND_BUF,.tcp_cpu=1};
    kernel_wait_queue_init(&socket.wait);
    struct kernel_open_file_description *pin=(void *)&socket;
    for (unsigned mode=0; mode<3; mode++) {
        struct kernel_socket_write_request writer={.socket=&socket,.pin=pin,.reserved=1};
        struct kernel_socket_write_segment segment={"c",1};
        struct kernel_socket_write_request older={.socket=&socket,.pin=pin,.reserved=1,
            .segments=&segment,.segment_count=1,.operation=SOCKET_PROTOCOL_STREAM,.queued=1};
        socket.tx_reserved=1;
        if (mode) {
            uintptr_t irq=sync_test_irq;
            active=&cpus[1]; sync_test_irq=1;
            if (mode==1) assert(tcp_owner_lock(&shard_worker_scope,owner,1));
            else {
                tasks[1]=(struct kernel_task){.cpu=active,.state=KERNEL_THREAD_STATE_RUNNING,.wait.on_cpu=1};
                active->current=&tasks[1];
                assert(!kernel_wait_prepare(0,0,0,&older.completion));
                active->current=&idles[1];
            }
            active=&cpus[0]; sync_test_irq=irq;
            if (mode==2) {
                SOCKET_STATE(state);
                socket.tx_reserved++;
                socket.writes_head=socket.writes_tail=&older;
                work_add_locked(&owner->ready,&socket,0);
            }
            shard_queued_writer=&writer;
        }
        shard_copy_cpu=0;
        assert(kernel_socket_stream_commit(&writer,"abd"+mode,1,0)==1);
        assert(shard_drains==mode && !shard_queued_writer && !writer.queued && !writer.segments &&
            !writer.reserved && !writer.completion.task && !socket.tx_reserved && !socket.writes_head &&
            !socket.writes_tail && !current_tcp_owner() && !tasks[0].io_context.locks);
        assert(shard_outputs==mode);
        if (!mode) {
            assert(owner->ready.head==&socket && socket.work_flags==SOCKET_WORK_OUTPUT);
            { SOCKET_STATE(state); work_remove(&owner->ready,&socket); socket.work_flags=0; }
        } else assert(!owner->ready.head && !socket.work_flags);
        if (mode==2) {
            uintptr_t irq=sync_test_irq;
            active=&cpus[1]; sync_test_irq=1; active->current=&tasks[1];
            assert(!older.queued && !older.reserved && !older.segments && older.result==1);
            assert(!kernel_wait_finish(&older.completion));
            active->current=&idles[1]; active=&cpus[0]; sync_test_irq=irq;
        }
    }
    assert(shard_copies==sizeof(shard_order));
    for (unsigned i=0;i<shard_copies;i++) assert(shard_order[i]=="abcd"[i]);
    assert(socket.tx_budget==TCP_SND_BUF-4 && pcb.snd_buf==TCP_SND_BUF-4);
    network_work_queue=0;
    puts("PASS TCP shard COPY: CPU0 copies under CPU1 owner without IP/output; busy and FIFO fallback complete on CPU1");
}

static void socket_shard_loopback_case(void)
{
    struct socket_tcp_owner *owner=tcp_owner_at(1);
    struct kernel_socket_protocol_scope scope;
    assert(tcp_owner_lock(&scope,owner,1));
    struct tcp_pcb tx={.boaros_loopback=1,.boaros_owner=1,.local_port=11,.remote_port=22};
    struct tcp_pcb rx={.boaros_loopback=1,.boaros_owner=1,.local_port=22,.remote_port=11};
    ip_addr_set_ip4_u32(&tx.local_ip,PP_HTONL(UINT32_C(0x7f000001)));
    ip_addr_copy(tx.remote_ip,tx.local_ip);
    ip_addr_copy(rx.remote_ip,tx.local_ip);
    ip_addr_copy(rx.local_ip,tx.remote_ip);
    unsigned bucket=tcp_route_bucket(&tx);
    { KERNEL_RAW_SCOPE(route,&tcp_route_lock);
      tcp_routes[bucket]=&rx;
      tcp_link_loopback_peer(&tx,bucket);
      tx.boaros_route_next=&rx;
      tcp_routes[bucket]=&tx; }
    assert(tx.boaros_loopback_peer==&rx && rx.boaros_loopback_peer==&tx);
    struct netif loop={0};fast_loopback=&loop;fast_loopback_state=LOOPBACK_AVAILABLE;
    struct tcp_hdr header={.src=lwip_htons(11),.dest=lwip_htons(22)};
    TCPH_HDRLEN_FLAGS_SET(&header,5,TCP_ACK);
    struct pbuf packet={.payload=&header,.len=TCP_HLEN};
    shard_loop_source=&packet;shard_forbid_route=1;
    int result;
    assert(tcp_local_output(&tx,&packet,&loop,&tx.local_ip,&tx.remote_ip,&result) && result==ERR_OK);
    struct socket_tcp_input queued;
    assert(tcp_owner_take_input(owner,&queued) && queued.packet==&shard_loop_clone && queued.interface==&loop);
    shard_revoke_loop=1;
    assert(tcp_local_output(&tx,&packet,&loop,&tx.local_ip,&tx.remote_ip,&result) && result==ERR_RTE);
    assert(!owner->output_reserved && shard_clone_freed==1 && !tcp_owner_take_input(owner,&queued));
    shard_revoke_loop=0;
    fast_loopback_state|=LOOPBACK_AVAILABLE;
    shard_forbid_route=0;
    tcp_pcb_forget(&rx);
    assert(!tx.boaros_loopback_peer && !rx.boaros_loopback_peer && !rx.boaros_owner);
    assert(!tcp_local_output(&tx,&packet,&loop,&tx.local_ip,&tx.remote_ip,&result));
    tcp_pcb_forget(&tx);
    assert(!tcp_routes[bucket]);
    fast_loopback=0;fast_loopback_state=0;shard_loop_source=0;
    kernel_socket_protocol_leave(&scope);
    puts("PASS TCP paired loopback: independent ingress, reservation rollback after interface revoke, peer retirement");
}

static int protocol_preempt,protocol_try_failure;
int __real_kernel_rwlock_try_write(struct kernel_rwlock *,struct kernel_lock_guard *);
int __wrap_kernel_rwlock_try_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (protocol_try_failure && lock==&protocol_mutex.lock) {
        protocol_try_failure=0;
        assert(tasks[0].user_service && active==&cpus[0]);
        return 0;
    }
    return __real_kernel_rwlock_try_write(lock,guard);
}
void __real_kernel_rwlock_write(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (protocol_preempt && lock==&protocol_mutex.lock) {
        protocol_preempt=0;
        /* mutex取得前的定时器交错；资格必须已发布，不能迁往用户CPU1。 */
        assert(!kernel_scheduler_yield_current());
        assert(active==&cpus[0] && active->current==&tasks[0] && tasks[0].user_service);
    }
    __real_kernel_rwlock_write(lock,guard);
}
static void socket_protocol_cases(void)
{
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(0,1,0);tasks[0].arch.user_mode=1;
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[0]);
      kernel_wait_switch_finish_locked(&idles[0]);active->switch_previous=0; }
    tasks[0].cpu_mask=2;
    protocol_preempt=1;
    struct kernel_socket_protocol_scope outer,nested;
    kernel_socket_protocol_enter(&outer);
    assert(!protocol_preempt && tasks[0].user_service && protocol_depth==1);
    assert(!kernel_scheduler_yield_current() && active==&cpus[0] && tasks[0].user_service);
    kernel_socket_protocol_enter(&nested);
    assert(protocol_depth==2 && !nested.outer && tasks[0].user_service);
    kernel_socket_protocol_leave(&nested);
    assert(protocol_depth==1 && tasks[0].user_service);
    kernel_socket_protocol_leave(&outer);
    assert(!protocol_depth && !protocol_owner && !tasks[0].io_context.locks &&
        !tasks[0].io_context.allocation_depth && !tasks[0].user_service && active==&cpus[1]);
    puts("PASS socket protocol: qualification survives deterministic preemption and nested borrow; mutex/allocation/service owners returned");
    struct kernel_socket_protocol_scope missed;
    assert(!protocol_enter(&missed,1,1) && !tasks[0].user_service && !protocol_depth && !protocol_owner);
    tasks[0].cpu_mask=1;
    assert(!kernel_scheduler_yield_current() && active==&cpus[0]);
    int previously_local=!active->logical_id;
    tasks[0].cpu_mask=2;
    assert(!kernel_scheduler_yield_current() && active==&cpus[1]);
    /* 模拟旧local判断后抢占迁移：实际进入必须重查，不在CPU1运行协议。 */
    assert(previously_local && !protocol_enter(&missed,1,1) && !tasks[0].user_service);
    puts("PASS socket protocol: migration after local observation rejects inline entry without acquiring owners");
    tasks[0].cpu_mask=1;
    assert(!kernel_scheduler_yield_current() && active==&cpus[0]);
    tasks[0].cpu_mask=2;
    struct kernel_mm_user_pin pin={0};
    tasks[0].io_context.user_pins=&pin;
    protocol_try_failure=1;
    uintptr_t interrupts=sync_test_irq;
    assert(!protocol_enter(&missed,1,1) && !protocol_try_failure);
    assert(active==&cpus[1] && !tasks[0].user_service && !protocol_owner &&
        !protocol_depth && !tasks[0].io_context.locks && !tasks[0].io_context.allocation_depth &&
        tasks[0].io_context.user_pins==&pin && sync_test_irq==interrupts);
    tasks[0].cpu_mask=1;
    assert(!kernel_scheduler_yield_current() && active==&cpus[0]);
    tasks[0].cpu_mask=2;
    assert(protocol_enter(&outer,1,1) && tasks[0].user_service && active==&cpus[0]);
    kernel_socket_protocol_leave(&outer);
    assert(active==&cpus[1] && tasks[0].io_context.user_pins==&pin && !tasks[0].user_service &&
        !tasks[0].io_context.locks && !tasks[0].io_context.allocation_depth);
    tasks[0].io_context.user_pins=0;
    puts("PASS socket protocol: CPU0 local pin debt keeps inline execution; try failure and leave restore service/IRQ without consuming pin");
    /* 既有外层业务服务范围由原owner结束，协议嵌套不能消费它。 */
    kernel_scheduler_enter_user_service();
    kernel_socket_protocol_enter(&outer);kernel_socket_protocol_leave(&outer);
    assert(tasks[0].user_service && active==&cpus[0]);
    kernel_scheduler_leave_user_service();
    assert(active==&cpus[1] && !tasks[0].user_service);
    puts("PASS socket protocol: borrowed pre-existing service qualification remains with its original owner");

}
#endif

extern void scheduler_user_service_set_locked(struct kernel_task *, int);
extern void scheduler_user_request_locked(struct kernel_task *);
static int cancel_stop_on_handoff, local_rotation_active, local_run_required, forbid_cpu0_run;
static const struct kernel_wait_token *local_park_token;
static unsigned local_park_case, local_park_switched;
enum kernel_scheduler_status __real_kernel_scheduler_wake_signal(struct kernel_task *);

static void local_park_wake(void)
{
    struct kernel_cpu *saved=active;
    active=&cpus[0];
    if (local_park_case==5) {
        {
            KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
            local_park_token->task->signal_pending=UINT64_C(1)<<9;
        }
        assert(!__real_kernel_scheduler_wake_signal(local_park_token->task));
    } else if (local_park_case==4) {
        KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
        assert(kernel_wait_notify_locked(local_park_token,KERNEL_WAIT_WOKEN));
    } else assert(!kernel_wait_notify(local_park_token,KERNEL_WAIT_WOKEN));
    active=saved;
}

void user_signal_cancel_stop(struct kernel_task *);
void user_signal_retarget_case(struct kernel_task *,struct kernel_task *);
void user_signal_take_retargeted(struct kernel_task *);
void user_signal_stop_selection(struct kernel_task *);
struct kernel_task *process_find_identity(kernel_pid_t pid, enum kernel_pid_role role)
{ (void)role; for (unsigned i=0; i<4; i++) if (tasks[i].tid==pid) return &tasks[i]; return 0; }
struct kernel_task *kernel_task_current(void) { return active->current; }
void process_identity_commit_locked(struct kernel_task *task)
{ (void)task;assert(!"host actors already own published identities"); }
struct kernel_cpu *riscv_smp_cpu(unsigned id) { assert(id<2);return &cpus[id]; }
enum kernel_scheduler_status scheduler_switch_current_away(struct kernel_task *task)
{ (void)task;assert(!"unexpected wait in the host signal transition");return KERNEL_SCHEDULER_STATUS_INVALID_STATE; }
enum kernel_scheduler_status scheduler_switch_current_away_locked(struct kernel_task *task,
    struct kernel_raw_guard *handoff)
{
    assert(handoff && handoff->lock==&kernel_wait_domain);
    kernel_raw_lock_release(handoff);
    return scheduler_switch_current_away(task);
}
uint32_t kernel_time_is_initialized(void) { return 0; }
uint64_t kernel_time_ticks_to_ns(uint64_t ticks) { (void)ticks;assert(!kernel_time_is_initialized());return 0; }
uint64_t riscv_time_read(void) { return 1; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *ticks)
{ (void)ns; (void)ticks; __builtin_trap(); }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_scheduler_status validate_queues_locked(void) { __builtin_trap(); }
static int scheduler_finish_local(struct kernel_cpu *cpu)
{
    KERNEL_RAW_SCOPE(run,kernel_run_lock(cpu));
    return scheduler_finish_local_locked(cpu);
}
/* 唯一模拟的是机器换栈：生产select/finish决定owner及两侧资格。 */
enum kernel_scheduler_status scheduler_switch_claimed(struct kernel_task *old,struct kernel_task *next)
{
    if (local_park_token) {
        assert(active==&cpus[1] && next==active->idle && old==local_park_token->task);
        assert(old->state==KERNEL_THREAD_STATE_BLOCKED && old->wait.on_cpu &&
            !old->ready_node.queued);
        local_park_switched++;
        if (local_park_case==3 || local_park_case==5) {
            local_park_wake();
            assert(old->wait.ready_pending && !old->ready_node.queued);
        }
        assert(scheduler_finish_local(active));
        if (local_park_case==4) local_park_wake();
        assert_ready_owner(old,active);
        struct kernel_task *resumed;
        uint64_t deadline;
        assert(scheduler_select_local(0,0,0,&resumed,&deadline) && resumed==old);
        assert(scheduler_finish_local(active));
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (local_rotation_active) {
        assert(next!=active->idle && old->wait.on_cpu && !old->ready_node.queued && !old->ready_cpu);
        if (!old->idle) assert(old->wait.ready_flags==SCHED_READY_LOCAL);
        if (local_run_required) {
            assert(scheduler_finish_local(active));
            local_run_required=0;
        } else {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            kernel_wait_switch_finish_locked(old);active->switch_previous=0;
        }
        if (!old->idle) assert_ready_owner(old,active);
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    assert(next==active->idle && old->wait.on_cpu && !old->ready_node.queued);
    /* 普通服务强制迁移也排在目标核已有工作之后，不能反复插队。 */
    if (old->scheduling.policy==KERNEL_SCHED_OTHER) assert(!old->wait.ready_flags);
    unsigned other=active->logical_id^1;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_wait_switch_finish_locked(old); active->switch_previous=0;
    }
    active=&cpus[other];
    if(cancel_stop_on_handoff && !active->logical_id){
        cancel_stop_on_handoff=0;
        user_signal_cancel_stop(old);
    }
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task *previous=active->current;
        assert(scheduler_select_next_locked(active,previous,1)==old);
        kernel_wait_switch_finish_locked(previous); active->switch_previous=0;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
/* 本模型省略存储/canary校验，实际切换仍由生产调度决策及机器seam交接。 */
enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{ return scheduler_reschedule(0,1,0); }
static unsigned borrow_quiescing, borrow_ended, quiesce_calls;
void __real_kernel_wait_backend_quiesce(void);
void __wrap_kernel_wait_backend_quiesce(void)
{
    assert(active==&cpus[0] && active->current->user_service);
    quiesce_calls++;
    __atomic_store_n(&borrow_quiescing,1,__ATOMIC_RELEASE);
    __real_kernel_wait_backend_quiesce();
    while (!__atomic_load_n(&borrow_ended,__ATOMIC_ACQUIRE)) {}
}
static void *end_service_borrow(void *unused)
{
    (void)unused; active=&cpus[1]; sync_test_irq=1;
    while (!__atomic_load_n(&borrow_quiescing,__ATOMIC_ACQUIRE)) {}
    kernel_wait_task_unpin(&tasks[0]);
    __atomic_store_n(&borrow_ended,1,__ATOMIC_RELEASE);
    return 0;
}
static unsigned stop_armed, stop_guard_left, stop_claimed;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
#ifdef SOCKET_PROTOCOL_CASES
    assert(!shard_forbid_route || (lock!=&tcp_route_lock && lock!=&socket_state_lock));
#endif
    assert(!local_run_required || lock!=&kernel_wait_domain);
    if (local_park_token && active==&cpus[1]) {
        assert(local_park_case==6 || lock!=&kernel_wait_domain);
        if (local_park_case==1 && lock==kernel_run_lock(active) &&
            kernel_wait_record_phase(&local_park_token->task->wait)==KERNEL_WAIT_PREPARED)
            local_park_wake();
    }
    assert(!forbid_cpu0_run || lock!=kernel_run_lock(&cpus[0]));
    __real_kernel_raw_lock_acquire(lock,guard);
}
void __real_kernel_raw_lock_release(struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    int pause=stop_armed && guard->lock==&kernel_wait_domain;
#ifdef SOCKET_PROTOCOL_CASES
    int drain=shard_queued_writer && guard->lock==&socket_state_lock &&
        shard_queued_writer->completion.task;
#endif
    if (pause) stop_armed=0;
    int early=local_park_token && local_park_case==2 && active==&cpus[1] &&
        guard->lock==kernel_run_lock(active) &&
        kernel_wait_record_phase(&local_park_token->task->wait)==KERNEL_WAIT_COMMITTED;
    __real_kernel_raw_lock_release(guard);
    if (early) local_park_wake();
#ifdef SOCKET_PROTOCOL_CASES
    if (drain) shard_drain_queued_writer();
#endif
    if (pause) {
        __atomic_store_n(&stop_guard_left,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&stop_claimed,__ATOMIC_ACQUIRE)) {}
    }
}
extern void user_stop_case(struct kernel_task *);
static void *claim_during_stop(void *unused)
{
    (void)unused; active=&cpus[1]; sync_test_irq=1;
    while (!__atomic_load_n(&stop_guard_left,__ATOMIC_ACQUIRE)) {}
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[3]);
      kernel_wait_switch_finish_locked(&idles[1]); active->switch_previous=0; }
    __atomic_store_n(&stop_claimed,1,__ATOMIC_RELEASE);
    return 0;
}
static void *late_service_notice(void *unused)
{
    (void)unused; active=&publishers[0]; sync_test_irq=1;
    int result=pthread_barrier_wait(&gate);
    assert(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); scheduler_user_request_locked(&tasks[0]); }
    return 0;
}

static void reservation_placement_cases(void)
{
    reset_two_queues();active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      tasks[2]=(struct kernel_task){.accounted=1,.arch.user_mode=1,.cpu_mask=2,
          .cpu=&cpus[1],.state=KERNEL_THREAD_STATE_RUNNING,.wait.on_cpu=1};
      tasks[2].scheduling.priority=60;
      cpus[1].current=&tasks[2];idles[1].wait.on_cpu=0;
      scheduler_user_service_set_locked(&tasks[2],1);
      tasks[2].state=KERNEL_THREAD_STATE_READY;tasks[2].wait.ready_pending=1;
      assert(scheduler_select_next_locked(active,&tasks[2],1)==&idles[1]);
      kernel_wait_switch_finish_locked(&tasks[2]);active->switch_previous=0;
      assert(tasks[2].reservation_cpu==&cpus[1] && tasks[2].ready_cpu==&cpus[0]); }
    active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      tasks[3]=(struct kernel_task){.accounted=1,.cpu=&cpus[0],
          .state=KERNEL_THREAD_STATE_RUNNING,.wait.on_cpu=1};
      cpus[0].current=&tasks[3];idles[0].wait.on_cpu=0;
      kernel_cpu_consume_schedule(&cpus[0]);kernel_cpu_consume_schedule(&cpus[1]);
      tasks[0]=(struct kernel_task){.accounted=1,.arch.user_mode=1,.cpu_mask=3,
          .state=KERNEL_THREAD_STATE_READY};
      tasks[0].scheduling.priority=40;
      /* CPU1虽idle，其RT60保留阻止RT40执行。 */
      ready_enqueue_locked(&tasks[0],0);
      assert_ready_owner(&tasks[0],&cpus[0]);
      assert(kernel_cpu_schedule_requested(&cpus[0]) && !kernel_cpu_schedule_requested(&cpus[1]));
      tasks[1]=(struct kernel_task){.accounted=1,.arch.user_mode=1,.cpu_mask=2,
          .state=KERNEL_THREAD_STATE_READY};
      tasks[1].scheduling.priority=40;ready_enqueue_locked(&tasks[1],0);
      assert_ready_owner(&tasks[1],&cpus[1]); }
    active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(!ready_best_locked());
      kernel_cpu_consume_schedule(&cpus[1]);
      scheduler_user_reservation_release_locked(&tasks[2]);
      assert(kernel_cpu_schedule_requested(&cpus[1]) && ready_best_locked()==&tasks[1]);
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[1]);
      kernel_wait_switch_finish_locked(&idles[1]);active->switch_previous=0;
      assert(tasks[1].wait.on_cpu && !tasks[1].ready_cpu);
      ready_remove_locked(&tasks[0]);ready_remove_locked(&tasks[2]); }
    puts("PASS reservation placement: prefer legal executor; fully blocked affinity queues until real reservation release");
}

static void local_fifo_cases(void)
{
    for (unsigned policy=KERNEL_SCHED_FIFO;policy<=KERNEL_SCHED_RR;policy++) {
        reset_two_queues();active=&cpus[0];
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          setup_task(0,1,40);setup_task(1,1,40);
          assert(scheduler_select_next_locked(active,active->current,1)==&tasks[0]);
          kernel_wait_switch_finish_locked(&idles[0]);active->switch_previous=0; }
        /* 片尾时另一核完全空闲；旧路径重新放置会把被轮转者移走。 */
        tasks[0].cpu_mask=3;
        tasks[0].scheduling.policy=policy;
        if (policy==KERNEL_SCHED_RR) tasks[0].scheduling.rr_remaining_ns=0;
        local_rotation_active=1;
        assert(!scheduler_reschedule(0,policy==KERNEL_SCHED_FIFO,0));
        local_rotation_active=0;
        assert(active==&cpus[0] && active->current==&tasks[1] && !cpus[1].runqueue.count);
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          assert_ready_owner(&tasks[0],&cpus[0]);
          assert(ready_best_locked()==&tasks[0]); }
        if (policy==KERNEL_SCHED_RR)
            assert(tasks[0].scheduling.rr_remaining_ns==KERNEL_SCHED_RR_NS);
    }
    puts("PASS FIFO yield and RR expiration: real decision/finish retain local FIFO despite an idle peer");
}

static void ordinary_local_claim(void)
{
    reset_two_queues();active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(0,2,0);setup_task(1,2,0);
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[0]);
      kernel_wait_switch_finish_locked(&idles[1]);active->switch_previous=0; }
    local_rotation_active=local_run_required=1;
    assert(!scheduler_reschedule(0,1,0));
    local_rotation_active=0;
    assert(!local_run_required && active->current==&tasks[1]);
    assert_ready_owner(&tasks[0],active);
}

static void ordinary_local_park(void)
{
    for (local_park_case=1;local_park_case<=6;local_park_case++) {
        reset_two_queues();active=&cpus[1];
        scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
        {
            KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
            setup_task(0,2,0);
            if (local_park_case>=5) tasks[0].arch.user_mode=1;
            if (local_park_case==6) tasks[0].signal_pending=UINT64_C(1)<<9;
            assert(scheduler_select_next_locked(active,active->idle,1)==&tasks[0]);
            kernel_wait_switch_finish_locked(&idles[1]);active->switch_previous=0;
        }
        struct kernel_wait_queue queue;
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        kernel_wait_queue_init(&queue);
        assert(!kernel_wait_prepare(&queue,0,local_park_case>=5,&token));
        local_park_switched=0;local_park_token=&token;
        assert(!kernel_wait_park(&token,&reason));
        local_park_token=NULL;
        assert(local_park_switched==(local_park_case>=3 && local_park_case<=5));
        assert(reason==(local_park_case>=5?KERNEL_WAIT_SIGNALLED:KERNEL_WAIT_WOKEN) && active->current==&tasks[0] &&
            tasks[0].state==KERNEL_THREAD_STATE_RUNNING && tasks[0].wait.on_cpu);
        assert(!tasks[0].wait.ready_pending && !tasks[0].ready_node.queued && !active->switch_previous);
        assert(!kernel_wait_finish(&token));
        assert(!kernel_wait_queue_close(&queue) && !kernel_wait_queue_destroy(&queue));
    }
    puts("PASS local park: early notification, old stack, locked notifier and interruptible waits");
}

static void ordinary_local_wake(void)
{
    for (unsigned scenario=0;scenario<4;scenario++) {
        unsigned timed=scenario/2, after=scenario%2;
        uint64_t deadline=timed?1000:0;
        reset_two_queues();active=&cpus[1];
        scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
        struct kernel_wait_token token;
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          setup_task(0,2,0);
          assert(scheduler_select_next_locked(active,active->idle,1)==&tasks[0]);
          kernel_wait_switch_finish_locked(&idles[1]);active->switch_previous=0; }
        assert(!kernel_wait_prepare(0,deadline,1,&token));
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          kernel_wait_record_transition(&tasks[0].wait,token.generation,KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
          kernel_wait_backend_commit(&tasks[0],deadline,1);
          assert(scheduler_select_next_locked(active,&tasks[0],1)==active->idle); }
        local_run_required=!timed;
        if (after) assert(scheduler_finish_local(active));
        active=&cpus[0];
        forbid_cpu0_run=1;
        assert(!kernel_wait_notify(&token,timed?KERNEL_WAIT_TIMEOUT:KERNEL_WAIT_WOKEN));
        forbid_cpu0_run=0;
        assert(!kernel_wait_notify(&token,KERNEL_WAIT_SIGNALLED));
        assert(kernel_wait_record_reason(&tasks[0].wait)==(timed?KERNEL_WAIT_TIMEOUT:KERNEL_WAIT_WOKEN));
        assert(!tasks[0].deadline_indexed && !scheduler.deadline_root);
        active=&cpus[1];
        if (!after) {
            assert(tasks[0].wait.on_cpu && tasks[0].wait.ready_pending && !tasks[0].ready_node.queued);
            assert(scheduler_finish_local(active));
        }
        local_run_required=0;
        assert_ready_owner(&tasks[0],active);
    }
    puts("PASS ordinary wakes (including timeout) and old-stack release use only the destination queue");
}

static void ordinary_wake_after_affinity_change(void)
{
    for (unsigned after=0;after<2;after++) {
        reset_two_queues();active=&cpus[1];
        scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
        struct kernel_wait_token token;
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          setup_task(0,2,0);tasks[0].tid=99;tasks[0].tid_owned=1;tasks[0].arch.user_mode=1;
          assert(scheduler_select_next_locked(active,active->idle,1)==&tasks[0]);
          kernel_wait_switch_finish_locked(&idles[1]);active->switch_previous=0; }
        assert(!kernel_wait_prepare(0,0,1,&token));
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          kernel_wait_record_transition(&tasks[0].wait,token.generation,KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
          kernel_wait_backend_commit(&tasks[0],0,1);
          assert(scheduler_select_next_locked(active,&tasks[0],1)==active->idle); }
        if (after) assert(scheduler_finish_local(active));
        active=&cpus[0];
        assert(!kernel_task_affinity_set(active->current,99,1));
        assert(!kernel_wait_notify(&token,KERNEL_WAIT_WOKEN));
        if (!after) {
            assert(tasks[0].wait.on_cpu && tasks[0].wait.ready_pending && !tasks[0].ready_node.queued);
            active=&cpus[1];
            assert(!scheduler_finish_local(active));
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            KERNEL_RUN_SET(runs);
            kernel_wait_switch_finish_locked(&tasks[0]);active->switch_previous=0;
        }
        assert(!cpus[1].runqueue.count);
        assert_ready_owner(&tasks[0],&cpus[0]);
    }
    puts("PASS ordinary wake after affinity change uses the new queue and respects the old stack");
}

static void ordinary_pull(void)
{
    for (unsigned history=0;history<2;history++) {
        reset_two_queues();active=&cpus[0];
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          setup_task(0,3,0);tasks[0].rttime_ticks=history;
          assert_ready_owner(&tasks[0],&cpus[0]); }
        active=&cpus[1];
        local_rotation_active=1;local_run_required=!history;
        assert(!scheduler_reschedule(0,0,0));
        local_rotation_active=0;
        assert(!local_run_required && !cpus[0].runqueue.count && !tasks[0].ready_cpu &&
               tasks[0].cpu==active && tasks[0].wait.on_cpu && active->current==&tasks[0]);
    }
    puts("PASS ordinary steal transfers execution directly under source/destination runqueue locks");
}

static void remote_rt_precedence(void)
{
    reset_two_queues();active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      for (unsigned i=0;i<2;i++) {
          tasks[i]=(struct kernel_task){.accounted=1,.cpu_mask=3,.cpu=&cpus[0],
              .state=KERNEL_THREAD_STATE_READY};
          tasks[i].scheduling.priority=40;
          ready_enqueue_locked(&tasks[i],SCHED_READY_LOCAL);
      } }
    active=&cpus[1];
    struct kernel_task *selected=0;
    uint64_t deadline=0;
    assert(!scheduler_select_local(0,0,0,&selected,&deadline));
    for (unsigned i=0;i<2;i++) {
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);ready_remove_locked(&tasks[i]); }
        assert(scheduler_select_local(0,0,0,&selected,&deadline)==(i==1));
        assert(!selected);
    }
    puts("PASS remote RT work defers ordinary selection until the last eligible task leaves");
}

int main(int argc,char **argv)
{
    (void)argv;
    if (argc==2) {kernel_wait_domain_init();ordinary_local_park();return 0;}
    assert(!controlled_smp_main());
    ordinary_local_park();
    local_fifo_cases();
    ordinary_local_claim();
    ordinary_local_wake();
    ordinary_wake_after_affinity_change();
    ordinary_pull();
    remote_rt_precedence();
    reservation_placement_cases();
    scheduler = (struct kernel_scheduler){0};
    scheduler.rt_configuration.period_ns = 1000000000;
    scheduler.rt_configuration.runtime_ns = 950000000;
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], i ? 19 : 7, 0);
        cpus[i].logical_id = i;
        cpus[i].state = KERNEL_CPU_ONLINE;
        active = &cpus[i];
        idles[i] = (struct kernel_task){.state = KERNEL_THREAD_STATE_IDLE};
        idles[i].wait.on_cpu = 1;
        cpus[i].current = &idles[i];
        scheduler_cpu_initialize(active, &idles[i], 1);
    }
    active = &cpus[0];
#ifdef SOCKET_PROTOCOL_CASES
    kernel_cpu_set_boot_count(1);
    socket_protocol_cases();socket_shard_copy_cases();socket_shard_loopback_case();return 0;
#endif
    {
        struct kernel_task worker={.accounted=1,.cpu_mask=1,.state=KERNEL_THREAD_STATE_READY};
        struct kernel_task services[2];
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for (unsigned i=0;i<2;i++) {
            services[i]=(struct kernel_task){.accounted=1,.cpu_mask=3,.arch.user_mode=1,
                .user_service=1,.state=KERNEL_THREAD_STATE_READY};
            ready_enqueue_locked(&services[i],0);
        }
        ready_enqueue_locked(&worker,0);
        for (unsigned i=0;i<2;i++) {
            assert(ready_best_locked()==&services[i]);
            ready_remove_locked(&services[i]);
            ready_enqueue_locked(&services[i],0);
        }
        assert(ready_best_locked()==&worker);
        ready_remove_locked(&worker);
        for (unsigned i=0;i<2;i++) ready_remove_locked(&services[i]);
        puts("PASS ordinary CPU0 services share FIFO eligibility with the worker they may wait for");
    }
    tasks[0] = (struct kernel_task){.accounted = 1, .cpu_mask = 2,
        .arch.user_mode = 1, .tid_owned=1, .tid=7, .state = KERNEL_THREAD_STATE_READY};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_user_service_set_locked(&tasks[0], 1);
        ready_enqueue_locked(&tasks[0], 0);
        assert(ready_best_locked() == &tasks[0]);
        assert(tasks[0].cpu_mask == 2); /* 临时服务资格不能改写请求affinity。 */
    }
    uint64_t mask;
    assert(!kernel_task_affinity_get(&tasks[0],0,&mask) && mask==2);
    assert(kernel_task_affinity_set(&tasks[0],0,4)==-22);
    active = &cpus[1];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!ready_best_locked());
        scheduler_user_service_set_locked(&tasks[0], 0);
        assert(ready_best_locked() == &tasks[0]);
        selected[1] = scheduler_select_next_locked(active, active->current, 1);
        assert(selected[1] == &tasks[0]);
        kernel_wait_switch_finish_locked(&idles[1]);
        cpus[1].switch_previous = 0;
        scheduler_user_request_locked(&tasks[0]);
    }
    assert(tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        scheduler_signal_request_locked(&tasks[0]);
    }
    assert(kernel_scheduler_user_return_pending());
    assert(kernel_scheduler_take_signal_work());
    assert(!kernel_scheduler_take_signal_work() && (tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0));
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_user_service_set_locked(&tasks[0], 1);
        tasks[0].state = KERNEL_THREAD_STATE_READY;
        tasks[0].wait.ready_pending = 1;
        selected[1] = scheduler_select_next_locked(active, &tasks[0], 1);
        assert(selected[1] == &idles[1]);
        assert(!tasks[0].ready_node.queued && tasks[0].wait.on_cpu);
    }
    active = &cpus[0];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!ready_best_locked()); /* 旧栈尚未完成：另一核不能取得执行资格。 */
    }
    active = &cpus[1];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_wait_switch_finish_locked(&tasks[0]);
        cpus[1].switch_previous = 0;
    }
    active = &cpus[0];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(ready_best_locked() == &tasks[0]);
        assert(tasks[0].ready_cpu==&cpus[0] && tasks[0].cpu_mask == 2);
    }
    /* 通知在CPU0消费旧位之后到达；恢复资格不能清掉这个新发布。 */
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); scheduler_user_service_set_locked(&tasks[0],0); }
    active=&cpus[1];
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(scheduler_select_next_locked(active,active->current,1)==&tasks[0]);
        kernel_wait_switch_finish_locked(&idles[1]); active->switch_previous=0;
    }
    kernel_scheduler_enter_user_service();
    assert(active==&cpus[0] && (tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0));
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); scheduler_signal_request_locked(&tasks[0]); }
    kernel_scheduler_ack_user_service();
    assert(!(tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0) && kernel_scheduler_user_return_pending());
    assert(kernel_scheduler_take_signal_work() && !kernel_scheduler_user_return_pending());
    assert(!pthread_barrier_init(&gate,0,2)); pthread_t notifier;
    assert(!pthread_create(&notifier,0,late_service_notice,0));
    int barrier=pthread_barrier_wait(&gate);
    assert(!barrier || barrier==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(notifier,0)); pthread_barrier_destroy(&gate);
    kernel_scheduler_leave_user_service();
    assert(active==&cpus[1] && (tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0));
    kernel_scheduler_enter_user_service();
    assert(active==&cpus[0] && (tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0));
    kernel_scheduler_ack_user_service();
    assert(!(tasks[0].user_return_pending & KERNEL_USER_RETURN_CPU0));
    assert(active==&cpus[0] && active->current==&tasks[0] && tasks[0].user_service && tasks[0].cpu_mask==2);
    puts("PASS local signal notices and CPU0 requests consume only their own work, preserving concurrent publications");
    /* 实际任务临时服务不能把来源CPU的FIFO运行资格让给同级任务。 */
    assert(!kernel_sched_policy_set(&tasks[0].scheduling,KERNEL_SCHED_FIFO,40,0));
    kernel_scheduler_leave_user_service();
    assert(active==&cpus[1]);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(1,2,40); tasks[1].arch.user_mode=1; }
    kernel_scheduler_enter_user_service();
    assert(active==&cpus[0]);
    active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); assert(!ready_best_locked()); }
    active=&cpus[0];
    /* 更高实时用户可越过低优先级保留者；它也进入服务后，两份
     * 逻辑资格同时存活，来源核不能运行同级/低级等待者。 */
    active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(2,2,60); tasks[2].arch.user_mode=1;
      assert(ready_best_locked()==&tasks[2]);
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[2]);
      kernel_wait_switch_finish_locked(&idles[1]); active->switch_previous=0;
      scheduler_user_service_set_locked(&tasks[2],1);
      assert(tasks[0].reservation_node.queued && tasks[2].reservation_node.queued);
      assert(!ready_best_locked());
      /* 模型手动完成CPU1->CPU0的机器尾部；生产finish结束旧栈owner。 */
      tasks[2].state=KERNEL_THREAD_STATE_READY; tasks[2].wait.ready_pending=1;
      assert(scheduler_select_next_locked(active,&tasks[2],1)==&idles[1]);
      kernel_wait_switch_finish_locked(&tasks[2]); active->switch_previous=0; }
    active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      /* 即使CPU0另有更高优先级USER任务，服务索引先接纳真实调用栈。 */
      setup_task(3,1,99); tasks[3].arch.user_mode=1;
      assert(ready_best_locked()==&tasks[2]);
      ready_remove_locked(&tasks[2]);
      scheduler_user_reservation_release_locked(&tasks[2]);
      assert(!tasks[2].reservation_node.queued && tasks[0].reservation_node.queued);
      scheduler_user_reservation_release_locked(&tasks[0]); }
    active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(ready_best_locked()==&tasks[1]);
      ready_remove_locked(&tasks[1]); ready_remove_locked(&tasks[3]);
      assert(!cpus[0].user_reservations.first && !cpus[1].user_reservations.first &&
             !scheduler.rt_services.first && !cpus[0].runqueue.first && !cpus[1].runqueue.first); }
    active=&cpus[0];
    scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
    struct kernel_wait_token borrowed_token;
    assert(!kernel_wait_prepare(0,0,0,&borrowed_token));
    kernel_wait_task_pin(&tasks[0]);
    pthread_t borrower; assert(!pthread_create(&borrower,0,end_service_borrow,0));
    assert(!kernel_wait_finish(&borrowed_token));
    assert(!pthread_join(borrower,0) && quiesce_calls==1 && !tasks[0].wait.borrows);
    assert(!borrowed_token.task && kernel_wait_record_phase(&tasks[0].wait)==KERNEL_WAIT_FINISHED);
    active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(3,2,60); tasks[3].arch.user_mode=1; tasks[3].magic=KERNEL_THREAD_MAGIC; }
    pthread_t stopper; assert(!pthread_create(&stopper,0,claim_during_stop,0));
    stop_armed=1; user_stop_case(&tasks[3]);
    assert(!pthread_join(stopper,0) && stop_claimed);
    assert(tasks[3].state==KERNEL_THREAD_STATE_RUNNING && tasks[3].wait.on_cpu);
    assert(tasks[3].signal_pending==(UINT64_C(1)<<18) && (tasks[3].user_return_pending & KERNEL_USER_RETURN_CPU0));
    assert(!tasks[3].ready_node.queued);
    active=&cpus[1];
    tasks[2]=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,
        .state=KERNEL_THREAD_STATE_RUNNING,.accounted=1,.cpu_mask=1};
    assert(!kernel_sched_policy_set(&tasks[2].scheduling,KERNEL_SCHED_OTHER,0,0));
    user_signal_retarget_case(&tasks[3],&tasks[2]);
    active=&cpus[0];
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task *previous=active->current;
        assert(scheduler_select_next_locked(active,previous,1)==&tasks[2]);
        kernel_wait_switch_finish_locked(previous);active->switch_previous=0;
    }
    user_signal_take_retargeted(&tasks[2]);
    struct kernel_wait_token delivered={&tasks[2],kernel_wait_record_generation(&tasks[2].wait)};
    assert(!kernel_wait_finish(&delivered));
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        tasks[2].state=KERNEL_THREAD_STATE_EXITED;
        assert(scheduler_select_next_locked(active,&tasks[2],1)!=&tasks[2]);
        kernel_wait_switch_finish_locked(&tasks[2]);active->switch_previous=0;
    }
    active=&cpus[1];cancel_stop_on_handoff=1;
    user_signal_stop_selection(&tasks[3]);
    assert(!cancel_stop_on_handoff && active==&cpus[1] && active->current==&tasks[3]);
    assert(!tasks[3].storage_borrows && !tasks[2].storage_borrows &&
           kernel_wait_record_phase(&tasks[2].wait)==KERNEL_WAIT_FINISHED &&
           kernel_wait_record_phase(&tasks[3].wait)==KERNEL_WAIT_FINISHED && !scheduler.stopped_head);
    puts("user SMP service eligibility and old-stack execution owner passed");
    return 0;
}
