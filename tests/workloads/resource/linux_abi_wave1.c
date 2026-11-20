#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/membarrier.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(test) do { \
    if (!(test)) { \
        printf("WAVE1 FAIL %s:%d: %s errno=%d\n", __func__, __LINE__, #test, errno); \
        return 1; \
    } \
} while (0)

static int release_version(void)
{
    struct utsname name;
    CHECK(uname(&name) == 0);
    int boaros = strstr(name.version, "BoarOS") != NULL;
    if (boaros)
        CHECK(strcmp(name.release, "6.7.0-boaros") == 0);
    int flags = personality(0xffffffffUL);
    CHECK(flags >= 0 && personality((unsigned)flags | UNAME26) >= 0);
    CHECK(uname(&name) == 0 && !strncmp(name.release, "2.6.", 4));
    if (boaros)
        CHECK(strcmp(name.release, "2.6.67-boaros") == 0);
    CHECK(personality((unsigned)flags) >= 0);
    puts("WAVE1 PASS uname");
    return 0;
}

static long sync_range(int fd, long offset, long length, unsigned flags)
{
    return syscall(84, fd, offset, length, flags);
}

static int range_writeback(void)
{
    int fd = open("/sync-range", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    int readonly = open("/sync-range", O_RDONLY);
    CHECK(readonly >= 0);
    int fifo[2];
    CHECK(pipe(fifo) == 0);
    CHECK(sync_range(-1, -1, -1, 8) == -1 && errno == EBADF);
    CHECK(sync_range(fifo[0], -1, 1, 1) == -1 && errno == EINVAL);
    CHECK(sync_range(fifo[0], 0, 1, 1) == -1 && errno == ESPIPE);
    CHECK(sync_range(fd, INT64_MAX, 1, 0) == -1 && errno == EINVAL);
    CHECK(sync_range(fd, 0, -1, 0) == -1 && errno == EINVAL);
    CHECK(sync_range(fd, 0, 1, 8) == -1 && errno == EINVAL);
    CHECK(sync_range(fd, 0, 0, 0) == 0);
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page == 4096 || page == 16384);
    CHECK(pwrite(fd, "A", 1, 1) == 1);
    CHECK(pwrite(fd, "B", 1, page + 1) == 1);
    CHECK(sync_range(readonly, 1, 1, 2) == 0);
    CHECK(sync_range(fd, 0, page, 4) == 0);
    CHECK(sync_range(fd, page, 0, 1 | 2 | 4) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    char byte = 0;
    CHECK(pread(fd, &byte, 1, page + 1) == 1 && byte == 'B');
    CHECK(close(fifo[0]) == 0 && close(fifo[1]) == 0);
    CHECK(close(readonly) == 0 && close(fd) == 0 && unlink("/sync-range") == 0);
    puts("WAVE1 PASS sync_file_range");
    return 0;
}

static int physical_byte(int dev, long offset)
{
    static _Alignas(16384) unsigned char block[4096];
    return pread(dev, block, sizeof(block), offset) == sizeof(block) ? block[0] : -1;
}

/* The fixture supplies the physical offsets of two preallocated ext4 blocks,
 * separated by 64 KiB. Inspect the block device before final filesystem sync. */
static int physical_range(void)
{
    if (access("/scratch-offsets", F_OK) != 0) {
        CHECK(errno == ENOENT);
        return 0;
    }
    FILE *offsets = fopen("/scratch-offsets", "r");
    long first, second;
    CHECK(offsets && fscanf(offsets, "%ld %ld", &first, &second) == 2);
    CHECK(fclose(offsets) == 0 && first > 0 && second > first);
    CHECK(mkdir("/devices", 0755) == 0 && mkdir("/scratch", 0755) == 0);
    CHECK(mount("devtmpfs", "/devices", "devtmpfs", 0, NULL) == 0);
    struct stat root, second_disk;
    CHECK(stat("/", &root) == 0 && stat("/devices/vdb", &second_disk) == 0);
    const char *device = root.st_dev == second_disk.st_rdev ? "/devices/vda" : "/devices/vdb";
    CHECK(mount(device, "/scratch", "ext4", 0, NULL) == 0);
    int dev = open(device, O_RDONLY | O_DIRECT);
    int fd = open("/scratch/data", O_RDWR);
    CHECK(dev >= 0 && fd >= 0);
    CHECK(physical_byte(dev, first) == 'A');
    CHECK(physical_byte(dev, second) == 'A');
    CHECK(pwrite(fd, "B", 1, 0) == 1 && pwrite(fd, "C", 1, 65536) == 1);
    CHECK(sync_range(fd, 0, 1, 2) == 0);
    CHECK(sync_range(fd, 0, 1, 4) == 0);
    CHECK(physical_byte(dev, first) == 'B');
    CHECK(physical_byte(dev, second) == 'A');
    CHECK(sync_range(fd, 65536, 0, 1 | 2 | 4) == 0);
    CHECK(physical_byte(dev, second) == 'C');
    CHECK(close(fd) == 0 && close(dev) == 0);
    CHECK(umount("/scratch") == 0 && umount("/devices") == 0);
    puts("WAVE1 PASS physical-range");
    return 0;
}

static int barrier(long command, unsigned flags)
{
    return (int)syscall(283, command, flags, 0);
}

struct barrier_peer {
    int cpu;
    int ready;
    unsigned stop;
    unsigned work;
    int failed;
};

static void *peer_main(void *argument)
{
    struct barrier_peer *peer = argument;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(peer->cpu, &mask);
    int error = sched_setaffinity(0, sizeof(mask), &mask);
    __atomic_store_n(&peer->ready, error ? -1 : 1, __ATOMIC_RELEASE);
    if (!error) {
        while (!__atomic_load_n(&peer->stop, __ATOMIC_ACQUIRE)) {
            if (barrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) != 0) {
                peer->failed = 1;
                break;
            }
            __atomic_add_fetch(&peer->work, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static int membarrier_contract(void)
{
    int commands = barrier(MEMBARRIER_CMD_QUERY, 0);
    unsigned common = MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED |
        MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | MEMBARRIER_CMD_PRIVATE_EXPEDITED |
        MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    CHECK(commands >= 0 && ((unsigned)commands & common) == common);
    CHECK(barrier(MEMBARRIER_CMD_QUERY, 1) == -1 && errno == EINVAL);
    CHECK(barrier(1 << 28, 0) == -1 && errno == EINVAL);
    CHECK(barrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == -1 && errno == EPERM);
    CHECK(barrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0);
    CHECK(barrier(MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED, 0) == 0);
    CHECK(barrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0);
    enum { GET_REGISTRATIONS = 1 << 9 };
    unsigned registrations = MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |
        MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    CHECK(barrier(GET_REGISTRATIONS, 0) == (int)registrations);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child)
        _exit(barrier(GET_REGISTRATIONS, 0) != (int)registrations ||
              barrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) != 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    cpu_set_t allowed;
    CHECK(sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    struct barrier_peer peer = {0};
    pthread_t thread;
    int callers = CPU_COUNT(&allowed);
    if (callers > 1) {
        int first = -1;
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (!CPU_ISSET(i, &allowed))
                continue;
            if (first >= 0) {
                peer.cpu = i;
                break;
            }
            first = i;
        }
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(first, &mask);
        CHECK(sched_setaffinity(0, sizeof(mask), &mask) == 0);
        CHECK(pthread_create(&thread, NULL, peer_main, &peer) == 0);
        while (!__atomic_load_n(&peer.ready, __ATOMIC_ACQUIRE))
            sched_yield();
        CHECK(peer.ready == 1);
    }
    CHECK(barrier(MEMBARRIER_CMD_GLOBAL, 0) == 0);
    for (unsigned i = 0; i < 64; i++) {
        CHECK(barrier(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0) == 0);
        CHECK(barrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == 0);
    }
    if (callers > 1) {
        __atomic_store_n(&peer.stop, 1, __ATOMIC_RELEASE);
        CHECK(pthread_join(thread, NULL) == 0 && peer.work > 0 && !peer.failed);
        CHECK(sched_setaffinity(0, sizeof(allowed), &allowed) == 0);
    }
    puts("WAVE1 PASS membarrier");
    return 0;
}

#if defined(__riscv)
struct hwprobe_pair { int64_t key; uint64_t value; };

static int hwprobe_contract(void)
{
    struct hwprobe_pair entries[] = {{3, 0}, {4, 0}, {7, 0}, {INT64_MAX, 1}};
    CHECK(syscall(258, entries, 4, 0, NULL, 0) == 0);
    CHECK(entries[0].key == 3 && (entries[0].value & 1));
    CHECK(entries[1].key == 4 && (entries[1].value & 3) == 3);
    CHECK(entries[2].key == 7 && entries[2].value > UINT32_MAX);
    CHECK(entries[3].key == -1 && entries[3].value == 0);
    CHECK(syscall(258, NULL, 1, 0, NULL, 0) == -1 && errno == EFAULT);
    CHECK(syscall(258, NULL, 0, 0, NULL, 2) == -1 && errno == EINVAL);
    unsigned long mask = 0;
    CHECK(syscall(258, entries, 1, sizeof(mask), &mask, 1) == 0 && mask);
    mask = 0;
    CHECK(syscall(258, entries, 1, sizeof(mask), &mask, 0) == -1 && errno == EINVAL);
    puts("WAVE1 PASS riscv_hwprobe");
    return 0;
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    failures += release_version();
    failures += range_writeback();
    failures += physical_range();
    failures += membarrier_contract();
#if defined(__riscv)
    failures += hwprobe_contract();
#endif
    printf("WAVE1 END %d\n", failures);
    sync();
    if (reboot(RB_POWER_OFF))
        perror("reboot");
    return failures ? 1 : 0;
}
