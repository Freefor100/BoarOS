#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BULK_BYTES (16U * 1024U * 1024U)
#define PARALLEL_BYTES (8U * 1024U * 1024U)
#define WORKERS 5U
#define CHILDREN 8U
#define PRESSURE_UDP_PAYLOAD 1472U
#define PRESSURE_UDP_PACKETS 128U
#define PRESSURE_TCP_BYTES 8192U
#define PRESSURE_RX_BUDGET 65536U
static const char *phase = "setup";
static unsigned stage_timeout = 180;
static pid_t owner_pid, children[CHILDREN], httpd_pid;
static int cost_control = -1;
static unsigned cost_epoch;
static unsigned char data[65536];

static int cleanup(void)
{
    int failed = 0;
    if (getpid() != owner_pid) return 0;
    /* -f 保持 httpd/请求/CGI 在 fixture 创建的同一进程组内。 */
    if (httpd_pid > 0 && kill(-httpd_pid, SIGKILL) < 0 && errno != ESRCH) failed = 1;
    for (unsigned i = 0; i < CHILDREN; i++)
        if (children[i] > 0 && kill(children[i], SIGKILL) < 0 && errno != ESRCH) failed = 1;
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (children[i] <= 0) continue;
        int status; pid_t result;
        do { result = waitpid(children[i], &status, 0); } while (result < 0 && errno == EINTR);
        if (result > 0) {
            printf("EXTERNAL CLEANUP pid=%ld waitstatus=%d\n", (long)result, status);
            if (result != children[i] || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) failed = 1;
        } else failed = 1;
        children[i] = 0;
    }
    /* 独立 guest 的 PID 1 接管 httpd 提前退出后的 request/CGI 孤儿。 */
    if (getpid() == 1) {
        int status; pid_t result;
        while ((result = waitpid(-1, &status, 0)) > 0) {
            printf("EXTERNAL CLEANUP adopted_pid=%ld waitstatus=%d\n", (long)result, status);
            if (!WIFEXITED(status) && (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)) failed = 1;
        }
        if (result != -1 || errno != ECHILD) failed = 1;
    }
    httpd_pid = 0;
    return failed;
}

static void fail(int line, const char *expression)
{
    int error = errno;
    fprintf(stderr, "EXTERNAL FAIL %s line=%d expression=%s errno=%d\n", phase, line, expression, error);
    (void)cleanup();
    exit(1);
}
#define CHECK(x) do { if (!(x)) fail(__LINE__, #x); } while (0)

static void timed_out(int signal_number)
{
    (void)signal_number;
    if (getpid() == owner_pid) {
        if (httpd_pid > 0) (void)kill(-httpd_pid, SIGKILL);
        for (unsigned i = 0; i < CHILDREN; i++)
            if (children[i] > 0) (void)kill(children[i], SIGKILL);
    }
    const char message[] = "EXTERNAL FAIL timeout errno=110\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

static uint64_t now_ns(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void protocol_report(const char *when)
{
    if (cost_control < 0) return;
    FILE *report = fopen("/proc/boaros_net_stats", "r"); CHECK(report != NULL);
    printf("NETWORK PROTOCOL name=external-%s phase=%s\n", phase, when);
    char buffer[1024]; size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), report)) > 0)
        CHECK(fwrite(buffer, 1, size, stdout) == size);
    CHECK(!ferror(report) && fclose(report) == 0); puts("NETWORK PROTOCOL END");
}

static void start_phase(const char *name)
{
    phase = name;
    alarm(stage_timeout);
    if (cost_control >= 0) {
        protocol_report("before");
        CHECK(write(cost_control, "begin\n", 6) == 6);
        cost_epoch++;
    }
}

static void end_phase(void)
{
    if (cost_control < 0) return;
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    unsigned retries = 0;
    for (;;) {
        ssize_t result = write(cost_control, "end\n", 4);
        if (result == 4) break;
        CHECK(result == -1 && errno == EBUSY && now_ns() < deadline);
        if (retries++ == 0) printf("EXTERNAL OBSERVE waiting phase=%s errno=%d\n", phase, EBUSY);
        struct timespec pause = {.tv_nsec = 20000000};
        while (nanosleep(&pause, &pause) < 0) CHECK(errno == EINTR);
    }
    printf("EXTERNAL OBSERVE end phase=%s retries=%u\n", phase, retries);
    printf("COST SNAPSHOT external-%s %u\n", phase, cost_epoch);
    FILE *report = fopen("/proc/boaros_cost", "r"); CHECK(report != NULL);
    char buffer[4096]; size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), report)) > 0)
        CHECK(fwrite(buffer, 1, size, stdout) == size);
    CHECK(!ferror(report) && fclose(report) == 0); puts("COST END");
    protocol_report("after");
}

static void enable_observation(void)
{
    if (access("/observe", F_OK) < 0) { CHECK(errno == ENOENT); return; }
    if (mkdir("/proc", 0555) < 0) CHECK(errno == EEXIST);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) CHECK(errno == EBUSY);
    cost_control = open("/proc/boaros_cost_control", O_WRONLY | O_CLOEXEC);
    CHECK(cost_control >= 0);
}

static void ready(void)
{
    printf("EXTERNAL READY %s\n", phase);
    CHECK(fflush(stdout) == 0);
}

static void remember(pid_t child)
{
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (children[i] == 0) { children[i] = child; return; }
    }
    errno = EOVERFLOW; CHECK(!"child registry full");
}

static void forget(pid_t child)
{
    for (unsigned i = 0; i < CHILDREN; i++)
        if (children[i] == child) { children[i] = 0; return; }
    errno = ECHILD; CHECK(!"unowned child");
}

static ssize_t read_retry(int fd, void *buffer, size_t size)
{
    ssize_t result;
    do { result = read(fd, buffer, size); } while (result < 0 && errno == EINTR);
    return result;
}

static void write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *cursor = buffer;
    while (size > 0) {
        ssize_t done = write(fd, cursor, size);
        if (done < 0 && errno == EINTR) continue;
        CHECK(done > 0 && (size_t)done <= size);
        cursor += done; size -= (size_t)done;
    }
}

static void read_all(int fd, void *buffer, size_t size)
{
    unsigned char *cursor = buffer;
    while (size > 0) {
        ssize_t done = read_retry(fd, cursor, size);
        CHECK(done > 0 && (size_t)done <= size);
        cursor += done; size -= (size_t)done;
    }
}

static uint64_t check_data(const unsigned char *buffer, size_t size, size_t offset)
{
    uint64_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char expected = (unsigned char)((offset + i) % 251);
        if (buffer[i] != expected) {
            fprintf(stderr, "EXTERNAL MISMATCH %s offset=%zu actual=%u expected=%u\n",
                    phase, offset + i, buffer[i], expected);
            errno = EBADMSG; CHECK(!"payload mismatch");
        }
        checksum += buffer[i];
    }
    return checksum;
}

static void fill_data(unsigned char *buffer, size_t size, size_t offset)
{
    for (size_t i = 0; i < size; i++) buffer[i] = (unsigned char)((offset + i) % 251);
}

static uint64_t receive_pattern(int fd, size_t bytes)
{
    size_t offset = 0; uint64_t checksum = 0;
    while (offset < bytes) {
        size_t wanted = bytes - offset;
        if (wanted > sizeof(data)) wanted = sizeof(data);
        ssize_t received = read_retry(fd, data, wanted);
        CHECK(received > 0 && (size_t)received <= wanted);
        checksum += check_data(data, (size_t)received, offset);
        offset += (size_t)received;
    }
    CHECK(read_retry(fd, data, 1) == 0);
    return checksum;
}

static void send_pattern(int fd, size_t bytes)
{
    for (size_t offset = 0; offset < bytes;) {
        size_t size = bytes - offset;
        if (size > sizeof(data)) size = sizeof(data);
        fill_data(data, size, offset); write_all(fd, data, size); offset += size;
    }
}

static int bound_socket(int type, unsigned port)
{
    int fd = socket(AF_INET, type, 0); CHECK(fd >= 0);
    int one = 1; CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                 .sin_addr = {htonl(INADDR_ANY)}};
    CHECK(bind(fd, (void *)&address, sizeof(address)) == 0);
    if (type == SOCK_STREAM) CHECK(listen(fd, 8) == 0);
    return fd;
}

static int accept_peer(int listener)
{
    int fd;
    do { fd = accept(listener, NULL, NULL); } while (fd < 0 && errno == EINTR);
    CHECK(fd >= 0); return fd;
}

static void configure_reference_network(void)
{
    /* 参考环境配置由 runner 标记；uname 不能表示 fixture 身份。 */
    if (access("/reference-network-setup", F_OK) < 0) { CHECK(errno == ENOENT); return; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    struct ifreq interface = {.ifr_name = "eth0"};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {inet_addr("10.77.0.2")}};
    memcpy(&interface.ifr_addr, &address, sizeof(address));
    CHECK(ioctl(fd, SIOCSIFADDR, &interface) == 0);
    address.sin_addr.s_addr = inet_addr("255.255.255.0");
    memcpy(&interface.ifr_netmask, &address, sizeof(address));
    CHECK(ioctl(fd, SIOCSIFNETMASK, &interface) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0);
    interface.ifr_flags |= IFF_UP; CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    memset(&interface, 0, sizeof(interface)); memcpy(interface.ifr_name, "lo", 3);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0);
    interface.ifr_flags |= IFF_UP; CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    CHECK(close(fd) == 0);
    puts("EXTERNAL CONFIG address=10.77.0.2 netmask=255.255.255.0");
}

static void interfaces(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    struct ifreq interface = {.ifr_name = "eth0"};
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0 && (interface.ifr_flags & IFF_UP));
    interface.ifr_flags &= ~IFF_UP; CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0 && !(interface.ifr_flags & IFF_UP));
    interface.ifr_flags |= IFF_UP; CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0 && (interface.ifr_flags & IFF_UP));
    CHECK(ioctl(fd, SIOCGIFADDR, &interface) == 0);
    struct sockaddr_in *address = (void *)&interface.ifr_addr;
    CHECK(address->sin_family == AF_INET && address->sin_addr.s_addr == inet_addr("10.77.0.2"));
    CHECK(ioctl(fd, SIOCGIFNETMASK, &interface) == 0);
    CHECK(address->sin_family == AF_INET && address->sin_addr.s_addr == inet_addr("255.255.255.0"));
    CHECK(ioctl(fd, SIOCGIFMTU, &interface) == 0 && interface.ifr_mtu == 1500);
    CHECK(ioctl(fd, SIOCGIFHWADDR, &interface) == 0 && interface.ifr_hwaddr.sa_family == 1);
    const unsigned char mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    CHECK(!memcmp(interface.ifr_hwaddr.sa_data, mac, sizeof(mac)));
    CHECK(ioctl(fd, SIOCGIFINDEX, &interface) == 0 && interface.ifr_ifindex > 0);
    int ethernet_index = interface.ifr_ifindex;
    CHECK(ioctl(fd, SIOCGIFNAME, &interface) == 0 && !strcmp(interface.ifr_name, "eth0"));
    memset(&interface, 0, sizeof(interface)); memcpy(interface.ifr_name, "lo", 3);
    CHECK(ioctl(fd, SIOCGIFINDEX, &interface) == 0 && interface.ifr_ifindex > 0 && interface.ifr_ifindex != ethernet_index);
    CHECK(close(fd) == 0);
    int loop = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(loop >= 0);
    struct sockaddr_in6 loop_address = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    CHECK(bind(loop, (void *)&loop_address, sizeof(loop_address)) == 0);
    socklen_t length = sizeof(loop_address);
    CHECK(getsockname(loop, (void *)&loop_address, &length) == 0 && length == sizeof(loop_address));
    CHECK(sendto(loop, "lo6", 3, 0, (void *)&loop_address, length) == 3);
    CHECK(recv(loop, data, sizeof(data), 0) == 3 && !memcmp(data, "lo6", 3) && close(loop) == 0);
    printf("EXTERNAL INTERFACE eth0 address=10.77.0.2 netmask=255.255.255.0 mac=52:54:00:12:34:56 mtu=1500 index=%d down_up=pass lo_ipv6=pass\n", ethernet_index);
}

static void tcp(void)
{
    start_phase("tcp"); int listener = bound_socket(SOCK_STREAM, 18080); ready();
    int peer = accept_peer(listener); CHECK(close(listener) == 0);
    uint64_t start = now_ns(), checksum = receive_pattern(peer, BULK_BYTES);
    send_pattern(peer, BULK_BYTES); CHECK(shutdown(peer, SHUT_WR) == 0);
    CHECK(close(peer) == 0);
    uint64_t end = now_ns(); end_phase();
    printf("EXTERNAL PASS tcp bytes_rx=%u bytes_tx=%u checksum=%llu start_ns=%llu end_ns=%llu\n",
           BULK_BYTES, BULK_BYTES, (unsigned long long)checksum,
           (unsigned long long)start, (unsigned long long)end);
}

struct worker_result {
    uint64_t start, end, checksum;
    uint32_t id, bytes;
};

static void parallel(void)
{
    start_phase("parallel"); int listener = bound_socket(SOCK_STREAM, 18080);
    int gate[2], reports[2]; CHECK(pipe(gate) == 0 && pipe(reports) == 0);
    pid_t workers[WORKERS]; int statuses[WORKERS] = {0};
    ready();
    for (unsigned i = 0; i < WORKERS; i++) {
        int peer = accept_peer(listener); pid_t child = fork(); CHECK(child >= 0);
        if (child == 0) {
            CHECK(close(listener) == 0 && close(gate[1]) == 0 && close(reports[0]) == 0);
            alarm(stage_timeout);
            char token; read_all(gate[0], &token, 1); CHECK(token == 'G' && close(gate[0]) == 0);
            struct worker_result report = {.start = now_ns(), .id = i, .bytes = PARALLEL_BYTES};
            report.checksum = receive_pattern(peer, PARALLEL_BYTES);
            send_pattern(peer, PARALLEL_BYTES);
            CHECK(shutdown(peer, SHUT_WR) == 0 && close(peer) == 0);
            report.end = now_ns();
            CHECK(write(reports[1], &report, sizeof(report)) == (ssize_t)sizeof(report));
            CHECK(close(reports[1]) == 0); _exit(0);
        }
        workers[i] = child; remember(child); CHECK(close(peer) == 0);
    }
    CHECK(close(listener) == 0 && close(gate[0]) == 0 && close(reports[1]) == 0);
    write_all(gate[1], "GGGGG", WORKERS); CHECK(close(gate[1]) == 0);
    for (unsigned count = 0; count < WORKERS; count++) {
        int status; pid_t child;
        do { child = waitpid(-1, &status, 0); } while (child < 0 && errno == EINTR);
        CHECK(child > 0); forget(child);
        unsigned index = 0; while (index < WORKERS && workers[index] != child) index++;
        CHECK(index < WORKERS); statuses[index] = status;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "EXTERNAL CHILD parallel id=%u waitstatus=%d\n", index, status);
            errno = ECHILD; CHECK(!"parallel child failed");
        }
    }
    unsigned seen = 0; uint64_t checksum = 0;
    for (unsigned i = 0; i < WORKERS; i++) {
        struct worker_result report; read_all(reports[0], &report, sizeof(report));
        CHECK(report.id < WORKERS && !(seen & (1U << report.id)) && report.bytes == PARALLEL_BYTES);
        seen |= 1U << report.id; checksum += report.checksum;
        printf("EXTERNAL CHILD parallel id=%u start_ns=%llu end_ns=%llu bytes_rx=%u bytes_tx=%u checksum=%llu waitstatus=%d\n",
               report.id, (unsigned long long)report.start, (unsigned long long)report.end,
               report.bytes, report.bytes, (unsigned long long)report.checksum, statuses[report.id]);
    }
    CHECK(read_retry(reports[0], data, 1) == 0 && close(reports[0]) == 0);
    end_phase();
    printf("EXTERNAL PASS parallel children=%u bytes_rx=%u bytes_tx=%u checksum=%llu\n",
           WORKERS, WORKERS * PARALLEL_BYTES, WORKERS * PARALLEL_BYTES, (unsigned long long)checksum);
}

static void datagram(int fd, size_t expected)
{
    struct sockaddr_in peer; socklen_t length = sizeof(peer); ssize_t size;
    do { size = recvfrom(fd, data, sizeof(data), 0, (void *)&peer, &length); }
    while (size < 0 && errno == EINTR);
    CHECK(size == (ssize_t)expected && length == sizeof(peer) && peer.sin_family == AF_INET);
    (void)check_data(data, expected, 0);
    CHECK(sendto(fd, data, expected, 0, (void *)&peer, length) == (ssize_t)expected);
}

static void udp(void)
{
    start_phase("udp"); int fd = bound_socket(SOCK_DGRAM, 18081); ready();
    uint64_t start = now_ns();
    for (unsigned i = 0; i < 10000; i++) datagram(fd, 64);
    const size_t sizes[] = {1472, 1473, 65507};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) datagram(fd, sizes[i]);
    CHECK(close(fd) == 0);
    uint64_t end = now_ns(); end_phase();
    printf("EXTERNAL PASS udp transactions=10000 payload=64 boundaries=1472,1473,65507 start_ns=%llu end_ns=%llu\n",
           (unsigned long long)start, (unsigned long long)end);
}

static void pressure(void)
{
    start_phase("pressure");
    int datagrams = bound_socket(SOCK_DGRAM, 18084);
    int receive_budget = (int)(PRESSURE_RX_BUDGET / 2U);
    CHECK(setsockopt(datagrams, SOL_SOCKET, SO_RCVBUF,
                     &receive_budget, sizeof(receive_budget)) == 0);
    socklen_t budget_length = sizeof(receive_budget);
    CHECK(getsockopt(datagrams, SOL_SOCKET, SO_RCVBUF,
                     &receive_budget, &budget_length) == 0);
    CHECK(budget_length == sizeof(receive_budget) && receive_budget == (int)PRESSURE_RX_BUDGET);
    int listener = bound_socket(SOCK_STREAM, 18085);
    uint64_t start = now_ns();
    printf("EXTERNAL START pressure start_ns=%llu udp_submitted=%u udp_payload=%u receive_budget=%u tcp_expected=%u\n",
           (unsigned long long)start, PRESSURE_UDP_PACKETS, PRESSURE_UDP_PAYLOAD,
           PRESSURE_RX_BUDGET, PRESSURE_TCP_BYTES);
    ready();
    int peer = accept_peer(listener);

    /* Host先发布UDP压力，再发同TAP上的TCP数据；完整TCP+EOF是进展握手。 */
    uint64_t tcp_checksum = receive_pattern(peer, PRESSURE_TCP_BYTES);
    send_pattern(peer, PRESSURE_TCP_BYTES);
    CHECK(shutdown(peer, SHUT_WR) == 0);

    unsigned accepted = 0;
    uint64_t udp_checksum = 0;
    for (;;) {
        ssize_t received;
        do { received = recvfrom(datagrams, data, sizeof(data), MSG_DONTWAIT, NULL, NULL); }
        while (received < 0 && errno == EINTR);
        if (received < 0) {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        if (received != (ssize_t)PRESSURE_UDP_PAYLOAD) {
            errno = EBADMSG;
            CHECK(received == (ssize_t)PRESSURE_UDP_PAYLOAD);
        }
        udp_checksum += check_data(data, (size_t)received, 0);
        accepted++;
    }
    unsigned accepted_bytes = accepted * PRESSURE_UDP_PAYLOAD;
    if (accepted == 0) { errno = ENODATA; CHECK(accepted > 0); }
    if (accepted >= PRESSURE_UDP_PACKETS || accepted_bytes > PRESSURE_RX_BUDGET) {
        errno = EOVERFLOW;
        CHECK(accepted < PRESSURE_UDP_PACKETS && accepted_bytes <= PRESSURE_RX_BUDGET);
    }
    CHECK(close(peer) == 0 && close(listener) == 0 && close(datagrams) == 0);
    uint64_t end = now_ns();
    end_phase();
    printf("EXTERNAL PASS pressure udp_submitted=%u udp_payload=%u udp_accepted=%u udp_accepted_bytes=%u udp_dropped_lower_bound=%u tcp_bytes_rx=%u tcp_bytes_tx=%u tcp_checksum=%llu udp_checksum=%llu start_ns=%llu end_ns=%llu\n",
           PRESSURE_UDP_PACKETS, PRESSURE_UDP_PAYLOAD, accepted, accepted_bytes,
           PRESSURE_UDP_PACKETS - accepted, PRESSURE_TCP_BYTES, PRESSURE_TCP_BYTES,
           (unsigned long long)tcp_checksum, (unsigned long long)udp_checksum,
           (unsigned long long)start, (unsigned long long)end);
}

static uint64_t check_pattern_file(const char *path)
{
    int fd = open(path, O_RDONLY); CHECK(fd >= 0);
    uint64_t checksum = receive_pattern(fd, BULK_BYTES); CHECK(close(fd) == 0); return checksum;
}

static pid_t command(char *const arguments[], int server)
{
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        if (server) CHECK(setpgid(0, 0) == 0);
        alarm(stage_timeout);
        execv(arguments[0], arguments); perror(arguments[0]); _exit(127);
    }
    remember(child);
    if (server) {
        httpd_pid = child;
        if (setpgid(child, child) < 0) CHECK(errno == EACCES || errno == ESRCH);
    }
    return child;
}

static void wait_ok(pid_t child, const char *name)
{
    int status; pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    CHECK(result == child); forget(child);
    printf("EXTERNAL COMMAND %s waitstatus=%d\n", name, status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void httpd_ready(void)
{
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(18082),
                                     .sin_addr = {htonl(INADDR_LOOPBACK)}};
        int result = connect(fd, (void *)&address, sizeof(address)); int error = errno;
        CHECK(close(fd) == 0);
        if (result == 0) return;
        CHECK(error == ECONNREFUSED && now_ns() < deadline);
        struct timespec pause = {.tv_nsec = 20000000};
        while (nanosleep(&pause, &pause) < 0) CHECK(errno == EINTR);
    }
}

static void http(void)
{
    start_phase("http");
    uint64_t http_start = now_ns();
    if (mkdir("/http", 0755) < 0) CHECK(errno == EEXIST);
    if (mkdir("/http/cgi-bin", 0755) < 0) CHECK(errno == EEXIST);
    CHECK(access("/http/cgi-bin/upload", X_OK) == 0);
    struct stat file;
    CHECK(stat("/http/data.bin", &file) == 0 && S_ISREG(file.st_mode) && file.st_size == BULK_BYTES);
    char *server[] = {"/busybox", "httpd", "-f", "-p", "18082", "-h", "/http", NULL};
    (void)command(server, 1); httpd_ready();
    int control = bound_socket(SOCK_STREAM, 18080); ready();
    int peer = accept_peer(control); CHECK(close(control) == 0);
    char token; read_all(peer, &token, 1); CHECK(token == 'H' && read_retry(peer, data, 1) == 0);
    CHECK(close(peer) == 0);
    uint64_t uploaded = check_pattern_file("/upload.bin");
    printf("EXTERNAL CONTENT http-upload bytes=%u checksum=%llu\n", BULK_BYTES, (unsigned long long)uploaded);

    char *get[] = {"/busybox", "wget", "-O", "/download.bin", "http://10.77.0.1:18083/data.bin", NULL};
    wait_ok(command(get, 0), "wget-get");
    uint64_t downloaded = check_pattern_file("/download.bin");
    char post_data[sizeof("--post-data=") + 4096];
    memcpy(post_data, "--post-data=", sizeof("--post-data=") - 1);
    for (unsigned i = 0; i < 4096; i++) post_data[sizeof("--post-data=") - 1 + i] = (char)('A' + i % 26);
    post_data[sizeof(post_data) - 1] = 0;
    char *post[] = {"/busybox", "wget", post_data, "-O", "/reply.txt", "http://10.77.0.1:18083/post", NULL};
    wait_ok(command(post, 0), "wget-post");
    int reply = open("/reply.txt", O_RDONLY); CHECK(reply >= 0);
    char text[8]; read_all(reply, text, sizeof(text));
    CHECK(!memcmp(text, "POST OK\n", sizeof(text)) && read_retry(reply, data, 1) == 0 && close(reply) == 0);
    CHECK(cleanup() == 0);
    end_phase();
    printf("EXTERNAL PASS http upload_bytes=%u upload_checksum=%llu download_bytes=%u download_checksum=%llu wget_post_bytes=4096 start_ns=%llu end_ns=%llu\n",
           BULK_BYTES, (unsigned long long)uploaded, BULK_BYTES, (unsigned long long)downloaded,
           (unsigned long long)http_start, (unsigned long long)now_ns());
}

int main(int argc, char **argv)
{
    owner_pid = getpid(); setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--timeout")) {
        char *end; unsigned long value = strtoul(argv[2], &end, 10);
        CHECK(*end == 0 && value > 0 && value <= 3600); stage_timeout = (unsigned)value;
    } else CHECK(argc == 1);
    struct sigaction action = {.sa_handler = timed_out}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    uint64_t program_start = now_ns();
    start_phase("setup"); configure_reference_network(); interfaces(); enable_observation();
    tcp(); parallel(); udp(); pressure(); http(); alarm(0);
    if (cost_control >= 0) CHECK(close(cost_control) == 0);
    printf("EXTERNAL PROGRAM start_ns=%llu end_ns=%llu\n",
           (unsigned long long)program_start, (unsigned long long)now_ns());
    puts("EXTERNAL PASS all");
    return 42;
}
