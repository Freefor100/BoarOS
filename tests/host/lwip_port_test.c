#include <kernel/random.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include <stddef.h>
#include <stdint.h>
#include "lwip_shared_payload.h"
#include "lwip_receive_policy.h"

static unsigned int received;
static unsigned char byte_received;
static unsigned int tcp_connected;
static struct tcp_pcb *tcp_accepted;
static unsigned int bulk_received;
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

static err_t receive_bulk(void *context, struct tcp_pcb *pcb, struct pbuf *packet, err_t error)
{
    (void)context;
    if (!packet) return ERR_OK;
    if (error != ERR_OK) __builtin_trap();
    for (u16_t i = 0; i < packet->tot_len; i++, bulk_received++)
        if (pbuf_get_at(packet, i) != (unsigned char)bulk_received) __builtin_trap();
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
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

static int tcp_vector_prefix(struct tcp_pcb *sender, struct tcp_pcb *receiver, struct netif *loop)
{
    const u16_t original_mss=sender->mss;
    const tcpwnd_size_t original_room=sender->snd_buf;
    const u32_t original_sequence=sender->snd_lbb;
    unsigned char seed[3]={0,1,2}, first[173], middle[227], last[800];
    static unsigned char scalar[1200];
    const struct kernel_socket_write_segment vector[]={
        {first,sizeof(first)}, {middle,sizeof(middle)}, {last,sizeof(last)}
    };
    unsigned char *const pieces[]={first,middle,last};
    unsigned offset=sizeof(seed);
    for(unsigned i=0;i<3;i++) for(unsigned j=0;j<vector[i].size;j++)
        pieces[i][j]=(unsigned char)offset++;

    sender->mss=512;
    if(tcp_write(sender,seed,sizeof(seed),TCP_WRITE_FLAG_COPY|TCP_WRITE_FLAG_MORE)!=ERR_OK ||
       !sender->unsent || sender->unsent->next || sender->unsent->len!=sizeof(seed)) return 47;
    struct tcp_seg *tail=sender->unsent;
    const u16_t tail_length=tail->p->tot_len;
    void *held[MEMP_NUM_TCP_SEG];unsigned count=0;
    while(count<MEMP_NUM_TCP_SEG && (held[count]=memp_malloc(MEMP_TCP_SEG))) count++;
    if(!count) return 48;
    memp_free(MEMP_TCP_SEG,held[--count]);

    /* One segment is available: scalar must roll back old tail and new queue. */
    if(tcp_write(sender,scalar,sizeof(scalar),TCP_WRITE_FLAG_COPY)!=ERR_MEM ||
       sender->unsent!=tail || tail->next || tail->len!=sizeof(seed) ||
       tail->p->tot_len!=tail_length || sender->snd_queuelen!=1 ||
       sender->snd_lbb!=original_sequence+sizeof(seed) ||
       sender->snd_buf!=original_room-sizeof(seed)) return 49;

    /* The vector keeps the filled old tail and one complete new segment.
     * The accepted prefix crosses all three discontiguous source buffers. */
    u16_t written=0;
    if(boaros_lwip_tcp_writev(sender,vector,3,sizeof(scalar),0,&written)!=ERR_MEM ||
       written!=1021 || sender->unsent!=tail || tail->len!=512 ||
       !tail->next || tail->next->len!=512 || tail->next->next ||
       sender->snd_queuelen!=2 || sender->snd_lbb!=original_sequence+1024 ||
       sender->snd_buf!=original_room-1024 || (TCPH_FLAGS(tail->next->tcphdr)&TCP_PSH)) return 50;
    const struct kernel_socket_write_segment remainder={last+621,179};
    if(boaros_lwip_tcp_writev(sender,&remainder,1,179,0,&written)!=ERR_MEM || written ||
       sender->snd_queuelen!=2 || sender->snd_lbb!=original_sequence+1024 ||
       sender->snd_buf!=original_room-1024) return 51;
    for(unsigned i=0;i<count;i++) memp_free(MEMP_TCP_SEG,held[i]);
    if(boaros_lwip_tcp_writev(sender,&remainder,1,179,0,&written)!=ERR_OK || written!=179 ||
       !tail->next->next || !(TCPH_FLAGS(tail->next->next->tcphdr)&TCP_PSH)) return 52;

#if TCP_CHECKSUM_ON_COPY && LWIP_CHECKSUM_CTRL_PER_NETIF
    u16_t checksum_flags=loop->chksum_flags;
    loop->chksum_flags|=NETIF_CHECKSUM_GEN_TCP|NETIF_CHECKSUM_CHECK_TCP;
#else
    (void)loop;
#endif
    tcp_recv(receiver,receive_bulk);
    if(tcp_output(sender)!=ERR_OK) return 53;
    netif_poll_all();
    tcp_ack_now(receiver);
    if(tcp_output(receiver)!=ERR_OK) return 54;
    netif_poll_all();
    if(bulk_received!=1203 || sender->unsent || sender->unacked ||
       sender->snd_queuelen || sender->snd_buf!=original_room) return 55;
    bulk_received=0;
    sender->mss=original_mss;
#if TCP_CHECKSUM_ON_COPY && LWIP_CHECKSUM_CTRL_PER_NETIF
    loop->chksum_flags=checksum_flags;
#endif
    return 0;
}

static int tcp_copy_heap_reserve(struct tcp_pcb *sender, struct tcp_pcb *receiver)
{
    static unsigned char bytes[8192];
    const mem_size_t baseline=STATS_GET(mem.used),reserve=MEMP_NUM_TCP_PCB*256U;
    const u32_t sequence=sender->snd_lbb;
    const tcpwnd_size_t room=sender->snd_buf;
    /* Measure the real allocator charges rather than assuming header layout. */
    struct pbuf *measure=pbuf_alloc(PBUF_TRANSPORT,sizeof(bytes),PBUF_RAM);
    if (!measure) return 62;
    mem_size_t charge=STATS_GET(mem.used)-baseline;
    pbuf_free(measure);
    void *probe=mem_malloc(32);
    if (!probe) return 62;
    mem_size_t header=STATS_GET(mem.used)-baseline-32U;
    mem_free(probe);
    if (baseline || charge<=reserve || !header) return 62;
    /* A near-fit hole would consume 8 bytes beyond the reserved budget.
     * The remaining tail is smaller than the 8 KiB COPY pbuf. */
    void *hole=mem_malloc(charge+MEM_ALIGNMENT-header);
    void *fence=mem_malloc(32);
    if (!hole || !fence) return 62;
    void *filler=mem_malloc(MEM_SIZE-reserve+MEM_ALIGNMENT-STATS_GET(mem.used)-header);
    if (!filler) return 62;
    mem_free(hole);
    if (MEM_SIZE-STATS_GET(mem.used)!=reserve+charge) return 62;
    for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(unsigned char)i;
    if (tcp_write(sender,bytes,sizeof(bytes),TCP_WRITE_FLAG_COPY)!=ERR_MEM ||
        sender->unsent || sender->unacked || sender->snd_queuelen ||
        sender->snd_lbb!=sequence || sender->snd_buf!=room) return 63;
    /* ACK/control metadata must still make progress from the same heap. */
    struct pbuf *ack=pbuf_alloc(PBUF_IP,TCP_HLEN,PBUF_RAM);
    if (!ack) return 64;
    pbuf_free(ack);
    mem_free(filler);
    mem_free(fence);
    tcp_recv(receiver,receive_bulk);
    if (STATS_GET(mem.used)!=baseline || bulk_received ||
        tcp_write(sender,bytes,sizeof(bytes),TCP_WRITE_FLAG_COPY)!=ERR_OK ||
        tcp_output(sender)!=ERR_OK) return 65;
    netif_poll_all();
    tcp_ack_now(receiver);
    if (tcp_output(receiver)!=ERR_OK) return 65;
    netif_poll_all();
    if (bulk_received!=sizeof(bytes) || STATS_GET(mem.used)!=baseline ||
        sender->unacked || sender->unsent || sender->snd_buf!=room) return 66;
    bulk_received=0;
    return 0;
}

static err_t hold_received(void *context, struct tcp_pcb *pcb, struct pbuf *packet, err_t error)
{
    (void)pcb;
    struct pbuf **held = context;
    if (error != ERR_OK || !packet || *held) __builtin_trap();
    *held = packet;
    return ERR_OK;
}

static int tcp_shared_retransmit(struct tcp_pcb *sender, struct tcp_pcb *receiver, struct netif *loop)
{
    static unsigned char bytes[8192];
    struct pbuf *held = NULL;
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(3U * i + 1U);
    tcp_arg(receiver, &held);
    tcp_recv(receiver, hold_received);
    if (tcp_write(sender, bytes, sizeof(bytes), TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_output(sender) != ERR_OK || netif_poll_budget(loop, 1) != 1 ||
        !held || held->tot_len != sizeof(bytes) || !sender->unacked) return 56;
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = 0;
    /* RX owns the original delivery, but its view must not make TX busy. */
    struct pbuf *old_tail = loop->loop_last;
    if (tcp_rexmit_rto_prepare(sender) != ERR_OK) return 57;
    tcp_rexmit_rto_commit(sender);
    if (!sender->unacked || loop->loop_last == old_tail) return 58;
    netif_poll_all();
    tcp_ack_now(receiver);
    if (tcp_output(receiver) != ERR_OK) return 59;
    netif_poll_all();
    if (sender->unacked || sender->unsent || tcp_sndbuf(sender) != 32768) return 60;
    /* The ACK removed the original TX descriptor before RX consumes it. */
    for (u16_t i = 0; i < sizeof(bytes); i++)
        if (pbuf_get_at(held, i) != (unsigned char)(3U * i + 1U)) return 61;
    tcp_recved(receiver, held->tot_len);
    pbuf_free(held);
    tcp_arg(receiver, NULL);
    tcp_recv(receiver, NULL);
    netif_poll_all();
    return 0;
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
    shared_payload_cases();
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
    receive_policy_cases();
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
    if (tcp_mss(tcp_client) != 8192 || tcp_mss(tcp_accepted) != 8192 ||
        tcp_sndbuf(tcp_client) != 32768 || tcp_sndbuf(tcp_accepted) != 32768 ||
        tcp_client->snd_wnd != 32768 || tcp_accepted->snd_wnd != 32768) {
        return 44;
    }
    int vector_result=tcp_vector_prefix(tcp_client,tcp_accepted,loop);
    if(vector_result) return vector_result;
    int reserve_result=tcp_copy_heap_reserve(tcp_client,tcp_accepted);
    if(reserve_result) return reserve_result;
    int retransmit_result=tcp_shared_retransmit(tcp_client,tcp_accepted,loop);
    if(retransmit_result) return retransmit_result;
    static unsigned char bulk[20000];
    for (unsigned int i = 0; i < sizeof(bulk); i++) bulk[i] = (unsigned char)i;
    tcp_recv(tcp_accepted, receive_bulk);
    if (tcp_write(tcp_client, bulk, sizeof(bulk), TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_output(tcp_client) != ERR_OK) return 45;
    netif_poll_all();
    if (bulk_received != sizeof(bulk)) return 46;
    tcp_recv(tcp_accepted, NULL);
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
