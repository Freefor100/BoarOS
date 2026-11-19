/* Production persist-timer split regression. This is a queue/state fixture,
 * not a network-peer test: the remote deliberately has no output route. */
#define main network_owner_unused_main
#include "network_owner.c"
#undef main
static void split_payload(const struct tcp_seg *seg,const unsigned char *expected,unsigned length)
{
    unsigned char bytes[1000];CHECK(seg->len==length && length<=sizeof(bytes));
    CHECK(pbuf_copy_partial(seg->p,bytes,(u16_t)length,seg->p->tot_len-seg->len)==length);
    CHECK(!memcmp(bytes,expected,length));
}
int main(void)
{
    CHECK(kernel_pid_allocator_init(&model_pid_numbers,model_pid_bitmap,1024)==KERNEL_PID_STATUS_OK);
    CHECK(kernel_pid_publish(&model_pid_registry,&model_pid)==KERNEL_PID_STATUS_OK);
    kernel_socket_network_initialize();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    unsigned char payload[1000];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(unsigned char)(i*37+11);
    const unsigned flags[]={TCP_FIN|TCP_PSH,TCP_FIN,TCP_PSH};
    for(unsigned wrap=0;wrap<2;wrap++)for(unsigned variant=0;variant<3;variant++) {
        unsigned memory=lwip_stats.mem.used,live=heap_live;
        struct tcp_pcb *pcb=tcp_new_ip_type(IPADDR_TYPE_V4);CHECK(pcb);
        IP_ADDR4(&pcb->local_ip,192,0,2,1);IP_ADDR4(&pcb->remote_ip,192,0,2,2);
        pcb->local_port=18001;pcb->remote_port=18002;pcb->state=ESTABLISHED;
        pcb->mss=1460;pcb->snd_wnd_max=65535;pcb->snd_wnd=65535;pcb->cwnd=65535;
        uint32_t sequence=wrap?0xffffff80U:9000U;
        pcb->snd_lbb=pcb->snd_nxt=pcb->lastack=sequence;
        TCP_REG_ACTIVE(pcb);
        CHECK(tcp_write(pcb,payload,sizeof(payload),TCP_WRITE_FLAG_COPY)==ERR_OK);
        CHECK(pcb->unsent && !pcb->unsent->next && pcb->unsent->len==sizeof(payload));
        if(flags[variant]&TCP_FIN)CHECK(tcp_send_fin(pcb)==ERR_OK);
        if(!(flags[variant]&TCP_PSH))TCPH_FLAGS_SET(pcb->unsent->tcphdr,TCPH_FLAGS(pcb->unsent->tcphdr)&~TCP_PSH);
        CHECK((TCPH_FLAGS(pcb->unsent->tcphdr)&(TCP_FIN|TCP_PSH))==flags[variant]);
        unsigned available=pcb->snd_buf;
        struct tcp_seg *original=pcb->unsent;
        pcb->snd_wnd=300;pcb->persist_backoff=1;pcb->persist_cnt=2;
        /* The first backoff expires on the next production slow-timer tick. */
        tcp_slowtmr();
        CHECK(pcb->state==ESTABLISHED && !pcb->unacked && pcb->unsent==original);
        struct tcp_seg *first=pcb->unsent,*last=first->next;
        CHECK(last && !last->next && first->len==300 && last->len==700);
        CHECK(!(TCPH_FLAGS(first->tcphdr)&(TCP_FIN|TCP_PSH)));
        CHECK((TCPH_FLAGS(last->tcphdr)&(TCP_FIN|TCP_PSH))==flags[variant]);
        CHECK(lwip_ntohl(first->tcphdr->seqno)==sequence && lwip_ntohl(last->tcphdr->seqno)==sequence+300U);
        split_payload(first,payload,300);split_payload(last,payload+300,700);
        CHECK(pcb->snd_buf==available && pcb->snd_queuelen==pbuf_clen(first->p)+pbuf_clen(last->p));
        CHECK(first->p->ref==1 && last->p->ref==1);
        /* Failed output must retain both pieces, not consume FIN or sequence. */
        CHECK(pcb->snd_nxt==sequence && pcb->lastack==sequence);
        tcp_abort(pcb);CHECK(!tcp_active_pcbs && lwip_stats.mem.used==memory && heap_live==live);
    }
    kernel_socket_protocol_leave(&scope);
    CHECK(!heap_live && !lwip_stats.mem.used);
    puts("PASS TCP persist split: FIN/PSH tail-only, sequence wrap, exact bytes, queue credit and complete cleanup (no wire claim)");
}
