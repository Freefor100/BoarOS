#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "epoll failure line=%d errno=%d: %s\n", __LINE__, errno, #x); exit(1); } } while (0)

static void delivery(unsigned mode, const char *name, unsigned prefix)
{
    int pipes[2][2], ep = epoll_create1(0);
    CHECK(ep >= 0);
    long page=sysconf(_SC_PAGESIZE);CHECK(page>=4096);
    char *pages = mmap(NULL, (size_t)page*2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED && !mprotect(pages + page, (size_t)page, PROT_NONE));
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!pipe(pipes[i]));
        struct epoll_event event = {.events = EPOLLIN | mode, .data.u64 = i + 1};
        CHECK(!epoll_ctl(ep, EPOLL_CTL_ADD, pipes[i][0], &event));
        CHECK(write(pipes[i][1], "x", 1) == 1);
    }
    /* prefix=0 faults immediately; 8 faults inside a native event; one whole
     * event succeeds before the next page faults in the remaining case. */
    struct epoll_event *fault = (void *)(pages + page - prefix);
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
    CHECK(!close(ep) && !munmap(pages, (size_t)page*2));
    printf("EPOLL PASS %s prefix=%u\n", name, prefix);
}

static void socket_event_interest(void)
{
    int pair[2], ep = epoll_create1(0);
    CHECK(ep >= 0);
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct epoll_event watch = {.events = EPOLLIN | EPOLLET, .data.u64 = 42};
    struct epoll_event event;
    char byte;
    CHECK(!epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &watch));
    CHECK(send(pair[1], "a", 1, 0) == 1);
    CHECK(epoll_wait(ep, &event, 1, 0) == 1 && event.events == EPOLLIN);

    /* Leave the first input unread. Returning the sender's output space
     * cannot deliver a second input edge. */
    CHECK(send(pair[0], "b", 1, 0) == 1);
    CHECK(recv(pair[1], &byte, 1, 0) == 1 && byte == 'b');
    CHECK(epoll_wait(ep, &event, 1, 0) == 0);
    CHECK(send(pair[1], "c", 1, 0) == 1);
    CHECK(epoll_wait(ep, &event, 1, 0) == 1 && event.events == EPOLLIN);

    watch.events = EPOLLOUT | EPOLLET;
    CHECK(!epoll_ctl(ep, EPOLL_CTL_MOD, pair[0], &watch));
    CHECK(epoll_wait(ep, &event, 1, 0) == 1 && event.events == EPOLLOUT);
    CHECK(send(pair[1], "d", 1, 0) == 1);
    CHECK(epoll_wait(ep, &event, 1, 0) == 0);
    CHECK(send(pair[0], "e", 1, 0) == 1);
    CHECK(recv(pair[1], &byte, 1, 0) == 1 && byte == 'e');
    CHECK(epoll_wait(ep, &event, 1, 0) == 1 && event.events == EPOLLOUT);
    CHECK(!shutdown(pair[0], SHUT_RDWR));
    CHECK(epoll_wait(ep, &event, 1, 0) == 1 && (event.events & EPOLLHUP));
    CHECK(!close(pair[0]) && !close(pair[1]) && !close(ep));
    puts("EPOLL PASS socket interest");
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
    socket_event_interest();
    puts("EPOLL PASS delivery");
    return 0;
}
