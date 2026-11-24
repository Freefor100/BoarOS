#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/random.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { perror(#expr); _exit(1); } } while (0)

int main(void)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    int fd = (int)syscall(447, O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(syscall(447, 1) == -1 && errno == EINVAL);
    CHECK(!ftruncate(fd, (off_t)size));
    CHECK(ftruncate(fd, (off_t)(2 * size)) == -1 && errno == EINVAL);
    struct stat stat;
    CHECK(!fstat(fd, &stat) && stat.st_size == (off_t)size);
    CHECK(mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED &&
          errno == EINVAL);

    struct rlimit limit, empty;
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct original[2], restricted[2];
    CHECK(!syscall(SYS_capget, &header, original));
    memcpy(restricted, original, sizeof(restricted));
    restricted[0].effective &= ~(1U << CAP_IPC_LOCK);
    CHECK(!syscall(SYS_capset, &header, restricted));
    CHECK(!getrlimit(RLIMIT_MEMLOCK, &limit));
    empty = limit;
    empty.rlim_cur = 0;
    CHECK(!setrlimit(RLIMIT_MEMLOCK, &empty));
    CHECK(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EAGAIN);
    CHECK(!setrlimit(RLIMIT_MEMLOCK, &limit));
    CHECK(!syscall(SYS_capset, &header, original));

    unsigned char *bytes = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(bytes != MAP_FAILED);
    CHECK(syscall(SYS_getrandom, bytes + size - 32, 32, GRND_INSECURE) == 32);
    CHECK(bytes[0] == 0 && bytes[size - 33] == 0);
    bytes[0] = 42;
    unsigned char byte = 0;
    CHECK(read(fd, &byte, 1) == -1 && errno == EINVAL);
    CHECK(write(fd, &byte, 1) == -1 && errno == EINVAL);
    int input = open("/init", O_RDONLY);
    CHECK(input >= 0);
    CHECK(read(input, bytes + 8, 4) == 4 && !memcmp(bytes + 8, "\177ELF", 4));
    CHECK(!close(input));
    int output = open("/secret-copy-test", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(output >= 0);
    CHECK(write(output, bytes, 1) == 1);
    CHECK(pread(output, bytes + 12, 1, 0) == 1 && bytes[12] == 42);
    CHECK(!close(output));
    int temporary = memfd_create("ordinary", 0);
    CHECK(temporary >= 0);
    CHECK(pwrite(temporary, bytes, 1, (off_t)size) == 1);
    CHECK(pread(temporary, bytes + 13, 1, (off_t)size) == 1 && bytes[13] == 42);
    CHECK(pread(temporary, bytes + 13, 1, 0) == 1 && bytes[13] == 0);
    CHECK(!close(temporary));
    struct iovec local = {&byte, 1}, remote = {bytes, 1};
    CHECK(syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == -1 &&
          errno == EFAULT);
    int pipefd[2];
    CHECK(!pipe(pipefd));
    CHECK(write(pipefd[1], bytes, 1) == 1);
    CHECK(read(pipefd[0], bytes + 1, 1) == 1 && bytes[1] == 42);
    CHECK(vmsplice(pipefd[1], &remote, 1, 0) == -1 && errno == EFAULT);
    CHECK(!close(pipefd[0]) && !close(pipefd[1]));
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    struct ifreq loopback = {.ifr_name = "lo"};
    CHECK(!ioctl(listener, SIOCGIFFLAGS, &loopback));
    if (!(loopback.ifr_flags & IFF_UP)) {
        loopback.ifr_flags |= IFF_UP;
        CHECK(!ioctl(listener, SIOCSIFFLAGS, &loopback));
    }
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t address_size = sizeof(address);
    CHECK(!bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 1));
    CHECK(!getsockname(listener, (struct sockaddr *)&address, &address_size));
    int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(client >= 0 && !connect(client, (struct sockaddr *)&address, sizeof(address)));
    int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    CHECK(peer >= 0 && write(client, bytes, 1) == 1);
    CHECK(read(peer, bytes + 14, 1) == 1 && bytes[14] == 42);
    CHECK(!close(peer) && !close(client) && !close(listener));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(bytes[0] == 42);
        bytes[0] = 73;
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(!close(fd) && bytes[0] == 73 && !munmap(bytes, size));

    puts("SECRET PASS");
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
