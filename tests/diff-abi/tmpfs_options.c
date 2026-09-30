#include "abi.h"

/* references/linux/{mm/shmem.c,lib/cmdline.c,lib/kstrtox.c,fs/inode.c},
 * f4cdf7ca9a1fdcca413157df19753f388a5a224e: mount numbers, directory
 * bookkeeping, hardlink inode reservations and nanosecond relatime. */
struct option_statfs {
    unsigned long type, bsize, blocks, bfree, bavail, files, ffree;
    int fsid[2];
    unsigned long namelen, frsize, flags, spare[4];
};
static void option(const char *id, const char *value)
{
    long result = SC5(40, "none", "/tmpfs-options", "tmpfs", 0, value);
    struct option_statfs fs;
    struct abi_stat st;
    unsigned long data[3] = {0};
    if (!result) {
        abi_require(SC2(43, "/tmpfs-options", &fs) == 0 &&
                    SC4(79, -100, "/tmpfs-options", &st, 0) == 0);
        data[0] = fs.blocks; data[1] = fs.files; data[2] = st.mode & 07777;
        abi_require(SC2(39, "/tmpfs-options", 0) == 0);
    }
    abi_record(id, result, -1, -1, 0, result ? 0 : data, result ? 0 : sizeof(data));
}
static long size(const char *path)
{
    struct abi_stat st;
    abi_require(SC4(79, -100, path, &st, 0) == 0);
    return st.size;
}
static unsigned long free_inodes(void)
{
    struct option_statfs fs;
    abi_require(SC2(43, "/tmpfs-options", &fs) == 0);
    return fs.ffree;
}
void abi_tmpfs_options_cases(void)
{
    abi_require(SC3(34, -100, "/tmpfs-options", 0755) == 0);
    option("tmpfs.options.hex", "size=0x2000,nr_inodes=0x10,mode=0750");
    option("tmpfs.options.octal", "nr_blocks=010,nr_inodes=020,mode=1755");
    option("tmpfs.options.tera", "size=1T,nr_inodes=8");
    option("tmpfs.options.peta", "size=1p,nr_inodes=8");
    option("tmpfs.options.exa", "size=1E,nr_inodes=8");
    option("tmpfs.options.round-up", "size=4097,nr_inodes=8");
    option("tmpfs.options.suffix-overflow", "size=16E,nr_inodes=8");
    option("tmpfs.options.number-overflow", "size=18446744073709551616,nr_inodes=8");
    option("tmpfs.options.blocks-overflow", "nr_blocks=9223372036854775808,nr_inodes=8");
    option("tmpfs.options.inodes-overflow", "size=4096,nr_inodes=18014398509481984");
    option("tmpfs.options.bad-octal", "size=08,nr_inodes=8");
    option("tmpfs.options.bad-suffix", "size=1KB,nr_inodes=8");
    option("tmpfs.options.mode-mask", "size=4096,nr_inodes=8,mode=17777");
    abi_require(SC5(40, "none", "/tmpfs-options", "tmpfs", 0, "size=4096,nr_inodes=8") == 0);
    long geometry[8];
    geometry[0] = size("/tmpfs-options");
    abi_require(SC3(34, -100, "/tmpfs-options/dir", 0755) == 0);
    geometry[1] = size("/tmpfs-options"); geometry[2] = size("/tmpfs-options/dir");
    long fd = abi_open("/tmpfs-options/a", 2 | 64);
    abi_require(fd >= 0 && SC3(64, fd, "x", 1) == 1);
    geometry[3] = size("/tmpfs-options");
    abi_require(SC5(37, -100, "/tmpfs-options/a", -100, "/tmpfs-options/b", 0) == 0);
    geometry[4] = size("/tmpfs-options");
    abi_require(SC5(276, -100, "/tmpfs-options/a", -100, "/tmpfs-options/dir/a", 0) == 0);
    geometry[5] = size("/tmpfs-options"); geometry[6] = size("/tmpfs-options/dir");
    abi_require(SC3(35, -100, "/tmpfs-options/b", 0) == 0);
    geometry[7] = size("/tmpfs-options");
    abi_record("tmpfs.directory-sizes", 0, -1, -1, 0, geometry, sizeof(geometry));
    long current[2]; struct abi_stat st;
    abi_require(SC2(113, 0, current) == 0);
    long times[4] = {current[0] + 3600, 800000000, current[0] + 3600, 100000000};
    abi_require(SC4(88, fd, 0, times, 0) == 0);
    char byte;
    abi_require(SC4(67, fd, &byte, 1, 0) == 1 && SC2(80, fd, &st) == 0);
    long retained = st.atime == times[0] && st.atime_nsec == (unsigned long)times[1];
    abi_record("tmpfs.relatime-nanoseconds", retained, -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0 && SC2(39, "/tmpfs-options", 0) == 0);

    abi_require(SC5(40, "none", "/tmpfs-options", "tmpfs", 0, "size=4096,nr_inodes=4") == 0);
    unsigned long quota[6]; quota[0] = free_inodes();
    fd = abi_open("/tmpfs-options/a", 2 | 64);
    abi_require(fd >= 0 && SC5(37, -100, "/tmpfs-options/a", -100, "/tmpfs-options/b", 0) == 0 &&
                SC5(37, -100, "/tmpfs-options/a", -100, "/tmpfs-options/c", 0) == 0);
    quota[1] = free_inodes();
    abi_record("tmpfs.hardlink-quota-full", abi_open("/tmpfs-options/d", 2 | 64), -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/tmpfs-options/b", 0) == 0); quota[2] = free_inodes();
    abi_require(SC3(35, -100, "/tmpfs-options/a", 0) == 0); quota[3] = free_inodes();
    abi_require(SC3(35, -100, "/tmpfs-options/c", 0) == 0); quota[4] = free_inodes();
    abi_require(SC1(57, fd) == 0); quota[5] = free_inodes();
    abi_record("tmpfs.hardlink-quota-release", 0, -1, -1, 0, quota, sizeof(quota));
    fd = abi_open("/tmpfs-options/a", 2 | 64);
    abi_require(fd >= 0 && SC5(37, -100, "/tmpfs-options/a", -100, "/tmpfs-options/b", 0) == 0 &&
                SC1(57, fd) == 0);
    abi_record("tmpfs.hardlink-unmount", SC2(39, "/tmpfs-options", 0), -1, -1, 0, 0, 0);
}
