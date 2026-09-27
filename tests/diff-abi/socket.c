#include "abi.h"

#include <stdint.h>

#define LINUX_AF_INET 2
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_NONBLOCK 00004000
#define LINUX_SOCK_CLOEXEC 02000000
#define LINUX_IPPROTO_TCP 6
#define LINUX_IPPROTO_UDP 17
#define LINUX_SOL_SOCKET 1
#define LINUX_SO_RCVTIMEO 20
#define LINUX_F_GETFD 1
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_O_WRONLY 1
#define LINUX_O_ACCMODE 3
#define LINUX_FD_CLOEXEC 1
#define LINUX_SIOCGIFFLAGS 0x8913
#define LINUX_SIOCSIFFLAGS 0x8914
#define LINUX_IFF_UP 1
#define LINUX_POLLIN 1
#define LINUX_EINPROGRESS 115
#define LINUX_EAGAIN 11

struct socket_address {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

struct socket_ifreq {
    char name[16];
    int16_t flags;
    uint8_t padding[22];
};

struct socket_timeval {
    int64_t seconds;
    int64_t microseconds;
};

struct socket_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct socket_timespec {
    int64_t seconds;
    int64_t nanoseconds;
};

_Static_assert(sizeof(struct socket_address) == 16, "sockaddr_in size");
_Static_assert(sizeof(struct socket_ifreq) == 40, "ifreq size");
_Static_assert(sizeof(struct socket_timeval) == 16, "timeval size");
_Static_assert(sizeof(struct socket_pollfd) == 8, "pollfd size");

static void record(const char *name, long result)
{
    abi_record(name, result, -1, -1, 0, 0, 0);
}

static long socket_result(long fd)
{
    return fd < 0 ? fd : 0;
}

static void close_socket(long fd)
{
    if (fd >= 0)
        abi_require(SC1(57, fd) == 0);
}

static long enable_loopback(long fd)
{
    struct socket_ifreq interface = {.name = "lo"};
    if (fd < 0)
        return fd;
    long result = SC3(29, fd, LINUX_SIOCGIFFLAGS, &interface);
    if (result < 0)
        return result;
    interface.flags |= LINUX_IFF_UP;
    return SC3(29, fd, LINUX_SIOCSIFFLAGS, &interface);
}

void abi_socket_cases(void)
{
    record("socket.bad-type-flags",
           SC3(198, LINUX_AF_INET,
               LINUX_SOCK_STREAM | 0x01000000, LINUX_IPPROTO_TCP));
    record("socket.bad-protocol",
           SC3(198, LINUX_AF_INET, LINUX_SOCK_STREAM,
               LINUX_IPPROTO_UDP));
    record("socket.bad-family", SC3(198, 9999, LINUX_SOCK_DGRAM, 0));
    record("socket.bind-bad-fd-and-pointer", SC3(200, -1, 1, 16));
    long fresh_stream = SC3(198, LINUX_AF_INET,
                            LINUX_SOCK_STREAM | LINUX_SOCK_NONBLOCK, 0);
    long fresh_datagram = SC3(198, LINUX_AF_INET,
                              LINUX_SOCK_DGRAM | LINUX_SOCK_NONBLOCK, 0);
    abi_require(fresh_stream >= 0 && fresh_datagram >= 0);
    record("socket.accept-fresh-stream", SC3(202, fresh_stream, 0, 0));
    record("socket.accept-datagram", SC3(202, fresh_datagram, 0, 0));
    close_socket(fresh_stream);
    close_socket(fresh_datagram);

    struct socket_address udp_address = {.family = LINUX_AF_INET};
    long udp_server = SC3(198, LINUX_AF_INET, LINUX_SOCK_DGRAM,
                          LINUX_IPPROTO_UDP);
    record("socket.udp-create", socket_result(udp_server));
    long loopback = enable_loopback(udp_server);
    record("socket.lo-up", loopback);
    long result = udp_server < 0 ? udp_server
                   : SC3(200, udp_server, &udp_address, sizeof(udp_address));
    record("socket.udp-bind", result);
    uint32_t address_length = sizeof(udp_address);
    result = result < 0 ? result
             : SC3(204, udp_server, &udp_address, &address_length);
    record("socket.udp-getsockname", result < 0 ? result
           : (address_length == sizeof(udp_address) &&
              udp_address.family == LINUX_AF_INET && udp_address.port != 0));
    long udp_flags = SC2(25, udp_server, LINUX_F_GETFL);
    record("socket.udp-fsetfl-getfl",
           udp_flags < 0 ? udp_flags
           : SC3(25, udp_server, LINUX_F_SETFL,
                 udp_flags | LINUX_SOCK_NONBLOCK));
    record("socket.udp-fsetfl-wronly",
           SC3(25, udp_server, LINUX_F_SETFL, LINUX_O_WRONLY));
    record("socket.udp-fsetfl-rdonly",
           SC3(25, udp_server, LINUX_F_SETFL, 0));
    record("socket.udp-fsetfl-mode-preserved",
           (SC2(25, udp_server, LINUX_F_GETFL) & LINUX_O_ACCMODE) ==
           (udp_flags & LINUX_O_ACCMODE));
    record("socket.udp-fsetfl-restore",
           udp_flags < 0 ? udp_flags
           : SC3(25, udp_server, LINUX_F_SETFL, udp_flags));
    uint32_t negative_length = UINT32_MAX;
    record("socket.udp-getsockname-negative-length",
           SC3(204, udp_server, &udp_address, &negative_length));
    long udp_ready = result;
    struct socket_timeval timeout = {.microseconds = 1};
    result = udp_server < 0 ? udp_server
             : SC5(208, udp_server, LINUX_SOL_SOCKET, LINUX_SO_RCVTIMEO,
                   &timeout, sizeof(timeout));
    record("socket.udp-timeout", result);

    long udp_client = SC3(198, LINUX_AF_INET, LINUX_SOCK_DGRAM,
                          LINUX_IPPROTO_UDP);
    record("socket.udp-client-create", socket_result(udp_client));
    udp_address.address = UINT32_C(0x0100007f); /* 127.0.0.1 in network order */
    long send = loopback < 0 ? loopback
                : udp_ready < 0 ? udp_ready
                : udp_client < 0 ? udp_client
                : SC6(206, udp_client, "x", 1, 0, &udp_address,
                      sizeof(udp_address));
    record("socket.udp-sendto", send);
    char payload = 0;
    struct socket_address udp_peer = {0};
    uint32_t peer_length = sizeof(udp_peer);
    long received = send == 1
                        ? SC6(207, udp_server, &payload, 1, 0,
                              &udp_peer, &peer_length)
                        : send;
    abi_record("socket.udp-recvfrom", received, -1, -1, 0,
               received > 0 ? &payload : 0, received > 0 ? 1 : 0);
    record("socket.udp-peer", received < 0 ? received
           : (peer_length == sizeof(udp_peer) &&
              udp_peer.family == LINUX_AF_INET && udp_peer.port != 0 &&
              udp_peer.address == UINT32_C(0x0100007f)));
    record("socket.udp-write-unconnected",
           udp_client < 0 ? udp_client : SC3(64, udp_client, "u", 1));
    send = udp_client < 0 ? udp_client
           : SC6(206, udp_client, "y", 1, 0, &udp_address,
                 sizeof(udp_address));
    payload = 0;
    received = send == 1 ? SC3(63, udp_server, &payload, 1) : send;
    abi_record("socket.udp-read", received, -1, -1, 0,
               received == 1 ? &payload : 0, received == 1 ? 1 : 0);
    record("socket.udp-send-empty-for-read",
           SC6(206, udp_client, "", 0, 0, &udp_address,
               sizeof(udp_address)));
    record("socket.udp-read-empty", SC3(63, udp_server, &payload, 1));
    record("socket.udp-send-after-empty-read",
           SC6(206, udp_client, "r", 1, 0, &udp_address,
               sizeof(udp_address)));
    record("socket.udp-read-after-empty", SC3(63, udp_server, &payload, 1));
    record("socket.udp-send-empty-for-readv",
           SC6(206, udp_client, "", 0, 0, &udp_address,
               sizeof(udp_address)));
    struct abi_iovec empty_vector = {&payload, 1};
    record("socket.udp-readv-empty",
           SC3(65, udp_server, &empty_vector, 1));
    record("socket.udp-send-after-empty-readv",
           SC6(206, udp_client, "s", 1, 0, &udp_address,
               sizeof(udp_address)));
    record("socket.udp-read-after-empty-readv",
           SC3(63, udp_server, &payload, 1));
    long udp_fault_map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(udp_fault_map >= 0 &&
                SC3(226, udp_fault_map + 4096, 4096, 0) == 0);
    send = SC6(206, udp_client, "wxyz", 4, 0, &udp_address,
               sizeof(udp_address));
    long udp_partial = send == 4
        ? SC6(207, udp_server, (void *)(udp_fault_map + 4094), 4, 0, 0, 0)
        : send;
    record("socket.udp-recvfrom-copy-fault", udp_partial);
    char after_udp_fault[4] = {0};
    long after_udp = SC6(207, udp_server, after_udp_fault, 4, 0, 0, 0);
    record("socket.udp-after-copy-fault", after_udp);
    record("socket.udp-empty-bad-range",
           SC6(207, udp_server, (void *)-1, 1, 0, 0, 0));
    send = SC6(206, udp_client, "q", 1, 0, &udp_address,
               sizeof(udp_address));
    negative_length = UINT32_MAX;
    received = send == 1
        ? SC6(207, udp_server, &payload, 1, 0, &udp_peer,
              &negative_length) : send;
    record("socket.udp-recvfrom-negative-length", received);
    record("socket.udp-after-negative-length",
           SC6(207, udp_server, &payload, 1, 0, 0, 0));
    abi_require(SC2(215, udp_fault_map, 8192) == 0);
    close_socket(udp_client);
    close_socket(udp_server);

    struct socket_address tcp_address = {.family = LINUX_AF_INET};
    long tcp_server = SC3(198, LINUX_AF_INET,
                          LINUX_SOCK_STREAM | LINUX_SOCK_CLOEXEC,
                          LINUX_IPPROTO_TCP);
    record("socket.tcp-cloexec-create", socket_result(tcp_server));
    long flags = tcp_server < 0 ? tcp_server
                 : SC3(25, tcp_server, LINUX_F_GETFD, 0);
    record("socket.tcp-cloexec-flag",
           flags < 0 ? flags : !!(flags & LINUX_FD_CLOEXEC));
    result = tcp_server < 0 ? tcp_server
             : SC3(200, tcp_server, &tcp_address, sizeof(tcp_address));
    record("socket.tcp-bind", result);
    address_length = sizeof(tcp_address);
    result = result < 0 ? result
             : SC3(204, tcp_server, &tcp_address, &address_length);
    record("socket.tcp-getsockname", result < 0 ? result
           : (address_length == sizeof(tcp_address) &&
              tcp_address.family == LINUX_AF_INET && tcp_address.port != 0));
    long tcp_ready = result;
    result = tcp_ready < 0 ? tcp_ready : SC2(201, tcp_server, 1);
    record("socket.tcp-listen", result);
    long listening = result;

    long tcp_client = SC3(198, LINUX_AF_INET,
                          LINUX_SOCK_STREAM | LINUX_SOCK_NONBLOCK,
                          LINUX_IPPROTO_TCP);
    record("socket.tcp-nonblock-create", socket_result(tcp_client));
    flags = tcp_client < 0 ? tcp_client
            : SC3(25, tcp_client, LINUX_F_GETFL, 0);
    record("socket.tcp-nonblock-flag",
           flags < 0 ? flags : !!(flags & LINUX_SOCK_NONBLOCK));
    tcp_address.address = UINT32_C(0x0100007f);
    long connected = loopback < 0 ? loopback
                     : listening < 0 ? listening
                     : tcp_client < 0 ? tcp_client
                     : SC3(203, tcp_client, &tcp_address,
                           sizeof(tcp_address));
    record("socket.tcp-connect",
           connected == -LINUX_EINPROGRESS ? 0 : connected);

    struct socket_pollfd listener = {
        .fd = (int32_t)tcp_server, .events = LINUX_POLLIN
    };
    struct socket_timespec deadline = {.seconds = 2};
    long readable = (connected == 0 || connected == -LINUX_EINPROGRESS)
                    ? SC5(73, &listener, 1, &deadline, 0, 0)
                    : connected;
    record("socket.tcp-listener-readable", readable < 0 ? readable
           : (readable == 1 && (listener.revents & LINUX_POLLIN) != 0));
    struct socket_address tcp_peer = {0};
    peer_length = sizeof(tcp_peer);
    long accepted = readable == 1 && (listener.revents & LINUX_POLLIN)
                    ? SC3(202, tcp_server, &tcp_peer, &peer_length)
                    : readable == 0 ? -LINUX_EAGAIN : readable;
    record("socket.tcp-accept", socket_result(accepted));
    record("socket.tcp-peer", accepted < 0 ? accepted
           : (peer_length == sizeof(tcp_peer) &&
              tcp_peer.family == LINUX_AF_INET && tcp_peer.port != 0 &&
              tcp_peer.address == UINT32_C(0x0100007f)));
    long tcp_written = accepted < 0 ? accepted
                       : SC3(64, tcp_client, "z", 1);
    record("socket.tcp-write", tcp_written);
    payload = 0;
    long tcp_read = tcp_written == 1 ? SC3(63, accepted, &payload, 1)
                                      : tcp_written;
    abi_record("socket.tcp-read", tcp_read, -1, -1, 0,
               tcp_read == 1 ? &payload : 0, tcp_read == 1 ? 1 : 0);
    struct abi_iovec outgoing[2] = {{"a", 1}, {"b", 1}};
    long vector_written = tcp_read == 1
                          ? SC3(66, tcp_client, outgoing, 2) : tcp_read;
    record("socket.tcp-writev", vector_written);
    char first = 0, second = 0;
    struct abi_iovec incoming[2] = {{&first, 1}, {&second, 1}};
    long vector_read = vector_written == 2
                       ? SC3(65, accepted, incoming, 2) : vector_written;
    if (vector_read == 1) {
        char remaining = 0;
        long rest = SC3(63, accepted, &remaining, 1);
        if (rest == 1) {
            second = remaining;
            vector_read++;
        } else {
            vector_read = rest;
        }
    }
    record("socket.tcp-readv", vector_read == 2 &&
           first == 'a' && second == 'b');
    /* A failed user copy must leave TCP data queued. A cross-page fault
     * commits only the bytes that really reached the caller. */
    long fault_map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(fault_map >= 0 &&
                SC3(226, fault_map + 4096, 4096, 0) == 0);
    abi_require(SC3(25, accepted, 4, LINUX_SOCK_NONBLOCK) == 0);
    struct socket_pollfd fault_ready = {.fd = accepted, .events = LINUX_POLLIN};
    struct socket_timespec fault_deadline = {.seconds = 2};
    abi_require(SC3(64, tcp_client, "cdef", 4) == 4 &&
                SC4(73, &fault_ready, 1, &fault_deadline, 0) == 1);
    record("socket.tcp-read-bad-pointer", SC3(63, accepted,
           (void *)(fault_map + 4096), 4));
    char fault_after[4] = {0};
    long fault_retained = SC3(63, accepted, fault_after, sizeof(fault_after));
    abi_record("socket.tcp-read-after-fault", fault_retained, -1, -1, 0,
               fault_retained > 0 ? fault_after : 0,
               fault_retained > 0 ? (usize)fault_retained : 0);
    long second_write = SC3(64, tcp_client, "ghij", 4);
    record("socket.tcp-fault-second-write", second_write);
    long second_ready = second_write == 4
        ? SC4(73, &fault_ready, 1, &fault_deadline, 0) : second_write;
    record("socket.tcp-fault-second-ready", second_ready);
    long fault_prefix = second_ready == 1
        ? SC3(63, accepted, (void *)(fault_map + 4094), 4) : second_ready;
    abi_record("socket.tcp-read-partial-copy", fault_prefix, -1, -1, 0,
               fault_prefix > 0 ? (void *)(fault_map + 4094) : 0,
               fault_prefix > 0 ? (usize)fault_prefix : 0);
    char fault_tail[4] = {0};
    long fault_remaining = SC3(63, accepted, fault_tail, sizeof(fault_tail));
    abi_record("socket.tcp-read-after-partial", fault_remaining, -1, -1, 0,
               fault_remaining > 0 ? fault_tail : 0,
               fault_remaining > 0 ? (usize)fault_remaining : 0);
    long vector_second_write = SC3(64, tcp_client, "lmno", 4);
    long vector_ready = vector_second_write == 4
        ? SC4(73, &fault_ready, 1, &fault_deadline, 0)
        : vector_second_write;
    char vector_prefix[2] = {0};
    struct abi_iovec fault_vector[2] = {
        {vector_prefix, 2}, {(void *)(fault_map + 4096), 2}
    };
    long vector_fault = vector_ready == 1
        ? SC3(65, accepted, fault_vector, 2) : vector_ready;
    abi_record("socket.tcp-readv-copy-fault", vector_fault, -1, -1, 0,
               vector_fault > 0 ? vector_prefix : 0,
               vector_fault > 0 ? (usize)(vector_fault < 2 ? vector_fault : 2) : 0);
    char vector_tail[4] = {0};
    long vector_after = SC3(63, accepted, vector_tail, sizeof(vector_tail));
    abi_record("socket.tcp-read-after-readv-fault", vector_after, -1, -1, 0,
               vector_after > 0 ? vector_tail : 0,
               vector_after > 0 ? (usize)vector_after : 0);
    abi_require(SC2(215, fault_map, 8192) == 0);
    record("socket.tcp-pread", accepted < 0 ? accepted
           : SC4(67, accepted, &payload, 1, 0));
    record("socket.tcp-pwrite", accepted < 0 ? accepted
           : SC4(68, accepted, &payload, 1, 0));
    record("socket.tcp-lseek", accepted < 0 ? accepted
           : SC3(62, accepted, 0, 0));
    struct abi_stat socket_stat;
    long stat_result = accepted < 0 ? accepted
                       : SC2(80, accepted, &socket_stat);
    record("socket.tcp-fstat", stat_result < 0 ? stat_result
           : ((socket_stat.mode & 0170000U) == 0140000U));
    long timeout_listener = SC3(198, LINUX_AF_INET, LINUX_SOCK_STREAM, 0);
    struct socket_address timeout_address = {.family = LINUX_AF_INET};
    struct socket_timeval accept_timeout = {.microseconds = 1000};
    abi_require(timeout_listener >= 0 &&
                SC3(200, timeout_listener, &timeout_address,
                    sizeof(timeout_address)) == 0 &&
                SC2(201, timeout_listener, 1) == 0 &&
                SC5(208, timeout_listener, LINUX_SOL_SOCKET,
                    LINUX_SO_RCVTIMEO, &accept_timeout,
                    sizeof(accept_timeout)) == 0);
    record("socket.tcp-accept-receive-timeout",
           SC3(202, timeout_listener, 0, 0));
    close_socket(timeout_listener);
    long negative_client = SC3(198, LINUX_AF_INET,
                               LINUX_SOCK_STREAM | LINUX_SOCK_NONBLOCK, 0);
    abi_require(negative_client >= 0);
    connected = SC3(203, negative_client, &tcp_address, sizeof(tcp_address));
    struct socket_pollfd negative_ready = {
        .fd = tcp_server, .events = LINUX_POLLIN
    };
    struct socket_timespec negative_deadline = {.seconds = 2};
    long negative_poll = SC4(73, &negative_ready, 1, &negative_deadline, 0);
    record("socket.tcp-negative-accept-ready", negative_poll);
    negative_length = UINT32_MAX;
    long negative_accept = negative_poll == 1
        ? SC3(202, tcp_server, &tcp_peer, &negative_length) : negative_poll;
    record("socket.tcp-accept-negative-length",
           negative_accept >= 0 ? 0 : negative_accept);
    if (negative_accept >= 0) close_socket(negative_accept);
    abi_require(SC3(25, tcp_server, 4, LINUX_SOCK_NONBLOCK) == 0);
    record("socket.tcp-after-negative-accept",
           SC3(202, tcp_server, 0, 0));
    close_socket(negative_client);
    long epfd = SC1(20, 0);
    int mixed_pipe[2];
    struct {
        uint32_t events, padding;
        uint64_t data;
    } ep_event = {LINUX_POLLIN, 0, 1}, ep_output = {0};
    abi_require(epfd >= 0 && SC2(59, mixed_pipe, 0) == 0 &&
                SC4(21, epfd, 1, accepted, &ep_event) == 0);
    ep_event.data = 2;
    abi_require(SC4(21, epfd, 1, mixed_pipe[0], &ep_event) == 0);
    abi_require(SC3(64, tcp_client, "p", 1) == 1 &&
                SC4(73, &fault_ready, 1, &fault_deadline, 0) == 1 &&
                SC3(63, accepted, &payload, 1) == 1);
    long delayed_write = SC3(64, tcp_client, "q", 1);
    long ep_wait = delayed_write == 1
        ? SC4(22, epfd, &ep_output, 1, 2000) : delayed_write;
    record("socket.tcp-epoll-protocol-timer",
           ep_wait == 1 ? !!(ep_output.events & LINUX_POLLIN) : ep_wait);
    abi_require(ep_wait == 1 && SC3(63, accepted, &payload, 1) == 1 &&
                payload == 'q');
    abi_require(SC3(64, tcp_client, "r", 1) == 1 &&
                SC4(73, &fault_ready, 1, &fault_deadline, 0) == 1 &&
                SC3(63, accepted, &payload, 1) == 1 && payload == 'r');
    delayed_write = SC3(64, tcp_client, "s", 1);
    ep_output.events = 0;
    ep_wait = delayed_write == 1
        ? SC4(22, epfd, &ep_output, 1, -1) : delayed_write;
    record("socket.tcp-epoll-infinite-timer",
           ep_wait == 1 ? !!(ep_output.events & LINUX_POLLIN) : ep_wait);
    abi_require(ep_wait == 1 && SC3(63, accepted, &payload, 1) == 1 &&
                payload == 's');
    abi_require(SC3(64, tcp_client, "t", 1) == 1 &&
                SC4(73, &fault_ready, 1, &fault_deadline, 0) == 1 &&
                SC3(63, accepted, &payload, 1) == 1 && payload == 't');
    abi_require(SC3(64, tcp_client, "u", 1) == 1);
    struct socket_pollfd mixed_wait[2] = {
        {.fd = accepted, .events = LINUX_POLLIN},
        {.fd = mixed_pipe[0], .events = LINUX_POLLIN}
    };
    long mixed_result = SC4(73, mixed_wait, 2, 0, 0);
    record("socket.tcp-ppoll-infinite-mixed",
           mixed_result == 1 ?
               !!(mixed_wait[0].revents & LINUX_POLLIN) &&
               mixed_wait[1].revents == 0 : mixed_result);
    close_socket(accepted);
    accepted = -1;
    ep_output.events = 0;
    record("socket.tcp-epoll-after-close",
           SC4(22, epfd, &ep_output, 1, 0));
    close_socket(mixed_pipe[0]);
    close_socket(mixed_pipe[1]);
    close_socket(epfd);
    close_socket(tcp_client);
    close_socket(tcp_server);

    /* The server must already be waiting when a nonblocking client sends SYN.
     * The client then performs no socket operation before checking completion. */
    struct socket_address blocked_address = {.family = LINUX_AF_INET};
    long blocked_server = SC3(198, LINUX_AF_INET, LINUX_SOCK_STREAM,
                               LINUX_IPPROTO_TCP);
    abi_require(blocked_server >= 0 &&
                SC3(200, blocked_server, &blocked_address,
                    sizeof(blocked_address)) == 0);
    address_length = sizeof(blocked_address);
    abi_require(SC3(204, blocked_server, &blocked_address,
                    &address_length) == 0 &&
                SC2(201, blocked_server, 1) == 0);
    int ready_pipe[2], done_pipe[2];
    abi_require(SC2(59, ready_pipe, 0) == 0 &&
                SC2(59, done_pipe, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        char marker = 'r';
        char outcome;
        SC1(57, ready_pipe[0]);
        SC1(57, done_pipe[0]);
        abi_require(SC3(64, ready_pipe[1], &marker, 1) == 1);
        long accepted_fd = SC3(202, blocked_server, 0, 0);
        outcome = accepted_fd >= 0 ? 0 : 1;
        if (accepted_fd >= 0) SC1(57, accepted_fd);
        abi_require(SC3(64, done_pipe[1], &outcome, 1) == 1);
        abi_exit(outcome);
    }
    SC1(57, ready_pipe[1]);
    SC1(57, done_pipe[1]);
    char marker;
    abi_require(SC3(63, ready_pipe[0], &marker, 1) == 1 && marker == 'r');
    struct socket_pollfd done_fd = {.fd = done_pipe[0], .events = LINUX_POLLIN};
    struct socket_timespec before_connect = {.nanoseconds = 100000000};
    abi_require(SC5(73, &done_fd, 1, &before_connect, 0, 0) == 0);
    long blocked_client = SC3(198, LINUX_AF_INET,
                              LINUX_SOCK_STREAM | LINUX_SOCK_NONBLOCK,
                              LINUX_IPPROTO_TCP);
    abi_require(blocked_client >= 0);
    blocked_address.address = UINT32_C(0x0100007f);
    connected = SC3(203, blocked_client, &blocked_address,
                    sizeof(blocked_address));
    abi_require(connected == 0 || connected == -LINUX_EINPROGRESS);
    struct socket_timespec after_connect = {.seconds = 2};
    long completed = SC5(73, &done_fd, 1, &after_connect, 0, 0);
    char outcome = 1;
    int status = 0;
    if (completed == 1)
        abi_require(SC3(63, done_pipe[0], &outcome, 1) == 1);
    else
        SC2(129, child, 9);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    record("socket.tcp-blocked-accept",
           completed == 1 && outcome == 0 && status == 0);
    close_socket(blocked_client);
    close_socket(blocked_server);
    SC1(57, ready_pipe[0]);
    SC1(57, done_pipe[0]);
}
