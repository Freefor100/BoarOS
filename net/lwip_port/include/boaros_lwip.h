#ifndef BOAROS_LWIP_HOOKS_H
#define BOAROS_LWIP_HOOKS_H
#include <stddef.h>
#include <stdint.h>
uint32_t boaros_lwip_centiseconds(void);
int boaros_lwip_ip6_mld_valid_header(void);
struct boaros_lwip_tcp_context;
struct ip_addr;
struct ip6_addr;
struct netif;
int boaros_lwip_ip6_prefix_bits(const struct ip6_addr *,const struct ip6_addr *);
int boaros_lwip_ip6_onlink(struct netif *,const struct ip6_addr *);
struct netif *boaros_lwip_ip6_route(const struct ip6_addr *,const struct ip6_addr *);
struct kernel_lock_guard;
struct pbuf;
struct tcp_pcb;
struct udp_pcb;
/* Socket sends offer a private pbuf. Loopback output either adopts it into
 * its queue or leaves it for the same protocol owner to deliver after send. */
enum {
    BOAROS_UDP_LOOPBACK_TRANSFER = 1,
    BOAROS_UDP_LOOPBACK_QUEUED,
    BOAROS_UDP_LOOPBACK_TRANSFER_INLINE,
    BOAROS_UDP_LOOPBACK_INLINE,
};
void boaros_lwip_udp_loopback_input(struct netif *, struct pbuf *);
struct udp_pcb *boaros_lwip_udp4_local_target(struct netif *, const struct ip_addr *,
                                             const struct ip_addr *, uint16_t, uint16_t);
int boaros_lwip_raw_input_idle(void);
struct pbuf *boaros_lwip_udp_copy_borrowed(struct pbuf *);
struct boaros_lwip_hooks {
    void (*netif_removed)(struct netif *);
    int (*udp_multicast_receive)(struct udp_pcb *,struct pbuf *,struct netif *,const void *,uint16_t,uint16_t);
    int (*udp_ingress_filter)(struct udp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
    int (*tcp_ingress_filter)(struct tcp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
    void (*tcp_output)(struct tcp_pcb *);
    void (*work)(void);
    void (*loopback_publish)(struct netif *);
    void (*capacity)(int);
    void *(*tcp_storage_allocate)(struct tcp_pcb *, uint32_t bytes, void **context);
    void (*tcp_storage_release)(void *context, void *allocation);
    void (*timewait_free)(struct tcp_pcb *);
    void (*tcp_info_retire)(struct tcp_pcb *);
    struct boaros_lwip_tcp_context *(*tcp_context)(void);
    int (*tcp_dispatch)(struct pbuf *, struct netif *, const struct ip_addr *, const struct ip_addr *);
    /* Before adding the IP header, with the TCP owner held. The hook must
     * validate its own interface/queue state and never take the original pbuf. */
    int (*tcp_local_output)(const struct tcp_pcb *, struct pbuf *, struct netif *,
                           const struct ip_addr *, const struct ip_addr *, int *);
    void (*pcb_forget)(struct tcp_pcb *);
    void (*pcb_timewait)(struct tcp_pcb *);
    int (*reclaim_remote_timewait)(void);
    int (*tcp_conflict)(const struct tcp_pcb *, const struct ip_addr *, uint16_t, const struct ip_addr *, uint16_t);
};
void boaros_lwip_netif_removed(struct netif *);
int boaros_lwip_udp_multicast_receive(struct udp_pcb *,struct pbuf *,struct netif *,const void *,uint16_t,uint16_t);
int boaros_lwip_udp_ingress_filter(struct udp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
int boaros_lwip_tcp_ingress_filter(struct tcp_pcb *,struct pbuf *,struct netif *,int,const void *,uint16_t,uint16_t);
void boaros_lwip_set_hooks(const struct boaros_lwip_hooks *hooks);
int boaros_lwip_tcp_socket_unfiltered(struct tcp_pcb *pcb);
void boaros_lwip_tcp_output(struct tcp_pcb *pcb);
/* A serialized Ethernet receive pass may coalesce adjacent plain ACKs.
 * Flush before handing other traffic to a protocol or returning the scope. */
void boaros_lwip_tcp_ack_batch_begin(void);
void boaros_lwip_tcp_ack_batch_flush(void);
void boaros_lwip_tcp_ack_batch_end(void);
void boaros_lwip_work_ready(void);
void boaros_lwip_loopback_publish(struct netif *);
void boaros_lwip_capacity_available(int pool);
struct kernel_heap;
void boaros_lwip_tcp_pcb_bind_heap(struct kernel_heap *);
void *boaros_lwip_tcp_pcb_allocate(size_t);
void boaros_lwip_tcp_pcb_release(void *);
void *boaros_lwip_tcp_storage_allocate(struct tcp_pcb *, uint32_t bytes, void **context);
void boaros_lwip_tcp_storage_release(void *context, void *allocation);
void boaros_lwip_tcp_timewait_free(struct tcp_pcb *pcb);
void boaros_lwip_tcp_info_retire(struct tcp_pcb *pcb);
struct boaros_lwip_tcp_context *boaros_lwip_tcp_context_lookup(void);
int boaros_lwip_tcp_dispatch(struct pbuf *, struct netif *, const struct ip_addr *, const struct ip_addr *);
int boaros_lwip_tcp_local_output(const struct tcp_pcb *, struct pbuf *, struct netif *,
                                const struct ip_addr *, const struct ip_addr *, int *);
void boaros_lwip_tcp_pcb_forget(struct tcp_pcb *);
void boaros_lwip_tcp_pcb_timewait(struct tcp_pcb *);
int boaros_lwip_tcp_reclaim_remote_timewait(void);
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
uint64_t boaros_lwip_ipv4_path_generation(void);
void boaros_lwip_ipv4_path_changed(void);
void boaros_lwip_ip_lock_shared(struct kernel_lock_guard *);
int boaros_lwip_ip_try_lock(struct kernel_lock_guard *);
struct kernel_lock_guard *boaros_lwip_ip_drop_current(void);
static inline void boaros_lwip_ip_lock(struct kernel_lock_guard *guard)
{
    if (boaros_lwip_parallel_enabled()) boaros_lwip_ip_lock_shared(guard);
}
#define BOAROS_LWIP_IP_SCOPE(name) \
    struct kernel_lock_guard name __attribute__((cleanup(kernel_lock_scope_release))) = {0}; \
    boaros_lwip_ip_lock(&name)
#else
static inline void boaros_lwip_ipv4_path_changed(void) {}
static inline void boaros_lwip_ip_lock(struct kernel_lock_guard *guard) { (void)guard; }
#define BOAROS_LWIP_IP_SCOPE(name) (void)0
#endif
#define LWIP_HOOK_TCP_TIMEWAIT_FREE(pcb) boaros_lwip_tcp_timewait_free(pcb)
#endif
