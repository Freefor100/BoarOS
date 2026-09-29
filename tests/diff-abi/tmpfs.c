#include "abi.h"

/* Fixed reference: references/linux/mm/shmem.c, commit
 * f4cdf7ca9a1fdcca413157df19753f388a5a224e: SGP_READ leaves holes
 * unallocated; shmem_inode_acct_blocks accounts instantiated pages;
 * shmem_statfs exposes per-mount quota counters. Only local deltas and
 * semantic predicates are compared, never machine-wide capacity or fsid. */

static void tmpfs_extended(void);
static void tmpfs_faults(void);
static void tmpfs_mapping_times(void);

void abi_tmpfs_cases(void)
{
    abi_require(SC3(34, -100, "/tmpfs-probe", 0755) == 0);
    long mounted = SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "size=8192,nr_inodes=8");
    abi_record("tmpfs.mount", mounted, -1, -1, 0, 0, 0);
    if (mounted) return;
    long fd = abi_open("/tmpfs-probe/data", 2 | 64);
    abi_require(fd >= 0 && SC2(46, fd, 16384) == 0);
    char byte = 1;
    abi_require(SC4(67, fd, &byte, 1, 12288) == 1 && byte == 0);
    abi_require(SC4(68, fd, "A", 1, 0) == 1 && SC4(68, fd, "B", 1, 4096) == 1);
    abi_record("tmpfs.quota", SC4(68, fd, "C", 1, 8192), -1, -1, 0, 0, 0);
    long mapping = SC6(222, 0, 4096, 3, 1, fd, 0);
    abi_require(mapping > 0);
    *(volatile char *)mapping = 'Z';
    abi_require(SC4(67, fd, &byte, 1, 0) == 1 && byte == 'Z');
    abi_record("tmpfs.busy", SC2(39, "/tmpfs-probe", 0), -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/tmpfs-probe/data", 0) == 0);
    abi_require(SC1(57, fd) == 0 && *(volatile char *)mapping == 'Z');
    abi_require(SC2(215, mapping, 4096) == 0);
    abi_record("tmpfs.unmount", SC2(39, "/tmpfs-probe", 0), -1, -1, 0, 0, 0);
    tmpfs_extended();
    tmpfs_faults();
    tmpfs_mapping_times();
}

static void tmpfs_mapping_times(void)
{
    /* Linux mm/memory.c fault_dirty_shared_page and mm/mprotect.c at
     * f4cdf7ca9a1fdcca413157df19753f388a5a224e: a direct shared WRITE
     * fault updates times. tmpfs read faults may publish writable PTEs;
     * neither a later store nor private COW must force another notification. */
    static const char *const names[] = {"tmpfs.mmap-times.shared-read-first",
        "tmpfs.mmap-times.shared-write-first", "tmpfs.mmap-times.private-write",
        "tmpfs.mmap-times.mprotect-read-first", "tmpfs.mmap-times.mprotect-write-first",
        "tmpfs.mmap-times.none-restore"};
    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "size=4096") == 0);
    long fd = abi_open("/tmpfs-probe/times", 2 | 64);
    abi_require(fd >= 0 && SC2(46, fd, 4096) == 0);
    for (unsigned mode = 0; mode < 6; mode++) {
        long address = SC6(222, 0, 4096, mode == 3 ? 1 : 3, mode == 2 ? 2 : 1, fd, 0);
        abi_require(address > 0);
        volatile unsigned char *page = (void *)address;
        if (mode == 4) *page = 'W';
        long times[4] = {123, 42, 123, 42};
        abi_require(SC4(88, fd, 0, times, 0) == 0);
        struct abi_stat before, after;
        if (mode != 1 && mode != 4) { volatile unsigned char read = *page; (void)read; }
        abi_require(SC2(80, fd, &before) == 0);
        if (mode >= 3) {
            if (mode >= 4) abi_require(SC3(226, address, 4096, mode == 5 ? 0 : 1) == 0);
            abi_require(SC3(226, address, 4096, 3) == 0);
        }
        *page = 'M';
        abi_require(SC2(80, fd, &after) == 0);
        long values[] = {before.mtime == 123 && before.mtime_nsec == 42,
                        after.mtime != 123 || after.mtime_nsec != 42};
        abi_record(names[mode], 0, -1, -1, 0, values, sizeof(values));
        abi_require(SC2(215, address, 4096) == 0);
    }
    abi_require(SC1(57, fd) == 0 && SC2(39, "/tmpfs-probe", 0) == 0);
}

static void tmpfs_result(const char *name, long result)
{
    abi_record(name, result, -1, -1, 0, 0, 0);
}

struct tmpfs_stats {
    long type, bsize, blocks, bfree, bavail, files, ffree;
    int fsid[2];
    long namelen, frsize, flags, spare[4];
};

static void tmpfs_extended(void)
{
    struct tmpfs_stats before, sparse, allocated, released;
    struct abi_stat st;
    char byte;
    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 0,
                    "size=8192,nr_inodes=16") == 0);
    tmpfs_result("tmpfs.mknod-regular", SC4(33, -100,
                 "/tmpfs-probe/mknod", 0100600, 0));
    abi_require(SC3(35, -100, "/tmpfs-probe/mknod", 0) == 0);
    long fd = abi_open("/tmpfs-probe/sparse", 2 | 64);
    abi_require(fd >= 0 && SC2(44, fd, &before) == 0);
    long directory = abi_open("/tmpfs-probe", 65536);
    abi_require(directory >= 0);
    tmpfs_result("tmpfs.directory-fsync", SC1(82, directory));
    abi_require(SC1(57, directory) == 0);
    long invalid_times[4] = {0, 1000000000, 0, 0};
    tmpfs_result("tmpfs.utime-invalid-nsec", SC4(88, fd, 0, invalid_times, 0));
    long now_before[2], now_after[2];
    abi_require(SC2(113, 0, now_before) == 0);
    tmpfs_result("tmpfs.utime-null", SC4(88, fd, 0, 0, 0));
    abi_require(SC2(113, 0, now_after) == 0 && SC2(80, fd, &st) == 0);
    long times[] = {st.atime >= now_before[0] && st.atime <= now_after[0],
        st.mtime >= now_before[0] && st.mtime <= now_after[0],
        st.atime_nsec < 1000000000UL, st.mtime_nsec < 1000000000UL};
    abi_record("tmpfs.utime-now-range", 0, -1, -1, 0, times, sizeof(times));
    abi_require(SC2(46, fd, 65536) == 0);
    char hole[2] = {1, 1};
    unsigned long vectors[4] = {(unsigned long)&hole[0], 1,
                               (unsigned long)&hole[1], 1};
    abi_require(SC3(63, fd, &byte, 1) == 1 && byte == 0 &&
                SC3(65, fd, vectors, 2) == 2 && !hole[0] && !hole[1] &&
                SC2(44, fd, &sparse) == 0);
    abi_record("tmpfs.read-holes-unallocated", sparse.bfree == before.bfree,
               -1, -1, 0, 0, 0);
    byte = 1;
    abi_require(SC4(67, fd, &byte, 1, 61440) == 1 && byte == 0 &&
                SC2(44, fd, &sparse) == 0 && SC2(80, fd, &st) == 0);
    long geometry[] = {before.type == 0x01021994, before.bsize == 4096,
        before.namelen == 255, before.bfree == before.bavail,
        sparse.bfree == before.bfree, st.blocks == 0};
    abi_record("tmpfs.sparse-statfs", 0, -1, -1, 0, geometry, sizeof(geometry));
    abi_require(SC4(68, fd, "X", 1, 0) == 1 &&
                SC4(68, fd, "Y", 1, 4096) == 1 && SC2(44, fd, &allocated) == 0);
    abi_require(SC2(46, fd, 4096) == 0 && SC2(44, fd, &released) == 0);
    long deltas[] = {before.bfree - allocated.bfree,
                    released.bfree - allocated.bfree};
    abi_record("tmpfs.truncate-quota-release", 0, -1, -1, 0, deltas, sizeof(deltas));
    abi_require(SC4(68, fd, "T", 1, 8192) == 1);
    tmpfs_result("tmpfs.reuse-quota", SC4(68, fd, "F", 1, 12288));
    abi_require(SC2(46, fd, 4096) == 0);
    long shared = SC6(222, 0, 4096, 3, 1, fd, 0);
    long private = SC6(222, 0, 4096, 3, 2, fd, 0);
    abi_require(shared > 0 && private > 0);
    ((volatile char *)private)[0] = 'P';
    ((volatile char *)private)[100] = 'Q';
    ((volatile char *)shared)[100] = 'S';
    abi_require(SC2(46, fd, 32) == 0);
    unsigned char content[] = {((volatile char *)shared)[0],
        ((volatile char *)shared)[100], ((volatile char *)private)[0],
        ((volatile char *)private)[100]};
    abi_record("tmpfs.private-cow-tail", 0, -1, -1, 0, content, sizeof(content));
    abi_require(SC2(46, fd, 4096) == 0 &&
                SC4(67, fd, &byte, 1, 100) == 1);
    abi_record("tmpfs.truncate-regrow-zero", 0, -1, -1, 0, &byte, 1);
    abi_require(SC2(215, shared, 4096) == 0 && SC2(215, private, 4096) == 0 &&
                SC1(57, fd) == 0 && SC2(39, "/tmpfs-probe", 0) == 0);

    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 0,
                    "size=8192,nr_inodes=3") == 0);
    fd = abi_open("/tmpfs-probe/one", 2 | 64);
    long second = abi_open("/tmpfs-probe/two", 2 | 64);
    abi_require(fd >= 0 && second >= 0);
    tmpfs_result("tmpfs.inode-quota", abi_open("/tmpfs-probe/three", 2 | 64));
    abi_require(SC3(35, -100, "/tmpfs-probe/one", 0) == 0);
    tmpfs_result("tmpfs.unlinked-inode-held", abi_open("/tmpfs-probe/three", 2 | 64));
    abi_require(SC1(57, fd) == 0);
    fd = abi_open("/tmpfs-probe/three", 2 | 64);
    tmpfs_result("tmpfs.inode-quota-reuse", fd < 0 ? fd : 0);
    abi_require(fd >= 0 && SC1(57, fd) == 0 && SC1(57, second) == 0 &&
                SC2(39, "/tmpfs-probe", 0) == 0);

    tmpfs_result("tmpfs.bad-size", SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "size=bogus"));
    tmpfs_result("tmpfs.bad-inodes", SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "nr_inodes=bad"));
    tmpfs_result("tmpfs.bad-option", SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "not_an_option=1"));
    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 1, "size=8192") == 0);
    tmpfs_result("tmpfs.readonly-create", abi_open("/tmpfs-probe/no", 2 | 64));
    abi_require(SC2(43, "/tmpfs-probe", &before) == 0);
    tmpfs_result("tmpfs.readonly-statfs", !!(before.flags & 1));
    fd = abi_open("/tmpfs-probe", 65536);
    abi_require(fd >= 0);
    tmpfs_result("tmpfs.readonly-fchmod", SC2(52, fd, 0700));
    tmpfs_result("tmpfs.readonly-utime", SC4(88, fd, 0, 0, 0));
    abi_require(SC1(57, fd) == 0);
    abi_require(SC2(39, "/tmpfs-probe", 0) == 0);

    abi_require(SC3(34, -100, "/tmpfs-other", 0755) == 0 &&
                SC5(40, "none", "/tmpfs-probe", "tmpfs", 0, "size=4096,nr_inodes=16") == 0 &&
                SC5(40, "none", "/tmpfs-other", "tmpfs", 0, "size=4096,nr_inodes=16") == 0);
    fd = abi_open("/tmpfs-probe/data", 2 | 64);
    second = abi_open("/tmpfs-other/data", 2 | 64);
    abi_require(fd >= 0 && second >= 0 && SC4(68, fd, "A", 1, 0) == 1 &&
                SC4(68, second, "B", 1, 0) == 1);
    unsigned char distinct[2];
    abi_require(SC4(67, fd, distinct, 1, 0) == 1 && SC4(67, second, distinct + 1, 1, 0) == 1);
    abi_record("tmpfs.independent-mounts", 0, -1, -1, 0, distinct, sizeof(distinct));
    abi_require(SC1(57, fd) == 0 && SC1(57, second) == 0);
    tmpfs_result("tmpfs.cross-mount-rename", SC5(276, -100, "/tmpfs-probe/data", -100, "/tmpfs-other/moved", 0));
    abi_require(SC3(34, -100, "/tmpfs-probe/old", 0755) == 0 &&
                SC3(36, "../data", -100, "/tmpfs-probe/old/link") == 0);
    tmpfs_result("tmpfs.directory-rename", SC5(276, -100, "/tmpfs-probe/old", -100, "/tmpfs-probe/new", 0));
    fd = abi_open("/tmpfs-probe/new/link", 0);
    abi_require(fd >= 0 && SC3(63, fd, &byte, 1) == 1 && SC1(57, fd) == 0);
    abi_record("tmpfs.symlink-after-rename", 0, -1, -1, 0, &byte, 1);
    abi_require(SC5(40, "none", "/tmpfs-probe/new", "tmpfs", 0, "size=4096") == 0);
    tmpfs_result("tmpfs.nested-hidden", abi_open("/tmpfs-probe/new/link", 0));
    tmpfs_result("tmpfs.nested-parent-busy", SC2(39, "/tmpfs-probe", 0));
    abi_require(SC2(39, "/tmpfs-probe/new", 0) == 0);
    fd = abi_open("/tmpfs-probe/new/link", 0);
    tmpfs_result("tmpfs.nested-restored", fd < 0 ? fd : 0);
    abi_require(fd >= 0 && SC1(57, fd) == 0 &&
                SC2(39, "/tmpfs-probe", 0) == 0 && SC2(39, "/tmpfs-other", 0) == 0);
}

/* Reopening an unlinked inode must not replace the inode seen by old PTEs. */
static void tmpfs_faults(void)
{
    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 0,
                    "size=4096,nr_inodes=16") == 0);
    long fd = abi_open("/tmpfs-probe/fault", 2 | 64);
    abi_require(fd >= 0 && SC2(46, fd, 8192) == 0);
    long mapped = SC6(222, 0, 8192, 3, 1, fd, 0);
    abi_require(mapped > 0);
    *(volatile char *)mapped = 'A';
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        *(volatile char *)(mapped + 4096) = 'B';
        abi_exit(91);
    }
    int status;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("tmpfs.mapping-quota-signal", status & 127, -1, -1, 0, 0, 0);
    abi_require(SC2(215, mapped, 8192) == 0 && SC1(57, fd) == 0 &&
                SC2(39, "/tmpfs-probe", 0) == 0);
    abi_require(SC3(34, -100, "/proc-probe", 0755) == 0);
    abi_require(SC5(40, "none", "/tmpfs-probe", "tmpfs", 0,
                    "size=16384,nr_inodes=16") == 0 &&
                SC5(40, "none", "/proc-probe", "proc", 0, 0) == 0);
    for (int cow = 0; cow < 2; cow++) {
        fd = abi_open("/tmpfs-probe/reopen", 2 | 64);
        abi_require(fd >= 0 && fd < 100 && SC2(46, fd, 8192) == 0);
        mapped = SC6(222, 0, 8192, 3, cow ? 2 : 1, fd, 0);
        abi_require(mapped > 0);
        *(volatile char *)(mapped + 4096) = 'C';
        abi_require(SC3(35, -100, "/tmpfs-probe/reopen", 0) == 0);
        char path[] = "/proc-probe/self/fd/00";
        usize pos = sizeof(path) - 3;
        if (fd >= 10) { path[pos++] = '0' + fd / 10; }
        path[pos++] = '0' + fd % 10; path[pos] = 0;
        long reopened = abi_open(path, 2);
        abi_require(reopened >= 0 && SC2(46, reopened, 4096) == 0);
        child = SC5(220, 17, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            volatile char value = *(volatile char *)(mapped + 4096);
            (void)value;
            abi_exit(92);
        }
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(cow ? "tmpfs.reopened-private-truncate" : "tmpfs.reopened-shared-truncate",
                   status & 127, -1, -1, 0, 0, 0);
        abi_require(SC1(57, reopened) == 0 && SC1(57, fd) == 0 &&
                    SC2(215, mapped, 8192) == 0);
    }
    abi_require(SC2(39, "/tmpfs-probe", 0) == 0 && SC2(39, "/proc-probe", 0) == 0);
}
