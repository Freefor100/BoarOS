#define _POSIX_C_SOURCE 200809L
#include "block_fault.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { MAX_REQUEST = 1024 * 1024 };
enum { NBD_OPT_GO = 7, NBD_OPT_INFO = 6, NBD_OPT_EXPORT_NAME = 1 };
enum { NBD_CMD_READ = 0, NBD_CMD_WRITE = 1, NBD_CMD_DISC = 2,
       NBD_CMD_FLUSH = 3 };

struct options {
    uint64_t cut_after;
    uint64_t fail_write;
    uint64_t fail_flush;
    const char *persist;
    int arm_on_signal;
    int control_stdin;
};

static volatile sig_atomic_t arm_requested;

static void arm_handler(int signal_number)
{
    (void)signal_number;
    arm_requested = 1;
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const unsigned char *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = value >> 8; p[1] = value;
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = value >> 24; p[1] = value >> 16;
    p[2] = value >> 8; p[3] = value;
}

static void put64(unsigned char *p, uint64_t value)
{
    put32(p, (uint32_t)(value >> 32)); put32(p + 4, (uint32_t)value);
}

static int transfer(int fd, void *buffer, size_t size, int write_out)
{
    unsigned char *cursor = buffer;
    while (size) {
        ssize_t done = write_out ? write(fd, cursor, size)
                                 : read(fd, cursor, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return -1;
        cursor += done;
        size -= (size_t)done;
    }
    return 0;
}

static int reply_option(int fd, uint32_t option, uint32_t type,
                        const unsigned char *payload, uint32_t size)
{
    unsigned char header[20];
    put64(header, UINT64_C(0x0003e889045565a9));
    put32(header + 8, option);
    put32(header + 12, type);
    put32(header + 16, size);
    return transfer(fd, header, sizeof(header), 1) ||
           (size && transfer(fd, (void *)payload, size, 1));
}

static int negotiate(int fd, struct fault_block *disk)
{
    unsigned char hello[18], flags[4], header[16], payload[MAX_REQUEST];
    put64(hello, UINT64_C(0x4e42444d41474943));
    put64(hello + 8, UINT64_C(0x49484156454f5054));
    put16(hello + 16, 3); /* fixed newstyle, no zeroes */
    if (transfer(fd, hello, sizeof(hello), 1) ||
        transfer(fd, flags, sizeof(flags), 0) ||
        (be32(flags) & 1U) == 0U) return -1;
    for (;;) {
        if (transfer(fd, header, sizeof(header), 0) ||
            be64(header) != UINT64_C(0x49484156454f5054)) return -1;
        uint32_t option = be32(header + 8), size = be32(header + 12);
        if (size > sizeof(payload) ||
            transfer(fd, payload, size, 0)) return -1;
        if (option == NBD_OPT_GO || option == NBD_OPT_INFO) {
            if (size < 6 || be32(payload) > size - 6 ||
                be32(payload) != 0) return -1;
            unsigned char info[12];
            put16(info, 0); /* NBD_INFO_EXPORT */
            put64(info + 2, disk->device.capacity_bytes);
            put16(info + 10, 5); /* HAS_FLAGS | SEND_FLUSH */
            if (reply_option(fd, option, 3, info, sizeof(info))) return -1;
            if (size >= 8 && be16(payload + 4) &&
                be16(payload + 6) == 3) {
                unsigned char block[14];
                put16(block, 3); /* NBD_INFO_BLOCK_SIZE */
                put32(block + 2, 512);
                put32(block + 6, 4096);
                put32(block + 10, MAX_REQUEST);
                if (reply_option(fd, option, 3, block,
                                 sizeof(block))) return -1;
            }
            if (reply_option(fd, option, 1, 0, 0)) return -1;
            if (option == NBD_OPT_GO) return 0;
        } else if (option == NBD_OPT_EXPORT_NAME) {
            if (size) return -1;
            unsigned char export[10];
            put64(export, disk->device.capacity_bytes);
            put16(export + 8, 5);
            if (transfer(fd, export, sizeof(export), 1)) return -1;
            return 0;
        } else if (reply_option(fd, option, UINT32_C(0x80000001), 0, 0))
            return -1; /* only simple replies are implemented */
    }
}

static int persist_cut(struct fault_block *disk, const char *policy)
{
    size_t count = disk->count;
    if (!strcmp(policy, "all") || !strcmp(policy, "reverse")) {
        for (size_t i = 0; i < count; ++i) {
            size_t event = !strcmp(policy, "reverse") ? count - 1 - i : i;
            if (fault_block_persist(disk, event)) return -1;
        }
    } else if (!strcmp(policy, "odd") || !strcmp(policy, "even")) {
        int parity = !strcmp(policy, "odd");
        for (size_t i = 0; i < count; ++i)
            if ((int)(i % 2) == parity && fault_block_persist(disk, i))
                return -1;
    } else if (strcmp(policy, "none")) return -1;
    if (fdatasync(disk->fd)) return -1;
    return fault_block_crash(disk);
}

/* Response gating is independent of disk execution. Tests release named
 * completions in any order, while FLUSH still changes stable storage in order. */
struct pending_reply {
    struct pending_reply *next;
    uint64_t id;
    size_t size;
    unsigned char bytes[];
};
struct response_gate {
    struct pending_reply *head;
    uint64_t sequence;
    int hold;
    char input[80];
    size_t used;
};
static void arm_faults(struct fault_block *disk, const struct options *options,
                       uint64_t *event, int *armed)
{
    arm_requested = 0;
    *armed = 1;
    *event = 0;
    disk->writes = disk->flushes = 0;
    disk->fail_write = options->fail_write;
    disk->fail_flush = options->fail_flush;
    fputs("armed=1\n", stderr);
    fflush(stderr);
}
static int gate_control(int fd, struct response_gate *gate,
                        struct fault_block *disk, const struct options *options,
                        uint64_t *event, int *armed)
{
    char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return -1;
    if (ch != '\n') {
        if (gate->used + 1 >= sizeof(gate->input)) return -1;
        gate->input[gate->used++] = ch;
        return 0;
    }
    gate->input[gate->used] = 0;
    gate->used = 0;
    int drain = !strcmp(gate->input, "drain");
    uint64_t id = 0;
    char tail;
    if (!strcmp(gate->input, "arm")) arm_faults(disk, options, event, armed);
    else if (!strcmp(gate->input, "hold")) gate->hold = 1;
    else if (drain || sscanf(gate->input, "release %" SCNu64 "%c", &id, &tail) == 1) {
        int found = drain;
        struct pending_reply **link = &gate->head;
        while (*link) {
            struct pending_reply *reply = *link;
            if (drain || reply->id == id) {
                if (transfer(fd, reply->bytes, reply->size, 1)) return -1;
                *link = reply->next;
                fprintf(stderr, "released=%" PRIu64 "\n", reply->id);
                free(reply);
                found = 1;
                if (!drain) break;
            } else link = &reply->next;
        }
        if (!found) return -1;
        if (drain) gate->hold = 0;
    } else return -1;
    fprintf(stderr, "control=%s\n", gate->input);
    fflush(stderr);
    return 0;
}
static int gated_transmit(int fd, struct fault_block *disk,
                    const struct options *options, struct response_gate *gate)

{
    unsigned char request[28], response[16], data[MAX_REQUEST];
    uint64_t event = 0;
    int armed = !options->arm_on_signal;
    for (;;) {
        if (options->control_stdin) {
            struct pollfd events[2] = {{fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
            int ready = poll(events, 2, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) return -1;
            if (events[1].revents) {
                if (gate_control(fd, gate, disk, options, &event, &armed)) return -1;
                continue;
            }
        }
        if (transfer(fd, request, sizeof(request), 0)) return 0;
        if (arm_requested) arm_faults(disk, options, &event, &armed);
        if (be32(request) != UINT32_C(0x25609513)) return -1;
        uint16_t flags = be16(request + 4), command = be16(request + 6);
        uint64_t offset = be64(request + 16);
        uint32_t size = be32(request + 24);
        if (command == NBD_CMD_DISC) return 0;
        if (size > MAX_REQUEST) return -1;
        if (command == NBD_CMD_WRITE && transfer(fd, data, size, 0))
            return -1;
        uint32_t error = 0;
        if (flags || (command != NBD_CMD_READ && command != NBD_CMD_WRITE &&
                      command != NBD_CMD_FLUSH) ||
            ((command == NBD_CMD_READ || command == NBD_CMD_WRITE) &&
             (offset > disk->device.capacity_bytes ||
              size > disk->device.capacity_bytes - offset))) {
            error = EINVAL;
        } else if (command == NBD_CMD_READ) {
            if (disk->device.read(disk, offset, data, size) !=
                KERNEL_BLOCK_STATUS_OK) error = EIO;
        } else if (command == NBD_CMD_WRITE) {
            if (disk->device.write(disk, offset, data, size) !=
                KERNEL_BLOCK_STATUS_OK) error = EIO;
        } else if (disk->device.flush(disk) != KERNEL_BLOCK_STATUS_OK)
            error = EIO;
        if (command == NBD_CMD_WRITE || command == NBD_CMD_FLUSH) {
            if (armed) ++event;
            fprintf(stderr, "event=%" PRIu64 " type=%s result=%u writes=%" PRIu64
                    " flushes=%" PRIu64 " pending=%zu\n", event,
                    command == NBD_CMD_WRITE ? "WRITE" : "FLUSH", error,
                    disk->writes, disk->flushes, disk->count);
            fflush(stderr);
            if (armed && event == options->cut_after) {
                shutdown(fd, SHUT_RDWR); /* freeze before the guest can clean up */
                if (persist_cut(disk, options->persist)) return -1;
                fprintf(stderr, "cut=%" PRIu64 " policy=%s\n", event,
                        options->persist);
                return 0;
            }
        }
        put32(response, UINT32_C(0x67446698));
        put32(response + 4, error);
        memcpy(response + 8, request + 8, 8);
        if (gate->hold) {
            size_t payload = command == NBD_CMD_READ && !error ? size : 0;
            struct pending_reply *reply = malloc(sizeof(*reply) + sizeof(response) + payload);
            if (!reply) return -1;
            reply->id = ++gate->sequence;
            reply->size = sizeof(response) + payload;
            memcpy(reply->bytes, response, sizeof(response));
            if (payload) memcpy(reply->bytes + sizeof(response), data, payload);
            reply->next = 0;
            struct pending_reply **link = &gate->head;
            while (*link) link = &(*link)->next;
            *link = reply;
            fprintf(stderr, "held=%" PRIu64 " command=%u offset=%" PRIu64 " size=%u\n",
                    reply->id, command, offset, size);
            fflush(stderr);
            continue;
        }
        if (transfer(fd, response, sizeof(response), 1) ||
            (command == NBD_CMD_READ && !error &&
             transfer(fd, data, size, 1))) return -1;
    }
}

static int transmit(int fd, struct fault_block *disk, const struct options *options)
{
    struct response_gate gate = {0};
    int result = gated_transmit(fd, disk, options, &gate);
    while (gate.head) {
        struct pending_reply *reply = gate.head;
        gate.head = reply->next;
        free(reply);
    }
    return result;
}

static int parse_u64(const char *text, uint64_t *result)
{
    char *end;
    errno = 0;
    *result = strtoull(text, &end, 10);
    return errno || !*text || *end;
}

int main(int argc, char **argv)
{
    struct options options = { .persist = "none" };
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE SOCKET [--fail-write=N] "
                "[--fail-flush=N] [--cut-after=N] "
                "[--persist=none|all|odd|even|reverse] [--arm-on-signal] [--control-stdin]\n",
                argv[0]);
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        if (!strncmp(argv[i], "--fail-write=", 13)) {
            if (parse_u64(argv[i] + 13, &options.fail_write)) return 2;
        } else if (!strncmp(argv[i], "--fail-flush=", 13)) {
            if (parse_u64(argv[i] + 13, &options.fail_flush)) return 2;
        } else if (!strncmp(argv[i], "--cut-after=", 12)) {
            if (parse_u64(argv[i] + 12, &options.cut_after)) return 2;
        } else if (!strncmp(argv[i], "--persist=", 10))
            options.persist = argv[i] + 10;
        else if (!strcmp(argv[i], "--arm-on-signal"))
            options.arm_on_signal = 1;
        else if (!strcmp(argv[i], "--control-stdin")) options.control_stdin = 1;
        else return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, arm_handler);
    struct fault_block disk;
    if (fault_block_open(&disk, argv[1])) { perror("image"); return 1; }
    disk.fail_write = options.arm_on_signal ? 0 : options.fail_write;
    disk.fail_flush = options.arm_on_signal ? 0 : options.fail_flush;
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (listener < 0 || strlen(argv[2]) >= sizeof(address.sun_path))
        return 1;
    strcpy(address.sun_path, argv[2]);
    unlink(argv[2]);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, 1)) { perror("socket"); return 1; }
    int client = accept(listener, 0, 0);
    int result = client < 0 || negotiate(client, &disk) ||
                 transmit(client, &disk, &options);
    if (client >= 0) close(client);
    close(listener);
    unlink(argv[2]);
    fault_block_close(&disk);
    return result ? 1 : 0;
}
