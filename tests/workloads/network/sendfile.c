#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "sendfile:%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static unsigned char data[65536];
static volatile sig_atomic_t signals;
static void notified(int number) { (void)number; signals++; }

static void pattern(unsigned char *buffer, size_t size, size_t offset)
{ for (size_t i = 0; i < size; i++) buffer[i] = (unsigned char)((offset + i) % 251); }
static void same(const unsigned char *buffer, size_t size, size_t offset)
{ for (size_t i = 0; i < size; i++) CHECK(buffer[i] == (unsigned char)((offset + i) % 251)); }
static void read_exact(int fd, size_t size, size_t offset)
{
    size_t done = 0;
    while (done < size) {
        size_t wanted = size - done > sizeof(data) ? sizeof(data) : size - done;
        ssize_t got = read(fd, data, wanted); CHECK(got > 0); same(data, (size_t)got, offset + done); done += (size_t)got;
    }
}
static int source(const char *path)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600); CHECK(fd >= 0);
    pattern(data, sizeof(data), 0); CHECK(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
    CHECK(lseek(fd, 0, SEEK_SET) == 0); return fd;
}
static void wait_ok(pid_t child)
{ int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0); }
static void wait_blocked(pid_t child)
{
    char path[64], line[512]; snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child);
    for (unsigned i = 0; i < 2000; i++) {
        FILE *state = fopen(path, "r"); CHECK(state != NULL);
        CHECK(fgets(line, sizeof(line), state) != NULL && fclose(state) == 0);
        char *end = strrchr(line, ')'); CHECK(end != NULL && end[1] == ' ');
        if (end[2] == 'S') return;
        CHECK(end[2] != 'Z'); struct timespec pause = {.tv_nsec = 1000000}; CHECK(nanosleep(&pause, NULL) == 0);
    }
    CHECK(!"child did not block");
}

static void files(int in)
{
    int out = open("output", O_CREAT | O_TRUNC | O_RDWR, 0600); CHECK(out >= 0);
    CHECK(sendfile(out, in, NULL, 5000) == 5000);
    CHECK(lseek(in, 0, SEEK_CUR) == 5000 && lseek(out, 0, SEEK_CUR) == 5000);
    off_t offset = 17; CHECK(lseek(in, 7, SEEK_SET) == 7);
    CHECK(sendfile(out, in, &offset, 4007) == 4007 && offset == 4024);
    CHECK(lseek(in, 0, SEEK_CUR) == 7 && lseek(out, 0, SEEK_CUR) == 9007);
    CHECK(lseek(out, 0, SEEK_SET) == 0); read_exact(out, 5000, 0); read_exact(out, 4007, 17);
    offset = sizeof(data); CHECK(sendfile(out, in, &offset, 1000) == 0 && offset == (off_t)sizeof(data));
    offset = 0; CHECK(sendfile(out, in, &offset, UINT64_C(0x80001000)) == (ssize_t)sizeof(data) && offset == (off_t)sizeof(data));
    CHECK(close(out) == 0 && lseek(in, 0, SEEK_SET) == 0);
    puts("SENDFILE PASS files shared-offset explicit-offset EOF capped-count");
}

static void aliases(void)
{
    for (unsigned explicit_offset = 0; explicit_offset < 2; explicit_offset++) {
        int in = source("same"), out = dup(in); CHECK(out >= 0);
        off_t offset = 99;
        CHECK(sendfile(out, in, explicit_offset ? &offset : NULL, 37) == 37);
        CHECK(lseek(in, 0, SEEK_CUR) == 37 && lseek(out, 0, SEEK_CUR) == 37);
        if (explicit_offset) CHECK(offset == 136);
        CHECK(lseek(in, 0, SEEK_SET) == 0); read_exact(in, 37, explicit_offset ? 99 : 0);
        CHECK(close(out) == 0 && close(in) == 0);
    }
    int in = source("same"); CHECK(sendfile(in, in, NULL, 37) == 37 && lseek(in, 0, SEEK_CUR) == 37);
    CHECK(close(in) == 0);
    int a = source("a"), b = source("b"), gate[2]; CHECK(pipe(gate) == 0);
    pid_t workers[2];
    for (unsigned i = 0; i < 2; i++) {
        workers[i] = fork(); CHECK(workers[i] >= 0);
        if (!workers[i]) { CHECK(close(gate[1]) == 0); char token; CHECK(read(gate[0], &token, 1) == 1);
            CHECK(sendfile(i ? a : b, i ? b : a, NULL, 128) == 128); _exit(0); }
    }
    CHECK(close(gate[0]) == 0 && write(gate[1], "GG", 2) == 2 && close(gate[1]) == 0);
    wait_ok(workers[0]); wait_ok(workers[1]);
    off_t a_position = lseek(a, 0, SEEK_CUR), b_position = lseek(b, 0, SEEK_CUR);
    CHECK(a_position >= 128 && a_position <= 256 && b_position >= 128 && b_position <= 256);
    CHECK(close(a) == 0 && close(b) == 0);
    puts("SENDFILE PASS aliases same-OFD and opposite file pairs");
}

static void errors(int in)
{
    int out = open("output", O_RDWR); CHECK(out >= 0);
    int append = open("output", O_WRONLY | O_APPEND), writeonly = open("input", O_WRONLY); CHECK(append >= 0 && writeonly >= 0);
    off_t offset = 0;
    CHECK(sendfile(append, in, &offset, 1) == -1 && errno == EINVAL && offset == 0);
    CHECK(sendfile(append, in, &offset, 0) == -1 && errno == EINVAL);
    CHECK(sendfile(out, writeonly, &offset, 1) == -1 && errno == EBADF);
    CHECK(sendfile(in, in, &offset, 1) == -1 && errno == EBADF);
    CHECK(sendfile(-1, -1, (void *)(uintptr_t)1, 1) == -1 && errno == EFAULT);
    offset = -1; CHECK(sendfile(-1, in, &offset, 1) == -1 && errno == EINVAL && offset == -1);
    offset = 0; CHECK(sendfile(-1, in, &offset, 0) == -1 && errno == EBADF && offset == 0);
    CHECK(sendfile(-1, in, &offset, (size_t)-1) == -1 && errno == EINVAL);
    int pipes[2]; CHECK(pipe(pipes) == 0);
    CHECK(sendfile(-1, pipes[0], &offset, 0) == -1 && errno == ESPIPE);
    CHECK(close(pipes[0]) == 0 && close(pipes[1]) == 0);
    long page = sysconf(_SC_PAGESIZE); CHECK(page > 0);
    off_t *readonly = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(readonly != MAP_FAILED);
    *readonly = 0; CHECK(mprotect(readonly, (size_t)page, PROT_READ) == 0);
    CHECK(lseek(in, 7, SEEK_SET) == 7 && lseek(out, 0, SEEK_SET) == 0);
    CHECK(sendfile(out, in, readonly, 4096) == -1 && errno == EFAULT && *readonly == 0);
    CHECK(lseek(in, 0, SEEK_CUR) == 7 && lseek(out, 0, SEEK_CUR) == 4096);
    CHECK(lseek(out, 0, SEEK_SET) == 0); read_exact(out, 4096, 0);
    CHECK(sendfile(-1, -1, readonly, 1) == -1 && errno == EFAULT);
    CHECK(munmap(readonly, (size_t)page) == 0 && close(out) == 0 && close(append) == 0 && close(writeonly) == 0);
    CHECK(lseek(in, 0, SEEK_SET) == 0);
    puts("SENDFILE PASS errors append errno-order and output-before-offset-fault");
}

static void outputs(int in)
{
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    int size = 1024; CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    int large = source("socket-input");
    for (unsigned i = 1; i < 16; i++) {
        pattern(data, sizeof(data), i * sizeof(data));
        CHECK(pwrite(large, data, sizeof(data), (off_t)(i * sizeof(data))) == (ssize_t)sizeof(data));
    }
    ssize_t sent = sendfile(pair[0], large, NULL, 16 * sizeof(data)); CHECK(sent > 0 && sent < (ssize_t)(16 * sizeof(data)));
    CHECK(lseek(large, 0, SEEK_CUR) == sent); read_exact(pair[1], (size_t)sent, 0);
    CHECK(lseek(large, 0, SEEK_SET) == 0);
    memset(data, 'f', sizeof(data));
    while (write(pair[0], data, sizeof(data)) > 0) {}
    CHECK(errno == EAGAIN);
    CHECK(sendfile(pair[0], large, NULL, sizeof(data)) == -1 && errno == EAGAIN && lseek(large, 0, SEEK_CUR) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0 && close(large) == 0 && unlink("socket-input") == 0);

    int pipes[2]; CHECK(pipe2(pipes, O_NONBLOCK) == 0);
    CHECK(lseek(in, 0, SEEK_SET) == 0 && sendfile(pipes[1], in, NULL, 4096) == 4096); read_exact(pipes[0], 4096, 0);
    memset(data, 'p', sizeof(data)); size_t filled = 0; ssize_t got;
    while ((got = write(pipes[1], data, sizeof(data))) > 0) filled += (size_t)got;
    CHECK(errno == EAGAIN);
    CHECK(read(pipes[0], data, 4096) == 4096 && lseek(in, 0, SEEK_SET) == 0);
    sent = sendfile(pipes[1], in, NULL, sizeof(data)); CHECK(sent > 0 && sent < (ssize_t)sizeof(data));
    CHECK(lseek(in, 0, SEEK_CUR) == sent);
    for (size_t remaining = filled - 4096; remaining > 0;) {
        size_t wanted = remaining > sizeof(data) ? sizeof(data) : remaining;
        got = read(pipes[0], data, wanted); CHECK(got > 0);
        for (ssize_t i = 0; i < got; i++) CHECK(data[i] == 'p');
        remaining -= (size_t)got;
    }
    read_exact(pipes[0], (size_t)sent, 0);
    memset(data, 'p', sizeof(data)); while (write(pipes[1], data, sizeof(data)) > 0) {}
    CHECK(errno == EAGAIN);
    off_t before = lseek(in, 0, SEEK_CUR);
    CHECK(sendfile(pipes[1], in, NULL, 4096) == -1 && errno == EAGAIN && lseek(in, 0, SEEK_CUR) == before);
    CHECK(fcntl(pipes[1], F_SETFL, fcntl(pipes[1], F_GETFL) & ~O_NONBLOCK) == 0);
    signals = 0; alarm(1);
    CHECK(sendfile(pipes[1], in, NULL, 4096) == -1 && errno == EINTR && signals == 1); alarm(0);
    CHECK(lseek(in, 0, SEEK_CUR) == before);
    pid_t blocked = fork(); CHECK(blocked >= 0);
    if (!blocked) { CHECK(close(pipes[0]) == 0); (void)sendfile(pipes[1], in, NULL, 4096); _exit(99); }
    wait_blocked(blocked);
    CHECK(kill(blocked, SIGKILL) == 0); int status;
    CHECK(waitpid(blocked, &status, 0) == blocked && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(close(pipes[0]) == 0); signals = 0;
    CHECK(sendfile(pipes[1], in, NULL, 1) == -1 && errno == EPIPE && signals == 1);
    CHECK(lseek(in, 0, SEEK_CUR) == before && close(pipes[1]) == 0);
    puts("SENDFILE PASS outputs socket partial/EAGAIN pipe EINTR/EPIPE and cancelled child");
}

static void pipe_composition(int in)
{
    int pipefd[2]; CHECK(pipe2(pipefd, O_NONBLOCK) == 0);
    off_t shared_position = lseek(in, 0, SEEK_CUR), offset;
    size_t queued = 0;
    for (;;) {
        offset = 0;
        ssize_t sent = sendfile(pipefd[1], in, &offset, 1);
        if (sent < 0) { CHECK(errno == EAGAIN && offset == 0); break; }
        CHECK(sent == 1 && offset == 1 && queued < 65536); queued++;
    }
    CHECK(queued > 0 && lseek(in, 0, SEEK_CUR) == shared_position);
    CHECK(write(pipefd[1], "X", 1) == -1 && errno == EAGAIN);
    CHECK(read(pipefd[0], data, 1) == 1 && data[0] == 0);
    CHECK(write(pipefd[1], "X", 1) == 1);
    struct iovec suffix[] = {{"Y", 1}, {"Z", 1}};
    CHECK(writev(pipefd[1], suffix, 2) == 2);
    offset = 0; CHECK(sendfile(pipefd[1], in, &offset, 1) == -1 && errno == EAGAIN && offset == 0);
    size_t drained = 0, expected = queued + 2;
    while (drained < expected) {
        size_t wanted = expected - drained;
        if (wanted > sizeof(data)) wanted = sizeof(data);
        ssize_t received = read(pipefd[0], data, wanted); CHECK(received > 0);
        for (ssize_t i = 0; i < received; i++) {
            size_t position = drained + (size_t)i;
            CHECK(data[i] == (position < queued - 1 ? 0 : (unsigned char)"XYZ"[position - (queued - 1)]));
        }
        drained += (size_t)received;
    }
    CHECK(read(pipefd[0], data, 1) == -1 && errno == EAGAIN);
    CHECK(lseek(in, 0, SEEK_CUR) == shared_position && close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    printf("SENDFILE PASS pipe-composition queued_bytes=%zu drained_bytes=%zu source_position=%lld\n",
           queued, drained, (long long)shared_position);
}

static void datagrams(int in)
{
    const size_t lengths[] = {1, 4097, 65507};
    int udp = socket(AF_INET, SOCK_DGRAM, 0), receiver = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(udp >= 0 && receiver >= 0);
    struct ifreq loopback = {.ifr_name = "lo"}; CHECK(ioctl(udp, SIOCGIFFLAGS, &loopback) == 0);
    if (!(loopback.ifr_flags & IFF_UP)) {
        loopback.ifr_flags |= IFF_UP; CHECK(ioctl(udp, SIOCSIFFLAGS, &loopback) == 0);
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {htonl(INADDR_LOOPBACK)}};
    CHECK(bind(receiver, (void *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address); CHECK(getsockname(receiver, (void *)&address, &length) == 0);
    off_t offset = 0;
    CHECK(sendfile(udp, in, &offset, 1) == -1 && errno == EDESTADDRREQ && offset == 0);
    CHECK(sendfile(udp, in, &offset, 65508) == -1 && errno == EDESTADDRREQ && offset == 0);
    CHECK(sendfile(udp, in, &offset, 65536) == -1 && errno == EMSGSIZE && offset == 0);
    CHECK(connect(udp, (void *)&address, length) == 0);
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        offset = 0; CHECK(sendfile(udp, in, &offset, lengths[i]) == (ssize_t)lengths[i] && offset == (off_t)lengths[i]);
        CHECK(recv(receiver, data, sizeof(data), 0) == (ssize_t)lengths[i]); same(data, lengths[i], 0);
    }
    offset = 0; CHECK(sendfile(udp, in, &offset, 65508) == -1 && errno == EMSGSIZE && offset == 0);
    CHECK(sendfile(udp, in, &offset, 0) == 0 && offset == 0);
    CHECK(recv(receiver, data, sizeof(data), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(close(udp) == 0 && close(receiver) == 0);
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, pair) == 0);
    offset = 0; CHECK(sendfile(pair[0], in, &offset, 4097) == 4097 && offset == 4097);
    CHECK(recv(pair[1], data, sizeof(data), 0) == 4097); same(data, 4097, 0);
    int large = source("dgram-input");
    pattern(data, 1, sizeof(data)); CHECK(pwrite(large, data, 1, sizeof(data)) == 1);
    ssize_t sent = sendfile(pair[0], large, NULL, 65537); CHECK(sent == 65536 || sent == 65537);
    CHECK(recv(pair[1], data, sizeof(data), 0) == 65536); same(data, sizeof(data), 0);
    if (sent == 65536) CHECK(sendfile(pair[0], large, NULL, 1) == 1);
    CHECK(recv(pair[1], data, sizeof(data), 0) == 1); same(data, 1, 65536);
    CHECK(lseek(large, 0, SEEK_CUR) == 65537 && close(large) == 0 && unlink("dgram-input") == 0);
    for (;;) {
        offset = 0; ssize_t queued = sendfile(pair[0], in, &offset, 4097);
        if (queued < 0) { CHECK(errno == EAGAIN && offset == 0); break; }
        CHECK(queued == 4097 && offset == 4097);
    }
    CHECK(fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL) & ~O_NONBLOCK) == 0);
    off_t shared_position = lseek(in, 0, SEEK_CUR);
    pid_t blocked = fork(); CHECK(blocked >= 0);
    if (!blocked) {
        CHECK(close(pair[1]) == 0); off_t child_offset = 0;
        (void)sendfile(pair[0], in, &child_offset, 4097); _exit(99);
    }
    wait_blocked(blocked); CHECK(kill(blocked, SIGKILL) == 0); int status;
    CHECK(waitpid(blocked, &status, 0) == blocked && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(lseek(in, 0, SEEK_CUR) == shared_position);
    ssize_t queued;
    while ((queued = recv(pair[1], data, sizeof(data), 0)) >= 0) {
        CHECK(queued == 4097); same(data, 4097, 0);
    }
    CHECK(errno == EAGAIN);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("SENDFILE PASS datagrams connected UDP limits and UNIX message boundaries");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int composition_only = argc == 2 && !strcmp(argv[1], "--pipe-composition");
    CHECK(argc == 1 || composition_only);
    struct sigaction action = {.sa_handler = notified}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0 && sigaction(SIGALRM, &action, NULL) == 0);
    if (mkdir("/tmp", 0755) < 0) CHECK(errno == EEXIST);
    if (getpid() == 1) {
        if (mkdir("/proc", 0555) < 0) CHECK(errno == EEXIST);
        if (mount("proc", "/proc", "proc", 0, NULL) < 0) CHECK(errno == EBUSY);
    }
    char directory[] = "/tmp/sendfile-XXXXXX"; CHECK(mkdtemp(directory) != NULL && chdir(directory) == 0);
    int rw = source("input"); CHECK(close(rw) == 0);
    int in = open("input", O_RDONLY); CHECK(in >= 0);
    if (composition_only) pipe_composition(in);
    else { files(in); aliases(); errors(in); outputs(in); datagrams(in); pipe_composition(in); }
    CHECK(close(in) == 0);
    if (!composition_only && getpid() == 1) {
        CHECK(mkdir("memory", 0755) == 0 && mount("tmpfs", "memory", "tmpfs", 0, NULL) == 0);
        int memory = source("memory/input"), pair[2]; CHECK(pipe(pair) == 0);
        off_t offset = 13; CHECK(sendfile(pair[1], memory, &offset, 4096) == 4096 && offset == 4109);
        read_exact(pair[0], 4096, 13);
        CHECK(close(pair[0]) == 0 && close(pair[1]) == 0 && close(memory) == 0);
        CHECK(unlink("memory/input") == 0 && umount("memory") == 0 && rmdir("memory") == 0);
        puts("SENDFILE PASS tmpfs actual memory-backed input");
    }
    CHECK(unlink("input") == 0);
    if (!composition_only) {
        const char *paths[] = {"output", "same", "a", "b"};
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) CHECK(unlink(paths[i]) == 0);
    }
    CHECK(chdir("/") == 0 && rmdir(directory) == 0);
    puts("NETWORK PASS sendfile"); return 0;
}
