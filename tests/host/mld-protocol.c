/* MLD control packets use the production Ethernet/IP6/ICMP6 ingress and
 * output assembly. Only the physical NIC transmission is captured. */
#define main network_owner_unused_main
#include "network_owner.c"
#undef main
static unsigned ml_sent,ml_length;
static unsigned char ml_frame[1600];
static int ml_filter_failure;
static unsigned ml_fail_report;
struct pbuf *__real_pbuf_alloc(pbuf_layer,u16_t,pbuf_type);
struct pbuf *__wrap_pbuf_alloc(pbuf_layer layer,u16_t length,pbuf_type type)
{
    if(ml_fail_report && layer==PBUF_IP && length==32){ml_fail_report=0;return NULL;}
    return __real_pbuf_alloc(layer,length,type);
}
static err_t ml_capture(struct netif *n,struct pbuf *p)
{
    (void)n;CHECK(p->tot_len<=sizeof(ml_frame));ml_sent++;ml_length=p->tot_len;
    CHECK(pbuf_copy_partial(p,ml_frame,p->tot_len,0)==p->tot_len);return ERR_OK;
}
static err_t ml_filter(struct netif *n,const ip6_addr_t *g,enum netif_mac_filter_action action)
{(void)n;(void)g;return ml_filter_failure==(int)action+1?ERR_IF:ERR_OK;}
static void ml_wire(struct netif *n,const ip6_addr_t *src,const ip6_addr_t *dst,
                    const ip6_addr_t *group,unsigned type,unsigned delay,unsigned mode,unsigned extra,unsigned code)
{
    /* mode 1 missing alert, 2 wrong alert, 3 hop, 4 checksum, 5 atomic,
     * 6 atomic followed by destination options, 7 chained fixed MLD header. */
    unsigned ext=8+(mode==5?8:mode==6?16:0),body=(mode==8?23:24)+extra,total=54+ext+body;
    unsigned char bytes[256]={0};CHECK(total<=sizeof(bytes));
    memcpy(bytes,n->hwaddr,6);memcpy(bytes+6,(unsigned char[]){0x52,0x55,0,0,0,2},6);
    bytes[12]=0x86;bytes[13]=0xdd;bytes[14]=0x60;bytes[18]=(ext+body)>>8;bytes[19]=ext+body;
    bytes[20]=0;bytes[21]=mode==3?2:1;memcpy(bytes+22,src->addr,16);memcpy(bytes+38,dst->addr,16);
    bytes[54]=mode==5 || mode==6?44:58;bytes[56]=mode==1?1:5;bytes[57]=2;bytes[59]=mode==2?1:0;
    if(mode==5 || mode==6)bytes[62]=mode==6?60:58;
    if(mode==6)bytes[70]=58;
    unsigned char *m=bytes+54+ext;m[0]=type;m[1]=code;m[4]=delay>>8;m[5]=delay;
    memcpy(m+8,group->addr,16);
    struct pbuf *p=pbuf_alloc(PBUF_RAW,(u16_t)body,PBUF_RAM);CHECK(p);CHECK(pbuf_take(p,m,body)==ERR_OK);
    uint16_t sum=ip6_chksum_pseudo(p,58,(u16_t)body,src,dst);pbuf_free(p);if(mode==4)sum^=0x1234;
    memcpy(m+2,&sum,2);
    unsigned first=mode==7?54+ext+10:total;
    p=pbuf_alloc(PBUF_RAW,(u16_t)first,PBUF_RAM);CHECK(p);CHECK(pbuf_take(p,bytes,first)==ERR_OK);
    if(first<total){struct pbuf *tail=pbuf_alloc(PBUF_RAW,(u16_t)(total-first),PBUF_RAM);CHECK(tail);
      CHECK(pbuf_take(tail,bytes+first,total-first)==ERR_OK);pbuf_cat(p,tail);}
    CHECK(ethernet_input(p,n)==ERR_OK);
}
static void ml_output(unsigned type,const ip6_addr_t *source,const ip6_addr_t *destination,const ip6_addr_t *group)
{
    CHECK(ml_length==86 && ml_frame[20]==0 && ml_frame[21]==1 && ml_frame[54]==58);
    CHECK(ml_frame[56]==5 && ml_frame[57]==2 && !ml_frame[58] && !ml_frame[59]);
    CHECK(ml_frame[62]==type && !memcmp(ml_frame+22,source->addr,16) &&
        !memcmp(ml_frame+38,destination->addr,16) && !memcmp(ml_frame+70,group->addr,16));
    struct pbuf *p=pbuf_alloc(PBUF_RAW,24,PBUF_RAM);CHECK(p);CHECK(pbuf_take(p,ml_frame+62,24)==ERR_OK);
    CHECK(!ip6_chksum_pseudo(p,58,24,source,destination));pbuf_free(p);
}
static void ml_unchanged(struct netif *n,struct mld_group *g,const ip6_addr_t *src,
    const ip6_addr_t *dst,const ip6_addr_t *payload,unsigned type,unsigned mode)
{
    unsigned timer=g->timer,state=g->group_state,reporter=g->last_reporter_flag,sent=ml_sent,memory=lwip_stats.mem.used;
    ml_wire(n,src,dst,payload,type,0,mode,0,0);
    CHECK(g->timer==timer && g->group_state==state && g->last_reporter_flag==reporter && ml_sent==sent);
    CHECK(lwip_stats.mem.used==memory);
}
/* Suppress even solicited-node obligations through production ingress. */
static void ml_suppress_all(struct netif *n,const ip6_addr_t *peer)
{
    for(struct mld_group *g=netif_mld6_data(n);g;g=g->next) {
        if(ip6_addr_multicast_scope(&g->group_address)>1 &&
           !ip6_addr_isallnodes_linklocal(&g->group_address))
            ml_wire(n,peer,&g->group_address,&g->group_address,131,0,0,0,0);
    }
}
static void ml_advance(unsigned milliseconds)
{
    clock_ns+=(uint64_t)milliseconds*1000000;
    sys_check_timeouts();
}
int main(void)
{
    CHECK(kernel_pid_allocator_init(&model_pid_numbers,model_pid_bitmap,1024)==KERNEL_PID_STATUS_OK);
    CHECK(kernel_pid_publish(&model_pid_registry,&model_pid)==KERNEL_PID_STATUS_OK);
    kernel_socket_network_initialize();struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct virtio_net_device device={.mac={0x52,0x54,0,0x12,0x34,0x56}};
    struct kernel_network network={.device=&device};struct netif *n=&network.interface;
    ip4_addr_t zero={0};CHECK(netif_add(n,&zero,&zero,&zero,&network,interface_init,ethernet_input));
    n->linkoutput=ml_capture;netif_create_ip6_linklocal_address(n,1);netif_set_up(n);netif_set_link_up(n);
    for(unsigned i=0;i<3;i++)nd6_tmr();
    CHECK(ip6_addr_isvalid(netif_ip6_addr_state(n,0)));
    ip6_addr_t peer,group,other,all,routers,global,unspecified;
    CHECK(ip6addr_aton("fe80::2",&peer));ip6_addr_assign_zone(&peer,IP6_UNICAST,n);
    CHECK(ip6addr_aton("ff02::1234",&group));ip6_addr_assign_zone(&group,IP6_MULTICAST,n);
    CHECK(ip6addr_aton("ff02::5678",&other));ip6_addr_assign_zone(&other,IP6_MULTICAST,n);
    ip6_addr_set_allnodes_linklocal(&all);ip6_addr_assign_zone(&all,IP6_MULTICAST,n);
    ip6_addr_set_allrouters_linklocal(&routers);ip6_addr_assign_zone(&routers,IP6_MULTICAST,n);
    CHECK(ip6addr_aton("2001:db8::2",&global));ip6_addr_set_zero(&unspecified);
    ml_sent=0;CHECK(mld6_joingroup_netif(n,&group)==ERR_OK && ml_sent==1);
    ml_output(131,netif_ip6_addr(n,0),&group,&group);
    struct mld_group *g=mld6_lookfor_group(n,&group);CHECK(g && g->use==1 && g->timer);
    for(unsigned mode=1;mode<=6;mode++)ml_unchanged(n,g,&peer,&all,&unspecified,130,mode);
    ml_unchanged(n,g,&peer,&all,&unspecified,130,8);
    ml_unchanged(n,g,&global,&all,&unspecified,130,0);
    ml_unchanged(n,g,&unspecified,&all,&unspecified,130,0);
    ml_unchanged(n,g,&unspecified,&group,&group,131,0);
    ml_unchanged(n,g,&peer,&group,&other,131,0);
    ml_unchanged(n,g,&peer,&group,&other,130,0);
    /* Nonzero Code and trailing bytes are explicitly ignored, not rejected. */
    ml_wire(n,&peer,&group,&group,131,0,7,8,7);CHECK(!g->timer && !g->last_reporter_flag);
    ml_sent=0;ml_wire(n,&peer,&group,&group,130,0,7,8,9);CHECK(ml_sent==1 && g->last_reporter_flag);
    ml_output(131,netif_ip6_addr(n,0),&group,&group);
    g->timer=30;g->group_state=1;
    ml_wire(n,&peer,&group,&group,130,1000,0,0,0);CHECK(g->timer<=10);
    unsigned timer=g->timer;ml_wire(n,&peer,&group,&group,130,2000,0,0,0);CHECK(g->timer==timer);
    ml_wire(n,&peer,&group,&group,131,0,0,0,0);CHECK(!g->timer && !g->last_reporter_flag);
    ml_sent=0;ml_fail_report=1;ml_wire(n,&peer,&group,&group,130,0,0,0,0);
    CHECK(!ml_fail_report && !ml_sent && g->timer==1 && !g->last_reporter_flag);
    mld6_tmr();CHECK(ml_sent>=1 && g->last_reporter_flag);
    unsigned reports=ml_sent;
    CHECK(mld6_leavegroup_netif(n,&group)==ERR_OK && ml_sent==reports+1);
    ml_output(132,netif_ip6_addr(n,0),&routers,&group);
    /* Failed filter installation does not publish a group; failed regular
     * removal keeps its reference, while final-owner removal cannot leak it. */
    n->mld_mac_filter=ml_filter;ml_filter_failure=NETIF_ADD_MAC_FILTER+1;
    CHECK(mld6_joingroup_netif(n,&group)==ERR_IF && !mld6_lookfor_group(n,&group));
    ml_filter_failure=0;CHECK(mld6_joingroup_netif(n,&group)==ERR_OK);g=mld6_lookfor_group(n,&group);CHECK(g);
    g->use=UINT32_MAX;CHECK(mld6_joingroup_netif(n,&group)==ERR_MEM && g->use==UINT32_MAX);g->use=1;
    ml_filter_failure=NETIF_DEL_MAC_FILTER+1;CHECK(mld6_leavegroup_netif(n,&group)==ERR_IF && g->use==1);
    CHECK(mld6_leavegroup_netif_force(n,&group)==ERR_OK && !mld6_lookfor_group(n,&group));
    ml_filter_failure=0;n->mld_mac_filter=NULL;
    /* A DAD-only interface has no usable LL source yet. RFC3590 permits
     * both outgoing report and done with ::, unlike incoming suppression. */
    u8_t old_state=n->ip6_addr_state[0];n->ip6_addr_state[0]=IP6_ADDR_TENTATIVE;
    ml_sent=0;CHECK(mld6_joingroup_netif(n,&other)==ERR_OK && ml_sent==1);
    ml_output(131,&unspecified,&other,&other);
    CHECK(mld6_leavegroup_netif(n,&other)==ERR_OK && ml_sent==2);
    ml_output(132,&unspecified,&routers,&other);n->ip6_addr_state[0]=old_state;
    CHECK(mld6_joingroup_netif(n,&group)==ERR_OK);g=mld6_lookfor_group(n,&group);CHECK(g);
    g->timer=2;g->group_state=1;netif_set_down(n);ml_sent=0;mld6_tmr();CHECK(!ml_sent && g->timer==2);netif_set_up(n);
    g->timer=2;netif_set_link_down(n);ml_sent=0;mld6_tmr();CHECK(!ml_sent && g->timer==2);netif_set_link_up(n);
    g->timer=2;n->mtu=1279;ml_sent=0;mld6_tmr();CHECK(!ml_sent && g->timer==2);n->mtu=1500;
    /* Idle keeps ND/ARP deadlines, not a permanent 100ms MLD poll.
     * Suppression consumes at most one previously reserved callback. */
    ml_suppress_all(n,&peer);ml_advance(100);
    CHECK(sys_timeouts_sleeptime()>100);
    unsigned idle_sent=ml_sent;
    for(unsigned i=0;i<3;i++) {
        ml_advance(100);CHECK(ml_sent==idle_sent && sys_timeouts_sleeptime()>100);
    }
    /* Failed immediate query reply deterministically restarts a one-tick
     * timeout. No early reply at 99ms; exactly one successful reply at 100. */
    ml_sent=0;ml_fail_report=1;
    ml_wire(n,&peer,&group,&group,130,0,0,0,0);
    CHECK(!ml_fail_report && !ml_sent && g->timer==1 && sys_timeouts_sleeptime()==100);
    ml_advance(99);CHECK(!ml_sent && g->timer==1 && sys_timeouts_sleeptime()==1);
    ml_advance(1);CHECK(ml_sent==1 && !g->timer && sys_timeouts_sleeptime()>100);
    ml_output(131,netif_ip6_addr(n,0),&group,&group);
    /* DOWN stops renewal, UP restarts existing report obligations. */
    ml_fail_report=1;ml_wire(n,&peer,&group,&group,130,0,0,0,0);
    CHECK(g->timer==1 && sys_timeouts_sleeptime()==100);
    netif_set_link_down(n);idle_sent=ml_sent;ml_advance(100);
    CHECK(ml_sent==idle_sent && g->timer==1 && sys_timeouts_sleeptime()>100);
    netif_set_link_up(n);CHECK(sys_timeouts_sleeptime()==100);
    ml_suppress_all(n,&peer);ml_advance(100);CHECK(sys_timeouts_sleeptime()>100);
    /* Destroy memberships with a real callback outstanding. The callback
     * carries no group/netif ownership and must not touch freed memory. */
    ml_fail_report=1;ml_wire(n,&peer,&group,&group,130,0,0,0,0);
    CHECK(g->timer==1 && sys_timeouts_sleeptime()==100);
    CHECK(mld6_leavegroup_netif_force(n,&group)==ERR_OK);
    netif_poll_all();nd6_cleanup_netif(n);netif_remove(n);
    idle_sent=ml_sent;ml_advance(100);
    CHECK(ml_sent==idle_sent && sys_timeouts_sleeptime()>100);
    kernel_socket_protocol_leave(&scope);
    CHECK(!heap_live && !lwip_stats.mem.used && !lwip_stats.memp[MEMP_MLD6_GROUP]->used);
    puts("PASS MLDv1 wire validation, query/report timers, Done destination/checksum, filter rollback/force, ref overflow, link lifecycle and on-demand timeout teardown");
}
