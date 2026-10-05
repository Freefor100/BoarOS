#ifndef BOAROS_LWIPOPTS_H
#define BOAROS_LWIPOPTS_H

#include "boaros_lwip.h"
#define LWIP_HOOK_TCP_INPACKET_PCB(pcb, hdr, optlen, opt1len, opt2, p) boaros_lwip_tcp_input(pcb)
#define LWIP_HOOK_NETIF_LOOPBACK_QUEUED(netif) boaros_lwip_work_ready()
#define LWIP_HOOK_MEMP_RELEASED(type) boaros_lwip_capacity_available(type)
#define LWIP_HOOK_MEM_AVAILABLE() boaros_lwip_capacity_available(-1)

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_IPV4 1
#define LWIP_IPV6 1
#define LWIP_IPV6_MLD 0
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_REASS 0
#define LWIP_IPV6_FRAG 0
#define LWIP_TCP 1
#define SO_REUSE 1
#define TCP_LISTEN_BACKLOG 1
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_UDP 1
#define LWIP_RAW 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define IP_REASSEMBLY 1
#define IP_REASS_MAX_PBUFS 48
#define MEMP_NUM_REASSDATA 8
#define IP_FRAG 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF 1
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0

/* Protocol and packet storage is bounded static memory, not kernel_heap. */
#define MEM_ALIGNMENT 8
#define MEM_SIZE (256U * 1024U)
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_TCP_PCB 32
#define MEMP_NUM_TCP_PCB_LISTEN 16
#define MEMP_NUM_TCP_SEG 128
#define MEMP_NUM_PBUF 64
#define PBUF_POOL_SIZE 64
#define PBUF_POOL_BUFSIZE 1536
#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0
#define LWIP_DEBUG 0

#endif
