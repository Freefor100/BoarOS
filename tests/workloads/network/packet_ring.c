#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"PACKET RING FAIL line=%d %s errno=%d\n",__LINE__,#expr,errno); \
    exit(1); \
} } while (0)

static uint32_t block_status(struct tpacket_block_desc *block)
{
    return __atomic_load_n(&block->hdr.bh1.block_status,__ATOMIC_ACQUIRE);
}

static uint64_t milliseconds(void)
{
    struct timespec now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    return (uint64_t)now.tv_sec*1000 + now.tv_nsec/1000000;
}

static void wait_block(struct tpacket_block_desc *block)
{
    uint64_t limit=milliseconds()+1500;
    while (!(block_status(block)&TP_STATUS_USER)) {
        CHECK(milliseconds()<limit);
        struct timespec pause={.tv_nsec=1000000};
        nanosleep(&pause,NULL);
    }
}

static void send_frame(int fd,const struct sockaddr_in *destination,unsigned char *payload,
    size_t length,unsigned char marker)
{
    payload[0]=marker;
    CHECK(sendto(fd,payload,length,0,(const void *)destination,sizeof(*destination))==(ssize_t)length);
}

static void check_frame(struct tpacket_block_desc *block,size_t page,int lo,size_t length,
    unsigned char expected)
{
    CHECK((block_status(block)&TP_STATUS_USER) && block->version==TPACKET_V3);
    CHECK(block->hdr.bh1.num_pkts==1 && block->hdr.bh1.blk_len<=page);
    unsigned offset=block->hdr.bh1.offset_to_first_pkt;
    CHECK(offset>=sizeof(*block) && offset<page-sizeof(struct tpacket3_hdr));
    struct tpacket3_hdr *packet=(void *)((unsigned char *)block+offset);
    struct sockaddr_ll *source=(void *)((unsigned char *)packet+TPACKET_ALIGN(sizeof(*packet)));
    CHECK(packet->tp_next_offset==0 && packet->tp_len==length+42 && packet->tp_snaplen==packet->tp_len);
    CHECK(source->sll_family==AF_PACKET && source->sll_ifindex==lo &&
          source->sll_protocol==htons(ETH_P_IP) && source->sll_pkttype==PACKET_HOST);
    CHECK(packet->tp_mac>=TPACKET3_HDRLEN && packet->tp_net==packet->tp_mac+14 &&
          offset+packet->tp_mac+packet->tp_snaplen<=page);
    unsigned char *network=(unsigned char *)packet+packet->tp_net;
    CHECK((network[0]>>4)==4 && network[9]==17 && network[28]==expected);
    for (size_t i=1;i<length;i++) CHECK(network[28+i]==0x61);
}

int main(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    int lo=(int)if_nametoindex("lo");
    CHECK(lo>0 && page>=4096);
    int fd=socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC,0);
    CHECK(fd>=0);
    int version=TPACKET_V3;
    CHECK(!setsockopt(fd,SOL_PACKET,PACKET_VERSION,&version,sizeof(version)));
    version=-1;
    socklen_t size=sizeof(version);
    CHECK(!getsockopt(fd,SOL_PACKET,PACKET_VERSION,&version,&size) && version==TPACKET_V3);
    struct tpacket_req3 req={.tp_block_size=page,.tp_block_nr=2,.tp_frame_size=page,
        .tp_frame_nr=2,.tp_retire_blk_tov=30,.tp_sizeof_priv=32};
    CHECK(!setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&req,sizeof(req)));
    errno=0;
    CHECK(mmap(NULL,page,PROT_READ,MAP_SHARED,fd,0)==MAP_FAILED && errno==EINVAL);
    unsigned char *mapping=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(mapping!=MAP_FAILED);
    struct tpacket_block_desc *first=(void *)mapping,*second=(void *)(mapping+page);
    struct tpacket_req3 disabled={0};
    errno=0;
    CHECK(setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&disabled,sizeof(disabled))==-1 && errno==EBUSY);

    struct sockaddr_ll link={.sll_family=AF_PACKET,.sll_ifindex=lo,.sll_protocol=htons(ETH_P_IP)};
    CHECK(!bind(fd,(const void *)&link,sizeof(link)));
    int sender=socket(AF_INET,SOCK_DGRAM,0), receiver=socket(AF_INET,SOCK_DGRAM,0);
    CHECK(sender>=0 && receiver>=0);
    struct sockaddr_in destination={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(!bind(receiver,(const void *)&destination,sizeof(destination)));
    size=sizeof(destination);
    CHECK(!getsockname(receiver,(void *)&destination,&size));
    size_t length=2*page/3;
    unsigned char *payload=malloc(length);
    CHECK(payload);
    memset(payload,0x61,length);
    send_frame(sender,&destination,payload,length,0x41);
    send_frame(sender,&destination,payload,length,0x42);
    struct pollfd ready={.fd=fd,.events=POLLIN};
    CHECK(poll(&ready,1,1500)==1 && (ready.revents&POLLIN));
    wait_block(first);
    wait_block(second);
    check_frame(first,page,lo,length,0x41);
    check_frame(second,page,lo,length,0x42);
    CHECK(second->hdr.bh1.seq_num>first->hdr.bh1.seq_num);

    send_frame(sender,&destination,payload,length,0x43);
    struct tpacket_stats_v3 stats={0};
    uint64_t limit=milliseconds()+1500;
    do {
        size=sizeof(stats);
        CHECK(!getsockopt(fd,SOL_PACKET,PACKET_STATISTICS,&stats,&size) && size==sizeof(stats));
        CHECK(milliseconds()<limit);
    } while (!stats.tp_drops);
    CHECK(stats.tp_packets>=stats.tp_drops && stats.tp_freeze_q_cnt<=stats.tp_drops+1);
    check_frame(first,page,lo,length,0x41);
    check_frame(second,page,lo,length,0x42);

    __atomic_store_n(&first->hdr.bh1.block_status,TP_STATUS_KERNEL,__ATOMIC_RELEASE);
    send_frame(sender,&destination,payload,length,0x44);
    wait_block(first);
    check_frame(first,page,lo,length,0x44);

    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(!close(pipefd[1]) && !close(fd));
        char ready_byte;
        CHECK(read(pipefd[0],&ready_byte,1)==1);
        check_frame(first,page,lo,length,0x44);
        CHECK(!munmap(mapping,2*page));
        _exit(0);
    }
    CHECK(!close(pipefd[0]) && !munmap(mapping,2*page));
    errno=0;
    CHECK(setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&disabled,sizeof(disabled))==-1 && errno==EBUSY);
    CHECK(write(pipefd[1],"x",1)==1 && !close(pipefd[1]));
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(!setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&disabled,sizeof(disabled)));
    req.tp_block_size=2*page;
    req.tp_frame_nr=4;
    CHECK(!setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&req,sizeof(req)));
    mapping=mmap(NULL,4*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(mapping!=MAP_FAILED);
    first=(void *)mapping;
    length=page+97;
    unsigned char *expanded=realloc(payload,length);
    CHECK(expanded);
    payload=expanded;
    memset(payload,0x61,length);
    send_frame(sender,&destination,payload,length,0x45);
    wait_block(first);
    CHECK(!close(fd));
    check_frame(first,2*page,lo,length,0x45);
    CHECK(!munmap(mapping,4*page));
    CHECK(!close(sender) && !close(receiver));
    free(payload);
    puts("PACKET RING PASS blocks, full-ring drops, poll, rearm, fork, close and unmap");
    return 0;
}
