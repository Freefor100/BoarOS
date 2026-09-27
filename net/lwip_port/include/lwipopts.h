#ifndef BOAROS_LWIPOPTS_H
#define BOAROS_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_RAW 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_ARP 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
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
