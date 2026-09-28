#include "abi.h"

static void proc_child_path(char *path, long pid)
{
    const char prefix[] = "/proc-probe/";
    unsigned i = 0;
    while (i < sizeof(prefix) - 1U) { path[i] = prefix[i]; i++; }
    char reverse[24];
    unsigned used = 0;
    do { reverse[used++] = '0' + pid % 10; pid /= 10; } while (pid);
    while (used) path[i++] = reverse[--used];
    path[i] = '\0';
}

static void proc_fd_path(char *path, long fd)
{
    const char prefix[] = "/proc-probe/self/fd/";
    unsigned i = 0;
    while (i < sizeof(prefix) - 1U) { path[i] = prefix[i]; i++; }
    char reverse[24];
    unsigned used = 0;
    do { reverse[used++] = '0' + fd % 10; fd /= 10; } while (fd);
    while (used) path[i++] = reverse[--used];
    path[i] = '\0';
}

static int proc_link_number(const char *text, long length,
                            const char *prefix, unsigned prefix_length)
{
    if (length <= (long)prefix_length + 1 ||
        text[length - 1] != ']') return 0;
    for (unsigned i = 0; i < prefix_length; i++)
        if (text[i] != prefix[i]) return 0;
    for (long i = prefix_length; i < length - 1; i++)
        if (text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

static int proc_directory_has(long fd, const char *wanted)
{
    if (fd < 0) return 0;
    char entries[4096];
    long got = SC3(61, fd, entries, sizeof(entries));
    for (long at = 0; got > 0 && at + 19 < got;) {
        unsigned short reclen = (unsigned char)entries[at + 16] |
            ((unsigned short)(unsigned char)entries[at + 17] << 8);
        if (reclen < 20 || at + reclen > got) break;
        long i = 0;
        while (at + 19 + i < at + reclen &&
               entries[at + 19 + i] && wanted[i]) {
            if (entries[at + 19 + i] != wanted[i]) break;
            i++;
        }
        if (at + 19 + i < at + reclen &&
            !wanted[i] && !entries[at + 19 + i]) return 1;
        at += reclen;
    }
    return 0;
}

static long proc_directory_finish(long fd)
{
    char entries[4096];
    if (fd < 0) return -9;
    for (unsigned attempt = 0; attempt < 1024U; attempt++) {
        long got = SC3(61, fd, entries, sizeof(entries));
        if (got <= 0) return got;
    }
    return -1;
}

/* Missing mount dispatch must not masquerade as a procfs consumer failure. */
void abi_proc_cases(void)
{
    abi_require(SC3(34, -100, "/proc-probe", 0755) == 0);
    long mounted = SC5(40, "proc", "/proc-probe", "proc", 0, 0);
    abi_record("proc.mount", mounted, -1, -1, 0, 0, 0);
    long root_directory = abi_open("/proc-probe", 0);
    abi_record("proc.root-list",
               proc_directory_has(root_directory, "meminfo") ? 0 : -1,
               -1, -1, 0, 0, 0);
    abi_record("proc.root-eof", proc_directory_finish(root_directory),
               -1, -1, 0, 0, 0);
    if (root_directory >= 0) abi_require(SC1(57, root_directory) == 0);
    long fd = abi_open("/proc-probe/meminfo", 0);
    abi_record("proc.meminfo-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    long writable = abi_open("/proc-probe/meminfo", 1);
    abi_record("proc.meminfo-write-open", writable < 0 ? writable : 0,
               -1, -1, 0, 0, 0);
    abi_record("proc.meminfo-write",
               writable >= 0 ? SC3(64, writable, "x", 1) : -9,
               -1, -1, 0, 0, 0);
    if (writable >= 0) abi_require(SC1(57, writable) == 0);
    long uptime_fd = abi_open("/proc-probe/uptime", 0);
    abi_record("proc.uptime-open", uptime_fd < 0 ? uptime_fd : 0,
               -1, -1, 0, 0, 0);
    if (uptime_fd >= 0) {
        char uptime[80] = {0};
        long length = SC3(63, uptime_fd, uptime, sizeof(uptime));
        int shape = length >= 10 && length < (long)sizeof(uptime);
        long dot_count = 0, space_count = 0;
        for (long i = 0; i < length && shape; i++) {
            char c = uptime[i];
            if (c == '.') dot_count++;
            else if (c == ' ') space_count++;
            else if (c != '\n' && (c < '0' || c > '9')) shape = 0;
        }
        shape = shape && dot_count == 2 && space_count == 1 &&
                uptime[length - 1] == '\n';
        abi_record("proc.uptime-shape", shape ? 0 : -1,
                   -1, -1, 0, 0, 0);
        abi_require(SC1(57, uptime_fd) == 0);
    }
    char self_link[16] = {0};
    long self_length = SC4(78, -100, "/proc-probe/self",
                           self_link, sizeof(self_link));
    abi_record("proc.self-link", self_length, -1, -1, 0,
               self_link, self_length > 0 ? (usize)self_length : 0);
    long self_directory = abi_open("/proc-probe/self", 0);
    abi_record("proc.self-open", self_directory < 0 ? self_directory : 0,
               -1, -1, 0, 0, 0);
    if (self_directory >= 0) abi_require(SC1(57, self_directory) == 0);
    char object_link[40] = {0};
    long object_length = SC4(78, -100, "/proc-probe/self/exe",
                              object_link, sizeof(object_link));
    abi_record("proc.exe-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    long executable = abi_open("/proc-probe/self/exe", 0);
    abi_record("proc.exe-open", executable < 0 ? executable : 0,
               -1, -1, 0, 0, 0);
    if (executable >= 0) {
        unsigned char elf[4] = {0};
        abi_record("proc.exe-magic", SC3(63, executable, elf, sizeof(elf)),
                   -1, -1, 0, elf, sizeof(elf));
        abi_require(SC1(57, executable) == 0);
    }
    object_length = SC4(78, -100, "/proc-probe/self/cwd",
                        object_link, sizeof(object_link));
    abi_record("proc.cwd-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    object_length = SC4(78, -100, "/proc-probe/self/root",
                        object_link, sizeof(object_link));
    abi_record("proc.root-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    char mounts_link[32] = {0};
    long mounts_length = SC4(78, -100, "/proc-probe/mounts",
                             mounts_link, sizeof(mounts_link));
    abi_record("proc.mounts-link", mounts_length, -1, -1, 0,
               mounts_link, mounts_length > 0 ? (usize)mounts_length : 0);
    long mounts_fd = abi_open("/proc-probe/self/mounts", 0);
    abi_record("proc.mounts-open", mounts_fd < 0 ? mounts_fd : 0,
               -1, -1, 0, 0, 0);
    int has_proc = 0;
    if (mounts_fd >= 0) {
        char mounts[4096];
        long got = SC3(63, mounts_fd, mounts, sizeof(mounts) - 1U);
        if (got > 0 && got < (long)sizeof(mounts)) {
            mounts[got] = '\0';
            const char *needle = " /proc-probe proc ";
            for (long i = 0; i + 18 <= got; i++) {
                int equal = 1;
                for (long j = 0; j < 18; j++)
                    if (mounts[i + j] != needle[j]) equal = 0;
                if (equal) {
                    has_proc = 1;
                    break;
                }
            }
        }
        abi_require(SC1(57, mounts_fd) == 0);
    }
    abi_record("proc.mounts-has-proc", has_proc ? 0 : -1,
               -1, -1, 0, 0, 0);
    long stat_fd = abi_open("/proc-probe/self/stat", 0);
    abi_record("proc.stat-open", stat_fd < 0 ? stat_fd : 0,
               -1, -1, 0, 0, 0);
    int stat_shape = 0;
    if (stat_fd >= 0) {
        char record[1024];
        long got = SC3(63, stat_fd, record, sizeof(record));
        if (got > 0 && got < (long)sizeof(record) && record[got - 1] == '\n') {
            long close = -1;
            for (long i = 0; i < got; i++) if (record[i] == ')') close = i;
            int fields = 2;
            if (close > 0 && record[close + 1] == ' ') {
                for (long i = close + 2; i < got; i++)
                    if (record[i] != ' ' && record[i] != '\n' &&
                        record[i - 1] == ' ') fields++;
                stat_shape = fields >= 24;
            }
        }
        abi_require(SC1(57, stat_fd) == 0);
    }
    abi_record("proc.stat-fields", stat_shape ? 0 : -1,
               -1, -1, 0, 0, 0);
    long status_fd = abi_open("/proc-probe/self/status", 0);
    abi_record("proc.status-open", status_fd < 0 ? status_fd : 0,
               -1, -1, 0, 0, 0);
    int status_shape = 0;
    if (status_fd >= 0) {
        char record[2048];
        long got = SC3(63, status_fd, record, sizeof(record));
        int name = 0, state = 0, pid = 0, rss = 0;
        for (long i = 0; i + 7 < got; i++) {
            if (record[i] == 'N' && record[i + 1] == 'a' &&
                record[i + 2] == 'm' && record[i + 3] == 'e' &&
                record[i + 4] == ':') name = 1;
            if (record[i] == 'S' && record[i + 1] == 't' &&
                record[i + 2] == 'a' && record[i + 3] == 't' &&
                record[i + 4] == 'e' && record[i + 5] == ':') state = 1;
            if (record[i] == 'P' && record[i + 1] == 'i' &&
                record[i + 2] == 'd' && record[i + 3] == ':') pid = 1;
            if (record[i] == 'V' && record[i + 1] == 'm' &&
                record[i + 2] == 'R' && record[i + 3] == 'S' &&
                record[i + 4] == 'S' && record[i + 5] == ':') rss = 1;
        }
        status_shape = name && state && pid && rss;
        abi_require(SC1(57, status_fd) == 0);
    }
    abi_record("proc.status-fields", status_shape ? 0 : -1,
               -1, -1, 0, 0, 0);
    long fd_directory = abi_open("/proc-probe/self/fd", 0);
    abi_record("proc.fd-dir", fd_directory < 0 ? fd_directory : 0,
               -1, -1, 0, 0, 0);
    long object_fd = SC4(56, -100, "/proc-fd-target", 0102, 0644);
    abi_require(object_fd >= 0);
    abi_require(SC3(64, object_fd, "abcdef", 6) == 6);
    abi_require(SC3(62, object_fd, 3, 0) == 3);
    char fd_path[48];
    proc_fd_path(fd_path, object_fd);
    const char *wanted_fd = fd_path + sizeof("/proc-probe/self/fd/") - 1;
    int listed = proc_directory_has(fd_directory, wanted_fd);
    abi_record("proc.fd-list-target", listed ? 0 : -1,
               -1, -1, 0, 0, 0);
    abi_record("proc.fd-eof", proc_directory_finish(fd_directory),
               -1, -1, 0, 0, 0);
    if (fd_directory >= 0) abi_require(SC1(57, fd_directory) == 0);
    char fd_link[64] = {0};
    long fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-link", fd_link_size, -1, -1, 0,
               fd_link, fd_link_size > 0 ? (usize)fd_link_size : 0);
    struct abi_stat fd_stat;
    long fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-link-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    long reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    char first = 0;
    abi_record("proc.fd-reopen-read",
               reopened >= 0 ? SC3(63, reopened, &first, 1) : -9,
               -1, -1, 0, &first, 1);
    abi_record("proc.fd-original-offset", abi_offset(object_fd),
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, object_fd) == 0);
    abi_record("proc.fd-closed-link",
               SC4(78, -100, fd_path, fd_link, sizeof(fd_link)),
               -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/proc-fd-target", 0) == 0);
    long reused = SC4(56, -100, "/proc-fd-second", 0102, 0644);
    abi_require(reused >= 0);
    proc_fd_path(fd_path, reused);
    long reused_length = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-reused-link", reused_length, -1, -1, 0,
               fd_link, reused_length > 0 ? (usize)reused_length : 0);
    abi_require(SC1(57, reused) == 0);
    abi_require(SC3(35, -100, "/proc-fd-second", 0) == 0);
    int pipe_fds[2] = {-1, -1};
    abi_require(SC2(59, pipe_fds, 0) == 0);
    proc_fd_path(fd_path, pipe_fds[0]);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-pipe-link",
               proc_link_number(fd_link, fd_link_size, "pipe:[", 6U)
                    ? 0 : -1, -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-pipe-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-pipe-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC3(64, pipe_fds[1], "P", 1) == 1);
    char pipe_byte = 0;
    abi_record("proc.fd-pipe-read",
               reopened >= 0 ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    long both = abi_open(fd_path, 2);
    abi_record("proc.fd-pipe-rdwr", both < 0 ? both : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, pipe_fds[0]) == 0);
    if (reopened >= 0) abi_require(SC3(64, pipe_fds[1], "Q", 1) == 1);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-after-close",
               reopened >= 0 ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    abi_record("proc.fd-pipe-rdwr-write",
               both >= 0 ? SC3(64, both, "R", 1) : -9,
               -1, -1, 0, 0, 0);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-rdwr-read",
               both >= 0 ? SC3(63, both, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    if (both >= 0) abi_require(SC1(57, both) == 0);
    proc_fd_path(fd_path, pipe_fds[1]);
    long reopened_writer = abi_open(fd_path, 1);
    abi_record("proc.fd-pipe-wr-reopen",
               reopened_writer < 0 ? reopened_writer : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, pipe_fds[1]) == 0);
    abi_record("proc.fd-pipe-wr-after-close",
               reopened_writer >= 0 ? SC3(64, reopened_writer, "T", 1) : -9,
               -1, -1, 0, 0, 0);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-wr-delivered",
               reopened_writer >= 0 && reopened >= 0
                   ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    if (reopened_writer >= 0)
        abi_require(SC1(57, reopened_writer) == 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    long socket_fd = SC3(198, 2, 2, 0);
    abi_require(socket_fd >= 0);
    proc_fd_path(fd_path, socket_fd);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-socket-link",
               proc_link_number(fd_link, fd_link_size, "socket:[", 8U)
                    ? 0 : -1, -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-socket-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-socket-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, socket_fd) == 0);
    long epoll_fd = SC1(20, 0);
    abi_require(epoll_fd >= 0);
    proc_fd_path(fd_path, epoll_fd);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    const char epoll_link[] = "anon_inode:[eventpoll]";
    int epoll_match = fd_link_size == (long)sizeof(epoll_link) - 1;
    for (unsigned i = 0; epoll_match && i < sizeof(epoll_link) - 1; i++)
        if (fd_link[i] != epoll_link[i]) epoll_match = 0;
    abi_record("proc.fd-epoll-link", epoll_match ? 0 : -1,
               -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-epoll-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-epoll-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, epoll_fd) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) abi_exit(0);
    char child_path[48];
    proc_child_path(child_path, child);
    long child_dir = abi_open(child_path, 0);
    abi_require(child_dir >= 0);
    int zombie = 0;
    for (unsigned attempt = 0; attempt < 10000U; attempt++) {
        long child_stat = SC4(56, child_dir, "stat", 0, 0);
        if (child_stat >= 0) {
            char text[256];
            long got = SC3(63, child_stat, text, sizeof(text));
            for (long i = 0; i + 3 < got; i++)
                if (text[i] == ')' && text[i + 1] == ' ' &&
                    text[i + 2] == 'Z') zombie = 1;
            abi_require(SC1(57, child_stat) == 0);
        }
        if (zombie) break;
        SC0(124);
    }
    abi_record("proc.zombie-stat", zombie ? 0 : -1,
               -1, -1, 0, 0, 0);
    int child_status = 0;
    abi_require(SC4(260, child, &child_status, 0, 0) == child);
    long old_stat = SC4(56, child_dir, "stat", 0, 0);
    abi_record("proc.reaped-old-stat", old_stat < 0 ? old_stat : 0,
               -1, -1, 0, 0, 0);
    if (old_stat >= 0) abi_require(SC1(57, old_stat) == 0);
    abi_require(SC1(57, child_dir) == 0);
    if (fd >= 0) {
        char header[9] = {0};
        abi_record("proc.meminfo-read", SC3(63, fd, header, sizeof(header)),
                   -1, -1, 0, header, sizeof(header));
        abi_record("proc.meminfo-rewind", SC3(62, fd, 0, 0),
                   -1, -1, 0, 0, 0);
        char first[4] = {0}, second[5] = {0};
        struct abi_iovec iov[2] = {{first, sizeof(first)},
                                   {second, sizeof(second)}};
        abi_record("proc.meminfo-readv", SC3(65, fd, iov, 2),
                   -1, -1, 0, 0, 0);
        abi_record("proc.meminfo-readv-first", 0, -1, -1, 0,
                   first, sizeof(first));
        abi_record("proc.meminfo-readv-second", 0, -1, -1, 0,
                   second, sizeof(second));
        char positioned[9] = {0};
        abi_record("proc.meminfo-pread", SC4(67, fd, positioned,
                   sizeof(positioned), 0), -1, abi_offset(fd), 0,
                   positioned, sizeof(positioned));
        abi_record("proc.meminfo-seek-end", SC3(62, fd, 0, 2),
                   -1, -1, 0, 0, 0);
        abi_require(SC3(62, fd, 1048576, 0) == 1048576);
        abi_record("proc.meminfo-eof", SC3(63, fd, header, sizeof(header)),
                   -1, -1, 0, 0, 0);
        abi_record("proc.busy-unmount", SC2(39, "/proc-probe", 0),
                   -1, -1, 0, 0, 0);
        abi_require(SC1(57, fd) == 0);
    }
    abi_record("proc.unmount", SC2(39, "/proc-probe", 0), -1, -1, 0, 0, 0);
    abi_record("proc.readonly-silent-mount",
               SC5(40, "proc", "/proc-probe", "proc", 32769, 0),
               -1, -1, 0, 0, 0);
    abi_record("proc.stack-mount",
               SC5(40, "proc", "/proc-probe", "proc", 0, 0),
               -1, -1, 0, 0, 0);
    abi_record("proc.stack-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC1(49, "/proc-probe") == 0);
    char cwd[32] = {0};
    abi_record("proc.cwd", SC2(17, cwd, sizeof(cwd)),
               -1, -1, 0, cwd, 12);
    abi_record("proc.cwd-busy-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC1(49, "/") == 0);
    abi_record("proc.readonly-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/proc-probe", 512) == 0);
}
