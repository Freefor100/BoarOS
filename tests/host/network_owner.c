/* Actual socket/ethernet/lwIP paths. Host heap, clock, empty wait queues and NIC
 * capacity are boundary models; TCP ownership and work queues are production. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/random.h>
#include "../../net/socket.c"
#include "../../net/ethernet.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
static struct kernel_heap test_heap;
static struct kernel_io_context test_context;
static unsigned heap_live, dma_eligible, nic_full, copy_attempts, copied_packets, segment_attempts, captured_size;
static uint64_t clock_ns = 1000000000ULL;
static unsigned char captured[1536];

struct kernel_io_context *kernel_io_context_current(void) { return &test_context; }
struct kernel_task *kernel_task_current(void) { return NULL; }
enum kernel_signal_status kernel_signal_send_task(struct kernel_task *task, uint32_t signal, kernel_pid_t sender)
{ (void)task; (void)signal; (void)sender; abort(); }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t n, size_t size, void **out)
{ CHECK(heap == &test_heap); *out = calloc(n, size); CHECK(*out); ++heap_live; return KERNEL_HEAP_STATUS_OK; }
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *heap, size_t size, void **out)
{ return kernel_heap_allocate_zeroed(heap, 1, size, out); }
enum kernel_heap_status kernel_heap_release(struct kernel_heap *heap, void *allocation)
{ CHECK(heap == &test_heap && allocation && heap_live); --heap_live; free(allocation); return KERNEL_HEAP_STATUS_OK; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ *queue = (struct kernel_wait_queue){.initialized = 1}; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ CHECK(queue->initialized && !queue->head); return KERNEL_SCHEDULER_STATUS_OK; }
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

static err_t physical_output(struct netif *interface, struct pbuf *packet, const ip4_addr_t *destination)
{ (void)destination; return link_output(interface, packet); }
static err_t physical_init(struct netif *interface)
{ interface->name[0] = 't'; interface->name[1] = 'e'; interface->mtu = 1500; interface->output = physical_output; return ERR_OK; }

static void copy_capacity(unsigned eligible)
{
    uintptr_t irq = kernel_socket_protocol_enter();
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
    dma_eligible = eligible; nic_full = 1; copy_attempts = copied_packets = segment_attempts = 0;
    CHECK(kernel_socket_write_buffer(socket, "owner", 5, 0) == 5);
    CHECK(socket->tcp->unsent && copy_attempts == 1 && segment_attempts == eligible && !copied_packets);
    kernel_socket_protocol_leave(irq);
    uint64_t generation = pool_generation, before_time = clock_ns;
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    CHECK(copy_attempts == 1 && !copied_packets);
    nic_full = 0; kernel_socket_network_capacity();
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    CHECK(copied_packets == 1 && !socket->tcp->unsent);
    CHECK(pool_generation == generation && clock_ns == before_time);
    CHECK(captured_size >= 5 && !memcmp(captured + captured_size - 5, "owner", 5));
    irq = kernel_socket_protocol_enter();
    tcp_abort(socket->tcp); kernel_socket_destroy(socket);
    netif_remove(&network.interface);
    kernel_socket_protocol_leave(irq);
    CHECK(!heap_live);
    printf("PASS copy fallback %s: NIC capacity alone retries real TCP output\n", eligible ? "no indirect" : "SG unsuitable");
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
    uintptr_t irq = kernel_socket_protocol_enter();
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
    kernel_socket_protocol_leave(irq);
    CHECK(!heap_live && !tcp_tw_pcbs);
    printf("PASS TIME_WAIT owner %s without deferred socket service\n", mode == 0 ? "capacity reclaim" : mode == 1 ? "timer expiry" : "already destroyed");
}

static void raw_error(void *context, err_t error) { (void)context; (void)error; abort(); }
static void raw_owner_control(void)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    unsigned marker = 17;
    for (unsigned with_arg = 0; with_arg < 2; ++with_arg) {
        struct tcp_pcb *pcb = tcp_new(); CHECK(pcb);
        tcp_arg(pcb, with_arg ? &marker : NULL); tcp_err(pcb, raw_error);
        pcb->state = TIME_WAIT; TCP_REG(&tcp_tw_pcbs, pcb);
        tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(irq);
    CHECK(marker == 17 && !heap_live);
    puts("PASS unowned raw TIME_WAIT PCBs do not invoke a socket owner");
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
        CHECK(!service.runnable && !kernel_socket_work_pending());

        /* 不运行 timer 或 inline service；真实入队必须发布可运行工作并请求唤醒。 */
        uintptr_t irq = kernel_socket_protocol_enter();
        loopback_emit(&observation);
        kernel_socket_protocol_leave(irq);
        CHECK(observation.error == ERR_OK && !observation.received);
        CHECK(kernel_socket_work_pending() && observation.wakes);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 1);

        /* 模拟 protocol timer 在本批 loopback 阶段之后入队；返回值仍须报告新工作。 */
        unsigned before_wakes = observation.wakes;
        loopback_prepare(&observation);
        sys_timeout(0, loopback_emit, &observation);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 1});
        CHECK(observation.error == ERR_OK && service.packets == 0 && service.timers == 1);
        CHECK(service.runnable && kernel_socket_work_pending() && observation.wakes > before_wakes);
        CHECK(observation.received == 1);
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 2);
        CHECK(!service.runnable && !kernel_socket_work_pending());

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
        irq = kernel_socket_protocol_enter();
        loopback_emit(&observation);
        kernel_socket_protocol_leave(irq);
        CHECK(observation.error == ERR_MEM && observation.wakes == before_wakes);
        CHECK(!kernel_socket_work_pending() && observation.received == 2);
        while (count) mem_free(held[--count]);
        CHECK(lwip_stats.mem.used == before_memory);
        loopback_prepare(&observation);
        irq = kernel_socket_protocol_enter();
        loopback_emit(&observation);
        kernel_socket_protocol_leave(irq);
        CHECK(observation.error == ERR_OK && kernel_socket_work_pending());
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){0, 1, 0});
        CHECK(service.packets == 1 && !service.timers && observation.received == 3);
        udp_remove(observation.sender); udp_remove(receiver);
        kernel_socket_network_hooks(NULL, NULL, NULL, NULL);
        CHECK(clock_ns == frozen && lwip_stats.mem.used == initial_memory);
        CHECK(lwip_stats.memp[MEMP_UDP_PCB]->used == initial_udp);
        printf("PASS IPv%u loopback work publication, late enqueue and OOM rollback with frozen clock\n", ipv6 ? 6 : 4);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "loopback-work")) loopback_work();
    else if (!strcmp(argv[1], "unix-sender")) unix_sender_owner();
    else if (!strcmp(argv[1], "copy-indirect")) copy_capacity(1);
    else if (!strcmp(argv[1], "copy-ineligible")) copy_capacity(0);
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
