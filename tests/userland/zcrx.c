#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { ENTRIES = 32, TOTAL = 128 * 1024 };
enum { RECV_DATA = 0x1122, CANCEL_DATA = 0x3344 };

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "ZCRX FAIL line=%d check=%s errno=%d\n", __LINE__, #test, errno); \
    return 1; \
} } while (0)

struct ring {
    int fd;
    void *sq, *cq;
    struct io_uring_sqe *entries;
    struct io_uring_params params;
    size_t sq_size, cq_size, sqes_size;
};

static uint32_t *word(void *mapping, uint32_t offset)
{ return (uint32_t *)((char *)mapping + offset); }

static int setup(struct ring *ring, size_t page)
{
    struct io_uring_params *p = &ring->params;
    p->flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQE32;
    ring->fd = syscall(SYS_io_uring_setup, 8, p);
    CHECK(ring->fd >= 0);
    ring->sq_size = p->sq_off.array + p->sq_entries * 4;
    ring->cq_size = p->cq_off.cqes + p->cq_entries * 32;
    ring->sqes_size = p->sq_entries * sizeof(*ring->entries);
    if ((p->features & IORING_FEAT_SINGLE_MMAP) && ring->sq_size < ring->cq_size)
        ring->sq_size = ring->cq_size;
    ring->sq_size = (ring->sq_size + page - 1) & -page;
    ring->cq_size = (ring->cq_size + page - 1) & -page;
    ring->sqes_size = (ring->sqes_size + page - 1) & -page;
    ring->sq = mmap(NULL, ring->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd,
        IORING_OFF_SQ_RING);
    ring->cq = p->features & IORING_FEAT_SINGLE_MMAP ? ring->sq :
        mmap(NULL, ring->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd,
            IORING_OFF_CQ_RING);
    ring->entries = mmap(NULL, ring->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd,
        IORING_OFF_SQES);
    CHECK(ring->sq != MAP_FAILED && ring->cq != MAP_FAILED && ring->entries != MAP_FAILED);
    return 0;
}

static int submit(struct ring *ring, struct io_uring_sqe sqe)
{
    const struct io_sqring_offsets *o = &ring->params.sq_off;
    uint32_t *tail = word(ring->sq, o->tail);
    uint32_t position = __atomic_load_n(tail, __ATOMIC_RELAXED);
    uint32_t index = position & *word(ring->sq, o->ring_mask);
    ring->entries[index] = sqe;
    word(ring->sq, o->array)[index] = index;
    __atomic_store_n(tail, position + 1, __ATOMIC_RELEASE);
    return syscall(SYS_io_uring_enter, ring->fd, 1, 0, 0, NULL, 0) == 1 ? 0 : 1;
}

static int pop(struct ring *ring, struct io_uring_cqe *cqe, struct io_uring_zcrx_cqe *data)
{
    const struct io_cqring_offsets *o = &ring->params.cq_off;
    uint32_t *head = word(ring->cq, o->head), *tail = word(ring->cq, o->tail);
    uint32_t position = __atomic_load_n(head, __ATOMIC_RELAXED);
    while (position == __atomic_load_n(tail, __ATOMIC_ACQUIRE))
        CHECK(syscall(SYS_io_uring_enter, ring->fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) == 0);
    uint32_t index = position & *word(ring->cq, o->ring_mask);
    const char *entry = (const char *)ring->cq + o->cqes + 32 * index;
    memcpy(cqe, entry, sizeof(*cqe));
    memcpy(data, entry + 16, sizeof(*data));
    __atomic_store_n(head, position + 1, __ATOMIC_RELEASE);
    return 0;
}

static int endpoint(int type, unsigned port)
{
    int fd = socket(AF_INET, type, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "eth1", 5) ||
        bind(fd, (void *)&address, sizeof(address))) return -1;
    return fd;
}

static int configure(unsigned *index)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq req = {.ifr_name = "eth1"};
    struct sockaddr_in *addr = (void *)&req.ifr_addr;
    addr->sin_family = AF_INET;
    CHECK(fd >= 0 && inet_pton(AF_INET, "10.77.0.3", &addr->sin_addr) == 1);
    CHECK(ioctl(fd, SIOCSIFADDR, &req) == 0);
    CHECK(inet_pton(AF_INET, "255.255.255.0", &addr->sin_addr) == 1);
    CHECK(ioctl(fd, SIOCSIFNETMASK, &req) == 0);
    req.ifr_flags = IFF_UP;
    CHECK(ioctl(fd, SIOCSIFFLAGS, &req) == 0 && ioctl(fd, SIOCGIFINDEX, &req) == 0);
    *index = req.ifr_ifindex;
    CHECK(*index && close(fd) == 0);
    return 0;
}

static int check_udp(int udp)
{
    unsigned char bytes[1472];
    struct sockaddr_in peer;
    socklen_t size = sizeof(peer);
    CHECK(recvfrom(udp, bytes, sizeof(bytes), 0, (void *)&peer, &size) == sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) CHECK(bytes[i] == (unsigned char)(13 * i));
    CHECK(sendto(udp, bytes, sizeof(bytes), 0, (void *)&peer, size) == sizeof(bytes));
    return 0;
}

static int report_dma_pages(unsigned char *bytes, size_t page)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    CHECK(fd >= 0);
    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t entry;
        off_t offset = ((uintptr_t)bytes / page + i) * sizeof(entry);
        CHECK(pread(fd, &entry, sizeof(entry), offset) == sizeof(entry));
        uint64_t pfn = entry & ((UINT64_C(1) << 55) - 1);
        CHECK((entry >> 63) && pfn);
        printf("ZCRX PAGE %llx\n", (unsigned long long)(pfn * page));
    }
    CHECK(close(fd) == 0);
    return 0;
}

int main(void)
{
    unsigned ifindex;
    CHECK(configure(&ifindex) == 0);
    size_t page = sysconf(_SC_PAGESIZE);
    CHECK(page == 4096 || page == 16384);
    struct ring ring = {0};
    CHECK(setup(&ring, page) == 0);
    unsigned char *bytes = mmap(NULL, page * ENTRIES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(bytes != MAP_FAILED);
    memset(bytes, 0xae, page * ENTRIES);
    struct io_uring_zcrx_area_reg area = {.addr = (uintptr_t)bytes, .len = page * ENTRIES};
    struct io_uring_region_desc region = {.size = page};
    struct io_uring_zcrx_ifq_reg ifq = {.if_idx = ifindex, .if_rxq = 1, .rq_entries = ENTRIES,
        .area_ptr = (uintptr_t)&area, .region_ptr = (uintptr_t)&region};
    CHECK(syscall(SYS_io_uring_register, ring.fd, IORING_REGISTER_ZCRX_IFQ, &ifq, 1) == 0);
    CHECK(ifq.zcrx_id == 0 && ifq.rq_entries == ENTRIES && ifq.rx_buf_len == page);
    CHECK((mkdir("/proc", 0755) == 0 || errno == EEXIST) &&
        (mount("proc", "/proc", "proc", 0, NULL) == 0 || errno == EBUSY));
    CHECK(report_dma_pages(bytes, page) == 0);
    void *rq = mmap(NULL, region.size, PROT_READ | PROT_WRITE, MAP_SHARED, ring.fd,
        region.mmap_offset);
    CHECK(rq != MAP_FAILED);
    struct io_uring_zcrx_rqe *entries = (void *)((char *)rq + ifq.offsets.rqes);
    uint32_t *rq_head = word(rq, ifq.offsets.head), *rq_tail = word(rq, ifq.offsets.tail);
    int udp = endpoint(SOCK_DGRAM, 18081), tcp = endpoint(SOCK_STREAM, 18080);
    CHECK(udp >= 0 && tcp >= 0 && listen(tcp, 1) == 0);
    puts("IGB NET READY");
    fflush(stdout);
    CHECK(check_udp(udp) == 0);
    int stream = accept(tcp, NULL, NULL);
    CHECK(stream >= 0);
    struct io_uring_sqe receive = {.opcode = IORING_OP_RECV_ZC, .fd = stream,
        .ioprio = IORING_RECV_MULTISHOT, .zcrx_ifq_idx = ifq.zcrx_id, .user_data = RECV_DATA};
    CHECK(submit(&ring, receive) == 0);
    uint32_t returned = 0;
    unsigned char pages[ENTRIES] = {0};
    unsigned reused = 0;
    for (unsigned done = 0; done < TOTAL;) {
        struct io_uring_cqe cqe;
        struct io_uring_zcrx_cqe data;
        CHECK(pop(&ring, &cqe, &data) == 0);
        CHECK(cqe.user_data == RECV_DATA && (cqe.flags & IORING_CQE_F_MORE));
        CHECK(cqe.res > 0 && (unsigned)cqe.res <= TOTAL - done && !data.__pad);
        size_t offset = data.off & ~IORING_ZCRX_AREA_MASK;
        CHECK((data.off & IORING_ZCRX_AREA_MASK) == area.rq_area_token);
        CHECK(offset < area.len && (size_t)cqe.res <= page - (offset % page));
        unsigned slot = offset / page;
        if (pages[slot]) reused++;
        pages[slot] = 1;
        for (unsigned i = 0; i < (unsigned)cqe.res; i++)
            CHECK(bytes[offset + i] == (unsigned char)(17 * (done + i)));
        ssize_t sent = 0;
        while (sent < cqe.res) {
            ssize_t step = write(stream, bytes + offset + sent, cqe.res - sent);
            CHECK(step > 0);
            sent += step;
        }
        CHECK(returned - __atomic_load_n(rq_head, __ATOMIC_ACQUIRE) < ENTRIES);
        entries[returned & (ENTRIES - 1)] = (struct io_uring_zcrx_rqe){
            .off = data.off, .len = cqe.res,
        };
        __atomic_store_n(rq_tail, ++returned, __ATOMIC_RELEASE);
        done += cqe.res;
    }
    CHECK(returned > ENTRIES && reused);
    struct io_uring_sqe cancel = {.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
        .addr = RECV_DATA, .user_data = CANCEL_DATA};
    CHECK(submit(&ring, cancel) == 0);
    unsigned completions = 0;
    while (completions != 3) {
        struct io_uring_cqe cqe;
        struct io_uring_zcrx_cqe data;
        CHECK(pop(&ring, &cqe, &data) == 0);
        if (cqe.user_data == RECV_DATA && cqe.res == -ECANCELED && !(cqe.flags & IORING_CQE_F_MORE))
            completions |= 1;
        else if (cqe.user_data == CANCEL_DATA && !cqe.res)
            completions |= 2;
        else CHECK(0);
    }
    CHECK(write(stream, "\xab", 1) == 1);
    unsigned char final;
    CHECK(read(stream, &final, 1) == 0);
    CHECK(close(stream) == 0 && close(tcp) == 0 && close(udp) == 0);
    CHECK(munmap(rq, region.size) == 0 && munmap(ring.entries, ring.sqes_size) == 0);
    if (ring.cq != ring.sq) CHECK(munmap(ring.cq, ring.cq_size) == 0);
    CHECK(munmap(ring.sq, ring.sq_size) == 0 && close(ring.fd) == 0);
    CHECK(munmap(bytes, page * ENTRIES) == 0);
    CHECK(umount("/proc") == 0);
    puts("ZCRX NETWORK PASS");
    return 0;
}
