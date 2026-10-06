#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "epoll failure line=%d errno=%d: %s\n", __LINE__, errno, #x); exit(1); } } while (0)

static void delivery(unsigned mode, const char *name, unsigned prefix)
{
    int pipes[2][2], ep = epoll_create1(0);
    CHECK(ep >= 0);
    char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED && !mprotect(pages + 4096, 4096, PROT_NONE));
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!pipe(pipes[i]));
        struct epoll_event event = {.events = EPOLLIN | mode, .data.u64 = i + 1};
        CHECK(!epoll_ctl(ep, EPOLL_CTL_ADD, pipes[i][0], &event));
        CHECK(write(pipes[i][1], "x", 1) == 1);
    }
    /* prefix=0 faults immediately; 8 faults inside a RV64 event; one whole
     * event succeeds before the next page faults in the remaining case. */
    struct epoll_event *fault = (void *)(pages + 4096 - prefix);
    errno = 0;
    int first = epoll_wait(ep, fault, 2, 0);
    unsigned delivered = prefix / sizeof(*fault);
    CHECK(delivered ? first == (int)delivered : first == -1 && errno == EFAULT);
    struct epoll_event events[2];
    int count = epoll_wait(ep, events, 2, 0);
    CHECK(count == (mode ? 2 - (int)delivered : 2));
    for (int i = 0; i < count; i++) {
        CHECK(events[i].events == EPOLLIN);
        CHECK(events[i].data.u64 >= 1 && events[i].data.u64 <= 2);
        if (mode && delivered) CHECK(events[i].data.u64 != fault[0].data.u64);
    }
    if (mode) CHECK(epoll_wait(ep, events, 2, 0) == 0);
    if (mode == EPOLLONESHOT) {
        struct epoll_event event = {.events = EPOLLIN | mode, .data.u64 = 9};
        CHECK(!epoll_ctl(ep, EPOLL_CTL_MOD, pipes[0][0], &event));
        CHECK(epoll_wait(ep, events, 2, 0) == 1 && events[0].data.u64 == 9);
    }
    for (unsigned i = 0; i < 2; i++) CHECK(!close(pipes[i][0]) && !close(pipes[i][1]));
    CHECK(!close(ep) && !munmap(pages, 8192));
    printf("EPOLL PASS %s prefix=%u\n", name, prefix);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const unsigned modes[] = {0, EPOLLET, EPOLLONESHOT};
    const char *names[] = {"LT", "ET", "ONESHOT"};
    for (unsigned i = 0; i < 3; i++) {
        delivery(modes[i], names[i], 0);
        delivery(modes[i], names[i], 8);
        delivery(modes[i], names[i], sizeof(struct epoll_event));
    }
    puts("EPOLL PASS delivery");
    return 0;
}
