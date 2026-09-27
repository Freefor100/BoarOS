#include "abi.h"

void abi_access_cases(void)
{
    long directory = abi_open("/", 0 | 0200000);
    abi_require(directory >= 0);
    abi_record("access.root-write", SC3(48, -100, "/", 2),
               -1, -1, 0, 0, 0);
    abi_record("access.file-read", SC3(48, -100, "/data", 4),
               -1, -1, 0, 0, 0);
    abi_record("access.file-exec", SC3(48, -100, "/data", 1),
               -1, -1, 0, 0, 0);
    abi_record("access.relative-write", SC3(48, directory, "data", 2),
               -1, -1, 0, 0, 0);
    abi_record("access.absolute-bad-dirfd", SC3(48, -9, "/data", 0),
               -1, -1, 0, 0, 0);
    abi_record("access.missing", SC3(48, -100, "/no-such-access-file", 0),
               -1, -1, 0, 0, 0);
    abi_record("access.bad-mode", SC3(48, -100, "/data", 8),
               -1, -1, 0, 0, 0);
    abi_record("access.bad-pointer", SC3(48, -100, 1, 0),
               -1, -1, 0, 0, 0);
    long source = SC4(56, -100, "/data", 0400, 0);
    abi_record("open.noctty-regular", source < 0 ? source : 0,
               -1, -1, 0, 0, 0);
    if (source >= 0) abi_require(SC1(57, source) == 0);
    long device = SC4(56, -100, "/dev/null", 0400, 0);
    abi_record("open.noctty-char", device < 0 ? device : 0,
               -1, -1, 0, 0, 0);
    if (device >= 0) abi_require(SC1(57, device) == 0);
    abi_record("open.noctty-bad-combination",
               SC4(56, -100, "/data", 0400 | 0100 | 0200000, 0600),
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, directory) == 0);
}
