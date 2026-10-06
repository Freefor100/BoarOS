#include <kernel/random.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include <stddef.h>
#include <stdint.h>

static unsigned int received;
static unsigned char byte_received;
static unsigned int tcp_connected;
static struct tcp_pcb *tcp_accepted;
static uint64_t simulated_ns = UINT64_C(1000000000);

static void counted_timeout(void *context)
{
    unsigned *count = context;
    if (++*count < 3) sys_timeout(0, counted_timeout, context);
}

static err_t connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)context;
    (void)pcb;
    if (error == ERR_OK) {
        tcp_connected++;
    }
    return ERR_OK;
}

static err_t accepted(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)context;
    if (error == ERR_OK) {
        tcp_accepted = pcb;
    }
    return ERR_OK;
}

uint64_t kernel_time_monotonic_ns(void)
{
    return simulated_ns;
}

int kernel_random_available(void)
{
    return 1;
}

enum kernel_random_status kernel_random_fill(void *buffer, size_t size)
{
    if (size != sizeof(uint32_t)) {
        return KERNEL_RANDOM_STATUS_INVALID_ARGUMENT;
    }
    *(uint32_t *)buffer = UINT32_C(0x33445566);
    return KERNEL_RANDOM_STATUS_OK;
}

static void receive(void *context, struct udp_pcb *pcb, struct pbuf *packet,
                    const ip_addr_t *peer, u16_t port)
{
    (void)context;
    (void)pcb;
    (void)peer;
    (void)port;
    if (packet->tot_len == 1U &&
        pbuf_copy_partial(packet, &byte_received, 1U, 0U) == 1U) {
        received++;
    }
    pbuf_free(packet);
}

int main(void)
{
    ip_addr_t loopback;
    struct udp_pcb *listener;
    struct udp_pcb *sender;
    struct pbuf *payload;
    struct udp_pcb *at_limit[MEMP_NUM_UDP_PCB];
    u16_t udp_before;
    u16_t pbuf_before;
    struct tcp_pcb *tcp_listener;
    struct tcp_pcb *tcp_client;
    err_t tcp_error;
    u16_t tcp_before;
    u16_t listen_before;
    u16_t segment_before;
    void *held_segments[MEMP_NUM_TCP_SEG];
    unsigned int held_count = 0;

    lwip_init();
    udp_before = lwip_stats.memp[MEMP_UDP_PCB]->used;
    pbuf_before = lwip_stats.memp[MEMP_PBUF]->used;
    listener = udp_new();
    sender = udp_new();
    if (listener == 0 || sender == 0) {
        return 1;
    }
    IP_ADDR4(&loopback, 127, 0, 0, 1);
    if (udp_bind(listener, &loopback, 0U) != ERR_OK ||
        listener->local_port == 0U) {
        return 2;
    }
    udp_recv(listener, receive, 0);
    payload = pbuf_alloc(PBUF_TRANSPORT, 1U, PBUF_RAM);
    if (payload == 0 || pbuf_take(payload, "x", 1U) != ERR_OK) {
        return 3;
    }
    if (udp_sendto(sender, payload, &loopback, listener->local_port) !=
        ERR_OK) {
        return 4;
    }
    pbuf_free(payload);
    netif_poll_all();
    if (received != 1U || byte_received != 'x') {
        return 5;
    }
    struct netif *loop = netif_list;
    while (loop && !(loop->name[0] == 'l' && loop->name[1] == 'o')) loop = loop->next;
    if (!loop) return 40;
    for (unsigned i = 0; i < 3; i++) {
        payload = pbuf_alloc(PBUF_TRANSPORT, 1U, PBUF_RAM);
        if (!payload || pbuf_take(payload, "x", 1) != ERR_OK ||
            udp_sendto(sender, payload, &loopback, listener->local_port) != ERR_OK) return 41;
        pbuf_free(payload);
    }
    if (netif_poll_budget(loop, 0) != 0 || received != 1 ||
        netif_poll_budget(loop, 1) != 1 || received != 2 ||
        netif_poll_budget(loop, 2) != 2 || received != 4 || loop->loop_first) return 42;
    unsigned expired = 0;
    sys_timeout(0, counted_timeout, &expired);
    if (sys_check_timeouts_budget(0) != 0 || expired ||
        sys_check_timeouts_budget(1) != 1 || expired != 1 ||
        sys_check_timeouts_budget(2) != 2 || expired != 3) return 43;
    udp_remove(sender);
    udp_remove(listener);
    for (unsigned int i = 0; i < MEMP_NUM_UDP_PCB; i++) {
        at_limit[i] = udp_new();
        if (at_limit[i] == 0) {
            return 6;
        }
    }
    if (udp_new() != 0 ||
        lwip_stats.memp[MEMP_UDP_PCB]->used !=
            (uint32_t)udp_before + MEMP_NUM_UDP_PCB) {
        return 7;
    }
    for (unsigned int i = 0; i < MEMP_NUM_UDP_PCB; i++) {
        udp_remove(at_limit[i]);
    }
    sender = udp_new();
    if (sender == 0) {
        return 8;
    }
    udp_remove(sender);
    if (lwip_stats.memp[MEMP_UDP_PCB]->used != udp_before ||
        lwip_stats.memp[MEMP_PBUF]->used != pbuf_before ||
        lwip_stats.mem.used != 0U) {
        return 9;
    }
    tcp_before = lwip_stats.memp[MEMP_TCP_PCB]->used;
    listen_before = lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used;
    segment_before = lwip_stats.memp[MEMP_TCP_SEG]->used;
    tcp_listener = tcp_new();
    tcp_client = tcp_new();
    if (tcp_listener == 0 || tcp_client == 0 ||
        tcp_bind(tcp_listener, &loopback, 0U) != ERR_OK) {
        return 10;
    }
    u16_t listen_port = tcp_listener->local_port;
    tcp_listener = tcp_listen_with_backlog_and_err(tcp_listener, 1U,
                                                   &tcp_error);
    if (tcp_listener == 0 || tcp_error != ERR_OK) {
        return 11;
    }
    tcp_accept(tcp_listener, accepted);
    if (tcp_connect(tcp_client, &loopback, listen_port, connected) !=
        ERR_OK) {
        return 12;
    }
    netif_poll_all();
    if (tcp_connected != 1U || tcp_accepted == 0) {
        return 13;
    }
    /* A different connection can exhaust the global segment pool while this
     * connected writer still has sndbuf. This is the ERR_MEM path that must
     * not appear as POLLOUT until a retry can make progress. */
    while (held_count < MEMP_NUM_TCP_SEG &&
           (held_segments[held_count] = memp_malloc(MEMP_TCP_SEG)) != 0) {
        held_count++;
    }
    if (held_count == 0U || tcp_sndbuf(tcp_client) == 0U ||
        tcp_write(tcp_client, "x", 1U, TCP_WRITE_FLAG_COPY) != ERR_MEM) {
        return 16;
    }
    for (unsigned int i = 0; i < held_count; i++) {
        memp_free(MEMP_TCP_SEG, held_segments[i]);
    }
    if (tcp_write(tcp_client, "x", 1U, TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_output(tcp_client) != ERR_OK) {
        return 17;
    }
    if (tcp_close(tcp_accepted) != ERR_OK ||
        tcp_close(tcp_client) != ERR_OK ||
        tcp_close(tcp_listener) != ERR_OK) {
        return 14;
    }
    /* FIN and TIME_WAIT may legitimately retain static PCBs. Their callbacks
     * have no heap owner, and the periodic protocol clock eventually frees them. */
    for (unsigned int tick = 0; tick < 800U; tick++) {
        simulated_ns += UINT64_C(250000000);
        netif_poll_all();
        sys_check_timeouts();
    }
    if (lwip_stats.memp[MEMP_TCP_PCB]->used != tcp_before ||
        lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used != listen_before ||
        lwip_stats.memp[MEMP_TCP_SEG]->used != segment_before) {
        return 15;
    }
    return 0;
}
