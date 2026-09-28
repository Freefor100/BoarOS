#include "abi.h"
void abi_mknod_cases(void)
{
    abi_record("mknod.null", SC4(33, -100, "/created-null", 0020666, 0x103), -1, -1, 0, 0, 0);
    abi_record("mknod.exists", SC4(33, -100, "/created-null", 0020600, 0x105), -1, -1, 0, 0, 0);
    struct abi_stat st;
    for (usize i = 0; i < sizeof(st); i++) ((volatile char *)&st)[i] = 0;
    long r = SC4(79, -100, "/created-null", &st, 0);
    long info[] = {st.mode, st.rdev};
    abi_record("mknod.stat", r, -1, -1, 0, info, sizeof(info));
    long fd = abi_open("/created-null", 2);
    abi_record("mknod.null-write", fd < 0 ? fd : SC3(64, fd, "abc", 3), -1, -1, 0, 0, 0);
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
    long dir = abi_open("/dev", 0x10000);
    abi_require(dir >= 0);
    abi_record("mknod.zero-dirfd", SC4(33, dir, "created-zero", 0020600, 0x105), -1, -1, 0, 0, 0);
    fd = SC4(56, dir, "created-zero", 0, 0);
    unsigned char data[4] = {1, 2, 3, 4};
    abi_record("mknod.zero-read", fd < 0 ? fd : SC3(63, fd, data, 4), -1, -1, 0, data, 4);
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
    abi_require(SC1(57, dir) == 0);
    abi_record("mknod.regular", SC4(33, -100, "/created-regular", 0600, -1), -1, -1, 0, 0, 0);
    abi_record("mknod.directory", SC4(33, -100, "/created-dir", 0040755, 0), -1, -1, 0, 0, 0);
    abi_record("mknod.invalid-type", SC4(33, -100, "/created-link", 0120777, 0), -1, -1, 0, 0, 0);
    abi_record("mknod.bad-pointer", SC4(33, -100, 8, 0020600, 0x103), -1, -1, 0, 0, 0);
    abi_record("mknod.missing-parent", SC4(33, -100, "/missing-parent/node", 0020600, 0x103), -1, -1, 0, 0, 0);
    abi_record("mknod.bad-dirfd", SC4(33, -1, "node", 0020600, 0x103), -1, -1, 0, 0, 0);
}
