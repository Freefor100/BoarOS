#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BOAR_NVME_URING_TEST
#include <linux/io_uring.h>
#include <linux/nvme_ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#define SYS_RING_SETUP 425
#define SYS_RING_ENTER 426
#endif

static unsigned char data[16384] __attribute__((aligned(16384)));
static unsigned char result[16384] __attribute__((aligned(16384)));

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "NVME FAIL line=%d check=%s errno=%d\n", __LINE__, #test, errno); \
    return 1; \
} } while (0)

#ifdef BOAR_NVME_URING_TEST
struct rings {
    int fd;
    void *sq, *cq, *entries;
    size_t sq_size, cq_size, entries_size;
    struct io_uring_params parameters;
};

static uint32_t *field(void *base, uint32_t offset)
{ return (uint32_t *)((char *)base + offset); }

static int ring_setup(struct rings *ring)
{
    struct io_uring_params *p = &ring->parameters;
    p->flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
    ring->fd = (int)syscall(SYS_RING_SETUP, 8, p);
    CHECK(ring->fd >= 0);
    ring->sq_size = p->sq_off.array + p->sq_entries * sizeof(uint32_t);
    ring->cq_size = p->cq_off.cqes + p->cq_entries * 32;
    ring->entries_size = p->sq_entries * 128;
    if (p->features & IORING_FEAT_SINGLE_MMAP) {
        if (ring->sq_size < ring->cq_size) ring->sq_size = ring->cq_size;
        ring->sq = mmap(NULL, ring->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
            ring->fd, IORING_OFF_SQ_RING);
        ring->cq = ring->sq;
    } else {
        ring->sq = mmap(NULL, ring->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
            ring->fd, IORING_OFF_SQ_RING);
        ring->cq = mmap(NULL, ring->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
            ring->fd, IORING_OFF_CQ_RING);
    }
    ring->entries = mmap(NULL, ring->entries_size, PROT_READ | PROT_WRITE, MAP_SHARED,
        ring->fd, IORING_OFF_SQES);
    CHECK(ring->sq != MAP_FAILED && ring->cq != MAP_FAILED && ring->entries != MAP_FAILED);
    return 0;
}

static int ring_command(struct rings *ring, int target, unsigned command,
    const struct nvme_uring_cmd *payload)
{
    struct io_uring_params *p = &ring->parameters;
    uint32_t *sq_tail = field(ring->sq, p->sq_off.tail);
    uint32_t tail = __atomic_load_n(sq_tail, __ATOMIC_RELAXED);
    uint32_t index = tail & *field(ring->sq, p->sq_off.ring_mask);
    struct io_uring_sqe *sqe = (void *)((char *)ring->entries + index * 128);
    memset(sqe, 0, 128);
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->fd = target;
    sqe->cmd_op = command;
    sqe->user_data = tail + 1;
    memcpy(sqe->cmd, payload, sizeof(*payload));
    field(ring->sq, p->sq_off.array)[index] = index;
    __atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
    CHECK(syscall(SYS_RING_ENTER, ring->fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0) == 1);
    uint32_t *cq_head = field(ring->cq, p->cq_off.head);
    uint32_t head = __atomic_load_n(cq_head, __ATOMIC_RELAXED);
    CHECK(head != __atomic_load_n(field(ring->cq, p->cq_off.tail), __ATOMIC_ACQUIRE));
    index = head & *field(ring->cq, p->cq_off.ring_mask);
    struct io_uring_cqe *entry = (void *)((char *)ring->cq + p->cq_off.cqes + index * 32);
    CHECK(entry->user_data == tail + 1);
    if (entry->res) {
        fprintf(stderr, "NVME command failed operation=%#x opcode=%u result=%d\n",
            command, payload->opcode, entry->res);
        return 1;
    }
    __atomic_store_n(cq_head, head + 1, __ATOMIC_RELEASE);
    return 0;
}

static int check_commands(int block)
{
    struct rings ring = {0};
    if (ring_setup(&ring)) return 1;
    int ctrl = open("/dev/nvme0", O_RDWR);
    int ns = open("/dev/ng0n1", O_RDWR);
    CHECK(ctrl >= 0 && ns >= 0);
    struct nvme_uring_cmd op = {.opcode = 6, .cdw10 = 1, .data_len = 4096,
        .addr = (uintptr_t)result };
    CHECK(!ring_command(&ring, ctrl, NVME_URING_CMD_ADMIN, &op));
    CHECK(result[0] || result[1]);
    struct iovec vectors[2] = {{result, 2048}, {result + 2048, 2048}};
    op.addr = (uintptr_t)vectors;
    op.data_len = 2;
    CHECK(!ring_command(&ring, ctrl, NVME_URING_CMD_ADMIN_VEC, &op));
    CHECK(result[0] || result[1]);

    memset(data, 0x2b, 1024);
    op = (struct nvme_uring_cmd){ .opcode = 1, .nsid = 1, .cdw10 = 4096,
        .cdw12 = 1, .data_len = 1024, .addr = (uintptr_t)data };
    CHECK(!ring_command(&ring, ns, NVME_URING_CMD_IO, &op));
    CHECK(pread(block, result, 1024, 4096 * 512) == 1024 && !memcmp(data, result, 1024));
    vectors[0] = (struct iovec){result, 512};
    vectors[1] = (struct iovec){result + 512, 512};
    memset(result, 0, 1024);
    op.addr = (uintptr_t)vectors;
    op.data_len = 2;
    op.opcode = 2;
    CHECK(!ring_command(&ring, ns, NVME_URING_CMD_IO_VEC, &op));
    CHECK(!memcmp(data, result, 1024));
    CHECK(!close(ns) && !close(ctrl));
    CHECK(!munmap(ring.entries, ring.entries_size));
    if (ring.cq != ring.sq) CHECK(!munmap(ring.cq, ring.cq_size));
    CHECK(!munmap(ring.sq, ring.sq_size) && !close(ring.fd));
    puts("NVME URING PASS");
    return 0;
}
#endif

int main(void)
{
    const char *path = "/dev/nvme0n1";
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL))
        CHECK(errno == EBUSY);
#ifdef BOAR_NVME_FS_TEST
    if (mkdir("/mnt", 0700)) CHECK(errno == EEXIST);
    CHECK(!mount(path, "/mnt", "ext4", 0, NULL));
    int fd = open("/mnt/record", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(fd >= 0);
    for (unsigned i = 0; i < 32; i++) {
        for (size_t j = 0; j < sizeof(data); j++) data[j] = (unsigned char)(i + 17 * j);
        CHECK(write(fd, data, sizeof(data)) == sizeof(data));
    }
    CHECK(!fsync(fd) && !close(fd) && !umount("/mnt"));
    CHECK(!mount(path, "/mnt", "ext4", MS_RDONLY, NULL));
    fd = open("/mnt/record", O_RDONLY);
    CHECK(fd >= 0);
    for (unsigned i = 0; i < 32; i++) {
        CHECK(read(fd, result, sizeof(result)) == sizeof(result));
        for (size_t j = 0; j < sizeof(data); j++) CHECK(result[j] == (unsigned char)(i + 17 * j));
    }
    CHECK(!close(fd) && !umount("/mnt"));
#else
    int fd = open(path, O_RDWR | O_DIRECT);
    CHECK(fd >= 0);
    uint64_t capacity = 0;
    unsigned sector = 0;
    CHECK(!ioctl(fd, BLKGETSIZE64, &capacity) && capacity == 8U * 1024U * 1024U);
    CHECK(!ioctl(fd, BLKSSZGET, &sector) && sector == 512);
    /* Enough commands to wrap both hardware rings. Check each data pattern
     * independently of the completion status. */
    for (unsigned i = 0; i < 72; ++i) {
        uint64_t offset = i * sizeof(data);
        for (unsigned j = 0; j < sizeof(data); ++j)
            data[j] = (unsigned char)(17 * i + j / 4096 + 31 * j);
        CHECK(pwrite(fd, data, sizeof(data), offset) == sizeof(data));
        CHECK(pread(fd, result, sizeof(result), offset) == sizeof(result));
        CHECK(!memcmp(data, result, sizeof(data)));
    }
    CHECK(!fsync(fd));
#ifdef BOAR_NVME_URING_TEST
    CHECK(!check_commands(fd) && !fsync(fd));
#endif
    CHECK(!close(fd));
#endif
    puts("NVME ALL PASS");
    return 0;
}
