#include <string.h>

struct receive_policy_case {
    struct tcp_pcb *accepted;
    unsigned connected, refuse, received;
};
static err_t receive_policy_data(void *argument, struct tcp_pcb *pcb,
                                struct pbuf *p, err_t error)
{
    struct receive_policy_case *test = argument;
    assert(error == ERR_OK);
    if (!p) return ERR_OK;
    if (test->refuse) return ERR_MEM;
    for (u16_t i = 0; i < p->tot_len; i++) assert(pbuf_get_at(p, i) == 0x5a);
    test->received += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static err_t receive_policy_accept(void *argument, struct tcp_pcb *pcb, err_t error)
{
    struct receive_policy_case *test = argument;
    assert(error == ERR_OK && !test->accepted);
    test->accepted = pcb;
    tcp_arg(pcb, test);
    tcp_recv(pcb, receive_policy_data);
    return ERR_OK;
}
static err_t receive_policy_connected(void *argument, struct tcp_pcb *pcb, err_t error)
{
    (void)pcb;
    assert(error == ERR_OK);
    ((struct receive_policy_case *)argument)->connected++;
    return ERR_OK;
}
static void receive_policy_cases(void)
{
    const mem_size_t memory_before = STATS_GET(mem.used);
    const u16_t pcbs_before = lwip_stats.memp[MEMP_TCP_PCB]->used;
    const u16_t listeners_before = lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used;
    struct receive_policy_case test = {0};
    struct tcp_pcb *listener = tcp_new(), *sender = tcp_new();
    ip_addr_t address;
    IP_ADDR4(&address, 127, 0, 0, 1);
    assert(listener && sender && tcp_bind(listener, &address, 0) == ERR_OK);
    u16_t port = listener->local_port;
    tcp_set_receive_limit(listener, 2048);
    listener = tcp_listen(listener);
    assert(listener);
    tcp_arg(listener, &test);
    tcp_accept(listener, receive_policy_accept);
    tcp_arg(sender, &test);
    tcp_set_receive_limit(sender, 4096);
    assert(tcp_connect(sender, &address, port, receive_policy_connected) == ERR_OK);
    netif_poll_all();
    assert(test.connected == 1 && test.accepted);
    struct tcp_pcb *receiver = test.accepted;
    assert(receiver->snd_wnd == 4096 && sender->snd_wnd == 2048);

    tcp_set_receive_limit(receiver, 8192);
    netif_poll_all();
    assert(sender->snd_wnd == 8192);
    static unsigned char data[8192];
    memset(data, 0x5a, sizeof(data));
    test.refuse = 1;
    assert(tcp_write(sender, data, 2048, TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(sender) == ERR_OK);
    netif_poll_all();
    assert(receiver->refused_data && receiver->rcv_wnd == 6144);
    u32_t promised = receiver->rcv_ann_right_edge;
    tcp_set_receive_limit(receiver, 1024);
    assert(receiver->rcv_ann_right_edge == promised && receiver->rcv_wnd == 6144);
    assert(tcp_rcv_wnd_limit(receiver) - receiver->rcv_wnd == 2048);
    test.refuse = 0;
    assert(tcp_process_refused_data(receiver) == ERR_OK);
    netif_poll_all();
    assert(!receiver->refused_data && test.received == 2048);
    assert(receiver->rcv_wnd == 6144 && receiver->rcv_ann_right_edge == promised);
    assert(tcp_write(sender, data, 6144, TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(sender) == ERR_OK);
    netif_poll_all();
    assert(test.received == sizeof(data) && tcp_rcv_wnd_limit(receiver) == 1024);
    assert(receiver->rcv_wnd == 1024 && sender->snd_wnd == 1024);

    /* The reduced window must bound admission and reopen after actual consume. */
    test.refuse = 1;
    assert(tcp_write(sender, data, 1024, TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(sender) == ERR_OK);
    netif_poll_all();
    assert(tcp_write(sender, data, 1024, TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(sender) == ERR_OK);
    netif_poll_all();
    assert(receiver->refused_data && receiver->refused_data->tot_len == 1024 &&
           receiver->rcv_wnd == 0 && sender->unsent);
    test.refuse = 0;
    assert(tcp_process_refused_data(receiver) == ERR_OK);
    netif_poll_all();
    assert(test.received == sizeof(data) + 2048 && receiver->rcv_wnd == 1024);

    /* Repeated allocation failure must keep the window ACK scheduled. */
    static void *held[MEM_SIZE / 64 + 1];
    unsigned count = 0;
    while (count < sizeof(held) / sizeof(held[0]) && (held[count] = mem_malloc(64))) count++;
    assert(count && count < sizeof(held) / sizeof(held[0]));
    tcp_set_receive_limit(receiver, 4096);
    assert(receiver->flags & TF_ACK_DELAY);
    tcp_fasttmr();
    assert(receiver->flags & TF_ACK_DELAY);
    tcp_fasttmr();
    assert(receiver->flags & TF_ACK_DELAY);
    while (count) mem_free(held[--count]);
    tcp_fasttmr();
    netif_poll_all();
    assert(sender->snd_wnd == 4096 && !(receiver->flags & (TF_ACK_DELAY | TF_ACK_NOW)));
    tcp_set_receive_limit(receiver, 0);
    netif_poll_all();
    assert(sender->snd_wnd == BOAROS_LWIP_LOOPBACK_WINDOW);
    tcp_abort(sender);
    netif_poll_all();
    assert(tcp_close(listener) == ERR_OK);
    assert(STATS_GET(mem.used) == memory_before &&
           lwip_stats.memp[MEMP_TCP_PCB]->used == pcbs_before &&
           lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used == listeners_before);
}
