/* TCP queue/retransmission regression using production TCP and RA ingress.
 * Capture replaces only the NIC's IPv6 output, after IP/TCP checksum assembly. */
#define main network_owner_unused_main
#include "network_owner.c"
#undef main
static unsigned mtu_alloc_fail, mtu_packet_count;
static struct {unsigned length;unsigned char bytes[1600];} mtu_packets[8];
struct pbuf *__real_pbuf_alloc(pbuf_layer,u16_t,pbuf_type);
struct pbuf *__wrap_pbuf_alloc(pbuf_layer layer,u16_t length,pbuf_type type)
{
    if(mtu_alloc_fail && !--mtu_alloc_fail)return NULL;
    return __real_pbuf_alloc(layer,length,type);
}
static err_t mtu_capture(struct netif *n,struct pbuf *p,const ip6_addr_t *destination)
{
    (void)destination;
    CHECK(p->tot_len<=netif_mtu6(n) && mtu_packet_count<8);
    unsigned i=mtu_packet_count++;mtu_packets[i].length=p->tot_len;
    CHECK(pbuf_copy_partial(p,mtu_packets[i].bytes,p->tot_len,0)==p->tot_len);
    return ERR_OK;
}
static void mtu_put16(unsigned char *p,unsigned value){p[0]=value>>8;p[1]=value;}
static void mtu_put32(unsigned char *p,uint32_t value)
{p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value;}
static uint32_t mtu_get32(const unsigned char *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void mtu_synack_inject(struct netif *n,const ip6_addr_t *peer,const ip6_addr_t *local,const unsigned char body[24])
{
    struct pbuf *p=pbuf_alloc(PBUF_RAW,78,PBUF_RAM);CHECK(p);
    unsigned char *b=p->payload;memset(b,0,78);memcpy(b,n->hwaddr,6);
    memcpy(b+6,(unsigned char[]){0x52,0x55,0,0,0,2},6);b[12]=0x86;b[13]=0xdd;
    b[14]=0x60;b[19]=24;b[20]=6;b[21]=64;
    memcpy(b+22,peer->addr,16);memcpy(b+38,local->addr,16);memcpy(b+54,body,24);
    CHECK(!pbuf_remove_header(p,54));
    uint16_t sum=ip6_chksum_pseudo(p,6,24,peer,local);memcpy(b+70,&sum,2);
    CHECK(!pbuf_add_header(p,54));CHECK(ethernet_input(p,n)==ERR_OK);
}
static void mtu_packets_check(struct tcp_pcb *pcb,const unsigned char *payload,uint32_t sequence)
{
    CHECK(mtu_packet_count==2);
    unsigned copied=0;
    for(unsigned i=0;i<2;i++) {
        unsigned char *b=mtu_packets[i].bytes;
        unsigned header=(b[52]>>4)*4,length=mtu_packets[i].length-40-header;
        CHECK(b[6]==6 && mtu_get32(b+44)==sequence+copied);
        CHECK(length==(i?200:1240) && !memcmp(b+40+header,payload+copied,length));
        CHECK((b[53]&(TCP_FIN|TCP_PSH))==(i?(TCP_FIN|TCP_PSH):0));
        struct pbuf *p=pbuf_alloc(PBUF_RAW,(u16_t)(mtu_packets[i].length-40),PBUF_RAM);CHECK(p);
        CHECK(pbuf_take(p,b+40,p->tot_len)==ERR_OK);
        CHECK(!ip6_chksum_pseudo(p,6,p->tot_len,ip_2_ip6(&pcb->local_ip),ip_2_ip6(&pcb->remote_ip)));
        pbuf_free(p);copied+=length;
    }
    CHECK(copied==1440 && !pcb->unsent && pcb->unacked && pcb->unacked->next);
    unsigned count=0;for(struct tcp_seg *s=pcb->unacked;s;s=s->next)count+=pbuf_clen(s->p);
    CHECK(pcb->snd_queuelen==count);
}
int main(void)
{
    CHECK(kernel_pid_allocator_init(&model_pid_numbers,model_pid_bitmap,1024)==KERNEL_PID_STATUS_OK);
    CHECK(kernel_pid_publish(&model_pid_registry,&model_pid)==KERNEL_PID_STATUS_OK);
    kernel_socket_network_initialize();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct virtio_net_device device={.mac={0x52,0x54,0,0x12,0x34,0x56}};
    struct kernel_network network={.device=&device};struct netif *n=&network.interface;
    ip4_addr_t local={lwip_htonl(0x0a4d0002)},mask={lwip_htonl(0xffffff00)},zero={0};
    CHECK(netif_add(n,&local,&mask,&zero,&network,interface_init,ethernet_input));
    netif_create_ip6_linklocal_address(n,1);netif_set_up(n);netif_set_link_up(n);
    for(unsigned i=0;i<3;i++)nd6_tmr();
    n->output_ip6=mtu_capture;
    ip6_addr_t peer,all;CHECK(ip6addr_aton("fe80::2",&peer));ip6_addr_assign_zone(&peer,IP6_UNICAST,n);
    CHECK(ip6addr_aton("ff02::1",&all));ip6_addr_assign_zone(&all,IP6_MULTICAST,n);
    unsigned char payload[1440];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(unsigned char)(i*31+7);
    for(unsigned retransmit=0;retransmit<2;retransmit++) {
        unsigned char ra[24]={134,0,0,0,64,0,0,0,0,0,0,0,0,0,0,0,5,1};
        mtu_put32(ra+20,1500);ipv6_inject(n,&peer,&all,ra,sizeof(ra));CHECK(netif_mtu6(n)==1500);
        unsigned memory=lwip_stats.mem.used,heap=heap_live;mtu_packet_count=0;
        struct tcp_pcb *pcb=tcp_new_ip_type(IPADDR_TYPE_V6);CHECK(pcb);
        ip_addr_t address;ip_addr_copy_from_ip6(address,*netif_ip6_addr(n,0));
        CHECK(tcp_bind(pcb,&address,0)==ERR_OK);
        ip_addr_copy_from_ip6(address,peer);
        CHECK(tcp_connect(pcb,&address,9000,NULL)==ERR_OK && pcb->state==SYN_SENT);
        unsigned char synack[24]={0};mtu_put16(synack,9000);mtu_put16(synack+2,pcb->local_port);
        mtu_put32(synack+4,1000);mtu_put32(synack+8,pcb->snd_nxt);
        synack[12]=0x60;synack[13]=TCP_SYN|TCP_ACK;mtu_put16(synack+14,60000);
        synack[20]=2;synack[21]=4;mtu_put16(synack+22,1460);
        mtu_synack_inject(n,&peer,netif_ip6_addr(n,0),synack);
        CHECK(pcb->state==ESTABLISHED && pcb->mss==1440 && !pcb->unacked);
        CHECK(tcp_write(pcb,payload,sizeof(payload),TCP_WRITE_FLAG_COPY)==ERR_OK);
        CHECK(pcb->unsent && pcb->unsent->len==1440 && !pcb->unsent->next);
        CHECK(tcp_send_fin(pcb)==ERR_OK);
        uint32_t sequence=lwip_ntohl(pcb->unsent->tcphdr->seqno);
        if(retransmit) {
            CHECK(tcp_output(pcb)==ERR_OK && pcb->unacked && !pcb->unsent);
            CHECK(tcp_rexmit_rto_prepare(pcb)==ERR_OK && pcb->unsent && !pcb->unacked);
        }
        mtu_packet_count=0;mtu_put32(ra+20,1300);ipv6_inject(n,&peer,&all,ra,sizeof(ra));
        CHECK(netif_mtu6(n)==1300 && !mtu_packet_count);
        struct tcp_seg *head=pcb->unsent;struct pbuf *owner=head->p;
        unsigned queue=pcb->snd_queuelen,available=pcb->snd_buf,used=lwip_stats.mem.used;
        pbuf_ref(owner);CHECK(tcp_output(pcb)==ERR_MEM);
        CHECK(pcb->unsent==head && head->len==1440 && !head->next && owner->ref==2 && !mtu_packet_count);
        pbuf_free(owner);mtu_alloc_fail=1;CHECK(tcp_output(pcb)==ERR_MEM && !mtu_alloc_fail);
        CHECK(pcb->unsent==head && head->len==1440 && !head->next && owner->ref==1 &&
            pcb->snd_queuelen==queue && pcb->snd_buf==available && lwip_stats.mem.used==used && !mtu_packet_count);
        CHECK(tcp_output(pcb)==ERR_OK);mtu_packets_check(pcb,payload,sequence);
        CHECK(pcb->boaros_pmtu==1300);
        CHECK(pcb->boaros_bytes_retrans==(retransmit?sizeof(payload):0));
        CHECK(pcb->snd_buf==available);
        mtu_packet_count=0;tcp_abort(pcb);CHECK(lwip_stats.mem.used==memory && heap_live==heap);
    }
    /* Lifetime expiry keeps both established and listening PCB identities.
     * Use the real Ethernet output boundary for the invalid-source attempt. */
    mtu_packet_count=0;
    unsigned char auto_ra[48]={134,0,0,0,64,0,0,0};
    ra_pio(auto_ra+16,"2001:db8:77::",64,0x40,10000,9000);
    ipv6_inject(n,&peer,&all,auto_ra,sizeof(auto_ra));
    for(unsigned i=0;i<3;i++)nd6_tmr();
    CHECK(ip6_addr_ispreferred(netif_ip6_addr_state(n,3)));
    ip6_addr_t automatic=*netif_ip6_addr(n,3);ip_addr_t local6,remote6;
    ip_addr_copy_from_ip6(local6,automatic);ip_addr_copy_from_ip6(remote6,peer);
    struct tcp_pcb *listener=tcp_new_ip_type(IPADDR_TYPE_V6),*client=tcp_new_ip_type(IPADDR_TYPE_V6);
    CHECK(listener && client && tcp_bind(listener,&local6,9001)==ERR_OK);
    listener=tcp_listen(listener);CHECK(listener);
    CHECK(tcp_bind(client,&local6,0)==ERR_OK);
    mtu_packet_count=0;CHECK(tcp_connect(client,&remote6,9000,NULL)==ERR_OK);
    unsigned char synack[24]={0};mtu_put16(synack,9000);mtu_put16(synack+2,client->local_port);
    mtu_put32(synack+4,2000);mtu_put32(synack+8,client->snd_nxt);
    synack[12]=0x60;synack[13]=TCP_SYN|TCP_ACK;mtu_put16(synack+14,60000);
    synack[20]=2;synack[21]=4;mtu_put16(synack+22,1460);
    mtu_synack_inject(n,&peer,&automatic,synack);CHECK(client->state==ESTABLISHED);
    CHECK(tcp_write(client,payload,64,TCP_WRITE_FLAG_COPY)==ERR_OK);
    netif_ip6_addr_set_valid_life(n,3,1);netif_ip6_addr_set_pref_life(n,3,0);
    nd6_tmr();CHECK(ip6_addr_isinvalid(netif_ip6_addr_state(n,3)));
    CHECK(client->state==ESTABLISHED && listener->state==LISTEN &&
        ip_addr_cmp(&client->local_ip,&local6) && ip_addr_cmp(&listener->local_ip,&local6));
    n->output_ip6=ipv6_output;captured_size=0;
    CHECK(tcp_output(client)==ERR_RTE && client->unsent && !captured_size);
    for(unsigned i=0;i<LWIP_ND6_NUM_NEIGHBORS;i++)CHECK(!neighbor_cache[i].q);
    n->output_ip6=mtu_capture;mtu_packet_count=0;
    ipv6_inject(n,&peer,&all,auto_ra,sizeof(auto_ra));
    for(unsigned i=0;i<3;i++)nd6_tmr();
    CHECK(ip6_addr_ispreferred(netif_ip6_addr_state(n,3)) && ip6_addr_cmp(&automatic,netif_ip6_addr(n,3)));
    mtu_packet_count=0;CHECK(tcp_output(client)==ERR_OK && !client->unsent && client->unacked && mtu_packet_count==1);
    CHECK(mtu_packets[0].length==124 && !memcmp(mtu_packets[0].bytes+60,payload,64));
    mtu_packet_count=0;tcp_abort(client);CHECK(tcp_close(listener)==ERR_OK);
    netif_poll_all();nd6_cleanup_netif(n);netif_remove(n);kernel_socket_protocol_leave(&scope);
    CHECK(!heap_live && !lwip_stats.mem.used);
    puts("PASS IPv6 TCP RA MTU: handshake, queued/RTO 1440->1240+200, busy/allocation rollback, FIN/PSH/sequence/checksum and accounting; SLAAC expiry preserves TCP/listener and re-add resumes");
}
