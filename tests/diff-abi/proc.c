#include "abi.h"

/* Missing mount dispatch must not masquerade as a procfs consumer failure. */
void abi_proc_cases(void)
{
    abi_require(SC3(34, -100, "/proc-probe", 0755) == 0);
    long mounted = SC5(40, "proc", "/proc-probe", "proc", 0, 0);
    abi_record("proc.mount", mounted, -1, -1, 0, 0, 0);
    long fd = abi_open("/proc-probe/meminfo", 0);
    abi_record("proc.meminfo-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    long writable = abi_open("/proc-probe/meminfo", 1);
    abi_record("proc.meminfo-write-open", writable < 0 ? writable : 0,
               -1, -1, 0, 0, 0);
    abi_record("proc.meminfo-write",
               writable >= 0 ? SC3(64, writable, "x", 1) : -9,
               -1, -1, 0, 0, 0);
    if (writable >= 0) abi_require(SC1(57, writable) == 0);
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
