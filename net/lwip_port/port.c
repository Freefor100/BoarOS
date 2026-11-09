#include <kernel/random.h>
#include <kernel/time.h>

#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "boaros_lwip.h"

#include <stdint.h>
#if BOAROS_USER_SMP
#include <kernel/sync.h>
#endif
#if defined(BOAROS_LWIP_SHARED_MEMORY) && (defined(__riscv) || defined(__loongarch__))
#include <arch/context.h>
#endif

#ifdef BOAROS_LWIP_SHARED_MEMORY
unsigned boaros_lwip_parallel;
static unsigned memory_busy;
sys_prot_t sys_arch_protect(void)
{
    sys_prot_t irq=0;
#if defined(__riscv) || defined(__loongarch__)
    irq=arch_interrupt_save();
#endif
    if (boaros_lwip_parallel_enabled()) {
        while(__atomic_exchange_n(&memory_busy,1,__ATOMIC_ACQUIRE))
            while(__atomic_load_n(&memory_busy,__ATOMIC_RELAXED)) {}
    }
    return irq;
}

void sys_arch_unprotect(sys_prot_t irq)
{
    if (boaros_lwip_parallel_enabled())
        __atomic_store_n(&memory_busy,0,__ATOMIC_RELEASE);
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
void boaros_lwip_ip_lock_shared(struct kernel_lock_guard *guard)
{
    if(!kernel_lock_held(&ip_mutex.lock,1))
        kernel_mutex_lock(&ip_mutex,guard);
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
int boaros_lwip_tcp_conflict(const struct tcp_pcb *pcb, const struct ip_addr *local, uint16_t local_port,
    const struct ip_addr *remote, uint16_t remote_port)
{ return protocol_hooks.tcp_conflict ? protocol_hooks.tcp_conflict(pcb,local,local_port,remote,remote_port) : 0; }

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
