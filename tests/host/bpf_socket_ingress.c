/* Real lwIP ingress and real cBPF verifier/converter/interpreter. The hook's
 * small program owner is fixture storage; syscall/lifetime coverage is in the
 * bpf_socket workload, not mocked here. */
#include <kernel/bpf_vm.h>
#include <kernel/random.h>
#include "boaros_lwip.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct filter {struct kernel_bpf_insn instructions[64];uint32_t count;unsigned calls;};
static void *allocate(void *opaque,size_t n){(void)opaque;return calloc(1,n);}
static void release(void *opaque,void *p){(void)opaque;free(p);}
static struct kernel_bpf_vm_env environment={.program_type=1,.flags=KERNEL_BPF_VM_CLASSIC,.alloc=allocate,.free=release};
static struct kernel_bpf_vm_workspace workspace;
static unsigned packets,bytes;
static unsigned char received[64];
static struct tcp_pcb *accepted;
static void compile(struct filter *filter,const struct kernel_bpf_classic_insn *code,unsigned count)
{assert(!kernel_bpf_classic_convert(code,count,filter->instructions,64,&filter->count,&environment));assert(!kernel_bpf_verify(filter->instructions,filter->count,&environment));}
static void constant(struct filter *f,unsigned result)
{struct kernel_bpf_classic_insn code[]={{6,0,0,result}};compile(f,code,1);}
static int read_packet(void *p,int64_t offset,uint32_t size,uint32_t *value)
{
 unsigned char data[4];if(offset<0 || size>4 || (uint64_t)offset+size>((struct pbuf *)p)->tot_len)return -1;
 assert(pbuf_copy_partial(p,data,size,offset)==size);*value=0;for(unsigned i=0;i<size;i++)*value=(*value<<8)|data[i];return 0;
}
static uint32_t packet_length(void *p){return ((struct pbuf *)p)->tot_len;}
static int ingress(void *context,struct pbuf *p,struct netif *input,int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
 if(!context)return 1;
 struct filter *filter=context;filter->calls++;assert(input && network && network_length>=(ipv6?40:20));
 assert(((*(const unsigned char *)network)>>4)==(ipv6?6:4));assert(header>=8 && p->tot_len>=header);
 struct kernel_bpf_vm_run run={.opaque=p,.packet=read_packet,.packet_length=packet_length};uint64_t length;
 assert(!kernel_bpf_vm_execute(filter->instructions,filter->count,&environment,&run,&workspace,&length));
 if(!length)return 0;
 if(length<header)length=header;
 if(length<p->tot_len)pbuf_realloc(p,length);
 return 1;
}
static int udp_ingress(struct udp_pcb *pcb,struct pbuf *p,struct netif *input,int ipv6,const void *network,uint16_t network_length,uint16_t header)
{return ingress(pcb->recv_arg,p,input,ipv6,network,network_length,header);}
static int tcp_ingress(struct tcp_pcb *pcb,struct pbuf *p,struct netif *input,int ipv6,const void *network,uint16_t network_length,uint16_t header)
{return ingress(pcb->callback_arg,p,input,ipv6,network,network_length,header);}
uint64_t kernel_time_monotonic_ns(void){return UINT64_C(1000000000);}
int kernel_random_available(void){return 1;}
enum kernel_random_status kernel_random_fill(void *p,size_t n){memset(p,0x37,n);return KERNEL_RANDOM_STATUS_OK;}
static void udp_receive(void *ctx,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *addr,u16_t port)
{(void)ctx;(void)pcb;(void)addr;(void)port;packets++;bytes=p->tot_len;assert(bytes<=sizeof(received));assert(pbuf_copy_partial(p,received,bytes,0)==bytes);pbuf_free(p);}
static err_t tcp_receive(void *ctx,struct tcp_pcb *pcb,struct pbuf *p,err_t error)
{(void)ctx;assert(error==ERR_OK);if(!p)return ERR_OK;assert(bytes+p->tot_len<=sizeof(received));assert(pbuf_copy_partial(p,received+bytes,p->tot_len,0)==p->tot_len);bytes+=p->tot_len;packets++;tcp_recved(pcb,p->tot_len);pbuf_free(p);return ERR_OK;}
static err_t accept_connection(void *ctx,struct tcp_pcb *pcb,err_t error)
{assert(error==ERR_OK);accepted=pcb;tcp_arg(pcb,ctx);tcp_recv(pcb,tcp_receive);return ERR_OK;}
static struct pbuf *udp_payload(void)
{
 struct pbuf *p=pbuf_alloc(PBUF_TRANSPORT,2,PBUF_RAM),*tail=pbuf_alloc(PBUF_RAW,4,PBUF_RAM);
 assert(p && tail);assert(pbuf_take(p,"Ye",2)==ERR_OK && pbuf_take(tail,"s!!!",4)==ERR_OK);pbuf_cat(p,tail);return p;
}
static void udp_case(void)
{
 ip_addr_t address;IP_ADDR4(&address,127,0,0,1);struct filter f={0};
 struct kernel_bpf_classic_insn code[]={{0x30,0,0,8},{0x15,0,1,'Y'},{6,0,0,11},{6,0,0,0}};compile(&f,code,4);
 struct udp_pcb *rx=udp_new(),*tx=udp_new();assert(rx && tx);assert(udp_bind(rx,&address,19823)==ERR_OK);udp_recv(rx,udp_receive,&f);
 struct pbuf *p=udp_payload();
 assert(udp_sendto(tx,p,&address,19823)==ERR_OK);netif_poll_all();assert(packets==1 && bytes==3 && !memcmp(received,"Yes",3));
 pbuf_free(p);p=udp_payload();constant(&f,0);assert(udp_sendto(tx,p,&address,19823)==ERR_OK);netif_poll_all();assert(packets==1);
 pbuf_free(p);p=udp_payload();struct kernel_bpf_classic_insn oob[]={{0x20,0,0,65535},{6,0,0,UINT32_MAX}};compile(&f,oob,2);assert(udp_sendto(tx,p,&address,19823)==ERR_OK);netif_poll_all();assert(packets==1 && f.calls==3);
 pbuf_free(p);udp_remove(rx);udp_remove(tx);assert(!lwip_stats.mem.used);packets=bytes=0;
}
static void tcp_case(void)
{
 ip_addr_t address;IP_ADDR4(&address,127,0,0,1);struct filter f={0};constant(&f,0);
 struct tcp_pcb *listener=tcp_new(),*client=tcp_new();assert(listener && client);assert(tcp_bind(listener,&address,19824)==ERR_OK);listener=tcp_listen(listener);assert(listener);tcp_arg(listener,&f);tcp_accept(listener,accept_connection);
 unsigned baseline=lwip_stats.memp[MEMP_TCP_PCB]->used;assert(tcp_connect(client,&address,19824,NULL)==ERR_OK);netif_poll_all();assert(!accepted && client->state==SYN_SENT && lwip_stats.memp[MEMP_TCP_PCB]->used==baseline && f.calls==1);
 constant(&f,UINT32_MAX);tcp_rexmit_rto(client);assert(tcp_output(client)==ERR_OK);netif_poll_all();assert(accepted && client->state==ESTABLISHED);
 u32_t sequence=accepted->rcv_nxt;constant(&f,0);assert(tcp_write(client,"hello",5,TCP_WRITE_FLAG_COPY)==ERR_OK && tcp_output(client)==ERR_OK);netif_poll_all();assert(!bytes && accepted->rcv_nxt==sequence && client->unacked);
 /* A positive filter result includes the TCP header, and ACK progression must
  * reflect the trimmed packet, not the original bytes rejected by the filter. */
 constant(&f,TCP_HLEN+2);tcp_rexmit_rto(client);assert(tcp_output(client)==ERR_OK);netif_poll_all();assert(bytes==2 && accepted->rcv_nxt==sequence+2 && !memcmp(received,"he",2));
 constant(&f,UINT32_MAX);tcp_rexmit_rto(client);assert(tcp_output(client)==ERR_OK);netif_poll_all();assert(bytes==5 && accepted->rcv_nxt==sequence+5 && !memcmp(received,"hello",5));
 tcp_arg(accepted,NULL);tcp_abort(accepted);tcp_abort(client);assert(tcp_close(listener)==ERR_OK);netif_poll_all();assert(!lwip_stats.mem.used && !lwip_stats.memp[MEMP_TCP_PCB]->used && !lwip_stats.memp[MEMP_TCP_SEG]->used && !lwip_stats.memp[MEMP_PBUF]->used);
}
int main(void)
{lwip_init();struct boaros_lwip_hooks hooks={.udp_ingress_filter=udp_ingress,.tcp_ingress_filter=tcp_ingress};boaros_lwip_set_hooks(&hooks);udp_case();tcp_case();puts("BPF socket ingress PASS: wire headers, chains, drop before SYN/data state, trim/ACK and pool return");return 0;}
