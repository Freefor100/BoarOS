#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "UFFD FAIL at %d: %s errno=%d\n", __LINE__, #test, errno); \
    fflush(stderr); reboot(RB_POWER_OFF); _exit(1); \
} } while (0)

#define USER_MODE_ONLY 1
#define FAULT_MESSAGE 0x12
#define REGISTER_MISSING 1
#define DONTWAKE 1

struct uffd_api { uint64_t api, features, ioctls; };
struct uffd_range { uint64_t start, len; };
struct uffd_register { struct uffd_range range; uint64_t mode, ioctls; };
struct uffd_copy { uint64_t dst, src, len, mode; int64_t copied; };
struct uffd_zero { struct uffd_range range; uint64_t mode; int64_t copied; };
struct fault_message { uint64_t header, flags, address, extra; };

#define UFFD_API_IOCTL _IOWR(0xaa, 0x3f, struct uffd_api)
#define UFFD_REGISTER _IOWR(0xaa, 0x00, struct uffd_register)
#define UFFD_UNREGISTER _IOR(0xaa, 0x01, struct uffd_range)
#define UFFD_WAKE _IOR(0xaa, 0x02, struct uffd_range)
#define UFFD_COPY _IOWR(0xaa, 0x03, struct uffd_copy)
#define UFFD_ZEROPAGE _IOWR(0xaa, 0x04, struct uffd_zero)

static size_t page_size;

static unsigned char *anonymous_pages(unsigned count)
{
    unsigned char *address = mmap(NULL, page_size * count, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(address != MAP_FAILED);
    return address;
}

static int open_faults(unsigned char *memory, unsigned count)
{
    int fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | USER_MODE_ONLY);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    struct uffd_api api = {.api = 0xaa};
    CHECK(ioctl(fd, UFFD_API_IOCTL, &api) == 0 && api.api == 0xaa);
    struct uffd_register registration = {
        .range = {(uintptr_t)memory, count * page_size}, .mode = REGISTER_MISSING,
    };
    CHECK(ioctl(fd, UFFD_REGISTER, &registration) == 0);
    struct fault_message msg;
    CHECK(read(fd, &msg, sizeof(msg)) == -1 && errno == EAGAIN);
    return fd;
}

static void *read_missing(void *memory)
{
    return (void *)(uintptr_t)*(volatile unsigned char *)memory;
}

static struct fault_message expect_fault(int fd, const unsigned char *memory)
{
    struct pollfd polled = {.fd = fd, .events = POLLIN};
    CHECK(poll(&polled, 1, 3000) == 1 && (polled.revents & POLLIN));
    struct fault_message msg;
    CHECK(read(fd, &msg, sizeof(msg)) == sizeof(msg));
    CHECK((msg.header & 0xff) == FAULT_MESSAGE);
    CHECK((msg.address & ~(page_size - 1)) == (uintptr_t)memory);
    return msg;
}

static void expect_value(pthread_t thread, unsigned value)
{
    void *reported;
    CHECK(pthread_join(thread, &reported) == 0);
    CHECK((uintptr_t)reported == value);
}

static void resolve_and_unregister(void)
{
    unsigned char *pages = anonymous_pages(3);
    unsigned char *payload = anonymous_pages(1);
    memset(payload, 0x51, page_size);
    int fd = open_faults(pages, 3);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, read_missing, pages) == 0);
    CHECK(pthread_create(&b, NULL, read_missing, pages) == 0);
    expect_fault(fd, pages);
    struct uffd_copy copy = {
        .dst = (uintptr_t)pages, .src = (uintptr_t)payload, .len = page_size,
        .mode = DONTWAKE,
    };
    CHECK(ioctl(fd, UFFD_COPY, &copy) == 0 && copy.copied == (int64_t)page_size);
    struct uffd_range one = {(uintptr_t)pages, page_size};
    CHECK(ioctl(fd, UFFD_WAKE, &one) == 0);
    expect_value(a, 0x51);
    expect_value(b, 0x51);
    CHECK(memcmp(pages, payload, page_size) == 0);

    CHECK(pthread_create(&a, NULL, read_missing, pages + page_size) == 0);
    expect_fault(fd, pages + page_size);
    struct uffd_zero zero = {.range = {(uintptr_t)(pages + page_size), page_size}};
    CHECK(ioctl(fd, UFFD_ZEROPAGE, &zero) == 0 && zero.copied == (int64_t)page_size);
    expect_value(a, 0);

    CHECK(pthread_create(&a, NULL, read_missing, pages + 2 * page_size) == 0);
    expect_fault(fd, pages + 2 * page_size);
    one.start = (uintptr_t)(pages + 2 * page_size);
    CHECK(ioctl(fd, UFFD_UNREGISTER, &one) == 0);
    expect_value(a, 0);
    CHECK(close(fd) == 0);
    CHECK(munmap(payload, page_size) == 0 && munmap(pages, 3 * page_size) == 0);
}

static void close_releases_waiter(void)
{
    unsigned char *memory = anonymous_pages(1);
    int fd = open_faults(memory, 1);
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, read_missing, memory) == 0);
    expect_fault(fd, memory);
    CHECK(close(fd) == 0);
    expect_value(reader, 0);
    CHECK(munmap(memory, page_size) == 0);
}

static void zero_page_writes_stay_private(void)
{
    unsigned char *memory = anonymous_pages(2);
    int fd = open_faults(memory, 2);
    struct uffd_zero zero = {.range = {(uintptr_t)memory, 2 * page_size}};
    CHECK(ioctl(fd, UFFD_ZEROPAGE, &zero) == 0 && zero.copied == (int64_t)(2 * page_size));
    CHECK(mprotect(memory, page_size, PROT_READ) == 0);
    CHECK(mprotect(memory, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(memory[0] == 0 && memory[page_size] == 0);
    memory[0] = 0x35;
    CHECK(memory[page_size] == 0);
    CHECK(close(fd) == 0);
    memory[page_size] = 0x69;
    CHECK(memory[0] == 0x35 && memory[page_size] == 0x69);
    CHECK(munmap(memory, 2 * page_size) == 0);
}

static void remote_resolution(void)
{
    unsigned char *memory = anonymous_pages(1);
    int fd = open_faults(memory, 1);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        unsigned char *page = anonymous_pages(1);
        memset(page, 0x6d, page_size);
        expect_fault(fd, memory);
        struct uffd_copy copy = {
            .dst = (uintptr_t)memory, .src = (uintptr_t)page, .len = page_size,
        };
        CHECK(ioctl(fd, UFFD_COPY, &copy) == 0 && copy.copied == (int64_t)page_size);
        _exit(0);
    }
    CHECK(*(volatile unsigned char *)memory == 0x6d);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(close(fd) == 0 && munmap(memory, page_size) == 0);
}

int main(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size == 4096 || page_size == 16384);
    resolve_and_unregister();
    close_releases_waiter();
    zero_page_writes_stay_private();
    remote_resolution();
    puts("UFFD PASS");
    fflush(stdout);
    return reboot(RB_POWER_OFF) < 0;
}
