/* Consume one host-generated seed only after its disk copy is durably erased. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef SEED_ARCH
#error SEED_ARCH must identify the target ELF machine
#endif
#define RNDADDENTROPY 0x40085203UL
#define RNDGETENTCNT 0x80045200UL
#define MIB (1024ULL * 1024)

static void erase(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (size--) *p++ = 0;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32(uint32_t crc, const unsigned char *p, size_t size)
{
    while (size--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
    }
    return crc;
}

static int device(const char *path, unsigned minor_number, uint64_t expected, int flags)
{
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    uint64_t bytes = 0;
    int readonly = -1, sector = 0;
    if (fstat(fd, &st) || ioctl(fd, BLKGETSIZE64, &bytes) ||
        ioctl(fd, BLKSSZGET, &sector) || ioctl(fd, BLKROGET, &readonly)) goto fail;
    if (!S_ISBLK(st.st_mode) || major(st.st_rdev) != 252 || minor(st.st_rdev) != minor_number ||
        sector != 512 || readonly || !bytes || (expected && bytes != expected)) {
        errno = EINVAL;
        goto fail;
    }
    return fd;
fail: {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
}

static int transfer(int fd, void *buffer, size_t size, off_t offset, int writing)
{
    unsigned char *p = buffer;
    while (size) {
        ssize_t done = writing ? pwrite(fd, p, size, offset) : pread(fd, p, size, offset);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) { if (!done) errno = EIO; return -1; }
        p += done; size -= (size_t)done; offset += done;
    }
    return 0;
}

int main(void)
{
    unsigned char slot[4096] = {0}, table[512] = {0}, ready_byte = 0;
    struct { int bits, size; unsigned char data[64]; } entropy = {256, 64, {0}};
    const char *stage = "root-stat";
    int raw = -1, random = -1, fd = -1, result = 125, error = 0;
    struct stat root;
    if (stat("/", &root)) goto fail;
    if (major(root.st_dev) != 252 || minor(root.st_dev) != 0) { errno = EXDEV; goto fail; }
    stage = "root-device";
    fd = device("/dev/vda", 0, 0, O_RDONLY);
    if (fd < 0) goto fail;
    if (close(fd)) { fd = -1; goto fail; } fd = -1;
    stage = "second-device";
    raw = device("/dev/vdb", 16, 2304 * MIB, O_RDWR);
    if (raw < 0) goto fail;
    const uint32_t starts[] = {2048, 129 * 2048, 193 * 2048};
    const uint32_t sectors[] = {128 * 2048, 64 * 2048, 2048 * 2048};
    const unsigned char types[] = {0x83, 0x0c, 0x83};
    const char *paths[] = {"/dev/vdb1", "/dev/vdb2", "/dev/vdb3"};
    stage = "partition-table";
    if (transfer(raw, table, sizeof(table), 0, 0)) goto fail;
    if (table[510] != 0x55 || table[511] != 0xaa) { errno = EINVAL; goto fail; }
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char *entry = table + 446 + 16 * i;
        if (entry[0] || entry[4] != types[i] || le32(entry + 8) != starts[i] ||
            le32(entry + 12) != sectors[i]) { errno = EINVAL; goto fail; }
        stage = paths[i];
        fd = device(paths[i], 17 + i, (uint64_t)sectors[i] * 512, O_RDONLY);
        if (fd < 0) goto fail;
        if (close(fd)) { fd = -1; goto fail; } fd = -1;
    }
    stage = "fourth-partition";
    for (unsigned i = 494; i < 510; ++i) if (table[i]) { errno = EINVAL; goto fail; }
    stage = "seed-read";
    if (transfer(raw, slot, sizeof(slot), 4096, 0)) goto fail;
    stage = "seed-envelope";
    if (memcmp(slot, "BOARSEED", 8) || le32(slot + 8) != 1 ||
        le32(slot + 12) != SEED_ARCH || le32(slot + 16) != 64 ||
        ~crc32(crc32(~0U, slot, 20), slot + 24, 64) != le32(slot + 20)) {
        errno = EBADMSG; goto fail;
    }
    for (unsigned i = 88; i < sizeof(slot); ++i) if (slot[i]) { errno = EBADMSG; goto fail; }
    memcpy(entropy.data, slot + 24, sizeof(entropy.data));
    erase(slot, sizeof(slot));
    stage = "seed-erase";
    if (transfer(raw, slot, sizeof(slot), 4096, 1)) goto fail;
    stage = "seed-fsync";
    if (fsync(raw)) goto fail;
    /* No credit is possible until the raw-device flush above succeeds. */
    stage = "random-open";
    random = open("/dev/random", O_RDWR | O_CLOEXEC);
    if (random < 0) goto fail;
    stage = "RNDADDENTROPY";
    if (ioctl(random, RNDADDENTROPY, &entropy)) goto fail;
    erase(&entropy, sizeof(entropy));
    int bits = 0;
    stage = "RNDGETENTCNT";
    if (ioctl(random, RNDGETENTCNT, &bits)) goto fail;
    if (bits < 256) { errno = EIO; goto fail; }
    stage = "getrandom-ready";
    ssize_t received = getrandom(&ready_byte, 1, GRND_NONBLOCK);
    if (received != 1) { if (!received) errno = EIO; goto fail; }
    stage = "close";
    if (close(random)) { random = -1; goto fail; } random = -1;
    if (close(raw)) { raw = -1; goto fail; } raw = -1;
    puts("BOAROS-SEED READY erased=4096 credited_bits=256 scratch=/dev/vdb3");
    result = 0;
    goto out;
fail:
    error = errno;
    fprintf(stderr, "BOAROS-SEED ERROR stage=%s errno=%d (%s)\n", stage, error, strerror(error));
out:
    erase(slot, sizeof(slot)); erase(table, sizeof(table));
    erase(&entropy, sizeof(entropy)); erase(&ready_byte, sizeof(ready_byte));
    if (fd >= 0) close(fd);
    if (random >= 0) close(random);
    if (raw >= 0) close(raw);
    return result;
}
