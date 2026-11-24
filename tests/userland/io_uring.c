#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "IO_URING FAIL at line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        exit(1); \
    } \
} while (0)

struct ring {
    struct io_uring_params params;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    uint32_t *sq_head, *sq_tail, *sq_array, *cq_head, *cq_tail, *sq_flags;
    void *shared;
    size_t shared_size, sqe_size;
    int fd;
};

static uint32_t *word(void *base, uint32_t offset)
{
    return (uint32_t *)((char *)base + offset);
}
static struct ring setup_flags(unsigned entries, unsigned flags)
{
    struct ring r = {0};
    r.params.flags = flags;
    r.fd = syscall(SYS_io_uring_setup, entries, &r.params);
    CHECK(r.fd >= 0);
    CHECK((r.params.features & IORING_FEAT_SINGLE_MMAP) != 0);
    CHECK(fcntl(r.fd, F_GETFD) == FD_CLOEXEC);
    size_t sq_size = r.params.sq_off.array + r.params.sq_entries * sizeof(uint32_t);
    size_t cq_size = r.params.cq_off.cqes + r.params.cq_entries * sizeof(*r.cqes);
    r.shared_size = sq_size > cq_size ? sq_size : cq_size;
    r.shared = mmap(NULL, r.shared_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    r.fd, IORING_OFF_SQ_RING);
    CHECK(r.shared != MAP_FAILED);
    r.sqe_size = r.params.sq_entries * sizeof(*r.sqes);
    r.sqes = mmap(NULL, r.sqe_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  r.fd, IORING_OFF_SQES);
    CHECK(r.sqes != MAP_FAILED);
    r.sq_head = word(r.shared, r.params.sq_off.head);
    r.sq_tail = word(r.shared, r.params.sq_off.tail);
    r.sq_array = word(r.shared, r.params.sq_off.array);
    r.sq_flags = word(r.shared, r.params.sq_off.flags);
    r.cq_head = word(r.shared, r.params.cq_off.head);
    r.cq_tail = word(r.shared, r.params.cq_off.tail);
    r.cqes = (struct io_uring_cqe *)((char *)r.shared + r.params.cq_off.cqes);
    CHECK(!*r.sq_head && !*r.sq_tail && !*r.cq_head && !*r.cq_tail);
    CHECK(*word(r.shared, r.params.sq_off.ring_mask) == r.params.sq_entries - 1);
    CHECK(*word(r.shared, r.params.cq_off.ring_mask) == r.params.cq_entries - 1);
    return r;
}

static struct ring setup(unsigned entries)
{
    return setup_flags(entries, 0);
}

static void push(struct ring *r, const struct io_uring_sqe *sqe)
{
    uint32_t tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    uint32_t slot = tail & (r->params.sq_entries - 1);
    r->sqes[slot] = *sqe;
    r->sq_array[slot] = slot;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
}
static long enter(struct ring *r, unsigned submit, unsigned min_complete, unsigned flags)
{
    return syscall(SYS_io_uring_enter, r->fd, submit, min_complete, flags, NULL, 0);
}
struct completion { uint64_t data; int32_t result; uint32_t flags; };
static struct completion pop(struct ring *r)
{
    uint32_t head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
    if (__atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE) == head)
        CHECK(enter(r, 0, 1, IORING_ENTER_GETEVENTS) == 0);
    CHECK(__atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE) != head);
    const struct io_uring_cqe *slot = &r->cqes[head & (r->params.cq_entries - 1)];
    struct completion cqe = {slot->user_data, slot->res, slot->flags};
    __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
    return cqe;
}
static void expect(struct ring *r, uint64_t user_data, int result)
{
    struct completion cqe = pop(r);
    CHECK(cqe.data == user_data && cqe.result == result && cqe.flags == 0);
}
static void destroy(struct ring *r)
{
    CHECK(!close(r->fd));
    CHECK(!munmap(r->sqes, r->sqe_size));
    CHECK(!munmap(r->shared, r->shared_size));
}

static void nop_and_overflow(void)
{
    struct ring r = setup(3);
    CHECK(r.params.sq_entries == 4 && r.params.cq_entries == 8);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 7};
    CHECK(ep >= 0 && !epoll_ctl(ep, EPOLL_CTL_ADD, r.fd, &interest));
    CHECK(!epoll_wait(ep, &interest, 1, 0));
    int alias = dup(r.fd);
    CHECK(alias >= 0 && !close(r.fd));
    r.fd = alias;
    for (unsigned i = 0; i < 12; i += 4) {
        for (unsigned j = 0; j < 4; j++) {
            struct io_uring_sqe sqe = {.opcode = IORING_OP_NOP, .fd = -1, .user_data = i+j};
            push(&r, &sqe);
        }
        CHECK(enter(&r, 4, 0, 0) == 4);
    }
    CHECK(*r.sq_head == 12);
    CHECK(epoll_wait(ep, &interest, 1, 0) == 1 && interest.data.u64 == 7);
    for (unsigned i = 0; i < 12; i++)
        expect(&r, i, 0);
    CHECK(!close(ep));
    destroy(&r);
    puts("IO_URING PASS mapped queues, wraparound, overflow and epoll");
}

static void regular_io(void)
{
    int fd = open("uring-data", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    struct ring r = setup(8);
    char first[] = "buffered hello", second[sizeof(first)] = {0};
    struct io_uring_sqe sqe = {
        .opcode = IORING_OP_WRITE, .fd = fd, .off = 0, .addr = (uintptr_t)first,
        .len = sizeof(first), .user_data = 21, .flags = IOSQE_IO_LINK,
    };
    push(&r, &sqe);
    sqe = (struct io_uring_sqe){
        .opcode = IORING_OP_READ, .fd = fd, .off = 0, .addr = (uintptr_t)second,
        .len = sizeof(second), .user_data = 22,
    };
    push(&r, &sqe);
    CHECK(enter(&r, 2, 2, IORING_ENTER_GETEVENTS) == 2);
    expect(&r, 21, sizeof(first));
    expect(&r, 22, sizeof(second));
    CHECK(!memcmp(first, second, sizeof(first)));
    CHECK(!close(fd) && !unlink("uring-data"));
    destroy(&r);
    puts("IO_URING PASS linked write and read");
}

static void registrations(void)
{
    struct ring r = setup(4);
    char source[] = "registered file borrow";
    int file = open("uring-registered", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(file >= 0 && write(file, source, sizeof(source)) == sizeof(source));
    CHECK(lseek(file, 0, SEEK_SET) == 0);
    CHECK(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_FILES, &file, 1) == 0);
    CHECK(!close(file));
    int notification = eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    CHECK(notification >= 0);
    CHECK(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_EVENTFD, &notification, 1) == 0);
    char received[sizeof(source)] = {0};
    struct io_uring_sqe sqe = {
        .opcode = IORING_OP_READ, .flags = IOSQE_FIXED_FILE, .fd = 0, .off = -1,
        .addr = (uintptr_t)received, .len = sizeof(received), .user_data = 31,
    };
    push(&r,&sqe);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,31,sizeof(received));
    CHECK(!memcmp(source,received,sizeof(source)));
    uint64_t signals;
    CHECK(read(notification,&signals,sizeof(signals)) == sizeof(signals) && signals == 1);
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_EVENTFD,NULL,0) == 0);
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_FILES,NULL,0) == 0);
    CHECK(!unlink("uring-registered") && !close(notification));
    destroy(&r);
    puts("IO_URING PASS registered file survives descriptor reuse and event notification");
}

static void pending_requests(void)
{
    struct ring r = setup(8);
    int source = eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    CHECK(source >= 0);
    struct io_uring_sqe sqe = {
        .opcode = IORING_OP_POLL_ADD,.fd = source,.poll32_events = POLLIN,.user_data = 41,
    };
    push(&r,&sqe);
    CHECK(enter(&r,1,0,0) == 1);
    CHECK(*r.cq_tail == *r.cq_head);
    uint64_t count = 1;
    CHECK(write(source,&count,sizeof(count)) == sizeof(count));
    expect(&r,41,POLLIN);
    CHECK(read(source,&count,sizeof(count)) == sizeof(count));
    sqe.user_data = 42;
    push(&r,&sqe);
    struct io_uring_sqe cancel = {
        .opcode = IORING_OP_POLL_REMOVE,.fd = -1,.addr = 42,.user_data = 43,
    };
    push(&r,&cancel);
    CHECK(enter(&r,2,2,IORING_ENTER_GETEVENTS) == 2);
    struct completion first = pop(&r), second = pop(&r);
    if (second.data == 42) { struct completion swap = first; first = second; second = swap; }
    CHECK(first.data == 42 && first.result == -ECANCELED &&
          second.data == 43 && second.result == 0);

    struct timespec duration = {.tv_nsec = 1000000};
    struct io_uring_sqe timeout = {
        .opcode = IORING_OP_TIMEOUT,.fd = -1,.addr = (uintptr_t)&duration,.len = 1,.user_data = 44,
    };
    push(&r,&timeout);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,44,-ETIME);
    duration.tv_sec = 30;
    timeout.user_data = 45;
    push(&r,&timeout);
    CHECK(enter(&r,1,0,0) == 1);
    CHECK(!close(source));
    destroy(&r);
    puts("IO_URING PASS deferred poll, cancellation, timer, and close");
}

static void buffer_selection(void)
{
    struct ring r = setup(8);
    int file = open("uring-selected", O_CREAT | O_TRUNC | O_RDWR, 0600);
    char storage[3][16] = {{0}};
    const char bytes[] = "selected";
    CHECK(file >= 0 && write(file, bytes, sizeof(bytes)) == sizeof(bytes));
    struct io_uring_sqe provide = {
        .opcode=IORING_OP_PROVIDE_BUFFERS, .fd=3, .off=247, .addr=(uintptr_t)storage,
        .len=sizeof(storage[0]), .buf_group=55, .user_data=51,
    };
    struct io_uring_sqe select = {
        .opcode=IORING_OP_READ, .fd=file, .len=64, .buf_group=55,
        .flags=IOSQE_BUFFER_SELECT, .user_data=52,
    };
    struct io_uring_sqe remove = {
        .opcode=IORING_OP_REMOVE_BUFFERS, .fd=2, .buf_group=55, .user_data=53,
    };
    push(&r,&provide);
    push(&r,&select);
    push(&r,&remove);
    CHECK(enter(&r,3,3,IORING_ENTER_GETEVENTS) == 3);
    expect(&r,51,0);
    struct completion event = pop(&r);
    CHECK(event.data == 52 && event.result == sizeof(bytes) &&
        event.flags == (IORING_CQE_F_BUFFER | 247U << 16));
    CHECK(!memcmp(bytes,storage[0],sizeof(bytes)));
    expect(&r,53,2);
    push(&r,&select);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,52,-ENOBUFS);
    CHECK(!close(file) && !unlink("uring-selected"));
    destroy(&r);
    puts("IO_URING PASS provided-buffer FIFO, length, removal and exhaustion");
}

static void fixed_buffers(void)
{
    struct ring r = setup(8);
    int file = open("uring-fixed", O_CREAT | O_TRUNC | O_RDWR, 0600);
    size_t pagesize = sysconf(_SC_PAGESIZE);
    unsigned char *buffer = mmap(NULL, pagesize * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(file >= 0 && buffer != MAP_FAILED);
    unsigned char *span = buffer + pagesize - 1;
    const unsigned char bytes[] = {19, 37, 93};
    CHECK(write(file,bytes,sizeof(bytes)) == sizeof(bytes));
    struct iovec iov = {.iov_base=span,.iov_len=sizeof(bytes)};
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_REGISTER_BUFFERS,&iov,1) == 0);
    int read_ready[2], writes_done[2];
    CHECK(!pipe(read_ready) && !pipe(writes_done));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        unsigned char child_expected[sizeof(bytes)] = {0};
        unsigned char byte;
        CHECK(write(read_ready[1],"r",1) == 1 && read(writes_done[0],&byte,1) == 1);
        CHECK(!memcmp(span,child_expected,sizeof(bytes)));
        _exit(0);
    }
    unsigned char byte;
    CHECK(read(read_ready[0],&byte,1) == 1);
    struct io_uring_sqe rw = {
        .opcode=IORING_OP_READ_FIXED, .fd=file, .addr=(uintptr_t)span,
        .len=sizeof(bytes), .user_data=61,
    };
    push(&r,&rw);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,61,sizeof(bytes));
    CHECK(!memcmp(bytes,span,sizeof(bytes)) && write(writes_done[1],"w",1) == 1);
    int status = -1;
    CHECK(waitpid(child,&status,0) == child && status == 0);
    CHECK(!close(read_ready[0]) && !close(read_ready[1]) &&
        !close(writes_done[0]) && !close(writes_done[1]));
    CHECK(!munmap(buffer,pagesize*2));
    CHECK(mmap(buffer,pagesize*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)
        == buffer);
    memset(span,11,sizeof(bytes));
    rw.opcode = IORING_OP_WRITE_FIXED;
    rw.off = sizeof(bytes);
    rw.user_data = 62;
    push(&r,&rw);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,62,sizeof(bytes));
    unsigned char written[sizeof(bytes)];
    CHECK(pread(file,written,sizeof(written),sizeof(bytes)) == sizeof(written));
    CHECK(!memcmp(bytes,written,sizeof(bytes)));
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_BUFFERS,NULL,0) == 0);
    CHECK(!munmap(buffer,pagesize*2) && !close(file) && !unlink("uring-fixed"));
    destroy(&r);
    puts("IO_URING PASS fixed pages cross page boundaries, survive unmap and preserve fork COW");
}

static int op(struct ring *r, struct io_uring_sqe sqe)
{
    sqe.user_data = 0x987;
    push(r, &sqe);
    CHECK(enter(r, 1, 1, IORING_ENTER_GETEVENTS) == 1);
    struct completion event = pop(r);
    CHECK(event.data == sqe.user_data);
    return event.result;
}

static void filesystem(void)
{
    const char path[] = "uring-adapter-dir";
    const char old[] = "uring-adapter-dir/original";
    const char link[] = "uring-adapter-dir/link";
    const char link2[] = "uring-adapter-dir/link2";
    const char sym[] = "uring-adapter-dir/symbol";
    unsigned char data = 43;
    struct ring r = setup(4);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=37,.fd=AT_FDCWD,.addr=(uintptr_t)path,.len=0700}) == 0);
    int fd = op(&r, (struct io_uring_sqe){.opcode=18,.fd=AT_FDCWD,.addr=(uintptr_t)old,
        .len=0600,.open_flags=O_RDWR|O_CREAT|O_EXCL});
    CHECK(fd >= 0);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=17,.fd=fd,.off=0,.addr=16384}) == 0);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=55,.fd=fd,.off=4096}) == 0);
    CHECK(pwrite(fd,&data,1,0) == 1);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=8,.fd=fd,.len=4096,.sync_range_flags=2}) == 0);
    struct statx st = {0};
    CHECK(op(&r, (struct io_uring_sqe){.opcode=21,.fd=AT_FDCWD,.addr=(uintptr_t)old,
        .off=(uintptr_t)&st,.len=STATX_SIZE}) == 0);
    CHECK((st.stx_mask & STATX_SIZE) && st.stx_size == 4096);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=41,.fd=fd,.addr=(uintptr_t)"user.ring",
        .off=(uintptr_t)"ok",.len=2}) == 0);
    char xattr[2] = {0};
    CHECK(op(&r, (struct io_uring_sqe){.opcode=44,.fd=AT_FDCWD,.addr=(uintptr_t)"user.ring",
        .off=(uintptr_t)xattr,.len=2,.addr3=(uintptr_t)old}) == 2);
    CHECK(!memcmp(xattr, "ok", 2));
    CHECK(op(&r, (struct io_uring_sqe){.opcode=39,.fd=AT_FDCWD,.addr=(uintptr_t)old,
        .off=(uintptr_t)link,.len=(uint32_t)AT_FDCWD}) == 0);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=35,.fd=AT_FDCWD,.addr=(uintptr_t)link,
        .off=(uintptr_t)link2,.len=(uint32_t)AT_FDCWD}) == 0);
    CHECK(op(&r, (struct io_uring_sqe){.opcode=38,.fd=AT_FDCWD,.addr=(uintptr_t)"link2",
        .off=(uintptr_t)sym}) == 0);
    struct open_how how = {.flags=O_RDONLY};
    int symlink_fd = op(&r,(struct io_uring_sqe){.opcode=28,.fd=AT_FDCWD,.addr=(uintptr_t)sym,
        .off=(uintptr_t)&how,.len=sizeof(how)});
    CHECK(symlink_fd >= 0 && pread(symlink_fd, &data, 1, 0) == 1 && data == 43);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=19,.fd=r.fd}) == -EBADF);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=19,.fd=symlink_fd}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=19,.fd=fd}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=36,.fd=AT_FDCWD,.addr=(uintptr_t)sym}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=36,.fd=AT_FDCWD,.addr=(uintptr_t)link2}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=36,.fd=AT_FDCWD,.addr=(uintptr_t)old}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=36,.fd=AT_FDCWD,.addr=(uintptr_t)path,
        .unlink_flags=AT_REMOVEDIR}) == 0);
    int pipe[2];
    CHECK(op(&r,(struct io_uring_sqe){.opcode=62,.addr=(uintptr_t)pipe,.rw_flags=O_CLOEXEC}) == 0);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event registration = {.events=EPOLLIN,.data.u64=0xa1b2}, ready;
    CHECK(op(&r,(struct io_uring_sqe){.opcode=29,.fd=ep,.off=pipe[0],.len=EPOLL_CTL_ADD,
        .addr=(uintptr_t)&registration}) == 0);
    CHECK(write(pipe[1],"a",1) == 1 && epoll_wait(ep,&ready,1,50) == 1 && ready.data.u64 == 0xa1b2);
    CHECK(!close(ep) && !close(pipe[0]) && !close(pipe[1]));
    destroy(&r);
}

static void network(void)
{
    struct ring r = setup(4);
    int ordinary[2];
    CHECK(!socketpair(AF_UNIX, SOCK_DGRAM, 0, ordinary));
    char tx[] = "uring-dgram", rx[sizeof(tx)] = {0};
    CHECK(op(&r, (struct io_uring_sqe){.opcode=26,.fd=ordinary[0],
        .addr=(uintptr_t)tx,.len=sizeof(tx),.msg_flags=MSG_DONTWAIT}) == sizeof(tx));
    CHECK(op(&r, (struct io_uring_sqe){.opcode=27,.fd=ordinary[1],
        .addr=(uintptr_t)rx,.len=sizeof(rx),.msg_flags=MSG_DONTWAIT}) == sizeof(rx));
    CHECK(!memcmp(tx,rx,sizeof(tx)));
    struct iovec iov = {.iov_base=tx,.iov_len=sizeof(tx)};
    struct msghdr msg = {.msg_iov=&iov,.msg_iovlen=1};
    CHECK(op(&r, (struct io_uring_sqe){.opcode=9,.fd=ordinary[1],
        .addr=(uintptr_t)&msg,.len=1,.msg_flags=MSG_DONTWAIT}) == sizeof(tx));
    memset(rx,0,sizeof(rx));
    iov.iov_base=rx;
    CHECK(op(&r, (struct io_uring_sqe){.opcode=10,.fd=ordinary[0],
        .addr=(uintptr_t)&msg,.len=1,.msg_flags=MSG_DONTWAIT}) == sizeof(rx));
    CHECK(!memcmp(tx,rx,sizeof(tx)));
    CHECK(!close(ordinary[0]) && !close(ordinary[1]));

    int listener = op(&r,(struct io_uring_sqe){.opcode=45,.fd=AF_UNIX,
        .off=SOCK_STREAM|SOCK_NONBLOCK});
    int connector = op(&r,(struct io_uring_sqe){.opcode=45,.fd=AF_UNIX,
        .off=SOCK_STREAM});
    CHECK(listener >= 0 && connector >= 0);
    struct sockaddr_un addr = {.sun_family=AF_UNIX,.sun_path="uring-socket"};
    CHECK(op(&r,(struct io_uring_sqe){.opcode=56,.fd=listener,.addr=(uintptr_t)&addr,
        .off=sizeof(addr)}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=57,.fd=listener,.len=5}) == 0);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=16,.fd=connector,.addr=(uintptr_t)&addr,
        .off=sizeof(addr)}) == 0);
    int accepted = op(&r,(struct io_uring_sqe){.opcode=13,.fd=listener,
        .accept_flags=SOCK_CLOEXEC});
    CHECK(accepted >= 0 && fcntl(accepted,F_GETFD) == FD_CLOEXEC);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=34,.fd=accepted,.len=SHUT_WR}) == 0);
    CHECK(read(connector,rx,1) == 0);
    CHECK(!close(accepted) && !close(connector) && !close(listener) && !unlink(addr.sun_path));
    destroy(&r);
}

static void ring_transfer(void)
{
    struct ring r = setup(4);
    int source[2], duplicate[2];
    CHECK(!pipe(source) && !pipe(duplicate));
    int output = open("uring-transfer", O_CREAT | O_RDWR | O_TRUNC, 0600);
    CHECK(output >= 0);
    int fixed[2] = {source[0],output};
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_REGISTER_FILES,fixed,2) == 0);
    CHECK(!close(source[0]) && write(source[1],"abcdef",6) == 6);
    struct io_uring_sqe tee = {.opcode=33,.fd=duplicate[1],.len=6,
        .splice_fd_in=0,.splice_flags=UINT32_C(0x80000002)};
    CHECK(op(&r,tee) == 6);
    char bytes[6];
    CHECK(read(duplicate[0],bytes,sizeof(bytes)) == sizeof(bytes));
    CHECK(!memcmp(bytes,"abcdef",sizeof(bytes)));
    struct io_uring_sqe splice = {.opcode=30,.fd=1,.flags=IOSQE_FIXED_FILE,.len=6,
        .off=4,.splice_off_in=UINT64_MAX,.splice_fd_in=0,
        .splice_flags=UINT32_C(0x80000002)};
    CHECK(op(&r,splice) == 6);
    CHECK(lseek(output,0,SEEK_CUR) == 0);
    CHECK(pread(output,bytes,sizeof(bytes),4) == sizeof(bytes));
    CHECK(!memcmp(bytes,"abcdef",sizeof(bytes)));
    CHECK(!syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_FILES,NULL,0));
    CHECK(!close(source[1]) && !close(duplicate[0]) && !close(duplicate[1]));
    CHECK(!close(output) && !unlink("uring-transfer"));
    destroy(&r);
}

static void fixed_slots(void)
{
    struct ring r = setup(4);
    struct io_uring_rsrc_register files = {.nr=2,.flags=IORING_RSRC_REGISTER_SPARSE};
    CHECK(syscall(SYS_io_uring_register,r.fd,13,&files,sizeof(files)) == 0);
    struct { uint32_t start, count; uint64_t reserved; } range = {.start=1,.count=1};
    CHECK(syscall(SYS_io_uring_register,r.fd,25,&range,0) == 0);
    struct io_uring_sqe open = {.opcode=18,.fd=AT_FDCWD,.addr=(uintptr_t)"uring-direct",
        .len=0600,.open_flags=O_RDWR|O_CREAT|O_TRUNC,.file_index=UINT32_MAX};
    CHECK(op(&r,open) == 1);
    open.open_flags=O_RDWR;
    CHECK(op(&r,open) == -ENFILE);
    struct io_uring_sqe install = {.opcode=54,.fd=1,.flags=IOSQE_FIXED_FILE};
    int fd = op(&r,install);
    CHECK(fd >= 0 && fcntl(fd,F_GETFD) == FD_CLOEXEC);
    CHECK(op(&r,(struct io_uring_sqe){.opcode=19,.file_index=2}) == 0);
    CHECK(op(&r,install) == -EBADF);
    CHECK(pwrite(fd,"Z",1,0) == 1);
    char result = 0;
    CHECK(pread(fd,&result,1,0) == 1 && result == 'Z');
    CHECK(close(fd) == 0 && unlink("uring-direct") == 0);
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_FILES,NULL,0) == 0);
    destroy(&r);
}

static void timer_clocks_and_update(void)
{
    struct ring r = setup(8);
    const unsigned immediate = 1U << 7;
    struct io_uring_sqe timeout = {
        .opcode=IORING_OP_TIMEOUT, .fd=-1, .addr=UINT64_C(30000000000), .len=1,
        .timeout_flags=immediate, .user_data=64,
    };
    push(&r,&timeout);
    CHECK(enter(&r,1,0,0) == 1);
    struct timespec wall;
    CHECK(!clock_gettime(CLOCK_REALTIME,&wall));
    uint64_t now = (uint64_t)wall.tv_sec * 1000000000 + wall.tv_nsec;
    struct io_uring_sqe abs = timeout;
    abs.timeout_flags = immediate | IORING_TIMEOUT_ABS | IORING_TIMEOUT_REALTIME;
    abs.addr = now + UINT64_C(30000000);
    abs.user_data = 65;
    push(&r,&abs);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,65,-ETIME);

    struct io_uring_sqe update = {
        .opcode=IORING_OP_TIMEOUT_REMOVE, .fd=-1, .addr=64, .off=UINT64_C(3000000),
        .timeout_flags=immediate|IORING_TIMEOUT_UPDATE, .user_data=66,
    };
    push(&r,&update);
    CHECK(enter(&r,1,2,IORING_ENTER_GETEVENTS) == 1);
    struct completion first = pop(&r), second = pop(&r);
    if (first.data == 66) { struct completion swap = first; first = second; second = swap; }
    CHECK(first.data == 64 && first.result == -ETIME && second.data == 66 && second.result == 0);
    destroy(&r);
    puts("IO_URING PASS realtime deadline and pending timeout replacement");
}

static void single_issuer(void)
{
    struct ring r = setup_flags(4, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED);
    CHECK(enter(&r,0,0,0) == -1 && errno == EBADFD);
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_REGISTER_ENABLE_RINGS,NULL,0) == 0);
    struct io_uring_sqe nop = {.opcode=IORING_OP_NOP,.user_data=67};
    push(&r,&nop);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(enter(&r,1,0,0) == -1 && errno == EEXIST);
        CHECK(syscall(SYS_io_uring_register,r.fd,IORING_REGISTER_PERSONALITY,NULL,0) == -1
            && errno == EEXIST);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child,&status,0) == child && status == 0);
    int personality = syscall(SYS_io_uring_register,r.fd,IORING_REGISTER_PERSONALITY,NULL,0);
    CHECK(personality >= 0);
    CHECK(syscall(SYS_io_uring_register,r.fd,IORING_UNREGISTER_PERSONALITY,NULL,personality) == 0);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,67,0);
    destroy(&r);
    puts("IO_URING PASS disabled enable and single issuer ownership");
}

static void linked_poll_timeout(void)
{
    struct ring r = setup(8);
    int event = eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    CHECK(event >= 0);
    struct io_uring_sqe wait = {
        .opcode=IORING_OP_POLL_ADD,.fd=event,.flags=IOSQE_IO_LINK,
        .poll32_events=POLLIN,.user_data=68,
    };
    struct timespec duration = {.tv_nsec=5000000};
    struct io_uring_sqe timeout = {
        .opcode=IORING_OP_LINK_TIMEOUT,.fd=-1,.addr=(uintptr_t)&duration,
        .len=1,.user_data=69,
    };
    push(&r,&wait);
    push(&r,&timeout);
    CHECK(enter(&r,2,2,IORING_ENTER_GETEVENTS) == 2);
    struct completion first = pop(&r), second = pop(&r);
    if (first.data == 69) { struct completion swap = first; first = second; second = swap; }
    CHECK(first.data == 68 && first.result == -ECANCELED &&
        second.data == 69 && second.result == -ETIME);

    duration.tv_sec = 30;
    wait.user_data = 70;
    timeout.user_data = 71;
    push(&r,&wait);
    push(&r,&timeout);
    CHECK(enter(&r,2,0,0) == 2);
    uint64_t one = 1;
    CHECK(write(event,&one,sizeof(one)) == sizeof(one));
    first = pop(&r); second = pop(&r);
    if (first.data == 71) { struct completion swap = first; first = second; second = swap; }
    CHECK(first.data == 70 && first.result == POLLIN &&
        second.data == 71 && second.result == -ECANCELED);
    CHECK(!close(event));
    destroy(&r);
    puts("IO_URING PASS linked poll races its timeout in both directions");
}

static void caller_backed_rings(void)
{
    const unsigned no_mmap = 1U << 14;
    size_t page = sysconf(_SC_PAGESIZE);
    void *shared = mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    void *entries = mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(shared != MAP_FAILED && entries != MAP_FAILED);
    struct io_uring_params in = {.flags=no_mmap};
    in.sq_off.user_addr = (uintptr_t)entries;
    CHECK(syscall(SYS_io_uring_setup,4,&in) == -1 && errno == EFAULT);
    in.cq_off.user_addr = (uintptr_t)shared + 1;
    CHECK(syscall(SYS_io_uring_setup,4,&in) == -1 && errno == EINVAL);
    in.cq_off.user_addr = (uintptr_t)shared;
    struct io_uring_params *readonly = mmap(NULL,page,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(readonly != MAP_FAILED);
    *readonly = in;
    CHECK(!mprotect(readonly,page,PROT_READ));
    CHECK(syscall(SYS_io_uring_setup,4,readonly) == -1 && errno == EFAULT);
    CHECK(!munmap(readonly,page));

    struct ring r = {.params=in,.shared=shared,.sqes=entries,.shared_size=page,.sqe_size=page};
    r.fd = syscall(SYS_io_uring_setup,4,&r.params);
    CHECK(r.fd >= 0 && r.params.sq_off.user_addr == (uintptr_t)entries &&
        r.params.cq_off.user_addr == (uintptr_t)shared);
    CHECK(mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,r.fd,IORING_OFF_SQ_RING) == MAP_FAILED &&
        errno == EINVAL);
    r.sq_head = word(shared,r.params.sq_off.head);
    r.sq_tail = word(shared,r.params.sq_off.tail);
    r.sq_array = word(shared,r.params.sq_off.array);
    r.sq_flags = word(shared,r.params.sq_off.flags);
    r.cq_head = word(shared,r.params.cq_off.head);
    r.cq_tail = word(shared,r.params.cq_off.tail);
    r.cqes = (struct io_uring_cqe *)((char *)shared + r.params.cq_off.cqes);
    struct io_uring_sqe nop = {.opcode=IORING_OP_NOP,.user_data=72};
    push(&r,&nop);
    CHECK(enter(&r,1,1,IORING_ENTER_GETEVENTS) == 1);
    expect(&r,72,0);
    destroy(&r);
    puts("IO_URING PASS caller-backed ring memory and setup rollback");
}

int main(void)
{
    struct io_uring_params p = {0};
    CHECK(syscall(SYS_io_uring_setup, 0, &p) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_io_uring_setup, 1, NULL) == -1 && errno == EFAULT);
    nop_and_overflow();
    regular_io();
    registrations();
    pending_requests();
    timer_clocks_and_update();
    single_issuer();
    linked_poll_timeout();
    caller_backed_rings();
    buffer_selection();
    fixed_buffers();
    filesystem();
    network();
    fixed_slots();
    ring_transfer();
    puts("IO_URING PASS");
    fflush(stdout);
    sync();
    struct utsname name;
    CHECK(!uname(&name));
    if (!strstr(name.version, "BoarOS")) {
        CHECK(!mount("/dev/vda", "/", "ext4", MS_REMOUNT | MS_RDONLY, NULL));
        CHECK(!reboot(RB_POWER_OFF));
    }
    return 0;
}
