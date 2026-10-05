#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS 28
#define PORT 18090
#define RTT_SAMPLES 16
static unsigned connections, rounds, nonblocking, bulk, mixed, transmit, external;
static size_t bytes;
static pid_t children[MAX_CONNECTIONS + 1];
static unsigned children_count;
static int cost_control = -1;
static FILE *evidence;
static int reports[2];
static int control_samples[2];
struct rr_sample { uint64_t start, end; };
struct connection_report {
    unsigned id, client;
    uint64_t receiver_bytes, start, end, rtt_min, rtt_p50, rtt_p95, rtt_max;
};
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BUDGET FAIL line=%d expression=%s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)

static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    for (unsigned i = 0; i < children_count; ++i) (void)kill(children[i], SIGKILL);
    const char message[] = "BUDGET FAIL timeout\n";
    (void)write(2, message, sizeof(message) - 1);
    _exit(1);
}

static void transfer(int fd, void *buffer, size_t length, int output)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = output ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = {.fd = fd, .events = output ? POLLOUT : POLLIN};
            int result;
            do { result = poll(&wait, 1, 30000); } while (result < 0 && errno == EINTR);
            CHECK(result == 1 && !(wait.revents & POLLNVAL));
            continue;
        }
        CHECK(n > 0 && (size_t)n <= length);
        p += n;
        length -= (size_t)n;
    }
}

static void eof(int fd)
{
    unsigned char b;
    for (;;) {
        ssize_t n = read(fd, &b, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = {.fd = fd, .events = POLLIN};
            CHECK(poll(&wait, 1, 30000) == 1);
            continue;
        }
        CHECK(n == 0);
        return;
    }
}

static void options(int fd)
{
    int one = 1;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
    if (nonblocking) {
        int flags = fcntl(fd, F_GETFL);
        CHECK(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    }
}

static unsigned char pattern(unsigned id, size_t position)
{
    return (unsigned char)((position + 17U * id) % 251U);
}

static void payload(int fd, unsigned id, int output)
{
    unsigned char buffer[65536];
    for (size_t offset = 0; offset < bytes;) {
        size_t length = bytes - offset;
        if (length > sizeof(buffer)) length = sizeof(buffer);
        if (output) for (size_t j = 0; j < length; ++j) buffer[j] = pattern(id, offset + j);
        transfer(fd, buffer, length, output);
        if (!output) for (size_t j = 0; j < length; ++j) CHECK(buffer[j] == pattern(id, offset + j));
        offset += length;
    }
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int is_bulk(unsigned id)
{
    return bulk && (!mixed || id + 1 < connections);
}

static void serve(int fd)
{
    options(fd);
    uint32_t header[2];
    transfer(fd, header, sizeof(header), 0);
    CHECK(ntohl(header[0]) == 0x424f4152U);
    unsigned id = ntohl(header[1]);
    CHECK(id < connections);
    unsigned char ping[64];
    for (unsigned i = 0; i < RTT_SAMPLES; ++i) {
        transfer(fd, ping, sizeof(ping), 0);
        for (unsigned j = 0; j < sizeof(ping); ++j) CHECK(ping[j] == pattern(id, i * sizeof(ping) + j));
        transfer(fd, ping, sizeof(ping), 1);
    }
    unsigned char go;
    transfer(fd, &go, 1, 0); CHECK(go == 'G');
    uint64_t start = now();
    if (is_bulk(id)) payload(fd, id, transmit);
    else for (unsigned i = 0; i < rounds; ++i) {
        transfer(fd, ping, sizeof(ping), 0);
        for (unsigned j = 0; j < sizeof(ping); ++j) CHECK(ping[j] == pattern(id, i * sizeof(ping) + j));
        transfer(fd, ping, sizeof(ping), 1);
    }
    /* 双向完成握手在内容校验之后；write 接纳本身不能代表接收完成。 */
    uint32_t complete = htonl(0x444f4e45U);
    if (is_bulk(id) && transmit) {
        CHECK(shutdown(fd, SHUT_WR) == 0);
        transfer(fd, &complete, sizeof(complete), 0);
        CHECK(ntohl(complete) == 0x444f4e45U);
        eof(fd);
    } else {
        eof(fd);
        transfer(fd, &complete, sizeof(complete), 1);
        CHECK(shutdown(fd, SHUT_WR) == 0);
    }
    uint64_t end = now();
    CHECK(close(fd) == 0);
    struct connection_report report = {.id = id, .receiver_bytes = is_bulk(id) ? bytes : (size_t)rounds * 64U,
                                       .start = start, .end = end};
    transfer(reports[1], &report, sizeof(report), 1);
    _exit(0);
}

static void client(unsigned id, int ready, int connected_gate, int measured_gate)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(PORT),
                               .sin_addr = {htonl(INADDR_LOOPBACK)}};
    CHECK(connect(fd, (void *)&peer, sizeof(peer)) == 0);
    options(fd);
    unsigned char token = 1;
    transfer(ready, &token, 1, 1);
    transfer(connected_gate, &token, 1, 0);
    CHECK(close(connected_gate) == 0);
    uint32_t header[2] = {htonl(0x424f4152U), htonl(id)};
    transfer(fd, header, sizeof(header), 1);
    unsigned char ping[64], expected[64];
    uint64_t rtt[RTT_SAMPLES];
    for (unsigned i = 0; i < RTT_SAMPLES; ++i) {
        for (unsigned j = 0; j < sizeof(ping); ++j) ping[j] = pattern(id, i * sizeof(ping) + j);
        memcpy(expected, ping, sizeof(ping));
        uint64_t start = now();
        transfer(fd, ping, sizeof(ping), 1);
        transfer(fd, ping, sizeof(ping), 0);
        rtt[i] = now() - start;
        CHECK(memcmp(ping, expected, sizeof(ping)) == 0);
    }
    struct rr_sample *samples = mixed && !is_bulk(id) ? calloc(rounds, sizeof(*samples)) : NULL;
    CHECK(!mixed || is_bulk(id) || samples);
    /* 所有流完成 RTT 探测后再共同开始 bulk/control，避免预热阶段假重叠。 */
    transfer(ready, &token, 1, 1); transfer(measured_gate, &token, 1, 0);
    CHECK(close(ready) == 0 && close(measured_gate) == 0);
    token = 'G'; transfer(fd, &token, 1, 1);
    uint64_t start = now();
    if (is_bulk(id)) payload(fd, id, !transmit);
    else for (unsigned i = 0; i < rounds; ++i) {
        for (unsigned j = 0; j < sizeof(ping); ++j) ping[j] = pattern(id, i * sizeof(ping) + j);
        memcpy(expected, ping, sizeof(ping));
        if (samples) samples[i].start = now();
        transfer(fd, ping, sizeof(ping), 1);
        transfer(fd, ping, sizeof(ping), 0);
        if (samples) samples[i].end = now();
        CHECK(memcmp(ping, expected, sizeof(ping)) == 0);
    }
    uint32_t complete = htonl(0x444f4e45U);
    if (is_bulk(id) && transmit) {
        eof(fd);
        transfer(fd, &complete, sizeof(complete), 1);
        CHECK(shutdown(fd, SHUT_WR) == 0);
    } else {
        CHECK(shutdown(fd, SHUT_WR) == 0);
        transfer(fd, &complete, sizeof(complete), 0);
        CHECK(ntohl(complete) == 0x444f4e45U);
        eof(fd);
    }
    uint64_t end = now();
    CHECK(close(fd) == 0);
    qsort(rtt, RTT_SAMPLES, sizeof(rtt[0]), compare_u64);
    struct connection_report report = {.id = id, .client = 1,
        .receiver_bytes = is_bulk(id) ? bytes : (size_t)rounds * 64U, .start = start, .end = end,
        .rtt_min = rtt[0], .rtt_p50 = rtt[8], .rtt_p95 = rtt[15], .rtt_max = rtt[15]};
    transfer(reports[1], &report, sizeof(report), 1);
    if (samples) {
        transfer(control_samples[1], samples, rounds * sizeof(*samples), 1);
        free(samples);
    }
    _exit(0);
}

static void wait_all(void)
{
    for (unsigned i = 0; i < children_count; ++i) {
        int status;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    children_count = 0;
}

static void protocol_report(const char *when)
{
    if (cost_control < 0) return;
    FILE *report = fopen("/proc/boaros_net_stats", "r");
    CHECK(report);
    fprintf(evidence, "BUDGET PROTOCOL %s\n", when);
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), report)) fputs(buffer, evidence);
    CHECK(!ferror(report) && fclose(report) == 0);
    fputs("BUDGET PROTOCOL END\n", evidence);
}

static void diagnostic_report(const char *path, const char *tag)
{
    FILE *report=fopen(path,"r");CHECK(report);
    fprintf(evidence,"BUDGET %s BEGIN\n",tag);
    char buffer[1024];
    while(fgets(buffer,sizeof(buffer),report))fputs(buffer,evidence);
    CHECK(!ferror(report) && fclose(report)==0);
    fprintf(evidence,"BUDGET %s END\n",tag);
}

static void setup_network(int reference)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    struct ifreq interface = {.ifr_name = "lo"};
    CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0);
    if (!(interface.ifr_flags & IFF_UP)) {
        interface.ifr_flags |= IFF_UP;
        CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    }
    if (external && reference) {
        memset(&interface, 0, sizeof(interface));
        strcpy(interface.ifr_name, "eth0");
        struct sockaddr_in *address = (struct sockaddr_in *)&interface.ifr_addr;
        address->sin_family = AF_INET;
        CHECK(inet_pton(AF_INET, "10.77.0.2", &address->sin_addr) == 1);
        CHECK(ioctl(fd, SIOCSIFADDR, &interface) == 0);
        CHECK(inet_pton(AF_INET, "255.255.255.0", &address->sin_addr) == 1);
        CHECK(ioctl(fd, SIOCSIFNETMASK, &interface) == 0);
        CHECK(ioctl(fd, SIOCGIFFLAGS, &interface) == 0);
        interface.ifr_flags |= IFF_UP;
        CHECK(ioctl(fd, SIOCSIFFLAGS, &interface) == 0);
    }
    CHECK(close(fd) == 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, timeout_handler);
    alarm(180);
    char path[16], mode[16], kind[16], direction[8];
    FILE *config = fopen(argc > 1 ? argv[1] : "/budget-config", "r");
    CHECK(config && fscanf(config, "%15s %15s %15s %u %zu %u %7s", path, mode, kind,
                           &connections, &bytes, &rounds, direction) == 7);
    CHECK(fclose(config) == 0);
    char result_path[1024];
    int path_length = snprintf(result_path, sizeof(result_path), "%s.results", argc > 1 ? argv[1] : "/budget-config");
    CHECK(path_length > 0 && (size_t)path_length < sizeof(result_path));
    evidence = fopen(result_path, "w"); CHECK(evidence);
    external = !strcmp(path, "tap"); nonblocking = !strcmp(mode, "nonblocking");
    mixed = !strcmp(kind, "mixed"); bulk = mixed || !strcmp(kind, "bulk"); transmit = !strcmp(direction, "tx");
    CHECK((external || !strcmp(path, "loopback")) && (nonblocking || !strcmp(mode, "blocking")));
    CHECK((bulk || !strcmp(kind, "rr")) && (transmit || !strcmp(direction, "rx")));
    CHECK(connections > 0 && connections <= (external ? MAX_CONNECTIONS : MAX_CONNECTIONS / 2));
    CHECK(bytes > 0 && rounds > 0 && rounds <= 65536 && (!mixed || connections >= 2));
    setup_network(argc > 2 && !strcmp(argv[2], "reference"));
    if (access("/observe", F_OK) == 0) {
        if (mount("proc", "/proc", "proc", 0, NULL) < 0) CHECK(errno == EBUSY);
        cost_control = open("/proc/boaros_cost_control", O_WRONLY | O_CLOEXEC);
        CHECK(cost_control >= 0);
        protocol_report("before");
        CHECK(fflush(evidence) == 0);
        CHECK(write(cost_control, "begin\n", 6) == 6);
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    CHECK(listener >= 0 && setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    CHECK(bind(listener, (void *)&address, sizeof(address)) == 0 && listen(listener, MAX_CONNECTIONS) == 0);
    CHECK(pipe(reports) == 0);
    if (mixed && !external) CHECK(pipe(control_samples) == 0);
    if (!external) {
        int ready[2], gate[2][2];
        /* 两轮放行各用一条管道，快参与者不能消费慢参与者的上一轮 token。 */
        CHECK(pipe(ready) == 0 && pipe(gate[0]) == 0 && pipe(gate[1]) == 0);
        pid_t coordinator = fork(); CHECK(coordinator >= 0);
        if (!coordinator) {
            close(listener); close(ready[0]); close(gate[0][1]); close(gate[1][1]);
            for (unsigned id = 0; id < connections; ++id) {
                pid_t child = fork(); CHECK(child >= 0);
                if (!child) { children_count = 0; client(id, ready[1], gate[0][0], gate[1][0]); }
                children[children_count++] = child;
            }
            close(ready[1]); close(gate[0][0]); close(gate[1][0]); wait_all(); _exit(0);
        }
        children[children_count++] = coordinator;
        close(ready[1]); close(gate[0][0]); close(gate[1][0]);
        for (unsigned id = 0; id < connections; ++id) {
            int fd = accept(listener, NULL, NULL); CHECK(fd >= 0);
            pid_t child = fork(); CHECK(child >= 0);
            if (!child) { children_count = 0; close(listener); close(ready[0]); close(gate[0][1]); close(gate[1][1]); serve(fd); }
            children[children_count++] = child;
            close(fd);
        }
        unsigned char token;
        for (unsigned phase = 0; phase < 2; ++phase) {
            for (unsigned id = 0; id < connections; ++id) transfer(ready[0], &token, 1, 0);
            for (unsigned id = 0; id < connections; ++id) transfer(gate[phase][1], &token, 1, 1);
            close(gate[phase][1]);
        }
        close(ready[0]);
    } else {
        puts("BUDGET READY tap");
        for (unsigned id = 0; id < connections; ++id) {
            int fd = accept(listener, NULL, NULL); CHECK(fd >= 0);
            pid_t child = fork(); CHECK(child >= 0);
            if (!child) { children_count = 0; close(listener); serve(fd); }
            children[children_count++] = child;
            close(fd);
        }
    }
    CHECK(close(listener) == 0);
    CHECK(close(reports[1]) == 0);
    struct connection_report completed[MAX_CONNECTIONS * 2];
    unsigned report_count = connections * (external ? 1 : 2);
    for (unsigned i = 0; i < report_count; ++i)
        transfer(reports[0], &completed[i], sizeof(completed[i]), 0);
    CHECK(close(reports[0]) == 0);
    struct rr_sample *samples = NULL;
    if (mixed && !external) {
        CHECK(close(control_samples[1]) == 0);
        samples = calloc(rounds, sizeof(*samples)); CHECK(samples);
        transfer(control_samples[0], samples, rounds * sizeof(*samples), 0);
        CHECK(close(control_samples[0]) == 0);
    }
    wait_all();
    if (cost_control >= 0) {
        uint64_t deadline = now() + 5000000000ULL;
        while (write(cost_control, "end\n", 4) != 4) {
            CHECK(errno == EBUSY && now() < deadline);
            struct timespec pause = {.tv_nsec = 20000000};
            nanosleep(&pause, NULL);
        }
        diagnostic_report("/proc/boaros_mem_stats","MEM_PEAK");
        protocol_report("after");
        diagnostic_report("/proc/boaros_cost","COST");
        CHECK(close(cost_control) == 0);
    }
    /* 全部传输结束后才输出，串口写入不混入其他连接的测量窗口。 */
    for (unsigned i = 0; i < report_count; ++i) {
        const struct connection_report *r = &completed[i];
        fprintf(evidence, "BUDGET %s id=%u receiver_bytes=%llu start_ns=%llu end_ns=%llu elapsed_ns=%llu rtt_min_ns=%llu rtt_p50_ns=%llu rtt_p95_ns=%llu rtt_max_ns=%llu complete=1\n",
               r->client ? "CONNECTION" : "SERVER", r->id, (unsigned long long)r->receiver_bytes,
               (unsigned long long)r->start, (unsigned long long)r->end,
               (unsigned long long)(r->end - r->start), (unsigned long long)r->rtt_min,
               (unsigned long long)r->rtt_p50, (unsigned long long)r->rtt_p95, (unsigned long long)r->rtt_max);
    }
    if (samples) {
        for (unsigned i = 0; i < rounds; ++i)
            fprintf(evidence, "BUDGET CONTROL_SAMPLE id=%u sequence=%u start_ns=%llu end_ns=%llu\n",
                   connections - 1, i, (unsigned long long)samples[i].start, (unsigned long long)samples[i].end);
        free(samples);
    }
    /* 串口会与内核 worker 日志交错；传输结束后持久化完整记录供宿主提取。 */
    fputs("BUDGET PASS all\n", evidence);
    CHECK(fflush(evidence) == 0 && fsync(fileno(evidence)) == 0 && fclose(evidence) == 0);
    puts("BUDGET PASS all");
    return 0;
}
