#ifndef BOAROS_LWIP_HOOKS_H
#define BOAROS_LWIP_HOOKS_H
#include <stdint.h>
struct boaros_lwip_tcp_context;
struct ip_addr;
struct kernel_lock_guard;
struct netif;
struct pbuf;
struct tcp_pcb;
struct udp_pcb;
struct boaros_lwip_hooks {
    int (*udp_ingress_filter)(struct udp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
    int (*tcp_ingress_filter)(struct tcp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
    void (*tcp_output)(struct tcp_pcb *);
    void (*work)(void);
    void (*loopback_publish)(struct netif *);
    void (*capacity)(int);
    void (*timewait_free)(struct tcp_pcb *);
    struct boaros_lwip_tcp_context *(*tcp_context)(void);
    int (*tcp_dispatch)(struct pbuf *, struct netif *, const struct ip_addr *, const struct ip_addr *);
    /* Before adding the IP header, with the TCP owner held. The hook must
     * validate its own interface/queue state and never take the original pbuf. */
    int (*tcp_local_output)(const struct tcp_pcb *, struct pbuf *, struct netif *,
                           const struct ip_addr *, const struct ip_addr *, int *);
    void (*pcb_forget)(struct tcp_pcb *);
    int (*tcp_conflict)(const struct tcp_pcb *, const struct ip_addr *, uint16_t, const struct ip_addr *, uint16_t);
};
int boaros_lwip_udp_ingress_filter(struct udp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
int boaros_lwip_tcp_ingress_filter(struct tcp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
void boaros_lwip_set_hooks(const struct boaros_lwip_hooks *hooks);
void boaros_lwip_tcp_output(struct tcp_pcb *pcb);
void boaros_lwip_work_ready(void);
void boaros_lwip_loopback_publish(struct netif *);
void boaros_lwip_capacity_available(int pool);
void boaros_lwip_tcp_timewait_free(struct tcp_pcb *pcb);
struct boaros_lwip_tcp_context *boaros_lwip_tcp_context_lookup(void);
int boaros_lwip_tcp_dispatch(struct pbuf *, struct netif *, const struct ip_addr *, const struct ip_addr *);
int boaros_lwip_tcp_local_output(const struct tcp_pcb *, struct pbuf *, struct netif *,
                                const struct ip_addr *, const struct ip_addr *, int *);
void boaros_lwip_tcp_pcb_forget(struct tcp_pcb *);
int boaros_lwip_tcp_conflict(const struct tcp_pcb *, const struct ip_addr *, uint16_t, const struct ip_addr *, uint16_t);
/* Boot topology is known before lwIP can start any secondary owner. */
void boaros_lwip_ip_init(unsigned cpu_count);
void boaros_lwip_ip_unlock(struct kernel_lock_guard *);
#if BOAROS_MAX_CPUS != 1
extern unsigned boaros_lwip_parallel;
static inline int boaros_lwip_parallel_enabled(void)
{ return __atomic_load_n(&boaros_lwip_parallel, __ATOMIC_RELAXED) != 0; }
#else
static inline int boaros_lwip_parallel_enabled(void) { return 0; }
#endif
#if BOAROS_USER_SMP
void boaros_lwip_ip_lock_shared(struct kernel_lock_guard *);
static inline void boaros_lwip_ip_lock(struct kernel_lock_guard *guard)
{
    if (boaros_lwip_parallel_enabled()) boaros_lwip_ip_lock_shared(guard);
}
#define BOAROS_LWIP_IP_SCOPE(name) \
    struct kernel_lock_guard name __attribute__((cleanup(kernel_lock_scope_release))) = {0}; \
    boaros_lwip_ip_lock(&name)
#else
static inline void boaros_lwip_ip_lock(struct kernel_lock_guard *guard) { (void)guard; }
#define BOAROS_LWIP_IP_SCOPE(name) (void)0
#endif
#define LWIP_HOOK_TCP_TIMEWAIT_FREE(pcb) boaros_lwip_tcp_timewait_free(pcb)
#endif
