#include <kernel/random.h>
#include <kernel/time.h>

#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/nd6.h"
#include "boaros_lwip.h"

#include <stdint.h>
#include <kernel/cost.h>
#if BOAROS_COST_DIAGNOSTICS
#include <kernel/cpu.h>
#endif
#if BOAROS_USER_SMP
#include <kernel/sync.h>
#endif
#if defined(BOAROS_LWIP_SHARED_MEMORY) && (defined(__riscv) || defined(__loongarch__))
#include <arch/context.h>
#endif

#ifdef BOAROS_LWIP_SHARED_MEMORY
unsigned boaros_lwip_parallel;
static unsigned memory_busy;
#if BOAROS_COST_DIAGNOSTICS
static struct lwip_memory_cost {
    struct kernel_cost_tag tag;
    uint64_t wait, acquired;
} memory_cost[BOAROS_MAX_CPUS];
_Static_assert(sizeof(struct lwip_memory_cost)==KERNEL_COST_CPU_LWIP_BYTES,
    "lwIP cost CPU budget");
#endif
sys_prot_t sys_arch_protect(void)
{
    sys_prot_t irq=0;
#if defined(__riscv) || defined(__loongarch__)
    irq=arch_interrupt_save();
#endif
#if BOAROS_COST_DIAGNOSTICS
    struct lwip_memory_cost *cost=&memory_cost[kernel_cpu_current()->logical_id];
    cost->tag=kernel_cost_capture();
    uint64_t started=cost->tag.epoch?kernel_cost_clock():0;
#endif
    if (boaros_lwip_parallel_enabled()) {
        while(__atomic_exchange_n(&memory_busy,1,__ATOMIC_ACQUIRE))
            while(__atomic_load_n(&memory_busy,__ATOMIC_RELAXED)) {}
    }
#if BOAROS_COST_DIAGNOSTICS
    if (cost->tag.epoch) {
        cost->acquired=kernel_cost_clock();
        cost->wait=cost->acquired-started;
    }
#endif
    return irq;
}

void sys_arch_unprotect(sys_prot_t irq)
{
#if BOAROS_COST_DIAGNOSTICS
    struct lwip_memory_cost *cost=&memory_cost[kernel_cpu_current()->logical_id];
    struct kernel_cost_tag tag=cost->tag;
    uint64_t wait=cost->wait;
    uint64_t held=tag.epoch?kernel_cost_clock()-cost->acquired:0;
    cost->tag.epoch=0;
#endif
    if (boaros_lwip_parallel_enabled())
        __atomic_store_n(&memory_busy,0,__ATOMIC_RELEASE);
#if BOAROS_COST_DIAGNOSTICS
    /* IRQ exclusion fixes the CPU until restore; the real storage lock is
     * already free before publishing to the COST aggregate. */
    if (tag.epoch) {
        kernel_cost_add_tag(tag,COST_NETWORK_POOL_WAIT_TICKS,wait);
        kernel_cost_add_tag(tag,COST_NETWORK_POOL_HOLD_TICKS,held);
    }
#endif
#if defined(__riscv) || defined(__loongarch__)
    arch_interrupt_restore(irq);
#else
    (void)irq;
#endif
}
#endif

#if BOAROS_USER_SMP
static struct kernel_mutex ip_mutex;
#endif
void boaros_lwip_ip_init(unsigned cpu_count)
{
#ifdef BOAROS_LWIP_SHARED_MEMORY
    __atomic_store_n(&boaros_lwip_parallel, cpu_count>1, __ATOMIC_RELAXED);
#endif
#if BOAROS_USER_SMP
    if (cpu_count>1) kernel_mutex_init(&ip_mutex,27,(uintptr_t)&ip_mutex);
#else
    (void)cpu_count;
#endif
}
#if BOAROS_USER_SMP
/* Configuration and neighbor writers publish under the IP mutex. Cached
 * TCP paths contain copies, so an already admitted frame needs no pin on ARP. */
static uint64_t ipv4_path_generation = 1;

uint64_t boaros_lwip_ipv4_path_generation(void)
{
    return __atomic_load_n(&ipv4_path_generation,__ATOMIC_ACQUIRE);
}

void boaros_lwip_ipv4_path_changed(void)
{
    if (boaros_lwip_parallel_enabled())
        __atomic_add_fetch(&ipv4_path_generation,1,__ATOMIC_RELEASE);
}

void boaros_lwip_ip_lock_shared(struct kernel_lock_guard *guard)
{
    if(!kernel_lock_held(&ip_mutex.lock,1)) {
        COST_SCOPE(acquire, NETWORK_IP_LOCK_TICKS);
        kernel_mutex_lock(&ip_mutex,guard);
    }
}

struct kernel_lock_guard *boaros_lwip_ip_drop_current(void)
{
    struct kernel_lock_guard *guard = kernel_io_context_current()->locks;
    if (!guard || guard->lock != &ip_mutex.lock) return NULL;
    kernel_lock_release(guard);
    return guard;
}
#endif
void boaros_lwip_ip_unlock(struct kernel_lock_guard *guard)
{
#if BOAROS_USER_SMP
    if(guard->lock)kernel_lock_release(guard);
#else
    (void)guard;
#endif
}

uint32_t boaros_lwip_centiseconds(void)
{
    return (uint32_t)(kernel_time_monotonic_ns() / UINT64_C(10000000));
}

u32_t sys_now(void)
{
    return (u32_t)(kernel_time_monotonic_ns() / UINT64_C(1000000));
}

unsigned int boaros_lwip_random(void)
{
    uint32_t value;
    static uint32_t fallback = UINT32_C(0x9e3779b9);

    if (kernel_random_available() &&
        kernel_random_fill(&value, sizeof(value)) == KERNEL_RANDOM_STATUS_OK) {
        return value;
    }
    /* Early boot and host fixtures can lack a supplied entropy seed. */
#ifdef BOAROS_LWIP_SHARED_MEMORY
    uint32_t salt=(uint32_t)kernel_time_monotonic_ns();
    uint32_t old=__atomic_load_n(&fallback,__ATOMIC_RELAXED);
    do {
        value=old^salt;
        value^=value<<13;
        value^=value>>17;
        value^=value<<5;
    } while(!__atomic_compare_exchange_n(&fallback,&old,value,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    return value;
#else
    fallback ^= (uint32_t)kernel_time_monotonic_ns();
    fallback ^= fallback << 13;
    fallback ^= fallback >> 17;
    fallback ^= fallback << 5;
    return fallback;
#endif
}

int atoi(const char *text)
{
    unsigned int value = 0U;

    if (text == 0) {
        return 0;
    }
    while (*text >= '0' && *text <= '9') {
        unsigned int digit = (unsigned int)(*text - '0');
        if (value > (unsigned int)(__INT_MAX__ - (int)digit) / 10U) {
            return __INT_MAX__;
        }
        value = value * 10U + digit;
        text++;
    }
    return (int)value;
}

static struct boaros_lwip_hooks protocol_hooks;
void boaros_lwip_set_hooks(const struct boaros_lwip_hooks *hooks)
{ protocol_hooks = hooks ? *hooks : (struct boaros_lwip_hooks){0}; }
void boaros_lwip_tcp_output(struct tcp_pcb *pcb)
{
    if (protocol_hooks.tcp_output) protocol_hooks.tcp_output(pcb);
    else (void)tcp_output(pcb);
}
void boaros_lwip_work_ready(void)
{ if (protocol_hooks.work) protocol_hooks.work(); }
void boaros_lwip_loopback_publish(struct netif *netif)
{ if (protocol_hooks.loopback_publish) protocol_hooks.loopback_publish(netif); }
void boaros_lwip_capacity_available(int pool)
{ if (protocol_hooks.capacity) protocol_hooks.capacity(pool); }
void boaros_lwip_tcp_info_retire(struct tcp_pcb *pcb)
{ if(protocol_hooks.tcp_info_retire)protocol_hooks.tcp_info_retire(pcb); }
void boaros_lwip_tcp_timewait_free(struct tcp_pcb *pcb)
{ if (protocol_hooks.timewait_free) protocol_hooks.timewait_free(pcb); }
struct boaros_lwip_tcp_context *boaros_lwip_tcp_context_lookup(void)
{ return protocol_hooks.tcp_context ? protocol_hooks.tcp_context() : 0; }
int boaros_lwip_tcp_dispatch(struct pbuf *p, struct netif *inp, const struct ip_addr *src, const struct ip_addr *dst)
{ return protocol_hooks.tcp_dispatch ? protocol_hooks.tcp_dispatch(p,inp,src,dst) : 0; }
int boaros_lwip_tcp_local_output(const struct tcp_pcb *pcb, struct pbuf *p, struct netif *netif,
    const struct ip_addr *src, const struct ip_addr *dst, int *result)
{
    return protocol_hooks.tcp_local_output ?
        protocol_hooks.tcp_local_output(pcb,p,netif,src,dst,result) : 0;
}
void boaros_lwip_tcp_pcb_forget(struct tcp_pcb *pcb)
{ if(protocol_hooks.pcb_forget)protocol_hooks.pcb_forget(pcb); }
void boaros_lwip_tcp_pcb_timewait(struct tcp_pcb *pcb)
{ if(protocol_hooks.pcb_timewait)protocol_hooks.pcb_timewait(pcb); }
int boaros_lwip_tcp_reclaim_remote_timewait(void)
{
    return protocol_hooks.reclaim_remote_timewait && protocol_hooks.reclaim_remote_timewait();
}
int boaros_lwip_tcp_conflict(const struct tcp_pcb *pcb, const struct ip_addr *local, uint16_t local_port,
    const struct ip_addr *remote, uint16_t remote_port)
{ return protocol_hooks.tcp_conflict ? protocol_hooks.tcp_conflict(pcb,local,local_port,remote,remote_port) : 0; }

void boaros_lwip_netif_removed(struct netif *n)
{if(protocol_hooks.netif_removed)protocol_hooks.netif_removed(n);}
int boaros_lwip_udp_multicast_receive(struct udp_pcb *pcb,struct pbuf *p,struct netif *n,const void *header,uint16_t length,uint16_t port)
{return protocol_hooks.udp_multicast_receive?protocol_hooks.udp_multicast_receive(pcb,p,n,header,length,port):0;}
int boaros_lwip_udp_ingress_filter(struct udp_pcb *pcb,struct pbuf *p,struct netif *input,
    int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
    return !protocol_hooks.udp_ingress_filter ||
        protocol_hooks.udp_ingress_filter(pcb,p,input,ipv6,network,network_length,header);
}
int boaros_lwip_tcp_ingress_filter(struct tcp_pcb *pcb,struct pbuf *p,struct netif *input,
    int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
    return !protocol_hooks.tcp_ingress_filter ||
        protocol_hooks.tcp_ingress_filter(pcb,p,input,ipv6,network,network_length,header);
}

/* Fixed-size interface configuration, under the existing shared IP ownership.
 * No separate FIB allocation or per-packet configuration snapshot is needed. */
int boaros_lwip_ip6_prefix_bits(const ip6_addr_t *a,const ip6_addr_t *b)
{
    const unsigned char *x=(const void *)a->addr,*y=(const void *)b->addr;
    unsigned bits=0;
    for(unsigned i=0;i<16;i++) {
        unsigned v=x[i]^y[i];
        if(!v){bits+=8;continue;}
        for(unsigned mask=128;!(v&mask);mask>>=1)bits++;
        break;
    }
    return (int)bits;
}
int boaros_lwip_ip6_onlink(struct netif *n,const ip6_addr_t *destination)
{
    int best=nd6_onlink_prefix_length(n,destination);
    if(ip6_addr_islinklocal(destination))return 64;
    for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++) {
        if(!ip6_addr_isvalid(netif_ip6_addr_state(n,i)))continue;
        const ip6_addr_t *a=netif_ip6_addr(n,i);
        unsigned prefix=n->ip6_configured[i]?n->ip6_prefix[i]:128;
        if(ip6_addr_islinklocal(a))continue;
        int bits=boaros_lwip_ip6_prefix_bits(a,destination);
        if(bits==128)return 128;
        if(bits>=(int)prefix && (int)prefix>best)best=(int)prefix;
    }
    return best;
}
struct netif *boaros_lwip_ip6_route(const ip6_addr_t *source,const ip6_addr_t *destination)
{
    if(ip6_addr_ismulticast(destination)) {
        struct netif *chosen=NULL;
        for(struct netif *n=netif_list;n;n=n->next) {
            if(!netif_is_up(n) || !netif_is_link_up(n) || (n->hwaddr_len && n->mtu<1280) ||
               (ip6_addr_has_zone(destination) && !ip6_addr_test_zone(destination,n)))continue;
            if(ip6_addr_ismulticast_iflocal(destination) && n->hwaddr_len)continue;
            int valid=0;
            for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
                if(ip6_addr_isvalid(netif_ip6_addr_state(n,i)) &&
                   (ip6_addr_isany(source) || ip6_addr_zoneless_eq(source,netif_ip6_addr(n,i))))valid=1;
            if(!valid)continue;
            if(n==netif_default)return n;
            if(!chosen || (!chosen->hwaddr_len && n->hwaddr_len))chosen=n;
        }
        return chosen;
    }
    struct netif *best=NULL,*fallback=NULL,*advertised=NULL;int length=-1;
    for(struct netif *n=netif_list;n;n=n->next) {
        if(!netif_is_up(n) || !netif_is_link_up(n) || (n->hwaddr_len && n->mtu<1280))continue;
        int source_ok=ip6_addr_isany(source),global=0;
        for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++) {
            if(!ip6_addr_isvalid(netif_ip6_addr_state(n,i)))continue;
            const ip6_addr_t *a=netif_ip6_addr(n,i);
            if(ip6_addr_zoneless_eq(a,source))source_ok=1;
            if(!ip6_addr_islinklocal(a) && !ip6_addr_isloopback(a))global=1;
        }
        if(!source_ok)continue;
        int prefix=boaros_lwip_ip6_onlink(n,destination);
        if(prefix>length){best=n;length=prefix;}
        if(global && !ip6_addr_isany(&n->ip6_gateway))fallback=n;
        else if(global && nd6_default_gateway(n))advertised=n;
    }
    return best?best:fallback?fallback:advertised;
}
