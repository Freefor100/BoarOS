#define _GNU_SOURCE
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr,"BPF UDP %s:%d: %s: %s\n",__FILE__,__LINE__,#cond,strerror(errno)); goto out; } } while (0)

int main(int argc, char **argv)
{
    struct bpf_object *object = NULL;
    int receiver = -1, sender = -1, result = 1;
    CHECK(argc == 2);
    object = bpf_object__open_file(argv[1], NULL);
    CHECK(object && !libbpf_get_error(object));
    CHECK(bpf_object__load(object) == 0);
    struct bpf_program *program = bpf_object__find_program_by_name(object,"udp_policy");
    CHECK(program);
    int program_fd = bpf_program__fd(program);
    int map_fd = bpf_object__find_map_fd_by_name(object,"udp_rule");
    CHECK(program_fd >= 0 && map_fd >= 0);
    receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(receiver >= 0 && sender >= 0);
    struct ifreq loopback={.ifr_name="lo"};
    CHECK(ioctl(receiver,SIOCGIFFLAGS,&loopback)==0);
    if(!(loopback.ifr_flags&IFF_UP)) {
        loopback.ifr_flags|=IFF_UP;
        CHECK(ioctl(receiver,SIOCSIFFLAGS,&loopback)==0);
    }
    struct sockaddr_in address = {.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(bind(receiver,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t address_size=sizeof(address);
    CHECK(getsockname(receiver,(struct sockaddr *)&address,&address_size)==0);
    CHECK(setsockopt(receiver,SOL_SOCKET,SO_ATTACH_BPF,&program_fd,sizeof(program_fd))==0);
    uint32_t key=0, allow=0, value=1;
    CHECK(bpf_map_update_elem(map_fd,&key,&allow,BPF_ANY)==0);
    CHECK(bpf_map_lookup_elem(map_fd,&key,&value)==0 && value==0);
    const char dropped[]="drop-by-real-filter";
    CHECK(sendto(sender,dropped,sizeof(dropped),0,(struct sockaddr *)&address,sizeof(address))==sizeof(dropped));
    struct pollfd ready={.fd=receiver,.events=POLLIN};
    CHECK(poll(&ready,1,30)==0);
    allow=1;
    CHECK(bpf_map_update_elem(map_fd,&key,&allow,BPF_ANY)==0);
    const char accepted[]="allow-by-real-map";
    CHECK(sendto(sender,accepted,sizeof(accepted),0,(struct sockaddr *)&address,sizeof(address))==sizeof(accepted));
    CHECK(poll(&ready,1,1000)==1 && (ready.revents&POLLIN));
    char packet[64];
    CHECK(recv(receiver,packet,sizeof(packet),0)==sizeof(accepted));
    CHECK(memcmp(packet,accepted,sizeof(accepted))==0);
    CHECK(bpf_map_lookup_elem(map_fd,&key,&value)==0 && value==1);
    CHECK(setsockopt(receiver,SOL_SOCKET,SO_DETACH_BPF,&program_fd,sizeof(program_fd))==0);
    puts("BPF UDP drop-map-allow BTF-global OK");
    result=0;
out:
    if(receiver>=0)close(receiver);
    if(sender>=0)close(sender);
    if(object && !libbpf_get_error(object))bpf_object__close(object);
    return result;
}
