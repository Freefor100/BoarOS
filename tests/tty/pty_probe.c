/* Same ELF on fixed Linux and BoarOS; PID 1 also owns script's adopted child.
 * Oracle: references/linux f4cdf7ca9a1f drivers/tty/{pty,n_tty,tty_io}.c;
 * BusyBox script/scriptreplay are the unmodified pinned 1.33.1 applets. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

#ifndef TIOCGPTPEER
#define TIOCGPTPEER 0x5441
#endif
#ifndef TIOCGPTLCK
#define TIOCGPTLCK 0x80045439
#endif
#ifndef TIOCGPKT
#define TIOCGPKT 0x80045438
#endif
#ifndef EXTPROC
#define EXTPROC 0x10000
#endif

struct pair { int master, slave; unsigned number; char path[128]; };
/* 不使用libc扩展termios；固定RV64 asm-generic分别为36/44字节。 */
struct raw_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
struct raw_termios2 { struct raw_termios basic; uint32_t ispeed, ospeed; };
_Static_assert(sizeof(struct raw_termios) == 36, "RV64 old termios layout");
_Static_assert(sizeof(struct raw_termios2) == 44, "RV64 termios2 layout");
#define RAW_TCGETS2 UINT32_C(0x802c542a)
#define RAW_TCSETS2 UINT32_C(0x402c542b)
#define RAW_TCSETSW2 UINT32_C(0x402c542c)
#define RAW_TCSETSF2 UINT32_C(0x402c542d)
#define RAW_CBAUD UINT32_C(0x100f)
#define RAW_CIBAUD UINT32_C(0x100f0000)
#define RAW_BOTHER UINT32_C(0x1000)
static unsigned records;
static const char *api_record = "real-musl-openpty-login_tty";
static volatile sig_atomic_t signals_seen;
static void fail(const char *what, long got, long wanted)
{
    dprintf(1, "PTY_RECORD FAIL %s got=%ld wanted=%ld errno=%d\n",
            what, got, wanted, errno);
    sync();
    exit(1);
}
static void exact(long got, long wanted, const char *what)
{ if (got != wanted) fail(what, got, wanted); }
static void truth(int value, const char *what)
{ if (!value) fail(what, 0, 1); }
static void error_is(long got, int wanted, const char *what)
{ int error = errno; exact(got, -1, what); exact(error, wanted, what); }
static void record(const char *name, long value)
{ dprintf(1, "PTY_RECORD %s %ld\n", name, value); records++; }
static uint64_t now(void)
{
    struct timespec t;
    exact(clock_gettime(CLOCK_MONOTONIC, &t), 0, "monotonic");
    return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}
static void write_all(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        truth(n > 0, "write progress"); bytes += n; size -= (size_t)n;
    }
}
static void read_all(int fd, void *data, size_t size)
{
    unsigned char *bytes = data;
    while (size) {
        ssize_t n = read(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        truth(n > 0, "read progress"); bytes += n; size -= (size_t)n;
    }
}
static void nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    truth(flags >= 0, "getfl"); exact(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0, "nonblock");
}
static short ready(int fd, short events, int timeout)
{
    struct pollfd p = {fd, events, 0}; int n;
    do n = poll(&p, 1, timeout); while (n < 0 && errno == EINTR);
    truth(n >= 0, "poll"); return n ? p.revents : 0;
}
static void readable(int fd)
{ truth(ready(fd, POLLIN, 5000) & POLLIN, "readable handshake"); }
static void raw(int fd)
{
    struct termios t; exact(tcgetattr(fd, &t), 0, "tcgetattr"); cfmakeraw(&t);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    exact(tcsetattr(fd, TCSANOW, &t), 0, "raw termios");
}
static struct pair pair_at(const char *mountpoint)
{
    struct pair p = {-1, -1, 0, {0}}; char path[128];
    snprintf(path, sizeof(path), "%s/ptmx", mountpoint);
    p.master = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    truth(p.master >= 0, "ptmx open");
    exact(ioctl(p.master, TIOCGPTN, &p.number), 0, "get pty number");
    snprintf(p.path, sizeof(p.path), "%s/%u", mountpoint, p.number);
    int locked = -1;
    exact(ioctl(p.master, TIOCGPTLCK, &locked), 0, "get initial lock");
    exact(locked, 1, "initially locked");
    errno = 0; error_is(open(p.path, O_RDWR | O_NOCTTY), EIO, "locked pathname");
    errno = 0; error_is(ioctl(p.master, TIOCGPTPEER, O_RDWR | O_NOCTTY), EIO, "locked peer");
    locked = 0; exact(ioctl(p.master, TIOCSPTLCK, &locked), 0, "unlock pty");
    p.slave = open(p.path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    truth(p.slave >= 0, "slave open");
    return p;
}
static struct pair pair_new(void) { return pair_at("/dev/pts"); }
static void pair_close(struct pair *p)
{
    if (p->slave >= 0) exact(close(p->slave), 0, "slave close");
    if (p->master >= 0) exact(close(p->master), 0, "master close");
    p->master = p->slave = -1;
}
static void node(const char *path, unsigned major_number, unsigned minor_number)
{
    truth(mknod(path, S_IFCHR | 0600, makedev(major_number, minor_number)) == 0 ||
          errno == EEXIST, "device node");
}
static void setup(void)
{
    truth(mkdir("/dev", 0755) == 0 || errno == EEXIST, "mkdir dev");
    truth(mkdir("/dev/pts", 0755) == 0 || errno == EEXIST, "mkdir pts");
    truth(mkdir("/proc", 0755) == 0 || errno == EEXIST, "mkdir proc");
    truth(mkdir("/tmp", 0777) == 0 || errno == EEXIST, "mkdir tmp");
    exact(mount("proc", "/proc", "proc", 0, NULL), 0, "mount proc");
    node("/dev/tty", 5, 0); node("/dev/ptmx", 5, 2);
    node("/dev/ttyS0", 4, 64); node("/dev/null", 1, 3);
    exact(mount("devpts", "/dev/pts", "devpts", 0,
                "newinstance,mode=620,ptmxmode=666,max=32"), 0, "mount devpts");
    exact(setenv("PATH", "/bin", 1), 0, "PATH");
    exact(setenv("SHELL", "/bin/sh", 1), 0, "SHELL");
    exact(setenv("LC_ALL", "C", 1), 0, "locale");
    exact(setenv("PS1", "PTY_SCRIPT$ ", 1), 0, "prompt");
    exact(klogctl(6, NULL, 0), 0, "console loglevel");
}
static void red(void)
{
    struct pair p = pair_new();
    raw(p.slave); write_all(p.master, "R", 1); readable(p.slave);
    unsigned char c; read_all(p.slave, &c, 1); exact(c, 'R', "red transfer");
    pair_close(&p); record("unix98-red-path", 1);
}
static void locks_and_nodes(void)
{
    struct pair p = pair_new(); struct stat a, b;
    exact(fstat(p.slave, &a), 0, "slave stat");
    truth(S_ISCHR(a.st_mode) && major(a.st_rdev) == 136 && minor(a.st_rdev) == p.number,
          "slave device identity");
    exact(a.st_mode & 0777, 0620, "slave mode");
    int peer = ioctl(p.master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    truth(peer >= 0, "unlocked peer"); exact(fstat(peer, &b), 0, "peer stat");
    truth(a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_rdev == b.st_rdev,
          "peer same inode");
    DIR *directory = opendir("/dev/pts"); truth(directory != NULL, "devpts directory");
    char number[32]; snprintf(number, sizeof(number), "%u", p.number);
    int saw_ptmx = 0, saw_slave = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, "ptmx")) saw_ptmx = 1;
        if (!strcmp(entry->d_name, number)) { saw_slave = 1; exact(entry->d_ino, a.st_ino, "dirent live slave identity"); }
    }
    truth(saw_ptmx && saw_slave, "devpts exposes live stable nodes"); exact(closedir(directory), 0, "directory close");
    exact(fchmod(p.slave, 0624), 0, "live slave fchmod");
    exact(fstat(peer, &b), 0, "peer mode after chmod"); exact(b.st_mode & 0777, 0624, "inode mode shared with peer");
    exact(stat(p.path, &b), 0, "pathname mode after chmod"); exact(b.st_mode & 0777, 0624, "inode mode shared with pathname");
    exact(fchmod(p.slave, 0620), 0, "restore slave mode");
    exact(mknod("/tmp/unbound-pty", S_IFCHR | 0600, makedev(136, 0)), 0, "ordinary unbound slave node");
    int unbound = open("/tmp/unbound-pty", O_PATH); truth(unbound >= 0, "unbound node O_PATH");
    struct stat unbound_stat; exact(fstat(unbound, &unbound_stat), 0, "unbound node metadata");
    truth(S_ISCHR(unbound_stat.st_mode) && unbound_stat.st_rdev == makedev(136, 0), "unbound node device identity");
    errno = 0; error_is(open("/tmp/unbound-pty", O_RDWR | O_NOCTTY), EIO, "ordinary slave node has no pair binding");
    exact(close(unbound), 0, "unbound path close"); exact(unlink("/tmp/unbound-pty"), 0, "unbound node unlink");
    truth(fcntl(peer, F_GETFD) & FD_CLOEXEC, "peer cloexec");
    truth(fcntl(peer, F_GETFL) & O_NONBLOCK, "peer nonblock");
    int lock = 1; exact(ioctl(p.master, TIOCSPTLCK, &lock), 0, "relock");
    errno = 0; error_is(open(p.path, O_RDWR | O_NOCTTY), EIO, "relocked new open");
    exact(tcgetattr(peer, &(struct termios){0}), 0, "existing slave survives relock");
    lock = 0; exact(ioctl(p.master, TIOCSPTLCK, &lock), 0, "unlock again");
    errno = 0; error_is(ioctl(p.master, TIOCGPTN, (void *)1), EFAULT, "number bad pointer");
    errno = 0; error_is(ioctl(p.master, TIOCSPTLCK, (void *)1), EFAULT, "lock bad pointer");
    exact(close(peer), 0, "peer close");
    record("lock-peer-stable-node", 1); pair_close(&p);
    truth(mkdir("/pts-other", 0755) == 0 || errno == EEXIST, "other mount dir");
    exact(mount("devpts", "/pts-other", "devpts", 0,
                "newinstance,mode=600,ptmxmode=666,max=2"), 0, "other devpts mount");
    struct pair first = pair_new(), other = pair_at("/pts-other"), second = pair_at("/pts-other");
    exact(fstat(first.slave, &a), 0, "first namespace stat");
    exact(fstat(other.slave, &b), 0, "other slave stat"); exact(b.st_mode & 0777, 0600, "mount slave mode");
    truth(a.st_dev != b.st_dev, "independent devpts instances");
    errno = 0; error_is(open("/pts-other/ptmx", O_RDWR | O_NOCTTY), ENOSPC, "mount max exhausted");
    pair_close(&second); struct pair reused = pair_at("/pts-other");
    pair_close(&reused);
    pair_close(&other); pair_close(&first);
    uint64_t deadline = now() + 5000000000ULL;
    while (umount("/pts-other") != 0) {
        exact(errno, EBUSY, "empty mount waits for worker owner");
        truth(now() < deadline, "devpts worker quiescence deadline"); usleep(1000);
    }
    record("mount-isolation-quota", 1);
}
static void transfers(void)
{
    struct pair p = pair_new(); unsigned char data[128];
    struct termios master_settings = {0}, slave_settings = {0};
    exact(tcgetattr(p.master, &master_settings), 0, "master termios aliases slave");
    exact(tcgetattr(p.slave, &slave_settings), 0, "slave initial termios");
    truth(master_settings.c_iflag == slave_settings.c_iflag &&
          master_settings.c_oflag == slave_settings.c_oflag &&
          master_settings.c_cflag == slave_settings.c_cflag &&
          master_settings.c_lflag == slave_settings.c_lflag &&
          !memcmp(master_settings.c_cc, slave_settings.c_cc, 19), "master TCGETS peer settings");
    struct termios changed = slave_settings; changed.c_iflag ^= INLCR;
    exact(tcsetattr(p.master, TCSANOW, &changed), 0, "master TCSETS applies to slave");
    exact(tcgetattr(p.slave, &master_settings), 0, "slave settings after master edit");
    exact(master_settings.c_iflag, changed.c_iflag, "master edit visible at slave");
    exact(tcsetattr(p.master, TCSANOW, &slave_settings), 0, "restore peer settings through master");
    raw(p.slave);
    write_all(p.master, "master-to-slave", 15); readable(p.slave);
    read_all(p.slave, data, 15); truth(!memcmp(data, "master-to-slave", 15), "raw m2s");
    write_all(p.slave, "slave-to-master", 15); readable(p.master);
    read_all(p.master, data, 15); truth(!memcmp(data, "slave-to-master", 15), "raw s2m");
    struct termios t; exact(tcgetattr(p.slave, &t), 0, "canonical get");
    t.c_lflag = ICANON; t.c_iflag = ICRNL; t.c_oflag = 0;
    t.c_cc[VERASE] = 127; t.c_cc[VEOF] = 4;
    exact(tcsetattr(p.slave, TCSANOW, &t), 0, "canonical set");
    write_all(p.master, "ab\177C", 4);
    truth(!(ready(p.slave, POLLIN, 30) & POLLIN), "canonical incomplete not readable");
    write_all(p.master, "\r", 1); readable(p.slave);
    exact(read(p.slave, data, sizeof(data)), 3, "canonical line length");
    truth(!memcmp(data, "aC\n", 3), "canonical edit and ICRNL");
    write_all(p.master, "E\004", 2); readable(p.slave);
    exact(read(p.slave, data, sizeof(data)), 1, "canonical EOF data"); exact(data[0], 'E', "EOF contents");
    write_all(p.master, "\004", 1); readable(p.slave); exact(read(p.slave, data, sizeof(data)), 0, "canonical EOF empty");
    raw(p.slave); nonblock(p.master); nonblock(p.slave);
    errno = 0; error_is(read(p.master, data, sizeof(data)), EAGAIN, "empty nonblocking master");
    errno = 0; error_is(read(p.slave, data, sizeof(data)), EAGAIN, "empty nonblocking slave");
    int ep = epoll_create1(EPOLL_CLOEXEC); truth(ep >= 0, "epoll create");
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 7};
    exact(epoll_ctl(ep, EPOLL_CTL_ADD, p.slave, &event), 0, "epoll pty add");
    write_all(p.master, "P", 1);
    exact(epoll_wait(ep, &event, 1, 5000), 1, "epoll pty ready");
    truth((event.events & EPOLLIN) && event.data.u64 == 7, "epoll identity");
    read_all(p.slave, data, 1); exact(close(ep), 0, "epoll close");
    record("raw-canonical-poll-epoll", 1); pair_close(&p);
}
static unsigned char packet(int fd, unsigned wanted)
{
    truth(ready(fd, POLLIN | POLLPRI, 5000) & POLLPRI, "packet priority ready");
    unsigned char value[8]; exact(read(fd, value, sizeof(value)), 1, "control packet length");
    exact(value[0], wanted, "control packet bits"); return value[0];
}
static void wait_input_count(int fd, int wanted)
{
    uint64_t deadline = now() + 5000000000ULL;
    for (;;) {
        int count; exact(ioctl(fd, FIONREAD, &count), 0, "flow input count");
        if (count == wanted) return;
        truth(now() < deadline, "flow input processing deadline");
    }
}
static void packets(void)
{
    struct pair p = pair_new(); raw(p.slave); int enabled = 1, actual = -1;
    exact(ioctl(p.master, TIOCPKT, &enabled), 0, "packet enable");
    exact(ioctl(p.master, TIOCGPKT, &actual), 0, "packet query"); exact(actual, 1, "packet active");
    unsigned char data[128]; write_all(p.slave, "ABCD", 4); readable(p.master);
    exact(read(p.master, data, 1), 1, "packet count1"); exact(data[0], 0, "packet count1 header");
    struct iovec iov[2] = {{data, 1}, {data + 1, 3}};
    exact(readv(p.master, iov, 2), 4, "packet readv");
    truth(!memcmp(data, "\0ABC", 4), "one header across readv");
    exact(read(p.master, data, sizeof(data)), 2, "packet remainder");
    truth(data[0] == 0 && data[1] == 'D', "packet remainder contents");
    exact(tcflow(p.slave, TCOOFF), 0, "stop output"); packet(p.master, TIOCPKT_STOP);
    exact(tcflow(p.slave, TCOOFF), 0, "duplicate stop output");
    truth(!(ready(p.master, POLLPRI, 0) & POLLPRI), "duplicate stop has no packet");
    exact(tcflow(p.slave, TCOON), 0, "start output"); packet(p.master, TIOCPKT_START);
    exact(tcflow(p.slave, TCOON), 0, "duplicate start output");
    truth(!(ready(p.master, POLLPRI, 0) & POLLPRI), "duplicate start has no packet");
    exact(tcflow(p.slave, TCOOFF), 0, "stop control fault");
    truth(ready(p.master, POLLIN | POLLPRI, 5000) & POLLPRI, "faulted control ready");
    size_t fault_page = (size_t)sysconf(_SC_PAGESIZE);
    void *fault = mmap(NULL, fault_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    truth(fault != MAP_FAILED, "control fault map");
    errno = 0; error_is(read(p.master, fault, 1), EFAULT, "control packet bad destination");
    exact(munmap(fault, fault_page), 0, "control fault unmap");
    truth(!(ready(p.master, POLLIN | POLLPRI, 0) & POLLPRI), "faulted control consumed");
    exact(tcflow(p.slave, TCOON), 0, "restart after control fault"); packet(p.master, TIOCPKT_START);
    exact(tcflush(p.slave, TCIFLUSH), 0, "flush input"); packet(p.master, TIOCPKT_FLUSHREAD);
    exact(tcflush(p.slave, TCOFLUSH), 0, "flush output"); packet(p.master, TIOCPKT_FLUSHWRITE);
    struct termios t; exact(tcgetattr(p.slave, &t), 0, "flow termios get");
    t.c_iflag |= IXON; t.c_cc[VSTART] = 17; t.c_cc[VSTOP] = 19;
    exact(tcsetattr(p.slave, TCSANOW, &t), 0, "packet DOSTOP"); packet(p.master, TIOCPKT_DOSTOP);
    t.c_iflag &= ~IXON; exact(tcsetattr(p.slave, TCSANOW, &t), 0, "packet NOSTOP");
    packet(p.master, TIOCPKT_NOSTOP);
    t.c_iflag |= IXON | IXANY;
    exact(tcsetattr(p.slave, TCSANOW, &t), 0, "packet IXANY mode"); packet(p.master, TIOCPKT_DOSTOP);
    write_all(p.master, "\023", 1); packet(p.master, TIOCPKT_STOP);
    write_all(p.master, "I", 1); packet(p.master, TIOCPKT_START);
    wait_input_count(p.slave, 1); exact(read(p.slave, data, 1), 1, "IXANY ordinary input"); exact(data[0], 'I', "IXANY contents");
    t.c_iflag &= ~IXANY; exact(tcsetattr(p.slave, TCSANOW, &t), 0, "IXON without IXANY");
    write_all(p.master, "\023", 1); packet(p.master, TIOCPKT_STOP);
    write_all(p.master, "\023S", 2); wait_input_count(p.slave, 1);
    exact(read(p.slave, data, 1), 1, "duplicate VSTOP handshake byte"); exact(data[0], 'S', "duplicate VSTOP contents");
    truth(!(ready(p.master, POLLPRI, 0) & POLLPRI), "duplicate VSTOP has no packet");
    write_all(p.master, "\021", 1); packet(p.master, TIOCPKT_START);
    write_all(p.master, "\021T", 2); wait_input_count(p.slave, 1);
    exact(read(p.slave, data, 1), 1, "duplicate VSTART handshake byte"); exact(data[0], 'T', "duplicate VSTART contents");
    truth(!(ready(p.master, POLLPRI, 0) & POLLPRI), "duplicate VSTART has no packet");
    t.c_lflag = ISIG | NOFLSH; t.c_cc[VINTR] = 3;
    exact(tcsetattr(p.slave, TCSANOW, &t), 0, "ISIG NOFLSH without ctty");
    write_all(p.master, "\023", 1); packet(p.master, TIOCPKT_STOP);
    write_all(p.master, "\003", 1); packet(p.master, TIOCPKT_START);
    t.c_lflag = 0; exact(tcsetattr(p.slave, TCSANOW, &t), 0, "restore nonsignal mode");
    write_all(p.master, "\023", 1); packet(p.master, TIOCPKT_STOP);
    t.c_iflag &= ~IXON; exact(tcsetattr(p.slave, TCSANOW, &t), 0, "disable stopped IXON");
    unsigned resumed = 0, resume_packets = 0;
    while (resumed != (TIOCPKT_START | TIOCPKT_NOSTOP)) {
        truth(++resume_packets <= 2, "IXON disable finite transition packets");
        truth(ready(p.master, POLLIN | POLLPRI, 5000) & POLLPRI, "IXON disable resume ready");
        exact(read(p.master, data, sizeof(data)), 1, "IXON disable control packet");
        truth(data[0] && !(data[0] & ~(TIOCPKT_START | TIOCPKT_NOSTOP)), "IXON disable allowed event bits");
        resumed |= data[0];
    }
    t.c_lflag |= ICANON | EXTPROC; exact(tcsetattr(p.slave, TCSANOW, &t), 0, "packet EXTPROC");
    packet(p.master, TIOCPKT_IOCTL);
    write_all(p.master, "Q", 1); readable(p.slave);
    exact(read(p.slave, data, sizeof(data)), 1, "EXTPROC bypass canonical"); exact(data[0], 'Q', "EXTPROC data");
    enabled = 0; exact(ioctl(p.master, TIOCPKT, &enabled), 0, "packet disable");
    write_all(p.slave, "N", 1); readable(p.master); exact(read(p.master, data, sizeof(data)), 1, "no packet prefix");
    exact(data[0], 'N', "packet disabled data");
    record("packet-data-controls-extproc", 1); pair_close(&p);
}
static void faults(void)
{
    struct pair p = pair_new(); raw(p.slave);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *area = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    truth(area != MAP_FAILED, "fault mmap"); exact(mprotect(area + page, page, PROT_NONE), 0, "fault guard");
    const unsigned prefixes[] = {1, 8, 63}; unsigned char data[128];
    for (unsigned j = 0; j < sizeof(prefixes) / sizeof(prefixes[0]); j++) {
        unsigned n = prefixes[j]; memset(area + page - n, 'W', n);
        errno = 0; error_is(write(p.master, area + page - n, 100), EFAULT, "short write fault rejects whole chunk");
        truth(!(ready(p.slave, POLLIN, 0) & POLLIN), "faulted first chunk unpublished");
    }
    memset(area + page - 2048, 'W', 2048);
    exact(write(p.master, area + page - 2048, 2049), 2048, "later write chunk fault keeps accepted prefix");
    readable(p.slave); unsigned char accepted[2048]; read_all(p.slave, accepted, sizeof(accepted));
    for (unsigned i = 0; i < sizeof(accepted); i++) exact(accepted[i], 'W', "accepted complete chunk contents");
    struct iovec write_vectors[2] = {{area + page - 8, 8}, {area + page, 1}};
    errno = 0; error_is(writev(p.master, write_vectors, 2), EFAULT, "writev first chunk imports across vectors");
    truth(!(ready(p.slave, POLLIN, 0) & POLLIN), "writev faulted chunk unpublished");
    write_vectors[0] = (struct iovec){area + page - 1024, 1024};
    write_vectors[1] = (struct iovec){area + page - 1024, 1025};
    exact(writev(p.master, write_vectors, 2), 2048, "writev second chunk fault prefix");
    readable(p.slave); read_all(p.slave, accepted, sizeof(accepted));
    for (unsigned i = 0; i < sizeof(accepted); i++) exact(accepted[i], 'W', "writev accepted complete chunk");
    int enabled = 1; exact(ioctl(p.master, TIOCPKT, &enabled), 0, "fault packet enable");
    memset(data, 'F', 100); write_all(p.slave, data, 100); readable(p.master);
    exact(read(p.master, area + page - 1, 128), 1, "packet read fault prefix");
    exact(area[page - 1], 0, "fault packet header copied");
    exact(read(p.master, data, sizeof(data)), 38, "packet consumed staging prefix");
    exact(data[0], 0, "remainder packet header");
    for (unsigned i = 1; i < 38; i++) exact(data[i], 'F', "packet fault remainder");
    memset(data, 'V', 100); write_all(p.slave, data, 100); readable(p.master);
    struct iovec iov[2] = {{data, 1}, {area + page, 127}};
    exact(readv(p.master, iov, 2), 1, "packet vector fault");
    exact(read(p.master, data, sizeof(data)), 38, "packet vector consumed staging");
    enabled = 0; exact(ioctl(p.master, TIOCPKT, &enabled), 0, "fault packet disable");
    exact(munmap(area, page * 2), 0, "fault unmap");
    record("write-readv-packet-fault-prefix", 1); pair_close(&p);
}
static void lifecycle(void)
{
    truth(mkdir("/pts-lifetime", 0755) == 0 || errno == EEXIST, "lifetime mount dir");
    exact(mount("devpts", "/pts-lifetime", "devpts", 0,
                "newinstance,mode=620,ptmxmode=666,max=2"), 0, "lifetime mount");
    struct pair p = pair_at("/pts-lifetime"); unsigned char data[16]; raw(p.slave); nonblock(p.master);
    exact(close(p.slave), 0, "last slave close"); p.slave = -1;
    truth(ready(p.master, POLLIN, 5000) & POLLHUP, "master slave-close HUP");
    errno = 0; error_is(read(p.master, data, sizeof(data)), EIO, "master no slave EIO");
    p.slave = open(p.path, O_RDWR | O_NOCTTY); truth(p.slave >= 0, "slave reopen"); raw(p.slave);
    truth(!(ready(p.master, POLLIN, 0) & POLLHUP), "reopen clears HUP");
    write_all(p.slave, "reopened", 8); readable(p.master); read_all(p.master, data, 8);
    truth(!memcmp(data, "reopened", 8), "reopened transfer");
    int path = open(p.path, O_PATH | O_NOFOLLOW); truth(path >= 0, "pin old devpts node");
    struct stat old, gone; exact(fstat(path, &old), 0, "old node stat");
    exact(close(p.master), 0, "master hangup"); p.master = -1;
    truth(ready(p.slave, POLLIN, 5000) & POLLHUP, "slave master-close HUP");
    exact(read(p.slave, data, sizeof(data)), 0, "hungup slave EOF");
    errno = 0; error_is(write(p.slave, "X", 1), EIO, "hungup slave write");
    errno = 0; error_is(stat(p.path, &gone), ENOENT, "master removes path");
    exact(fstat(path, &gone), 0, "old inode retained");
    truth(old.st_dev == gone.st_dev && old.st_ino == gone.st_ino && old.st_rdev == gone.st_rdev,
          "old metadata identity retained"); exact(gone.st_nlink, 0, "old node unlinked");
    struct pair concurrent = pair_at("/pts-lifetime"); truth(concurrent.number != p.number, "live old slave reserves number");
    pair_close(&p); pair_close(&concurrent);
    struct pair next = pair_at("/pts-lifetime");
    uint64_t deadline = now() + 5000000000ULL;
    while (next.number != p.number) {
        pair_close(&next); truth(now() < deadline, "retired index reuse deadline");
        usleep(1000); next = pair_at("/pts-lifetime");
    }
    char reopen[80]; snprintf(reopen, sizeof(reopen), "/proc/self/fd/%d", path);
    errno = 0; error_is(open(reopen, O_RDWR | O_NOCTTY), EIO, "old pinned node cannot rebind");
    exact(fstat(path, &gone), 0, "old stat after reuse"); exact(gone.st_nlink, 0, "old node stays unlinked");
    exact(close(path), 0, "old path release"); pair_close(&next);
    while (umount("/pts-lifetime") != 0) {
        exact(errno, EBUSY, "lifetime mount worker owner");
        truth(now() < deadline, "lifetime worker deadline"); usleep(1000);
    }
    record("close-reopen-unlink-stable-old-node", 1);
}
static void signal_handler(int signal)
{
    if (signal == SIGWINCH) signals_seen |= 1;
    if (signal == SIGINT) signals_seen |= 2;
    if (signal == SIGHUP) signals_seen |= 4;
    if (signal == SIGCONT) signals_seen |= 8;
}
static void wait_signal(unsigned mask)
{
    uint64_t end = now() + 5000000000ULL;
    while (((unsigned)signals_seen & mask) != mask) {
        truth(now() < end, "signal deadline"); usleep(1000);
    }
}
static void controlling(int acquire_master)
{
    struct pair p = pair_new(); int report[2], gate[2];
    exact(pipe(report), 0, "ctty report pipe"); exact(pipe(gate), 0, "ctty gate pipe");
    pid_t child = fork(); truth(child >= 0, "ctty fork");
    if (!child) {
        close(report[0]); close(gate[1]);
        exact(setsid(), getpid(), "ctty setsid");
        exact(ioctl(acquire_master ? p.master : p.slave, TIOCSCTTY, 0), 0, "ctty acquire");
        close(p.master);
        exact(tcgetpgrp(p.slave), getpgrp(), "slave foreground pgrp");
        pid_t sid = -1; exact(ioctl(p.slave, TIOCGSID, &sid), 0, "ctty sid"); exact(sid, getsid(0), "ctty own sid");
        int alias = open("/dev/tty", O_RDWR | O_NOCTTY); truth(alias >= 0, "ctty alias"); close(alias);
        struct sigaction a = {.sa_handler = signal_handler}; sigemptyset(&a.sa_mask);
        exact(sigaction(SIGWINCH, &a, NULL), 0, "WINCH handler");
        exact(sigaction(SIGINT, &a, NULL), 0, "INT handler");
        exact(sigaction(SIGHUP, &a, NULL), 0, "HUP handler");
        exact(sigaction(SIGCONT, &a, NULL), 0, "CONT handler");
        write_all(report[1], "R", 1); wait_signal(1);
        struct winsize w; exact(ioctl(p.slave, TIOCGWINSZ, &w), 0, "slave resized");
        truth(w.ws_row == 37 && w.ws_col == 91, "winsize propagates"); write_all(report[1], "W", 1);
        wait_signal(2); write_all(report[1], "I", 1);
        unsigned char c; read_all(gate[0], &c, 1); wait_signal(4 | 8);
        errno = 0; error_is(open("/dev/tty", O_RDWR | O_NOCTTY), ENXIO, "hangup clears ctty");
        write_all(report[1], "H", 1); close(p.slave); _exit(0);
    }
    close(report[1]); close(gate[0]); close(p.slave); p.slave = -1;
    unsigned char c; read_all(report[0], &c, 1); exact(c, 'R', "ctty ready");
    struct winsize w = {.ws_row = 37, .ws_col = 91};
    exact(ioctl(p.master, TIOCSWINSZ, &w), 0, "master resize"); read_all(report[0], &c, 1); exact(c, 'W', "WINCH delivered");
    exact(ioctl(p.master, TIOCSIG, SIGINT), 0, "master signal"); read_all(report[0], &c, 1); exact(c, 'I', "INT delivered");
    exact(close(p.master), 0, "ctty master close"); p.master = -1;
    write_all(gate[1], "H", 1); read_all(report[0], &c, 1); exact(c, 'H', "HUP delivered");
    int status; exact(waitpid(child, &status, 0), child, "ctty child reap"); exact(status, 0, "ctty child status");
    close(report[0]); close(gate[1]);
    record(acquire_master ? "master-acquire-slave-ctty-winsize-signal-hangup" :
                           "slave-ctty-winsize-signal-hangup", 1);
}
static struct raw_termios2 get_termios2(int fd)
{
    unsigned char guarded[8 + 44 + 8] __attribute__((aligned(8)));
    memset(guarded, 0xa5, sizeof(guarded));
    exact(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCGETS2, guarded + 8), 0, "raw TCGETS2");
    for (unsigned i = 0; i < 8; i++) {
        exact(guarded[i], 0xa5, "termios2 front guard");
        exact(guarded[8 + 44 + i], 0xa5, "termios2 exact44 guard");
    }
    struct raw_termios2 value; memcpy(&value, guarded + 8, sizeof(value)); return value;
}
static void termios2_endpoint(int fd, int serial)
{
    struct raw_termios2 original = get_termios2(fd), request = original, got;
    struct raw_termios old;
    exact(syscall(SYS_ioctl, fd, 0x5401UL, &old), 0, "old TCGETS still36");
    truth(!memcmp(&old, &original.basic, sizeof(old)), "old36 equals termios2 basic prefix");
    uint32_t standard = serial ? UINT32_C(0x1002) : 13U;
    uint32_t standard_rate = serial ? 115200U : 9600U;
    request.basic.cflag = (request.basic.cflag & ~(RAW_CBAUD | RAW_CIBAUD)) | standard;
    request.ispeed = 11111; request.ospeed = 22222;
    exact(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCSETS2, &request), 0, "termios2 standard set");
    got = get_termios2(fd);
    exact(got.ispeed, standard_rate, "standard input ignores numeric field");
    exact(got.ospeed, standard_rate, "standard output ignores numeric field");
    request = got;
    request.basic.cflag = (request.basic.cflag & ~(RAW_CBAUD | RAW_CIBAUD)) |
                          RAW_BOTHER | (RAW_BOTHER << 16);
    request.ispeed = 4800; request.ospeed = serial ? 9600 : 12345;
    exact(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCSETSW2, &request), 0, "termios2 BOTHER drain set");
    got = get_termios2(fd);
    exact(got.ospeed, request.ospeed, "BOTHER output metadata");
    exact(got.ispeed, serial ? request.ospeed : request.ispeed, "transport input clock contract");
    exact(got.basic.cflag & RAW_CBAUD, serial ? 13 : RAW_BOTHER, "output baud encoding");
    exact(got.basic.cflag & RAW_CIBAUD, serial ? (13U << 16) : (RAW_BOTHER << 16), "input baud encoding");
    errno = 0;
    error_is(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCGETS2, (void *)1), EFAULT, "TCGETS2 bad destination");
    errno = 0;
    error_is(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCSETS2, (void *)1), EFAULT, "TCSETS2 bad source");
    struct raw_termios2 unchanged = get_termios2(fd);
    truth(!memcmp(&got, &unchanged, sizeof(got)), "bad source preserves complete settings");
    unsigned long wide = (UINT64_C(0x12345678) << 32) | RAW_TCGETS2;
    exact(syscall(SYS_ioctl, fd, wide, &unchanged), 0, "ioctl command casts to32bits");
    truth(!memcmp(&got, &unchanged, sizeof(got)), "wide command same44 response");
    exact(syscall(SYS_ioctl, fd, (unsigned long)RAW_TCSETSF2, &original), 0, "termios2 flush restore");
    got = get_termios2(fd);
    truth(!memcmp(&got, &original, sizeof(got)), "termios2 restores original complete settings");
    exact(syscall(SYS_ioctl, fd, 0x5401UL, &old), 0, "old36 after restore");
    truth(!memcmp(&old, &original.basic, sizeof(old)), "old36 remains unchanged");
}
static void termios2_cases(void)
{
    struct pair p = pair_new(); termios2_endpoint(p.master, 0);
    struct raw_termios2 master = get_termios2(p.master), slave = get_termios2(p.slave);
    truth(!memcmp(&master, &slave, sizeof(master)), "master termios2 aliases slave");
    pair_close(&p);
    int serial = open("/dev/ttyS0", O_RDWR | O_NOCTTY); truth(serial >= 0, "serial termios2 open");
    termios2_endpoint(serial, 1); exact(close(serial), 0, "serial termios2 close");
    record("pty-serial-termios2-44byte-speeds-fault-restore", 1);
}
static void libc_api(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    truth(master >= 0, "libc posix_openpt"); exact(grantpt(master), 0, "libc grantpt");
    exact(unlockpt(master), 0, "libc unlockpt"); char name[128];
    exact(ptsname_r(master, name, sizeof(name)), 0, "libc ptsname_r");
    int slave = open(name, O_RDWR | O_NOCTTY); truth(slave >= 0, "libc ptsname open");
    raw(slave); write_all(master, "libc", 4); readable(slave); unsigned char data[32];
    read_all(slave, data, 4); truth(!memcmp(data, "libc", 4), "libc transfer");
    close(slave); close(master);
    struct winsize w = {.ws_row = 25, .ws_col = 83};
    exact(openpty(&master, &slave, name, NULL, &w), 0, "real openpty"); raw(slave);
    struct winsize got; exact(ioctl(master, TIOCGWINSZ, &got), 0, "openpty winsize");
    truth(got.ws_row == 25 && got.ws_col == 83, "openpty requested winsize");
    pid_t child = fork(); truth(child >= 0, "login_tty fork");
    if (!child) {
        close(master); exact(login_tty(slave), 0, "real login_tty");
        truth(isatty(0) && tcgetpgrp(0) == getpgrp(), "login_tty stdio and ctty");
        write_all(1, "PTY_LIBC_LOGIN", 14); _exit(0);
    }
    close(slave); readable(master); read_all(master, data, 14);
    truth(!memcmp(data, "PTY_LIBC_LOGIN", 14), "login_tty output");
    int status; exact(waitpid(child, &status, 0), child, "login_tty reap"); exact(status, 0, "login_tty child status");
    close(master); record(api_record, 1);
}
static unsigned char payload(unsigned stream, uint64_t offset)
{ return (unsigned char)((offset * 37U + (offset >> 8U) + stream * 53U) & 255U); }
struct memory_sample { unsigned long long free_kib, available_kib; };
static struct memory_sample memory_sample(void)
{
    int fd = open("/proc/meminfo", O_RDONLY); truth(fd >= 0, "performance meminfo");
    char text[4096]; ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
    truth(n > 0, "performance meminfo read"); text[n] = 0;
    struct memory_sample result; char *free_value = strstr(text, "MemFree:");
    char *available = strstr(text, "MemAvailable:");
    truth(free_value && available && sscanf(free_value, "MemFree: %llu", &result.free_kib) == 1 &&
          sscanf(available, "MemAvailable: %llu", &result.available_kib) == 1, "performance memory fields");
    return result;
}
static uint64_t timeval_ns(const struct timeval *value)
{ return (uint64_t)value->tv_sec * 1000000000ULL + (uint64_t)value->tv_usec * 1000U; }
static void performance(unsigned pairs, uint64_t size)
{
    truth(pairs > 0 && pairs <= 8 && size > 0, "performance parameters");
    struct pair p[8]; uint64_t sent[16] = {0}, received[16] = {0};
    uint64_t completed_ns[16] = {0};
    struct pollfd fds[16]; unsigned char data[4096];
    struct memory_sample before = memory_sample();
    for (unsigned i = 0; i < pairs; i++) {
        p[i] = pair_new(); raw(p[i].slave); nonblock(p[i].master); nonblock(p[i].slave);
        fds[2 * i] = (struct pollfd){p[i].master, POLLIN | POLLOUT, 0};
        fds[2 * i + 1] = (struct pollfd){p[i].slave, POLLIN | POLLOUT, 0};
    }
    struct memory_sample active = memory_sample();
    struct rusage cpu_before = {0}, cpu_after = {0};
    int rusage_available = getrusage(RUSAGE_SELF, &cpu_before) == 0;
    struct tms ticks_before, ticks_after;
    truth(times(&ticks_before) >= 0, "coordinator CPU tick baseline");
    uint64_t start = now(), deadline = start + 120000000000ULL; unsigned done = 0;
    while (done < pairs * 2) {
        truth(now() < deadline, "performance deadline");
        int n = poll(fds, pairs * 2, 1000); if (n < 0 && errno == EINTR) continue; truth(n >= 0, "performance poll");
        for (unsigned i = 0; i < pairs * 2; i++) {
            if ((fds[i].revents & POLLOUT) && sent[i] < size) {
                size_t count = size - sent[i] < sizeof(data) ? (size_t)(size - sent[i]) : sizeof(data);
                for (size_t j = 0; j < count; j++) data[j] = payload(i, sent[i] + j);
                ssize_t used = write(fds[i].fd, data, count);
                if (used < 0) truth(errno == EAGAIN || errno == EINTR, "performance write errno");
                else { truth(used > 0, "performance write prefix"); sent[i] += (uint64_t)used; }
            }
            if ((fds[i].revents & POLLIN) && received[i] < size) {
                ssize_t used = read(fds[i].fd, data, sizeof(data));
                if (used < 0) truth(errno == EAGAIN || errno == EINTR, "performance read errno");
                else {
                    truth(used > 0 && (uint64_t)used <= size - received[i], "performance read count");
                    for (ssize_t j = 0; j < used; j++) exact(data[j], payload(i ^ 1U, received[i] + (uint64_t)j), "performance content");
                    received[i] += (uint64_t)used;
                    if (received[i] == size) { done++; completed_ns[i] = now() - start; }
                }
            }
            fds[i].events = (received[i] < size ? POLLIN : 0) | (sent[i] < size ? POLLOUT : 0);
        }
    }
    uint64_t elapsed = now() - start;
    truth(times(&ticks_after) >= 0, "coordinator CPU tick result");
    if (rusage_available) exact(getrusage(RUSAGE_SELF, &cpu_after), 0, "coordinator rusage result");
    struct memory_sample completed = memory_sample();
    for (unsigned i = 0; i < pairs * 2; i++) exact((long)sent[i], (long)size, "all transmitted bytes");
    for (unsigned i = 0; i < pairs * 2; i++)
        dprintf(1, "PTY_PROGRESS pair=%u direction=%u bytes=%llu complete_ns=%llu\n", i / 2, i % 2,
                (unsigned long long)size, (unsigned long long)completed_ns[i]);
    dprintf(1, "PTY_TIMING pairs=%u bytes_each_direction=%llu elapsed_ns=%llu\n", pairs,
            (unsigned long long)size, (unsigned long long)elapsed);
    for (unsigned i = 0; i < pairs; i++) pair_close(&p[i]);
    struct memory_sample closed = memory_sample();
    unsigned long long minimum = before.free_kib;
    if (active.free_kib < minimum) minimum = active.free_kib;
    if (completed.free_kib < minimum) minimum = completed.free_kib;
    if (closed.free_kib < minimum) minimum = closed.free_kib;
    dprintf(1, "PTY_MEMORY before_free_kib=%llu active_free_kib=%llu completed_free_kib=%llu after_close_free_kib=%llu sampled_min_free_kib=%llu before_available_kib=%llu active_available_kib=%llu\n",
            before.free_kib, active.free_kib, completed.free_kib, closed.free_kib, minimum,
            before.available_kib, active.available_kib);
    if (rusage_available)
        dprintf(1, "PTY_CPU scope=coordinator source=getrusage user_ns=%llu system_ns=%llu\n",
                (unsigned long long)(timeval_ns(&cpu_after.ru_utime) - timeval_ns(&cpu_before.ru_utime)),
                (unsigned long long)(timeval_ns(&cpu_after.ru_stime) - timeval_ns(&cpu_before.ru_stime)));
    else
        dprintf(1, "PTY_CPU scope=coordinator source=times coarse=1 hz=%ld user_ticks=%ld system_ticks=%ld getrusage_available=0\n",
                sysconf(_SC_CLK_TCK), (long)(ticks_after.tms_utime - ticks_before.tms_utime),
                (long)(ticks_after.tms_stime - ticks_before.tms_stime));
    record("full-duplex-exact-tail", (long)(pairs * 2U * size));
}
static void libc_both(void)
{
    libc_api();
    pid_t child = fork(); truth(child >= 0, "glibc API fork");
    if (!child) {
        execl("/pty-glibc-api", "pty-glibc-api", "--api", "glibc", (char *)NULL);
        perror("exec real glibc API"); _exit(127);
    }
    int status; exact(waitpid(child, &status, 0), child, "glibc API reap");
    exact(status, 0, "real glibc API status"); records++;
}
static void script_payload(uint64_t size, int status, int hold)
{
    truth(isatty(0) && isatty(1) && isatty(2), "script child tty stdio");
    truth(tcgetpgrp(0) == getpgrp() && getsid(0) == getpid(), "script child controlling session");
    raw(1); unsigned char data[4096];
    for (uint64_t offset = 0; offset < size;) {
        size_t count = size - offset < sizeof(data) ? (size_t)(size - offset) : sizeof(data);
        for (size_t j = 0; j < count; j++) data[j] = payload(23, offset + j);
        write_all(1, data, count); offset += count;
    }
    exact(tcdrain(1), 0, "script child real drain");
    if (hold) {
        uint64_t deadline = now() + 120000000000ULL;
        while (access("/tmp/script.release", F_OK) != 0) {
            truth(now() < deadline, "script capture visibility deadline"); usleep(1000);
        }
    }
    _exit(status);
}
static void check_payload_file(const char *path, uint64_t offset, uint64_t size, int exact_size)
{
    int fd = open(path, O_RDONLY); truth(fd >= 0, "payload file open");
    exact(lseek(fd, (off_t)offset, SEEK_SET), (off_t)offset, "payload seek");
    unsigned char data[4096]; uint64_t done = 0;
    while (done < size) {
        size_t count = size - done < sizeof(data) ? (size_t)(size - done) : sizeof(data);
        read_all(fd, data, count);
        for (size_t j = 0; j < count; j++) exact(data[j], payload(23, done + j), "script exact payload and tail");
        done += count;
    }
    if (exact_size) exact(read(fd, data, 1), 0, "script no trailing bytes");
    close(fd);
}
static int file_contains(const char *path, const char *wanted)
{
    int fd = open(path, O_RDONLY); truth(fd >= 0, "text capture open");
    char data[8192]; ssize_t n = read(fd, data, sizeof(data) - 1);
    truth(n >= 0, "text capture read"); data[n] = 0; close(fd);
    return strstr(data, wanted) != NULL;
}
static void script_expect(const char *path, const char *wanted, off_t *cursor)
{
    uint64_t deadline = now() + 30000000000ULL;
    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            exact(lseek(fd, *cursor, SEEK_SET), *cursor, "script transcript cursor");
            char text[8192]; ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
            truth(n >= 0, "script handshake read"); text[n] = 0;
            char *match = strstr(text, wanted);
            if (match) { *cursor += match - text + (off_t)strlen(wanted); return; }
        } else exact(errno, ENOENT, "script handshake file");
        truth(now() < deadline, wanted); usleep(1000);
    }
}
static void scene_foreground(pid_t shell, const char *role)
{
    char path[80]; snprintf(path, sizeof(path), "/proc/%d/stat", shell);
    int fd = open(path, O_RDONLY); truth(fd >= 0, "original shell proc state");
    char text[1024]; ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
    truth(n > 0, "original shell state read"); text[n] = 0;
    char *tail = strrchr(text, ')'); char state; long parent, group, session, tty, foreground;
    truth(tail && sscanf(tail + 2, "%c %ld %ld %ld %ld %ld", &state, &parent,
          &group, &session, &tty, &foreground) == 6 && tty && foreground > 1 &&
          foreground != shell, "actual foreground job identity");
    fd = open("/tmp/script-jobs.pids", O_WRONLY | O_APPEND); truth(fd >= 0, "foreground job journal");
    char line[80]; int length = snprintf(line, sizeof(line), "%s %ld\n", role, foreground);
    write_all(fd, line, (size_t)length); close(fd);
}
static pid_t script_job_scene(int input, const char *typescript)
{
    off_t cursor = 0; script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    const char *line = "printf 'shell %s\\n' \"$$\" >/tmp/script-jobs.pids; stty sane; stty rows 24 cols 80; test \"$(stty size)\" = '24 80' && printf 'PTY_JOB_%s\\n' STTY_OK\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_STTY_OK", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    FILE *pids = fopen("/tmp/script-jobs.pids", "r"); truth(pids != NULL, "script actual shell identity");
    char role[32]; long shell; truth(fscanf(pids, "%31s %ld", role, &shell) == 2 &&
                                  !strcmp(role, "shell") && shell > 1, "script shell pid record"); fclose(pids);
    line = "sleep 600 & sleep_pid=$!; /gate wait-foreground \"$sleep_pid\" & watcher_pid=$!; printf 'sleep %s\\nwatcher %s\\n' \"$sleep_pid\" \"$watcher_pid\" >>/tmp/script-jobs.pids; fg %sleep\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "TTY_GATE_SLEEP_READY", &cursor);
    write_all(input, "\003", 1); script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    line = "wait \"$watcher_pid\"; printf 'PTY_JOB_%s\\n' SLEEP_C_OK\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_SLEEP_C_OK", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    write_all(input, "/gate cpu\n", 10); script_expect(typescript, "TTY_GATE_CPU", &cursor);
    scene_foreground((pid_t)shell, "cpu");
    write_all(input, "\003", 1); script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    line = "printf 'PTY_JOB_%s\\n' CPU_C_OK\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_CPU_C_OK", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    write_all(input, "/gate sleep\n", 12); script_expect(typescript, "TTY_GATE_SLEEP", &cursor);
    scene_foreground((pid_t)shell, "gate");
    write_all(input, "\032", 1); script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    write_all(input, "jobs\n", 5); script_expect(typescript, "Stopped", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    off_t background = cursor, prompt = cursor;
    write_all(input, "bg\n", 3); script_expect(typescript, "TTY_GATE_BACKGROUND_RESUMED", &background);
    script_expect(typescript, "PTY_SCRIPT$ ", &prompt);
    cursor = background > prompt ? background : prompt;
    write_all(input, "fg\n", 3); script_expect(typescript, "TTY_GATE_FOREGROUND_RESUMED", &cursor);
    write_all(input, "\003", 1); script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    line = "cat & read_pid=$!; printf 'reader %s\\n' \"$read_pid\" >>/tmp/script-jobs.pids; /pty-probe --wait-stopped \"$read_pid\"; jobs; printf 'PTY_JOB_%s\\n' BG_READ_STOPPED\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_READER_STOPPED", &cursor);
    script_expect(typescript, "Stopped", &cursor); script_expect(typescript, "PTY_JOB_BG_READ_STOPPED", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    line = "kill -KILL \"$read_pid\"; wait \"$read_pid\"; while kill -0 \"$read_pid\" 2>/dev/null; do :; done; jobs >/dev/null; printf 'PTY_JOB_%s\\n' BG_REAPED\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_BG_REAPED", &cursor);
    script_expect(typescript, "PTY_SCRIPT$ ", &cursor);
    line = "printf 'PTY_JOB_%s\\n' ALL_OK; exit 7\n";
    write_all(input, line, strlen(line)); script_expect(typescript, "PTY_JOB_ALL_OK", &cursor);
    return (pid_t)shell;
}
static void stopped_reader(pid_t child)
{
    char path[80]; snprintf(path, sizeof(path), "/proc/%d/stat", child);
    uint64_t deadline = now() + 30000000000ULL;
    for (;;) {
        int fd = open(path, O_RDONLY); truth(fd >= 0, "background reader still exists");
        char text[1024]; ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
        truth(n > 0, "background reader state"); text[n] = 0;
        char *tail = strrchr(text, ')'); truth(tail && tail[1] == ' ', "proc stat state layout");
        if (tail[2] == 'T') { puts("PTY_JOB_READER_STOPPED"); return; }
        truth(now() < deadline, "background reader SIGTTIN deadline"); usleep(1000);
    }
}
static void adopted_job(pid_t child, int status)
{
    FILE *pids = fopen("/tmp/script-jobs.pids", "r"); truth(pids != NULL, "adopted job identities");
    char role[32], selected[32] = {0}; long number; int found = 0;
    while (fscanf(pids, "%31s %ld", role, &number) == 2) {
        if (number == child) { found = 1; memcpy(selected, role, sizeof(selected)); }
    }
    fclose(pids); truth(found && strcmp(selected, "shell"), "recognized adopted original-shell job");
    /* 原ash可选择自己回收；若被PID1收养，保留真实状态而不是丢弃。 */
    dprintf(1, "PTY_SCRIPT_ADOPTED role=%s raw_status=%d\n", selected, status);
}
static void script_run(const char *name, const char *output, const char *input,
                       const char *command_text, unsigned options, int wanted_status,
                       uint64_t visible_size)
{
    /* flags: 1=quiet, 2=append, 4=flush, 8=timing-file, 16=timing-stderr, 32=jobs. */
    char capture[128], errors[128], timing[160];
    snprintf(capture, sizeof(capture), "/tmp/%s.stdout", name);
    snprintf(errors, sizeof(errors), "/tmp/%s.stderr", name);
    snprintf(timing, sizeof(timing), "-t/tmp/%s.timing", name);
    unlink("/tmp/script.release");
    struct pair input_tty = pair_new();
    if (!(options & 32)) raw(input_tty.slave);
    pid_t script = fork(); truth(script >= 0, "original script fork");
    if (!script) {
        close(input_tty.master);
        int out = open(capture, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        int err = open(errors, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        truth(out >= 0 && err >= 0, "script capture files");
        exact(dup2(input_tty.slave, 0), 0, "script stdin tty");
        exact(dup2(out, 1), 1, "script stdout capture");
        exact(dup2(err, 2), 2, "script stderr capture");
        close(out); close(err); if (input_tty.slave > 2) close(input_tty.slave);
        char *argv[14]; unsigned count = 0;
        argv[count++] = "/busybox"; argv[count++] = "script";
        if (options & 1) argv[count++] = "-q";
        if (options & 2) argv[count++] = "-a";
        if (options & 4) argv[count++] = "-f";
        if (options & 8) argv[count++] = timing;
        if (options & 16) argv[count++] = "-t";
        if (command_text) { argv[count++] = "-c"; argv[count++] = (char *)command_text; }
        if (output) argv[count++] = (char *)output;
        argv[count] = NULL; execv(argv[0], argv); perror("exec original script"); _exit(127);
    }
    close(input_tty.slave); input_tty.slave = -1;
    pid_t shell_pid = 0;
    if (options & 32) shell_pid = script_job_scene(input_tty.master, output);
    if (input) {
        /* 原script以TCSAFLUSH切换stdin；等原shell真正发布prompt后才输入。 */
        const char *typescript = output ? output : "/tmp/typescript";
        uint64_t prompt_deadline = now() + 30000000000ULL;
        for (;;) {
            int fd = open(typescript, O_RDONLY);
            if (fd >= 0) {
                char text[256]; ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
                truth(n >= 0, "default script prompt read"); text[n] = 0;
                if (strstr(text, "PTY_SCRIPT$ ")) break;
            } else exact(errno, ENOENT, "default script prompt file");
            int status; pid_t stopped = waitpid(script, &status, WNOHANG);
            exact(stopped, 0, "script remains live until default prompt");
            truth(now() < prompt_deadline, "default script prompt deadline"); usleep(1000);
        }
        write_all(input_tty.master, input, strlen(input));
    }
    unsigned completed = 0, released = !visible_size; int command_status = -1, script_status = -1;
    uint64_t deadline = now() + 120000000000ULL;
    for (;;) {
        truth(now() < deadline, "script and adopted child deadline");
        if (!released) {
            struct stat st;
            if (stat(output, &st) == 0 && (uint64_t)st.st_size == visible_size) {
                int fd = open("/tmp/script.release", O_CREAT | O_WRONLY | O_TRUNC, 0600);
                truth(fd >= 0, "release captured script child"); close(fd); released = 1;
            }
        }
        int status; pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            if (child == script) script_status = status;
            else if (!(options & 32) || child == shell_pid) {
                truth(command_status == -1, "exactly one adopted script command"); command_status = status;
            } else adopted_job(child, status);
            completed++;
        } else {
            truth(child == 0 || errno == ECHILD, "script reaping errno");
            if (child < 0 && errno == ECHILD && script_status >= 0 && command_status >= 0) break;
            usleep(1000);
        }
    }
    truth(completed >= 2, "script and real command both reaped");
    exact(script_status, 0, "original script returns zero");
    if (wanted_status >= 0) {
        truth(WIFEXITED(command_status), "adopted command exited");
        exact(WEXITSTATUS(command_status), wanted_status, "adopted actual command exit");
    } else {
        truth(WIFSIGNALED(command_status), "adopted command signaled");
        exact(WTERMSIG(command_status), -wanted_status, "adopted actual command signal");
    }
    truth(released, "script -f capture visible before child exit");
    pair_close(&input_tty); unlink("/tmp/script.release");
    char key[96]; snprintf(key, sizeof(key), "%s-script-exit", name); record(key, 0);
    snprintf(key, sizeof(key), "%s-adopted-command-status", name); record(key, wanted_status);
}
static void replay(const char *timing, const char *transcript, const char *output)
{
    pid_t child = fork(); truth(child >= 0, "original replay fork");
    if (!child) {
        int fd = open(output, O_CREAT | O_WRONLY | O_TRUNC, 0600); truth(fd >= 0, "replay output");
        exact(dup2(fd, 1), 1, "replay stdout"); if (fd > 2) close(fd);
        execl("/busybox", "busybox", "scriptreplay", timing, transcript, "1000000", (char *)NULL);
        perror("exec original replay"); _exit(127);
    }
    int status; exact(waitpid(child, &status, 0), child, "replay reap"); exact(status, 0, "original replay exit");
}
static void check_timing(const char *path, uint64_t size)
{
    FILE *file = fopen(path, "r"); truth(file != NULL, "original timing file");
    uint64_t total = 0; unsigned lines = 0; double delay; unsigned long count;
    while (fscanf(file, "%lf %lu", &delay, &count) == 2) {
        truth(isfinite(delay) && delay >= 0 && count > 0 && count <= size - total,
              "original finite timing record"); total += count; lines++;
    }
    truth(feof(file) && lines > 0, "complete timing parse"); fclose(file);
    exact((long)total, (long)size, "timing bytes equal typescript");
}
static void scripts(void)
{
    exact(chdir("/tmp"), 0, "script working directory");
    script_run("script-default", NULL, "printf 'PTY_DEFAULT_OK\\n'; exit 7\n", NULL, 0, 7, 0);
    truth(file_contains("/tmp/typescript", "PTY_DEFAULT_OK"), "default original shell capture");
    truth(file_contains("/tmp/script-default.stdout", "Script started, file is typescript") &&
          file_contains("/tmp/script-default.stdout", "Script done, file is typescript"), "default notices");
    script_run("script-command", "/tmp/command.typescript", NULL, "exec /pty-probe --emit 257 7 0", 0, 7, 0);
    check_payload_file("/tmp/command.typescript", 0, 257, 1);
    script_run("script-quiet", "/tmp/append.typescript", NULL, "exec /pty-probe --emit 127 7 0", 1, 7, 0);
    check_payload_file("/tmp/script-quiet.stdout", 0, 127, 1);
    script_run("script-append", "/tmp/append.typescript", NULL, "exec /pty-probe --emit 131 7 0", 1 | 2, 7, 0);
    check_payload_file("/tmp/append.typescript", 0, 127, 0);
    check_payload_file("/tmp/append.typescript", 127, 131, 1);
    script_run("script-timing-stderr", "/tmp/stderr.typescript", NULL,
               "exec /pty-probe --emit 4096 7 1", 1 | 16, 7, 4096);
    check_payload_file("/tmp/stderr.typescript", 0, 4096, 1);
    check_timing("/tmp/script-timing-stderr.stderr", 4096);
    script_run("script-immediate-tail", "/tmp/immediate.typescript", NULL,
               "exec /pty-probe --emit 1048576 7 0", 1, 7, 0);
    check_payload_file("/tmp/immediate.typescript", 0, 1048576, 1);
    check_payload_file("/tmp/script-immediate-tail.stdout", 0, 1048576, 1);
    script_run("script-megabyte", "/tmp/megabyte.typescript", NULL,
               "exec /pty-probe --emit 1048576 7 1", 1 | 4 | 8, 7, 1048576);
    check_payload_file("/tmp/megabyte.typescript", 0, 1048576, 1);
    check_payload_file("/tmp/script-megabyte.stdout", 0, 1048576, 1);
    check_timing("/tmp/script-megabyte.timing", 1048576);
    replay("/tmp/script-megabyte.timing", "/tmp/megabyte.typescript", "/tmp/megabyte.replay");
    check_payload_file("/tmp/megabyte.replay", 0, 1048576, 1);
    script_run("script-signaled", "/tmp/signaled.typescript", NULL,
               "exec /pty-probe --signal-child", 1, -SIGTERM, 0);
    script_run("script-jobs", "/tmp/jobs.typescript", NULL, NULL, 1 | 32, 7, 0);
    record("original-script-ash-stty-jobcontrol-cleanup", 1);
    record("original-script-default-command-quiet-append-timing-flush", 1);
    record("original-scriptreplay-exact-megabyte-tail", 1048576);
    const char *recordings[] = {"/tmp/megabyte.typescript", "/tmp/immediate.typescript",
                               "/tmp/megabyte.replay", "/tmp/append.typescript"};
    for (unsigned i = 0; i < sizeof(recordings) / sizeof(recordings[0]); i++) {
        int fd = open(recordings[i], O_RDONLY); truth(fd >= 0, "durable recording open");
        exact(fsync(fd), 0, "recording fsync"); exact(close(fd), 0, "recording fsync close");
    }
    int directory = open("/tmp", O_RDONLY | O_DIRECTORY); truth(directory >= 0, "recording directory");
    exact(fsync(directory), 0, "recording namespace fsync"); exact(close(directory), 0, "recording directory close");
    int selected = open("/pty-case", O_WRONLY | O_TRUNC); truth(selected >= 0, "recording reboot selector");
    static const char next[] = "recording-check 1 0\n";
    write_all(selected, next, sizeof(next) - 1);
    exact(fsync(selected), 0, "reboot selector fsync"); exact(close(selected), 0, "reboot selector close");
}
static void recording_check(void)
{
    check_payload_file("/tmp/megabyte.typescript", 0, 1048576, 1);
    check_payload_file("/tmp/immediate.typescript", 0, 1048576, 1);
    check_payload_file("/tmp/megabyte.replay", 0, 1048576, 1);
    check_payload_file("/tmp/append.typescript", 0, 127, 0);
    check_payload_file("/tmp/append.typescript", 127, 131, 1);
    record("recordings-after-real-reboot-exact-content-tail", 1048576);
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--api")) {
        api_record = "real-glibc-openpty-login_tty"; libc_api(); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--emit")) {
        truth(argc == 5, "script emitter parameters");
        script_payload(strtoull(argv[2], NULL, 10), atoi(argv[3]), atoi(argv[4]));
    }
    if (argc > 1 && !strcmp(argv[1], "--signal-child")) {
        exact(kill(getpid(), SIGTERM), 0, "script command signal"); _exit(111);
    }
    if (argc == 3 && !strcmp(argv[1], "--wait-stopped")) {
        long child = strtol(argv[2], NULL, 10); truth(child > 1 && child <= INT32_MAX, "reader pid");
        stopped_reader((pid_t)child); return 0;
    }
    setup();
    char selected[32] = "core"; unsigned pairs = 1; unsigned long long bytes = 4U * 1024U * 1024U;
    FILE *config = fopen("/pty-case", "r");
    if (config) { truth(fscanf(config, "%31s %u %llu", selected, &pairs, &bytes) >= 1, "case config"); fclose(config); }
    if (!strcmp(selected, "red")) red();
    else if (!strcmp(selected, "core")) { locks_and_nodes(); transfers(); packets(); faults(); lifecycle(); controlling(0); controlling(1); termios2_cases(); }
    else if (!strcmp(selected, "libc")) libc_both();
    else if (!strcmp(selected, "script")) scripts();
    else if (!strcmp(selected, "recording-check")) recording_check();
    else if (!strcmp(selected, "performance")) performance(pairs, bytes);
    else fail("unknown case", 0, 1);
    if (!strcmp(selected, "performance")) {
        uint64_t durable_start = now(); sync();
        dprintf(1, "PTY_DURABLE scope=sync elapsed_ns=%llu\n", (unsigned long long)(now() - durable_start));
    } else sync();
    dprintf(1, "PTY_PROBE_PASS records=%u\n", records);
    /* 软件console异步发送；先排空，不能与停机raw诊断拼接一行。 */
    exact(tcdrain(STDOUT_FILENO), 0, "final observation console drain");
    exact(klogctl(7, NULL, 0), 0, "restore console loglevel");
    if (!strcmp(selected, "script") || !strcmp(selected, "recording-check"))
        exact(klogctl(8, NULL, 7), 0, "publish actual init exit status");
    return 42;
}
