#include <kernel/io_uring.h>
#include "private.h"
#include "../../fs/uaccess_iov_internal.h"

#include <kernel/credentials.h>
#include <kernel/bpf.h>
#include <kernel/bpf_vm.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/socket_call.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <arch/context.h>

#include <stdint.h>
#include <stddef.h>

/* RV64 Linux UAPI values; see fixed references/linux/net/socket.c. */
#define LINUX_AF_UNIX 1
#define LINUX_AF_INET 2
#define LINUX_AF_INET6 10
#define LINUX_AF_NETLINK 16
#define LINUX_AF_PACKET 17
#define LINUX_AF_MAX 46
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_SEQPACKET 5
#define LINUX_SOCK_RAW 3
#define LINUX_SOCK_MAX 11
#define LINUX_SOCK_TYPE_MASK 0xf
#define LINUX_SOCK_NONBLOCK 00004000
#define LINUX_SOCK_CLOEXEC 02000000
#define LINUX_IPPROTO_TCP 6
#define LINUX_IPPROTO_UDP 17
#define LINUX_IPPROTO_ICMP 1
#define LINUX_IPPROTO_MAX 263
#define LINUX_SOCKADDR_STORAGE_SIZE 128
#define LINUX_SOCKADDR_UN_SIZE 110
#define LINUX_SOL_SOCKET 1
#define LINUX_SOL_PACKET 263
#define LINUX_SO_OOBINLINE 10
#define LINUX_SO_PEERCRED 17
#define LINUX_SO_RCVTIMEO 20
#define LINUX_SO_SNDTIMEO 21

struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

struct linux_sockaddr_in6 {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    uint8_t address[16];
    uint32_t scope;
};
_Static_assert(sizeof(struct linux_sockaddr_in6) == 28U, "RV64 sockaddr_in6 size");

struct linux_msghdr {
    uint64_t name;
    int32_t name_length;
    uint32_t padding;
    uint64_t iov, iov_count, control, control_length;
    int32_t flags;
    uint32_t tail_padding;
};
_Static_assert(sizeof(struct linux_msghdr) == 56, "RV64 msghdr size");

struct linux_mmsghdr {
    struct linux_msghdr header;
    uint32_t length, padding;
};
_Static_assert(sizeof(struct linux_mmsghdr)==64, "native mmsghdr size");
_Static_assert(offsetof(struct linux_mmsghdr,length)==56, "native mmsghdr msg_len");
struct linux_timespec { int64_t seconds, nanoseconds; };

struct linux_timeval {
    int64_t seconds;
    int64_t microseconds;
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16U,
               "RV64 sockaddr_in size");

static uint16_t network_port(uint16_t port)
{
    return (uint16_t)((port << 8) | (port >> 8));
}

static enum kernel_syscall_status borrow_socket(
    struct kernel_task *caller, int32_t fd, struct kernel_files **files_out,
    struct kernel_open_file_description **file_out, uint32_t accept_flags,
    int64_t *result)
{
    enum kernel_task_status status = kernel_task_files_borrow(caller, files_out);
    if (status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        *result = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (status != KERNEL_TASK_STATUS_OK ||
        kernel_files_pin_data(*files_out, fd, file_out, result) !=
            KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    /* accept4 checks its flags after fd lookup, before sock_from_file. */
    if (*result == 0 &&
        (accept_flags & ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK))) {
        if (kernel_open_file_release(file_out) != KERNEL_OPEN_FILE_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        *result = -KERNEL_EINVAL;
    }
    if (*result == 0 && kernel_open_file_socket(*file_out) == 0) {
        if (kernel_open_file_release(file_out) != KERNEL_OPEN_FILE_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        *result = -KERNEL_ENOTSOCK;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status release_socket(
    struct kernel_open_file_description **file)
{
    return kernel_open_file_release(file) == KERNEL_OPEN_FILE_STATUS_OK
               ? KERNEL_SYSCALL_STATUS_OK
               : KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
}

union linux_sockaddr_storage {
    struct linux_sockaddr_in v4;
    struct linux_sockaddr_in6 v6;
    uint8_t bytes[LINUX_SOCKADDR_STORAGE_SIZE];
};

static int copy_address_snapshot(struct kernel_mm *mm, uint64_t user,
    uint64_t raw_length, union linux_sockaddr_storage *snapshot)
{
    /* bind/connect/sendto take an int addrlen, including raw 64-bit calls.
     * Import every declared byte before the protocol interprets the snapshot. */
    int32_t length = (int32_t)raw_length;
    size_t copied = 0;
    if (length < 0 || length > (int32_t)sizeof(*snapshot)) return -KERNEL_EINVAL;
    if (length == 0) return 0;
    if (kernel_copy_from_user(mm, snapshot, user, (size_t)length, &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != (size_t)length) return -KERNEL_EFAULT;
    return 0;
}

static int parse_address(const union linux_sockaddr_storage *snapshot,
    int32_t length, int unix_socket, struct kernel_socket_address *address)
{
    uint16_t family;
    if (length < (int32_t)sizeof(family)) return -KERNEL_EINVAL;
    __builtin_memcpy(&family, snapshot->bytes, sizeof(family));
    *address = (struct kernel_socket_address){.family = family};
    if (family == LINUX_AF_UNIX) {
        if (length > LINUX_SOCKADDR_UN_SIZE) return -KERNEL_EINVAL;
        address->unix_length = (uint16_t)(length - sizeof(family));
        address->unix_path = snapshot->bytes + sizeof(family);
    } else if (family == LINUX_AF_INET) {
        if ((size_t)length < sizeof(snapshot->v4)) return -KERNEL_EINVAL;
        __builtin_memcpy(address->bytes, &snapshot->v4.address, 4);
        address->port = network_port(snapshot->v4.port);
    } else if (family == LINUX_AF_INET6) {
        if (length < 24) return -KERNEL_EINVAL;
        __builtin_memcpy(address->bytes, snapshot->v6.address, 16);
        address->scope = snapshot->v6.scope;
        address->port = network_port(snapshot->v6.port);
    } else if (family == LINUX_AF_NETLINK) {
        if(length<12)return -KERNEL_EINVAL;
        __builtin_memcpy(&address->scope,snapshot->bytes+4,4);
        __builtin_memcpy(address->bytes,snapshot->bytes+8,4);
    } else if (family == LINUX_AF_PACKET) {
        if (length < 20) return -KERNEL_EINVAL;
        uint16_t protocol;
        __builtin_memcpy(&protocol,snapshot->bytes+2,2);
        address->port=network_port(protocol);
        __builtin_memcpy(&address->scope,snapshot->bytes+4,4);
        address->link_length=snapshot->bytes[11];
        address->link_size=(uint8_t)length;
        __builtin_memcpy(address->link_address,snapshot->bytes+12,8);
    } else if (family != 0 && !unix_socket) return -KERNEL_EAFNOSUPPORT;
    return 0;
}

static int copy_address_in(struct kernel_mm *mm, uint64_t user,
    uint64_t raw_length, union linux_sockaddr_storage *snapshot,
    int unix_socket, struct kernel_socket_address *address)
{
    int result = copy_address_snapshot(mm, user, raw_length, snapshot);
    return result ? result : parse_address(snapshot, (int32_t)raw_length,
                                           unix_socket, address);
}

static int copy_address_out(struct kernel_mm *mm, uint64_t user_address,
                            uint64_t user_length,
                            const struct kernel_socket_address *address)
{
    union linux_sockaddr_storage local = {0};
    size_t size;
    if (address->family == 0) {
        size = 0;
    } else if (address->family == LINUX_AF_UNIX) {
        local.v4.family = LINUX_AF_UNIX;
        size = sizeof(uint16_t) + address->unix_length;
        if (address->unix_length > 109U) __builtin_trap();
        if (address->unix_length)
            __builtin_memcpy(local.bytes + sizeof(uint16_t),
                             address->unix_path, address->unix_length);
    } else if (address->family == LINUX_AF_NETLINK) {
        local.v4.family=LINUX_AF_NETLINK;
        __builtin_memcpy(local.bytes+4,&address->scope,4);
        __builtin_memcpy(local.bytes+8,address->bytes,4);
        size=12;
    } else if (address->family == LINUX_AF_PACKET) {
        uint16_t protocol=network_port(address->port);
        local.v4.family=LINUX_AF_PACKET;
        __builtin_memcpy(local.bytes+2,&protocol,2);
        __builtin_memcpy(local.bytes+4,&address->scope,4);
        __builtin_memcpy(local.bytes+8,&address->link_type,2);
        local.bytes[10]=address->link_packet_type;
        local.bytes[11]=address->link_length;
        __builtin_memcpy(local.bytes+12,address->link_address,8);
        size=address->link_size;
    } else if (address->family == LINUX_AF_INET6) {
        local.v6.family = LINUX_AF_INET6;
        local.v6.port = network_port(address->port);
        local.v6.scope = address->scope;
        __builtin_memcpy(local.v6.address, address->bytes, 16);
        size = sizeof(local.v6);
    } else {
        local.v4.family = LINUX_AF_INET;
        local.v4.port = network_port(address->port);
        __builtin_memcpy(&local.v4.address, address->bytes, 4);
        size = sizeof(local.v4);
    }
    int32_t length;
    size_t copied = 0;
    if (kernel_copy_from_user(mm, &length, user_length, sizeof(length), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    if (length < 0) return -KERNEL_EINVAL;
    size_t written = (size_t)length < size ? (size_t)length : size;
    /* Linux publishes the full length even if the following data copy faults. */
    length = (int32_t)size;
    if (kernel_copy_to_user(mm, user_length, &length, sizeof(length), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    if (written != 0 &&
        (kernel_copy_to_user(mm, user_address, &local, written, &copied) !=
             KERNEL_UACCESS_STATUS_OK || copied != written)) return -KERNEL_EFAULT;
    return 0;
}

static int socket_option(int level, int name, enum kernel_socket_option *option)
{
    if (level == LINUX_SOL_SOCKET) {
        switch (name) {
        case 2: *option = KERNEL_SOCKET_REUSEADDR; return 0;
        case 3: *option = KERNEL_SOCKET_TYPE; return 0;
        case 4: *option = KERNEL_SOCKET_ERROR; return 0;
        case 6: *option = KERNEL_SOCKET_BROADCAST; return 0;
        case 7: *option = KERNEL_SOCKET_SNDBUF; return 0;
        case 8: *option = KERNEL_SOCKET_RCVBUF; return 0;
        case 9: *option = KERNEL_SOCKET_KEEPALIVE; return 0;
        case LINUX_SO_OOBINLINE: *option = KERNEL_SOCKET_OOBINLINE; return 0;
        case 16: *option = KERNEL_SOCKET_PASSCRED; return 0;
        case 30: *option = KERNEL_SOCKET_ACCEPTCONN; return 0;
        case 32: *option = KERNEL_SOCKET_SNDBUFFORCE; return 0;
        }
    } else if (level == LINUX_IPPROTO_TCP) {
        if (name == 1) { *option = KERNEL_SOCKET_NODELAY; return 0; }
        if (name == 2) { *option = KERNEL_SOCKET_MAXSEG; return 0; }
        if (name == 3) { *option = KERNEL_SOCKET_CORK; return 0; }
        if (name == 4) { *option = KERNEL_SOCKET_KEEPIDLE; return 0; }
        if (name == 5) { *option = KERNEL_SOCKET_KEEPINTVL; return 0; }
        if (name == 6) { *option = KERNEL_SOCKET_KEEPCNT; return 0; }
    } else if (level == 41 && name == 26) {
        *option = KERNEL_SOCKET_V6ONLY; return 0;
    }
    return -KERNEL_ENOPROTOOPT;
}

struct accept_peer_output {
    struct kernel_mm *mm;
    uint64_t address, length;
};

static int copy_accept_peer(void *context,
                            const struct kernel_socket_address *address)
{
    const struct accept_peer_output *output = context;
    return copy_address_out(output->mm, output->address, output->length, address);
}

static int filter_sockopt(struct kernel_socket *socket,struct kernel_files *files,
    struct kernel_mm *mm,const struct kernel_syscall_request *request,int get,int32_t length)
{
    int name=(int32_t)request->arguments[2],value=0;
    uint64_t address=request->arguments[3];size_t copied=0;
    struct kernel_bpf_program *program=NULL;
    if(get) {
        if(name==44) {
            value=kernel_socket_filter_locked(socket);
            if(length>4)length=4;
            if(kernel_copy_to_user(mm,address,&value,length,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=(size_t)length)return -KERNEL_EFAULT;
        } else if(name==26) {
            program=kernel_socket_filter_get(socket);
            const struct kernel_bpf_classic_insn *original=NULL;
            uint32_t count=kernel_bpf_program_classic(program,&original);
            int error=program && !original?-KERNEL_EACCES:0;
            if(!error && length && (uint32_t)length<count)error=-KERNEL_EINVAL;
            if(!error && length && count &&
                (kernel_copy_to_user(mm,address,original,count*sizeof(*original),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=count*sizeof(*original)))error=-KERNEL_EFAULT;
            kernel_bpf_program_put(program);
            if(error)return error;
            length=(int32_t)count;
        } else return -KERNEL_ENOPROTOOPT;
        if(kernel_copy_to_user(mm,request->arguments[4],&length,sizeof(length),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(length))return -KERNEL_EFAULT;
        return 0;
    }
    /* SOL_SOCKET imports its initial int before dispatch, including detach. */
    if(length<4)return -KERNEL_EINVAL;
    if(kernel_copy_from_user(mm,&value,address,4,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=4)return -KERNEL_EFAULT;
    if(name==44)return kernel_socket_filter_lock(socket,value);
    if(name==27)return kernel_socket_filter_set(socket,NULL);
    int error;
    if(name==26) {
        if(kernel_socket_is_tcp(socket) && !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_ADMIN))return -KERNEL_EPERM;
        struct {uint16_t count;uint16_t padding[3];uint64_t instructions;} fprog;
        _Static_assert(sizeof(fprog)==16 && offsetof(__typeof__(fprog),instructions)==8,"native sock_fprog");
        if(length!=sizeof(fprog))return -KERNEL_EINVAL;
        if(kernel_copy_from_user(mm,&fprog,address,sizeof(fprog),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(fprog))return -KERNEL_EFAULT;
        if(kernel_socket_filter_locked(socket))return -KERNEL_EPERM;
        if(!fprog.instructions || !fprog.count || fprog.count>KERNEL_BPF_CLASSIC_MAX_INSNS)return -KERNEL_EINVAL;
        struct kernel_bpf_classic_insn *instructions=NULL;
        size_t size=fprog.count*sizeof(*instructions);
        enum kernel_heap_status allocated=kernel_heap_allocate(files->heap,size,(void **)&instructions);
        if(allocated!=KERNEL_HEAP_STATUS_OK) {
            if(allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
            return -KERNEL_ENOMEM;
        }
        if(kernel_copy_from_user(mm,instructions,fprog.instructions,size,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=size)error=-KERNEL_EFAULT;
        else error=kernel_bpf_classic_create(files->heap,instructions,fprog.count,&program);
        kernel_heap_release(files->heap,instructions);
    } else {
        if(length!=4)return -KERNEL_EINVAL;
        if(kernel_socket_filter_locked(socket))return -KERNEL_EPERM;
        error=kernel_bpf_program_fd(files,value,1,&program);
    }
    if(error)return error;
    error=kernel_socket_filter_set(socket,program);
    if(error)kernel_bpf_program_put(program);
    return error;
}

static int device_sockopt(struct kernel_socket *socket, struct kernel_mm *mm,
    const struct kernel_syscall_request *request, int get, int32_t length)
{
    char device[16] = {0};
    size_t copied = 0;
    if (!get) {
        size_t input = (uint32_t)length < sizeof(device) - 1 ?
            (uint32_t)length : sizeof(device) - 1;
        if (input && (kernel_copy_from_user(mm, device, request->arguments[3], input, &copied) !=
                KERNEL_UACCESS_STATUS_OK || copied != input)) return -KERNEL_EFAULT;
        return kernel_socket_set_bound_device(socket, device);
    }
    uint32_t output = (uint32_t)length;
    int result = kernel_socket_get_bound_device(socket, device, &output);
    if (result) return result;
    if (output && (kernel_copy_to_user(mm, request->arguments[3], device, output, &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != output)) return -KERNEL_EFAULT;
    length = (int32_t)output;
    if (kernel_copy_to_user(mm, request->arguments[4], &length, sizeof(length), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    return 0;
}

/* SOL_IP has byte-sized scalar inputs and publishes optlen before optval.
 * Its multicast interface also has three native input layouts. */
static int ipv4_sockopt(struct kernel_socket *socket, struct kernel_mm *mm,
    const struct kernel_syscall_request *request, int get, int32_t length)
{
    int name = (int32_t)request->arguments[2];
    int value = 0;
    uint32_t address = 0;
    size_t copied = 0;
    enum kernel_socket_option option = name == 2 ? KERNEL_SOCKET_IPV4_TTL :
        name == 34 ? KERNEL_SOCKET_IPV4_MULTICAST_LOOP :
        name == 49 ? KERNEL_SOCKET_IPV4_MULTICAST_ALL : KERNEL_SOCKET_MULTICAST_TTL;
    if (name == 35 || name == 36 || name == 42 || name == 45) {
        int family=kernel_socket_family(socket);
        if (get || (family!=LINUX_AF_INET && family!=LINUX_AF_INET6)) return -KERNEL_ENOPROTOOPT;
        struct { uint32_t group, address, index; } membership = {0};
        if (name == 35 || name == 36) {
            if (kernel_socket_is_tcp(socket)) return -KERNEL_EPROTO;
            if (length < 8) return -KERNEL_EINVAL;
            size_t input = length >= 12 ? 12U : 8U;
            if (kernel_copy_from_user(mm, &membership, request->arguments[3], input, &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != input) return -KERNEL_EFAULT;
        } else {
            /* Native group_req has an eight-byte-aligned sockaddr_storage
             * after the interface; its protocol address is a sockaddr_in. */
            struct {
                uint32_t index, pad;
                uint16_t family, port;
                uint32_t group;
                unsigned char remainder[120];
            } input;
            _Static_assert(sizeof(input)==136,"Linux group_req layout");
            if (length < (int32_t)sizeof(input)) return -KERNEL_EINVAL;
            if (kernel_copy_from_user(mm, &input, request->arguments[3], sizeof(input), &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != sizeof(input)) return -KERNEL_EFAULT;
            if (input.family != LINUX_AF_INET) return -KERNEL_EINVAL;
            membership.group = input.group;
            membership.index = input.index;
        }
        return kernel_socket_ipv4_membership(socket,membership.group,membership.address,
            membership.index,name==36 || name==45);
    }
    if (!get) {
        if (name == 32) { /* IP_MULTICAST_IF */
            struct { uint32_t group, address; int32_t index; } selection = {0};
            if (length < 4) return -KERNEL_EINVAL;
            size_t input = length >= 12 ? 12U : length >= 8 ? 8U : 4U;
            void *target = input == 4 ? (void *)&selection.address : (void *)&selection;
            if (kernel_copy_from_user(mm, target, request->arguments[3], input, &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != input) return -KERNEL_EFAULT;
            return kernel_socket_set_multicast_interface(socket,
                (uint32_t)selection.index, selection.address);
        }
        if (length >= (int32_t)sizeof(value)) {
            if (kernel_copy_from_user(mm, &value, request->arguments[3], sizeof(value), &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != sizeof(value)) return -KERNEL_EFAULT;
        } else if (length) {
            unsigned char byte;
            if (kernel_copy_from_user(mm, &byte, request->arguments[3], sizeof(byte), &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != sizeof(byte)) return -KERNEL_EFAULT;
            value = byte;
        }
        if (!length) return -KERNEL_EINVAL;
        return kernel_socket_set_option(socket, option, value);
    }
    int result = name == 32 ? kernel_socket_get_multicast_interface(socket, &address) :
        kernel_socket_get_option(socket, option, &value);
    if (result) return result;
    unsigned char byte = (unsigned char)value;
    const void *output = name == 32 ? (const void *)&address : (const void *)&value;
    if (name != 32 && length > 0 && length < (int32_t)sizeof(value) && value >= 0 && value <= 255) {
        length = 1;
        output = &byte;
    } else if (length > (int32_t)sizeof(value)) length = sizeof(value);
    if (kernel_copy_to_user(mm, request->arguments[4], &length, sizeof(length), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    if (length && (kernel_copy_to_user(mm, request->arguments[3], output, (uint32_t)length, &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != (uint32_t)length)) return -KERNEL_EFAULT;
    return 0;
}

/* Linux IPv6 uses int scalar inputs (not SOL_IP's byte form), with a
 * common initial int read even for membership requests. NULL scalar input
 * means zero; memberships still import the complete ipv6_mreq afterward. */
static int ipv6_multicast_sockopt(struct kernel_socket *socket, struct kernel_mm *mm,
    const struct kernel_syscall_request *request, int get)
{
    int name=(int32_t)request->arguments[2];
    int32_t length=(int32_t)request->arguments[4];
    size_t copied=0;
    if(!get && length<0)return -KERNEL_EINVAL;
    if(kernel_socket_family(socket)!=KERNEL_SOCKET_AF_INET6)
        return get || kernel_socket_is_unix(socket)?-KERNEL_EOPNOTSUPP:-KERNEL_ENOPROTOOPT;
    enum kernel_socket_option option=name==18?KERNEL_SOCKET_IPV6_MULTICAST_HOPS:
        name==19?KERNEL_SOCKET_IPV6_MULTICAST_LOOP:KERNEL_SOCKET_IPV6_MULTICAST_ALL;
    int value=0;
    if(get) {
        if(kernel_copy_from_user(mm,&length,request->arguments[4],sizeof(length),&copied)!=
                KERNEL_UACCESS_STATUS_OK || copied!=sizeof(length))return -KERNEL_EFAULT;
        if(name==20 || name==21)return -KERNEL_ENOPROTOOPT;
        int result;
        if(name==17) {
            uint32_t index=0;
            result=kernel_socket_get_ipv6_multicast_interface(socket,&index);
            value=(int32_t)index;
        } else result=kernel_socket_get_option(socket,option,&value);
        if(result)return result;
        /* min_t(unsigned int, sizeof(int), len), including negative len. */
        uint32_t output=(uint32_t)length<sizeof(value)?(uint32_t)length:sizeof(value);
        if(kernel_copy_to_user(mm,request->arguments[4],&output,sizeof(output),&copied)!=
                KERNEL_UACCESS_STATUS_OK || copied!=sizeof(output))return -KERNEL_EFAULT;
        if(output && (kernel_copy_to_user(mm,request->arguments[3],&value,output,&copied)!=
                KERNEL_UACCESS_STATUS_OK || copied!=output))return -KERNEL_EFAULT;
        return 0;
    }
    if(request->arguments[3] && length>=(int32_t)sizeof(value) &&
       (kernel_copy_from_user(mm,&value,request->arguments[3],sizeof(value),&copied)!=
            KERNEL_UACCESS_STATUS_OK || copied!=sizeof(value)))return -KERNEL_EFAULT;
    if(name==20 || name==21) {
        struct { uint8_t address[16];uint32_t index; } membership;
        _Static_assert(sizeof(membership)==20,"Linux ipv6_mreq layout");
        if(length<(int32_t)sizeof(membership))return -KERNEL_EINVAL;
        if(kernel_socket_is_tcp(socket))return -KERNEL_EPROTO;
        if(kernel_copy_from_user(mm,&membership,request->arguments[3],sizeof(membership),&copied)!=
                KERNEL_UACCESS_STATUS_OK || copied!=sizeof(membership))return -KERNEL_EFAULT;
        return kernel_socket_ipv6_membership(socket,membership.address,membership.index,name==21);
    }
    if((name==17 || name==18) && kernel_socket_is_stream(socket))return -KERNEL_ENOPROTOOPT;
    if(length<(int32_t)sizeof(value))return -KERNEL_EINVAL;
    if(name==17)return kernel_socket_set_ipv6_multicast_interface(socket,(uint32_t)value);
    return kernel_socket_set_option(socket,option,value);
}

static int packet_sockopt(struct kernel_socket *socket, struct kernel_mm *mm,
    const struct kernel_syscall_request *request, unsigned name, int get, int32_t length)
{
    if (kernel_socket_family(socket)!=KERNEL_SOCKET_AF_PACKET) return -KERNEL_ENOPROTOOPT;
    uint64_t address=request->arguments[3];
    size_t copied;
    if (!get) {
        if (name==KERNEL_PACKET_RX_RING) {
            struct kernel_socket_packet_ring_request ring;
            _Static_assert(sizeof(ring)==28,"native tpacket_req3 size");
            if ((uint32_t)length<sizeof(ring)) return -KERNEL_EINVAL;
            if (kernel_copy_from_user(mm,&ring,address,sizeof(ring),&copied) ||
                copied!=sizeof(ring)) return -KERNEL_EFAULT;
            return kernel_socket_packet_ring_configure(socket,&ring);
        }
        if (name!=KERNEL_PACKET_VERSION && name!=KERNEL_PACKET_RESERVE)
            return -KERNEL_ENOPROTOOPT;
        int32_t value;
        if (length<(int32_t)sizeof(value)) return -KERNEL_EINVAL;
        if (kernel_copy_from_user(mm,&value,address,sizeof(value),&copied) ||
            copied!=sizeof(value)) return -KERNEL_EFAULT;
        return kernel_socket_packet_option_set(socket,name,value);
    }
    uint32_t selection=0, values[3];
    unsigned count;
    if (name==KERNEL_PACKET_HDRLEN) {
        if (length<(int32_t)sizeof(selection)) return -KERNEL_EINVAL;
        if (kernel_copy_from_user(mm,&selection,address,sizeof(selection),&copied) ||
            copied!=sizeof(selection)) return -KERNEL_EFAULT;
    }
    int error=kernel_socket_packet_option_get(socket,name,selection,values,&count);
    if (error) return error;
    if ((uint32_t)length>count*sizeof(values[0])) length=(int32_t)(count*sizeof(values[0]));
    if (kernel_copy_to_user(mm,request->arguments[4],&length,sizeof(length),&copied) ||
        copied!=sizeof(length)) return -KERNEL_EFAULT;
    if (length && (kernel_copy_to_user(mm,address,values,(size_t)length,&copied) ||
        copied!=(size_t)length)) return -KERNEL_EFAULT;
    return 0;
}

static int sockopt(struct kernel_socket *socket, struct kernel_files *files, struct kernel_mm *mm,
                   const struct kernel_syscall_request *request, int get)
{
    int level = (int32_t)request->arguments[1];
    int name = (int32_t)request->arguments[2];
    int32_t length = (int32_t)request->arguments[4];
    size_t copied = 0;
    union { int value; struct linux_timeval timeout;
            struct kernel_socket_peercred peercred; } payload = {0};
    size_t size = sizeof(int);
    int timed = level == LINUX_SOL_SOCKET &&
                (name == LINUX_SO_RCVTIMEO || name == LINUX_SO_SNDTIMEO);
    int peercred = get && level == LINUX_SOL_SOCKET && name == LINUX_SO_PEERCRED;
    enum kernel_socket_option option = KERNEL_SOCKET_TYPE;
    if(level==41 && ((name>=17 && name<=21) || name==29))
        return ipv6_multicast_sockopt(socket,mm,request,get);
    if (get) {
        /* TCP delegates other levels to the IP family; a foreign level is
         * rejected before that family imports optlen. */
        if (level != LINUX_SOL_SOCKET && level != LINUX_IPPROTO_TCP &&
            level != 0 && kernel_socket_is_tcp(socket) &&
            (level != 41 || kernel_socket_family(socket) != LINUX_AF_INET6))
            return -KERNEL_EOPNOTSUPP;
        if (kernel_copy_from_user(mm, &length, request->arguments[4], sizeof(length), &copied) !=
                KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    }
    if (length < 0) return -KERNEL_EINVAL;
    if (level == LINUX_SOL_PACKET) return packet_sockopt(socket,mm,request,(unsigned)name,get,length);
    /* TCP publishes optlen before touching optval (Linux tcp.c). */
    int tcp_extended = level == LINUX_IPPROTO_TCP &&
        ((name >= 4 && name <= 6) || name == 11);
    if (tcp_extended) {
        if (!kernel_socket_is_tcp(socket))
            return get || kernel_socket_is_unix(socket) ? -KERNEL_EOPNOTSUPP : -KERNEL_ENOPROTOOPT;
        if (get) {
            int value,error;struct kernel_socket_tcp_info info;
            uint32_t capacity = (uint32_t)length, output;
            const void *data;
            if(name==11) {
                error=kernel_socket_tcp_info(socket,&info);data=&info;output=sizeof(info);
            } else {
                error=socket_option(level,name,&option);
                if(!error)error=kernel_socket_get_option(socket,option,&value);
                data=&value;output=sizeof(value);
            }
            if (error) return error;
            if (output > capacity) output = capacity;
            if (kernel_copy_to_user(mm, request->arguments[4], &output, sizeof(output), &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != sizeof(output)) return -KERNEL_EFAULT;
            if (output && (kernel_copy_to_user(mm, request->arguments[3], data, output, &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != output)) return -KERNEL_EFAULT;
            return 0;
        }
    }
    if(level==LINUX_IPPROTO_TCP && name==11) {
        int value;
        if(length<(int32_t)sizeof(value))return -KERNEL_EINVAL;
        if(kernel_copy_from_user(mm,&value,request->arguments[3],sizeof(value),&copied)!=
            KERNEL_UACCESS_STATUS_OK || copied!=sizeof(value))return -KERNEL_EFAULT;
        return -KERNEL_ENOPROTOOPT;
    }
    if (level == LINUX_SOL_SOCKET && name == 25)
        return device_sockopt(socket, mm, request, get, length);
    if (level == 0 && (name == 2 || (name >= 32 && name <= 36) ||
            name == 42 || name == 45 || name == 49))
        return ipv4_sockopt(socket, mm, request, get, length);
    if(level==LINUX_SOL_SOCKET && (name==26 || name==27 || name==44 || name==50)) {
        /* Classic-program import/export holds separate heap/program owners
         * across user faults. Keep their existing service lifetime. */
        KERNEL_SERVICE_SCOPE(service);
        return filter_sockopt(socket,files,mm,request,get,length);
    }
    if (timed) size = sizeof(payload.timeout);
    else if (peercred) size = sizeof(payload.peercred);
    else {
        int result = socket_option(level, name, &option);
        if (result != 0) return result;
    }
    if (!get) {
        if ((uint32_t)length < size) return -KERNEL_EINVAL;
        if (kernel_copy_from_user(mm, &payload, request->arguments[3], size, &copied) !=
                KERNEL_UACCESS_STATUS_OK || copied != size) return -KERNEL_EFAULT;
        if (!timed) return kernel_socket_set_option(socket, option, payload.value);
        if (payload.timeout.seconds < 0 || payload.timeout.microseconds < 0 ||
            payload.timeout.microseconds >= 1000000 ||
            (uint64_t)payload.timeout.seconds >
                (UINT64_MAX - (uint64_t)payload.timeout.microseconds * 1000U) /
                    UINT64_C(1000000000)) return -KERNEL_EINVAL;
        uint64_t ns = (uint64_t)payload.timeout.seconds * UINT64_C(1000000000) +
                      (uint64_t)payload.timeout.microseconds * 1000U;
        if (name == LINUX_SO_RCVTIMEO) kernel_socket_set_receive_timeout(socket, ns);
        else kernel_socket_set_send_timeout(socket, ns);
        return 0;
    }
    if (timed) {
        uint64_t ns = name == LINUX_SO_RCVTIMEO ? kernel_socket_receive_timeout(socket)
                                              : kernel_socket_send_timeout(socket);
        payload.timeout.seconds = (int64_t)(ns / UINT64_C(1000000000));
        payload.timeout.microseconds = (int64_t)((ns % UINT64_C(1000000000)) / 1000U);
    } else if (peercred) {
        int result = kernel_socket_peercred(socket, &payload.peercred);
        if (result != 0) return result;
    } else {
        int result = kernel_socket_get_option(socket, option, &payload.value);
        if (result != 0) return result;
    }
    size_t written = (uint32_t)length < size ? (uint32_t)length : size;
    if (kernel_copy_to_user(mm, request->arguments[3], &payload, written, &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != written) return -KERNEL_EFAULT;
    length = (int32_t)written;
    if (kernel_copy_to_user(mm, request->arguments[4], &length, sizeof(length), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(length)) return -KERNEL_EFAULT;
    return 0;
}

static int wait_ready(struct kernel_open_file_description *file,
                      uint32_t event, uint64_t timeout_ns, uint32_t flags)
{
    uint64_t target = 0;
    if (timeout_ns) {
        uint64_t now = kernel_time_monotonic_ns();
        target = UINT64_MAX - now < timeout_ns ? UINT64_MAX : now + timeout_ns;
    }
    if (kernel_open_file_flags(file) & LINUX_SOCK_NONBLOCK) flags |= KERNEL_SOCKET_MSG_DONTWAIT;
    return kernel_socket_wait_ready(kernel_open_file_socket(file), event, target, flags);
}

static int socket_creation_error(int family, int type, int protocol)
{
    if (((type & ~LINUX_SOCK_TYPE_MASK) &
         ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK)) != 0) {
        return -KERNEL_EINVAL;
    }
    int base_type = type & LINUX_SOCK_TYPE_MASK;
    if (family < 0 || family >= LINUX_AF_MAX) {
        return -KERNEL_EAFNOSUPPORT;
    }
    if (base_type >= LINUX_SOCK_MAX) {
        return -KERNEL_EINVAL;
    }
    if (family != LINUX_AF_UNIX && family != LINUX_AF_INET && family != LINUX_AF_INET6 && family != LINUX_AF_NETLINK && family != LINUX_AF_PACKET) {
        return -KERNEL_EAFNOSUPPORT;
    }
    if (family==LINUX_AF_PACKET) {
        if (!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_RAW))
            return -KERNEL_EPERM;
        return base_type==LINUX_SOCK_RAW || base_type==LINUX_SOCK_DGRAM ?
            0 : -KERNEL_ESOCKTNOSUPPORT;
    }
    if(family==LINUX_AF_NETLINK) {
        if(base_type!=LINUX_SOCK_RAW && base_type!=LINUX_SOCK_DGRAM)return -KERNEL_ESOCKTNOSUPPORT;
        return protocol==0?0:-KERNEL_EPROTONOSUPPORT;
    }
    if (family == LINUX_AF_UNIX) {
        if (protocol != 0 && protocol != LINUX_AF_UNIX) {
            return -KERNEL_EPROTONOSUPPORT;
        }
    } else if (protocol < 0 || protocol >= LINUX_IPPROTO_MAX) {
        return -KERNEL_EINVAL;
    }
    if (family != LINUX_AF_UNIX && base_type == LINUX_SOCK_RAW && protocol == 0) {
        return -KERNEL_EPROTONOSUPPORT;
    }
    if (family == LINUX_AF_INET && base_type == LINUX_SOCK_RAW) {
        if (protocol != LINUX_IPPROTO_ICMP) return -KERNEL_EPROTONOSUPPORT;
        return kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_NET_RAW) ?
            0 : -KERNEL_EPERM;
    }
    if (family != LINUX_AF_UNIX && ((base_type == LINUX_SOCK_STREAM &&
         protocol != 0 && protocol != LINUX_IPPROTO_TCP) ||
        (base_type == LINUX_SOCK_DGRAM &&
         protocol != 0 && protocol != LINUX_IPPROTO_UDP))) {
        return -KERNEL_EPROTONOSUPPORT;
    }
    if (base_type != LINUX_SOCK_STREAM && base_type != LINUX_SOCK_DGRAM &&
        !(family == LINUX_AF_UNIX && base_type == LINUX_SOCK_SEQPACKET)) {
        return -KERNEL_ESOCKTNOSUPPORT;
    }
    return 0;
}

enum kernel_syscall_status syscall_handle_socket(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    int family = (int32_t)request->arguments[0];
    int type = (int32_t)request->arguments[1];
    int protocol = (int32_t)request->arguments[2];
    int base_type;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    int error = socket_creation_error(family, type, protocol);
    if (error) {
        decoded->value = error;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    base_type = type & LINUX_SOCK_TYPE_MASK;
    struct kernel_files *files;
    enum kernel_task_status task_status =
        kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->value = -KERNEL_EMFILE;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_files_socket_create(files, family, base_type, protocol,
                                   (uint32_t)type, &decoded->value) !=
            KERNEL_FILES_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_socketpair(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    int family = (int32_t)request->arguments[0];
    int type = (int32_t)request->arguments[1];
    int protocol = (int32_t)request->arguments[2];
    uint64_t user_sv = request->arguments[3];
    int base_type;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (((type & ~LINUX_SOCK_TYPE_MASK) &
         ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK)) != 0) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    base_type = type & LINUX_SOCK_TYPE_MASK;
    int creation_error = socket_creation_error(family, type, protocol);
    if (!creation_error && family != LINUX_AF_UNIX)
        creation_error = -KERNEL_EOPNOTSUPP;
    struct kernel_files *files;
    struct kernel_mm *mm;
    enum kernel_task_status task_status =
        kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->value = -KERNEL_EMFILE;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK ||
        kernel_files_socketpair_create(files, mm, base_type,
                                       (uint32_t)type, creation_error, user_sv,
                                       &decoded->value) !=
            KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_bind(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_files *files;
    struct kernel_open_file_description *file = 0;
    struct kernel_mm *mm;
    struct kernel_socket_address address;
    union linux_sockaddr_storage snapshot = {0};
    int64_t linux_result = 0;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (borrow_socket(caller, (int32_t)request->arguments[0],
                      &files, &file, 0, &linux_result) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    (void)files;
    if (linux_result != 0) {
        decoded->value = linux_result;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    linux_result = copy_address_in(mm, request->arguments[1],
                                   request->arguments[2], &snapshot,
                                   kernel_socket_is_unix(kernel_open_file_socket(file)),
                                   &address);
    if (linux_result == 0)
        linux_result = kernel_socket_bind(kernel_open_file_socket(file),
                                          &address);
    if (release_socket(&file) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

/* Import metadata is dead once packet admission has consumed its name.
 * Reuse that storage for the request held across the payload copy. */
union socket_packet_phase {
    struct {
        struct linux_msghdr header;
        union linux_sockaddr_storage name;
    } import;
    struct kernel_socket_write_request write;
};

/* The name view is valid only during import. Packet admission resolves it
 * to a borrowed endpoint before this frame leaves the payload copy path. */
static __attribute__((noinline)) enum kernel_files_status socket_prepare_message_packet(
    struct kernel_socket_call *call,struct kernel_mm *mm,uint64_t user_header,
    uint32_t *flags,int batch,struct kernel_uaccess_iov_snapshot *snapshot,
    union socket_packet_phase *phase,int64_t *result)
{
    struct kernel_files *files=call->files;
    struct kernel_open_file_description **file=call->socket_owner;
    struct kernel_socket *socket=kernel_open_file_socket(*file);
    int unix_socket=kernel_socket_is_unix(socket);
    int seqpacket=kernel_socket_is_seqpacket(socket);
    uint32_t allowed=KERNEL_SOCKET_MSG_DONTWAIT|KERNEL_SOCKET_MSG_NOSIGNAL;
    if(batch)allowed|=KERNEL_SOCKET_MSG_EOR;
    if(kernel_socket_is_udp(socket))allowed|=KERNEL_SOCKET_MSG_MORE;
    if(seqpacket)allowed|=KERNEL_SOCKET_MSG_EOR|KERNEL_SOCKET_MSG_MORE;
    struct linux_msghdr *header=&phase->import.header;
    size_t copied=0;
    *result=-KERNEL_EFAULT;
    if(kernel_copy_from_user(mm,header,user_header,sizeof(*header),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(*header))return KERNEL_FILES_STATUS_OK;
    if(batch)*flags|=(uint32_t)header->flags&KERNEL_SOCKET_MSG_EOR;
    if((*flags&~allowed) && !seqpacket) {*result=-KERNEL_EOPNOTSUPP;return KERNEL_FILES_STATUS_OK;}
    if(!header->name)header->name_length=0;
    if(header->name_length<0) {*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    if(header->name_length>LINUX_SOCKADDR_STORAGE_SIZE)header->name_length=LINUX_SOCKADDR_STORAGE_SIZE;
    phase->import.name=(union linux_sockaddr_storage){0};
    struct kernel_socket_address address={0};
    const struct kernel_socket_address *destination=NULL;
    if(header->name && header->name_length) {
        *result=copy_address_snapshot(mm,header->name,(uint32_t)header->name_length,&phase->import.name);
        if(*result)return KERNEL_FILES_STATUS_OK;
    }
    if(header->iov_count>1024) {*result=-KERNEL_EMSGSIZE;return KERNEL_FILES_STATUS_OK;}
    call->message.flags=*flags;
    enum kernel_files_status status=kernel_uaccess_iov_import(snapshot,files->heap,mm,header->iov,header->iov_count,result);
    if(status || *result)return status;
    if(header->control_length) {
        if(!unix_socket) {*result=-KERNEL_EOPNOTSUPP;return KERNEL_FILES_STATUS_OK;}
        *result=kernel_socket_control_copy(call,mm,header->control,header->control_length);
        if(*result)return KERNEL_FILES_STATUS_OK;
    }
    *result=kernel_socket_prepare_send(socket);if(*result)return KERNEL_FILES_STATUS_OK;
    *result=kernel_socket_control_parse(call,header->control_length);if(*result)return KERNEL_FILES_STATUS_OK;
    if(*flags&~allowed) {*result=-KERNEL_EOPNOTSUPP;return KERNEL_FILES_STATUS_OK;}
    if(header->name && header->name_length && !seqpacket) {
        *result=parse_address(&phase->import.name,header->name_length,unix_socket,&address);
        if(*result)return KERNEL_FILES_STATUS_OK;
        destination=&address;
    }
    if(kernel_open_file_flags(*file)&LINUX_SOCK_NONBLOCK)*flags|=KERNEL_SOCKET_MSG_DONTWAIT;
#if BOAROS_USER_SMP
    if(batch>1 && kernel_socket_is_udp(socket))*flags|=KERNEL_SOCKET_IO_MORE;
#endif
    *result=kernel_socket_packet_prepare(&phase->write,file,snapshot->total,*flags,destination,0,&call->message);
    return KERNEL_FILES_STATUS_OK;
}

static __attribute__((noinline)) enum kernel_syscall_status socket_send_message_packet(
    struct kernel_socket_call *call,struct kernel_mm *mm,uint64_t user_header,
    uint32_t flags,int batch,uint64_t *requested,int64_t *result)
{
    struct kernel_uaccess_iov_snapshot snapshot={0};
    union socket_packet_phase phase;
    call->iov_snapshot=&snapshot;
    flags&=~UINT32_C(0x80000000);
    enum kernel_files_status status=socket_prepare_message_packet(call,mm,user_header,&flags,
        batch,&snapshot,&phase,result);
    if(requested)*requested=snapshot.total;
    if(!status && !*result) {
#if BOAROS_USER_SMP
        if (batch && kernel_socket_is_udp(phase.write.socket))
            phase.write.udp_batch_interface=&call->udp_batch_interface;
#endif
        struct kernel_uaccess_iov_cursor cursor={snapshot.view,snapshot.count,0,0};
        *result=kernel_socket_packet_write(&phase.write,mm,&cursor,NULL,NULL,&call->message);
    }
    if(batch && status==KERNEL_FILES_STATUS_OK && *result>=0) {
        uint32_t length=(uint32_t)*result;size_t copied=0;
        if(kernel_copy_to_user(mm,user_header+offsetof(struct linux_mmsghdr,length),
            &length,sizeof(length),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(length))*result=-KERNEL_EFAULT;
    }
    kernel_socket_call_message_end(call);
    return status==KERNEL_FILES_STATUS_OK?KERNEL_SYSCALL_STATUS_OK:KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
}

static enum kernel_syscall_status socket_message_pinned(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **file, uint64_t user_header,
    uint32_t flags, int writing, int batch, uint64_t *requested, int64_t *result,
    struct kernel_socket_call *call)
{
    if(writing && kernel_socket_preserves_messages(kernel_open_file_socket(*file)))
        return socket_send_message_packet(call,mm,user_header,flags,batch,requested,result);
    /* 固定RV/LA Linux未配置COMPAT，最高位不选择32位控制消息布局。 */
    flags&=~UINT32_C(0x80000000);
    struct linux_msghdr header;
    struct kernel_uaccess_iov_snapshot snapshot={0};
    struct kernel_socket *socket=kernel_open_file_socket(*file);
    int unix_socket=kernel_socket_is_unix(socket);
    int seqpacket=kernel_socket_is_seqpacket(socket);
    uint32_t allowed=KERNEL_SOCKET_MSG_DONTWAIT | (writing?KERNEL_SOCKET_MSG_NOSIGNAL:
        KERNEL_SOCKET_MSG_TRUNC|UINT32_C(0x40000000)|KERNEL_SOCKET_MSG_PEEK);
    if (writing && batch) allowed|=KERNEL_SOCKET_MSG_EOR;
    if (kernel_socket_is_tcp(socket)) allowed |= writing?KERNEL_SOCKET_MSG_MORE:KERNEL_SOCKET_MSG_WAITALL;
    if (seqpacket)
        allowed |= writing?(KERNEL_SOCKET_MSG_EOR|KERNEL_SOCKET_MSG_MORE):KERNEL_SOCKET_MSG_WAITALL;
    if(!writing && kernel_socket_is_netlink(socket))
        allowed|=KERNEL_SOCKET_MSG_WAITALL|KERNEL_SOCKET_MSG_NOSIGNAL;
    enum kernel_files_status status=KERNEL_FILES_STATUS_OK;
    size_t copied=0;
    struct kernel_socket_address address={0};
    call->address=&address;
    call->iov_snapshot=&snapshot;
    *result=-KERNEL_EFAULT;
    if (kernel_copy_from_user(mm,&header,user_header,sizeof(header),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(header)) goto out;
    if (writing && batch) flags |= (uint32_t)header.flags & KERNEL_SOCKET_MSG_EOR;
    if (writing && (flags&~allowed) && !seqpacket) { *result=-KERNEL_EOPNOTSUPP; goto out; }
    if (!header.name) header.name_length=0;
    if (header.name_length<0) { *result=-KERNEL_EINVAL; goto out; }
    if (header.name_length>LINUX_SOCKADDR_STORAGE_SIZE) header.name_length=LINUX_SOCKADDR_STORAGE_SIZE;
    union linux_sockaddr_storage name_snapshot={0};
    if (writing && header.name && header.name_length) {
        *result=copy_address_snapshot(mm,header.name,(uint32_t)header.name_length,&name_snapshot);
        if (*result) goto out;
    }
    if (header.iov_count>1024) { *result=-KERNEL_EMSGSIZE; goto out; }
    call->message.flags=flags;
    status=kernel_uaccess_iov_import(&snapshot,files->heap,mm,header.iov,header.iov_count,result);
    if (status || *result) goto out;
    if (writing && header.control_length) {
        if (!unix_socket && !kernel_socket_is_tcp(socket)) { *result=-KERNEL_EOPNOTSUPP; goto out; }
        *result=kernel_socket_control_copy(call,mm,header.control,header.control_length);
        if (*result) goto out;
        if (!unix_socket) {
            *result=kernel_socket_control_parse_tcp(call,header.control_length);
            if (*result) goto out;
        }
    }
    if (writing) {
        *result=kernel_socket_prepare_send(socket);
        if (*result) goto out;
        if (unix_socket) {
            *result=kernel_socket_control_parse(call,header.control_length);
            if (*result) goto out;
        }
    }
    if (flags&~allowed) {
        if (!writing) {
            if ((flags & (KERNEL_SOCKET_MSG_OOB | KERNEL_SOCKET_MSG_ERRQUEUE)) &&
                kernel_socket_is_tcp(socket)) {
                *result=kernel_socket_tcp_receive_control(socket,flags);
                goto out;
            }
            *result=kernel_socket_prepare_receive(socket);
            if (*result) goto out;
        }
        *result=-KERNEL_EOPNOTSUPP;goto out;
    }
    if (writing && header.name && header.name_length && !seqpacket) {
        *result=parse_address(&name_snapshot,header.name_length,unix_socket,&address);
        if (*result) goto out;
    }
    uint64_t count=snapshot.total;
    if (requested) *requested=count;
    uint32_t message_size=0;
    status=kernel_files_socket_iov_io(files,mm,file,snapshot.view,snapshot.count,count,
        flags|KERNEL_SOCKET_IO_MESSAGE,writing,!writing && header.name?&address:0,
        &message_size,result,&call->message);
    if (status || writing) goto out;
    if (*result==-KERNEL_EINTR || *result==-KERNEL_ERESTARTSYS) goto out;
    if (*result<0 && call->message.receive_state!=KERNEL_SOCKET_RECEIVE_STREAM_EMPTY &&
        call->message.receive_state!=KERNEL_SOCKET_RECEIVE_STREAM_FAULT) goto out;
    header.flags=(flags&UINT32_C(0x40000000)) | (message_size>count?KERNEL_SOCKET_MSG_TRUNC:0);
    if ((flags&KERNEL_SOCKET_MSG_TRUNC) && kernel_socket_preserves_messages(socket)) *result=message_size;
    uint64_t control_used=0;
    if (unix_socket) {
        int passcred=0;
        if (kernel_socket_get_option(socket,KERNEL_SOCKET_PASSCRED,&passcred)) __builtin_trap();
        uint32_t output_flags=(uint32_t)header.flags;
        kernel_socket_control_export(call,mm,header.control,header.control_length,flags,passcred,&control_used,&output_flags);
        header.flags=(int32_t)output_flags;
    }
    if (*result<0) goto out;
    header.control_length=control_used;
    if (header.name) {
        if (!unix_socket && !kernel_socket_preserves_messages(socket)) {
            int error=kernel_socket_getpeer(socket,&address);
            if (error) { *result=error; goto out; }
        }
        if (address.family) {
            int error=copy_address_out(mm,header.name,user_header+offsetof(struct linux_msghdr,name_length),&address);
            if (error) { *result=error; goto out; }
        } else {
            header.name_length=0;
            if (kernel_copy_to_user(mm,user_header+offsetof(struct linux_msghdr,name_length),
                    &header.name_length,sizeof(header.name_length),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(header.name_length)) {
                *result=-KERNEL_EFAULT; goto out;
            }
        }
    }
    /* Linux publishes name, flags, then controllen after protocol consumption. */
    if (kernel_copy_to_user(mm,user_header+offsetof(struct linux_msghdr,flags),&header.flags,sizeof(header.flags),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(header.flags) ||
        kernel_copy_to_user(mm,user_header+offsetof(struct linux_msghdr,control_length),&header.control_length,sizeof(header.control_length),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(header.control_length)) *result=-KERNEL_EFAULT;
out:
    /* msg_len is published after protocol and ancillary effects, and before
     * ending the registered owner that protects faulting output copies. */
    if (batch && status==KERNEL_FILES_STATUS_OK && *result>=0) {
        uint32_t length=(uint32_t)*result;
        if (kernel_copy_to_user(mm,user_header+offsetof(struct linux_mmsghdr,length),
                &length,sizeof(length),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(length))
            *result=-KERNEL_EFAULT;
    }
    kernel_socket_call_message_end(call);
    return status==KERNEL_FILES_STATUS_OK ? KERNEL_SYSCALL_STATUS_OK :
        KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
}

enum kernel_syscall_status syscall_handle_socket_message(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    struct kernel_open_file_description *file=0;
    int64_t result=0;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if (borrow_socket(caller,(int32_t)request->arguments[0],&files,&file,0,&result)!=KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (result) { decoded->value=result; return KERNEL_SYSCALL_STATUS_OK; }
    if (kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK) {
        release_socket(&file);
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    struct kernel_socket_call call={0};
    kernel_socket_call_begin(&call,files,&file);
    enum kernel_syscall_status status=socket_message_pinned(files,mm,&file,
        request->arguments[1],(uint32_t)request->arguments[2],request->number==211,
        0,0,&result,&call);
    kernel_socket_call_end(&call);
    if (release_socket(&file)!=KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value=result;
    return status;
}

enum kernel_syscall_status syscall_handle_socket_mmessage(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    struct kernel_open_file_description *file=0;
    int writing=request->number==269;
    uint32_t count=(uint32_t)request->arguments[2];
    uint32_t flags=(uint32_t)request->arguments[3];
    uint64_t user_timeout=writing?0:request->arguments[4];
    struct linux_timespec timeout={0}, end={0};
    size_t copied=0;
    int64_t result=0;
    unsigned completed=0;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=-KERNEL_EINVAL;
    if (kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (user_timeout) {
        decoded->value=-KERNEL_EFAULT;
        if (kernel_copy_from_user(mm,&timeout,user_timeout,sizeof(timeout),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(timeout))
            return KERNEL_SYSCALL_STATUS_OK;
        decoded->value=-KERNEL_EINVAL;
        if (timeout.seconds<0 || timeout.nanoseconds<0 || timeout.nanoseconds>=1000000000)
            return KERNEL_SYSCALL_STATUS_OK;
        uint64_t now=kernel_time_monotonic_ns();
        int64_t seconds=(int64_t)(now/UINT64_C(1000000000));
        end.nanoseconds=timeout.nanoseconds+(int64_t)(now%UINT64_C(1000000000));
        seconds+=end.nanoseconds/1000000000;
        end.nanoseconds%=1000000000;
        if (timeout.seconds>INT64_MAX-seconds) {
            end.seconds=INT64_MAX; end.nanoseconds=999999999;
        } else end.seconds=timeout.seconds+seconds;
    }
    if (writing && count>1024) count=1024;
    if (borrow_socket(caller,(int32_t)request->arguments[0],&files,&file,0,&result)!=KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (result) { decoded->value=result; return KERNEL_SYSCALL_STATUS_OK; }
    struct kernel_socket_call call={0};
    kernel_socket_call_begin(&call,files,&file);
    struct kernel_socket *socket=kernel_open_file_socket(file);
    if (!writing && !(flags&UINT32_C(0x2000))) {
        int error=0;
        if (kernel_socket_get_option(socket,KERNEL_SOCKET_ERROR,&error)) __builtin_trap();
        if (error) { result=-error; goto out; }
    }
    while (completed<count) {
        uint64_t requested=0;
        uint64_t entry=request->arguments[1]+(uint64_t)completed*sizeof(struct linux_mmsghdr);
        int batch=1;
#if BOAROS_USER_SMP
        if (writing && completed+1<count) batch=2;
#endif
        if (socket_message_pinned(files,mm,&file,entry,
                writing?flags:(flags&~UINT32_C(0x10000)),writing,batch,&requested,&result,&call)!=KERNEL_SYSCALL_STATUS_OK) {
            kernel_socket_call_end(&call);
            release_socket(&file);
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        if (result<0) break;
        ++completed;
        if (writing) {
            if ((uint64_t)result<requested) break;
        } else {
            if (flags&UINT32_C(0x10000)) flags|=KERNEL_SOCKET_MSG_DONTWAIT;
            /* Linux checks this deadline only after a completed message;
             * the blocking receive itself still uses SO_RCVTIMEO. */
            if (user_timeout) {
                uint64_t now=kernel_time_monotonic_ns();
                timeout.seconds=end.seconds-(int64_t)(now/UINT64_C(1000000000));
                timeout.nanoseconds=end.nanoseconds-(int64_t)(now%UINT64_C(1000000000));
                if (timeout.nanoseconds<0) { --timeout.seconds; timeout.nanoseconds+=1000000000; }
                if (timeout.seconds<0 || (!timeout.seconds && !timeout.nanoseconds)) {
                    timeout=(struct linux_timespec){0};
                    break;
                }
            }
        }
    }
    if (completed && result==-KERNEL_ERESTARTSYS)
        kernel_signal_clear_syscall_restart(caller);
    if (!writing && completed && result<0 && result!=-KERNEL_EAGAIN)
        kernel_socket_save_error(socket,(int)result);
out:
    kernel_socket_call_end(&call);
    if (release_socket(&file)!=KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value=completed?(int64_t)completed:result;
    if (user_timeout && completed &&
        (kernel_copy_to_user(mm,user_timeout,&timeout,sizeof(timeout),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(timeout)))
        decoded->value=-KERNEL_EFAULT;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status socket_operation_owned(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **file_owner,
    const struct kernel_syscall_request *request, uint64_t range_size,
    int force_nonblock, int64_t *output)
{
    int op = (int)request->number;
    int local = (unsigned)op - 204U <= 5U;
    KERNEL_SERVICE_SCOPE_IF(service, force_nonblock && !local);
    struct kernel_socket *socket = kernel_open_file_socket(*file_owner);
    if (!socket) {
        *output = -KERNEL_ENOTSOCK;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_socket_address address;
    union linux_sockaddr_storage snapshot = {0};
    struct kernel_socket_address peer_address
        __attribute__((cleanup(kernel_socket_address_release))) = {0};
    struct kernel_socket_call data_call __attribute__((cleanup(kernel_socket_call_end))) = {0};
    int64_t result = 0;
    if (local) {
        kernel_socket_call_begin(&data_call, files, file_owner);
        data_call.address = &peer_address;
    }
    switch (op) {
    case 201: /* listen */
        result = kernel_socket_listen(socket, (int32_t)request->arguments[1]);
        break;
    case 204: /* getsockname */
    case 205: /* getpeername */
        result = op == 204 ? kernel_socket_getname(socket, &peer_address)
                           : kernel_socket_getpeer(socket, &peer_address);
        if (result == 0)
            result = copy_address_out(mm, request->arguments[1],
                                      request->arguments[2], &peer_address);
        break;
    case 203: /* connect */
        result = copy_address_in(mm, request->arguments[1],
                                 request->arguments[2], &snapshot,
                                 kernel_socket_is_unix(socket), &address);
        if (result == 0)
            result = kernel_socket_connect(socket, &address,
                     force_nonblock || (kernel_open_file_flags(*file_owner) & LINUX_SOCK_NONBLOCK) != 0U);
        if (result == -KERNEL_EINPROGRESS && !force_nonblock &&
            (kernel_open_file_flags(*file_owner) & LINUX_SOCK_NONBLOCK) == 0U) {
            result = wait_ready(*file_owner, KERNEL_POLLOUT | KERNEL_POLLERR,
                                kernel_socket_send_timeout(socket), 0U);
            if (result == 0) result = kernel_socket_connection_result(socket);
        }
        break;
    case 206: { /* sendto / send */
        uint32_t flags = (uint32_t)request->arguments[3];
        if (force_nonblock) flags |= KERNEL_SOCKET_MSG_DONTWAIT;
        uint32_t allowed=KERNEL_SOCKET_MSG_DONTWAIT | KERNEL_SOCKET_MSG_NOSIGNAL;
        if (kernel_socket_is_tcp(socket) || kernel_socket_is_udp(socket)) allowed|=KERNEL_SOCKET_MSG_MORE;
        if (kernel_socket_is_seqpacket(socket)) allowed|=KERNEL_SOCKET_MSG_EOR|KERNEL_SOCKET_MSG_MORE;
        if ((flags & ~allowed) && !kernel_socket_is_seqpacket(socket)) { result = -KERNEL_EOPNOTSUPP; break; }
        const struct kernel_socket_address *destination = 0;
        if (request->arguments[4] != 0) {
            if (kernel_socket_is_seqpacket(socket)) {
                result=copy_address_snapshot(mm,request->arguments[4],request->arguments[5],&snapshot);
            } else {
                result = copy_address_in(mm, request->arguments[4], request->arguments[5],
                                         &snapshot, kernel_socket_is_unix(socket), &address);
                destination = &address;
            }
            if (result != 0) break;
        }
        if (flags & ~allowed) {
            result=kernel_socket_prepare_send(socket);
            if (!result) result=-KERNEL_EOPNOTSUPP;
            break;
        }
        if (kernel_socket_preserves_messages(socket)) {
            const struct kernel_uaccess_iovec iov = {request->arguments[1], range_size};
            if (kernel_open_file_flags(*file_owner) & LINUX_SOCK_NONBLOCK) flags |= KERNEL_SOCKET_MSG_DONTWAIT;
            result = kernel_socket_write_datagram(file_owner, mm, &iov, 1, iov.length, flags, destination, NULL);
        } else if (kernel_files_socket_io(files, mm, file_owner, request->arguments[1],
                    range_size, flags, 1, &result) != KERNEL_FILES_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        break;
    }
    case 207: { /* recvfrom / recv */
        uint32_t flags = (uint32_t)request->arguments[3];
        if (force_nonblock) flags |= KERNEL_SOCKET_MSG_DONTWAIT;
        data_call.message.flags=flags;
        uint32_t allowed=KERNEL_SOCKET_MSG_DONTWAIT | KERNEL_SOCKET_MSG_TRUNC | KERNEL_SOCKET_MSG_PEEK;
        if (kernel_socket_is_seqpacket(socket) || kernel_socket_is_tcp(socket)) allowed|=KERNEL_SOCKET_MSG_WAITALL;
        if (kernel_socket_is_netlink(socket))allowed|=KERNEL_SOCKET_MSG_WAITALL|KERNEL_SOCKET_MSG_NOSIGNAL;
        if (flags & ~allowed) {
            if ((flags & (KERNEL_SOCKET_MSG_OOB | KERNEL_SOCKET_MSG_ERRQUEUE)) &&
                kernel_socket_is_tcp(socket))
                result=kernel_socket_tcp_receive_control(socket,flags);
            else {
                result=kernel_socket_prepare_receive(socket);
                if (!result) result=-KERNEL_EOPNOTSUPP;
            }
            break;
        }
        const struct kernel_uaccess_iovec iov = {request->arguments[1],range_size};
        uint32_t message_size = 0;
        if (kernel_files_socket_iov_io(files,mm,file_owner,&iov,1,iov.length,
                flags | KERNEL_SOCKET_IO_MESSAGE,0,
                request->arguments[4] ? &peer_address : 0,&message_size,&result,
                (flags & KERNEL_SOCKET_MSG_PEEK)?&data_call.message:NULL) != KERNEL_FILES_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if (result >= 0) {
            if ((flags & KERNEL_SOCKET_MSG_TRUNC) && kernel_socket_preserves_messages(socket)) result = message_size;
            if (!kernel_socket_is_unix(socket) && !kernel_socket_preserves_messages(socket) && request->arguments[4] != 0) {
                int peer_result = kernel_socket_getpeer(socket, &peer_address);
                if (peer_result != 0) result = peer_result;
            }
        }
        if (result >= 0 && request->arguments[4] != 0) {
            if (peer_address.family) {
                int address_result = copy_address_out(mm, request->arguments[4], request->arguments[5], &peer_address);
                if (address_result != 0) result = address_result;
            } else {
                uint32_t no_address=0;size_t copied=0;
                if (kernel_copy_to_user(mm,request->arguments[5],&no_address,sizeof(no_address),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(no_address))
                    result=-KERNEL_EFAULT;
            }
        }
        break;
    }
    case 208: /* setsockopt */
    case 209: /* getsockopt */
        result = sockopt(socket, files, mm, request, op == 209);
        break;
    case 210: /* shutdown */
        result = kernel_socket_shutdown(socket, (int32_t)request->arguments[1]);
        break;
    case 202: /* accept */
    case 242: { /* accept4 */
        struct accept_peer_output output = {
            mm, request->arguments[1], request->arguments[2]
        };
        if (kernel_files_socket_accept_owned(files, *file_owner,
                op == 242 ? (uint32_t)request->arguments[3] : 0U, force_nonblock,
                output.address ? copy_accept_peer : 0, &output, &result) !=
            KERNEL_FILES_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        break;
    }
    default:
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    *output = result;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_socket_operation(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int op = (int)request->number;
    int local = (unsigned)op - 204U <= 5U;
    KERNEL_SERVICE_SCOPE_IF(service, !local);
    struct kernel_files *files;
    struct kernel_open_file_description *file = NULL;
    struct kernel_mm *mm;
    int64_t result = 0;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    uint64_t count = request->arguments[2];
    if (op == 206 || op == 207) {
        if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
        if (kernel_user_range_check(request->arguments[1], (size_t)count)) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    enum kernel_syscall_status status = borrow_socket(caller, (int32_t)request->arguments[0],
        &files, &file, op == 242 ? (uint32_t)request->arguments[3] : 0, &result);
    if (status) return status;
    if (!result) {
        if (kernel_task_mm_borrow_mutable(caller, &mm))
            status = KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        else status = socket_operation_owned(files, mm, &file, request, count, 0, &result);
        if (release_socket(&file)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = result;
    return status;
}

enum {
    URING_SENDMSG = 9, URING_RECVMSG = 10, URING_ACCEPT = 13, URING_CONNECT = 16,
    URING_SEND = 26, URING_RECV = 27, URING_SHUTDOWN = 34, URING_SOCKET = 45,
    URING_BIND = 56, URING_LISTEN = 57,
};

int kernel_io_uring_socket_supported(unsigned op)
{
    return op == URING_SENDMSG || op == URING_RECVMSG || op == URING_ACCEPT ||
        op == URING_CONNECT || op == URING_SEND || op == URING_RECV ||
        op == URING_SHUTDOWN || op == URING_SOCKET || op == URING_BIND || op == URING_LISTEN;
}

int kernel_io_uring_socket_needs_file(unsigned op)
{
    return op != URING_SOCKET;
}

int kernel_io_uring_socket_validate(const struct uring_sqe *sqe)
{
    if (sqe->ioprio || (sqe->opcode != URING_SEND && sqe->file_index))
        return -KERNEL_EOPNOTSUPP;
    if (sqe->buffer || (sqe->opcode == URING_SEND && sqe->file_index >> 16))
        return -KERNEL_EINVAL;
    switch (sqe->opcode) {
    case URING_SENDMSG: case URING_RECVMSG:
        return sqe->length != 1 ? -KERNEL_EINVAL : 0;
    case URING_CONNECT: case URING_BIND:
        return sqe->length || sqe->op_flags ? -KERNEL_EINVAL : 0;
    case URING_SHUTDOWN: case URING_LISTEN:
        return sqe->address || sqe->offset || sqe->op_flags ? -KERNEL_EINVAL : 0;
    case URING_SOCKET:
        return sqe->address || sqe->op_flags ? -KERNEL_EINVAL : 0;
    case URING_ACCEPT:
        return (sqe->length || (sqe->op_flags & ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK))) ?
            -KERNEL_EINVAL : 0;
    default:
        return 0;
    }
}

int64_t kernel_io_uring_socket_execute(struct kernel_files *files, struct kernel_mm *mm,
    const struct uring_sqe *sqe, struct kernel_open_file_description **owner)
{
    int64_t result = 0;
    if (sqe->opcode == URING_SOCKET) {
        int type = (int32_t)sqe->offset;
        int error = socket_creation_error(sqe->fd, type, (int32_t)sqe->length);
        if (error) return error;
        return kernel_files_socket_create(files, sqe->fd, type & LINUX_SOCK_TYPE_MASK,
            (int32_t)sqe->length, (uint32_t)type, &result) ? -KERNEL_EIO : result;
    }
    struct kernel_socket *socket = kernel_open_file_socket(*owner);
    if (!socket) return -KERNEL_ENOTSOCK;
    switch (sqe->opcode) {
    case URING_BIND: {
        KERNEL_SERVICE_SCOPE(service);
        union linux_sockaddr_storage snapshot = {0};
        struct kernel_socket_address address;
        int error = copy_address_in(mm, sqe->address, sqe->offset, &snapshot,
            kernel_socket_is_unix(socket), &address);
        return error ? error : kernel_socket_bind(socket, &address);
    }
    case URING_SENDMSG: case URING_RECVMSG: {
        struct kernel_socket_call call = {0};
        kernel_socket_call_begin(&call, files, owner);
        enum kernel_syscall_status status = socket_message_pinned(files, mm, owner,
            sqe->address, sqe->op_flags | KERNEL_SOCKET_MSG_DONTWAIT |
                (sqe->opcode == URING_SENDMSG ? KERNEL_SOCKET_MSG_NOSIGNAL : 0),
            sqe->opcode == URING_SENDMSG, 0, NULL, &result, &call);
        kernel_socket_call_end(&call);
        return status ? -KERNEL_EIO : result;
    }
    default:
        break;
    }
    struct kernel_syscall_request request = {0};
    uint64_t count = 0;
    switch (sqe->opcode) {
    case URING_SEND: case URING_RECV:
        request.number = sqe->opcode == URING_SEND ? 206 : 207;
        count = sqe->length;
        if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
        if (kernel_user_range_check(sqe->address, (size_t)count)) return -KERNEL_EFAULT;
        request.arguments[1] = sqe->address;
        request.arguments[2] = count;
        request.arguments[3] = sqe->op_flags;
        if (sqe->opcode == URING_SEND) {
            request.arguments[3] |= KERNEL_SOCKET_MSG_NOSIGNAL;
            request.arguments[4] = sqe->offset;
            request.arguments[5] = (uint16_t)sqe->file_index;
        }
        break;
    case URING_CONNECT:
        request.number = 203;
        request.arguments[1] = sqe->address;
        request.arguments[2] = sqe->offset;
        break;
    case URING_ACCEPT:
        request.number = 242;
        request.arguments[1] = sqe->address;
        request.arguments[2] = sqe->offset;
        request.arguments[3] = sqe->op_flags;
        break;
    case URING_LISTEN: case URING_SHUTDOWN:
        request.number = sqe->opcode == URING_LISTEN ? 201 : 210;
        request.arguments[1] = sqe->length;
        break;
    default:
        return -KERNEL_EINVAL;
    }
    return socket_operation_owned(files, mm, owner, &request, count, 1, &result) ?
        -KERNEL_EIO : result;
}
