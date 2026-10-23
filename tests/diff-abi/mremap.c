#include "abi.h"

#define P ((long)ABI_PAGE_SIZE)
static long mapping(long pages, long flags)
{
    long address = SC6(222, 0, pages * P, 3, flags, -1, 0);
    abi_require(address > 0);
    return address;
}
static void unmap(long address, long pages)
{ abi_require(SC2(215, address, pages * P) == 0); }
static void result(const char *name, long value)
{ abi_record(name, value < 0 ? value : 0, -1, -1, 0, 0, 0); }
static void page_pattern(long address, long pages)
{
    for (long page = 0; page < pages; ++page)
        for (long i = 0; i < P; ++i)
            ((unsigned char *)address)[page * P + i] = (unsigned char)(0x30 + page);
}
static int pattern(long address, long pages, long first)
{
    for (long page = 0; page < pages; ++page)
        for (long i = 0; i < P; ++i)
            if (((unsigned char *)address)[page * P + i] != 0x30 + first + page)
                return 0;
    return 1;
}
/* 子进程实际访问撤销的旧地址，不依赖页表私有布局。 */
static long access_status(long address)
{
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        volatile unsigned char value = *(volatile unsigned char *)address;
        (void)value;
        abi_exit(0);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    return status;
}
static void parameters(void)
{
    long source = mapping(4, 0x22), target = mapping(4, 0x22);
    result("mremap.flags", SC5(216, source, P, P, 8, 0));
    result("mremap.old-align", SC5(216, source + 1, P, P, 0, 0));
    result("mremap.new-zero", SC5(216, source, P, 0, 0, 0));
    result("mremap.length-overflow", SC5(216, source, P, -1, 1, 0));
    result("mremap.fixed-needs-move", SC5(216, source, P, P, 2, target));
    result("mremap.new-align", SC5(216, source, P, P, 3, target + 1));
    result("mremap.overlap", SC5(216, source, 2 * P, P, 3, source + P));
    result("mremap.zero-private", SC5(216, source, 0, P, 1, 0));
    result("mremap.dontunmap-size", SC5(216, source, P, 2 * P, 5, 0));
    result("mremap.dontunmap-needs-move", SC5(216, source, P, P, 4, 0));
    unmap(target, 4);
    result("mremap.source-hole", SC5(216, target, P, 2 * P, 1, 0));
    unmap(source, 4);
}
static void resize(void)
{
    long source = mapping(4, 0x22);
    page_pattern(source, 4);
    long moved = SC5(216, source, 4 * P, 2 * P, 0, 0);
    abi_record("mremap.shrink", moved < 0 ? moved : 0,
               moved >= 0 && moved == source && pattern(moved, 2, 0), -1, 0, 0, 0);
    abi_record("mremap.shrink-tail", 0, -1, -1,
               moved < 0 ? 0 : access_status(source + 2 * P), 0, 0);
    unmap(source, 4);
    source = mapping(6, 0x22);
    page_pattern(source, 4);
    unmap(source + 4 * P, 2);
    moved = SC5(216, source, 4 * P, 6 * P, 0, 0);
    int good = moved >= 0 && moved == source && pattern(moved, 4, 0);
    if (good) for (long i = 4 * P; i < 6 * P; ++i)
        if (((unsigned char *)moved)[i]) good = 0;
    abi_record("mremap.inplace-grow", moved < 0 ? moved : 0, good, -1, 0, 0, 0);
    unmap(source, 6);
    source = mapping(4, 0x22);
    page_pattern(source, 4);
    abi_require(SC3(226, source + 2 * P, 2 * P, 1) == 0);
    result("mremap.blocked-grow", SC5(216, source, 2 * P, 4 * P, 0, 0));
    moved = SC5(216, source, 2 * P, 4 * P, 1, 0);
    good = moved >= 0 && moved != source && pattern(moved, 2, 0) && pattern(source + 2 * P, 2, 2);
    if (good) for (long i = 2 * P; i < 4 * P; ++i)
        if (((unsigned char *)moved)[i]) good = 0;
    abi_record("mremap.move-grow", moved < 0 ? moved : 0, good, -1, 0, 0, 0);
    abi_record("mremap.move-old-hole", 0, -1, -1,
               moved < 0 ? 0 : access_status(source), 0, 0);
    if (moved >= 0) unmap(moved, 4);
    unmap(source, 4);
}
static void fixed_subrange(void)
{
    long source = mapping(4, 0x22), target = mapping(4, 0x22);
    page_pattern(source, 4); page_pattern(target, 4);
    long moved = SC5(216, source + P, 2 * P, 2 * P, 3, target);
    int good = moved >= 0 && moved == target && pattern(target, 2, 1) &&
               pattern(source, 1, 0) && pattern(source + 3 * P, 1, 3) && pattern(target + 2 * P, 2, 2);
    abi_record("mremap.fixed-subrange", moved < 0 ? moved : 0, good, -1, 0, 0, 0);
    abi_record("mremap.fixed-old-hole", 0, -1, -1,
               moved < 0 ? 0 : access_status(source + P), 0, 0);
    unmap(source, 4); unmap(target, 4);
    source = mapping(2, 0x22); target = mapping(2, 0x22);
    page_pattern(source, 2);
    abi_require(SC3(226, source + P, P, 0) == 0);
    moved = SC5(216, source + P, P, P, 3, target);
    abi_record("mremap.prot-none", moved < 0 ? moved : 0,
               moved >= 0 ? access_status(target) : -1, -1, 0, 0, 0);
    good = 0;
    if (moved >= 0) {
        abi_require(SC3(226, target, P, 3) == 0);
        good = pattern(target, 1, 1);
    }
    abi_record("mremap.prot-none-content", 0, good, -1, 0, 0, 0);
    unmap(source, 2); unmap(target, 2);
}
static void fork_mapping(int shared)
{
    long source = mapping(2, shared ? 0x21 : 0x22), target = mapping(2, 0x22);
    page_pattern(source, 2);
    int ready[2], release[2];
    abi_require(SC2(59, ready, 0) == 0 && SC2(59, release, 0) == 0);
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        char byte = 1;
        if (SC3(64, ready[1], &byte, 1) != 1 || SC3(63, release[0], &byte, 1) != 1) abi_exit(91);
        abi_exit(((unsigned char *)source)[0] == (shared ? 0x77 : 0x30) ? 0 : 92);
    }
    char byte;
    abi_require(SC3(63, ready[0], &byte, 1) == 1);
    long moved = SC5(216, source, 2 * P, 2 * P, 3, target);
    int good = moved >= 0 && moved == target && pattern(target, 2, 0);
    if (moved >= 0) ((unsigned char *)target)[0] = 0x77;
    abi_require(SC3(64, release[1], &byte, 1) == 1);
    int status;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record(shared ? "mremap.shared-anon-fork" : "mremap.private-cow-fork",
               moved < 0 ? moved : 0, good, -1, status, 0, 0);
    for (int i = 0; i < 2; ++i)
        abi_require(SC1(57, ready[i]) == 0 && SC1(57, release[i]) == 0);
    unmap(source, 2); unmap(target, 2);
}
static unsigned char file_page[ABI_PAGE_SIZE];
static void file_mapping(int shared)
{
    long fd = abi_open("/mremap-file", 2 | 64 | 512);
    abi_require(fd >= 0);
    for (long page = 0; page < 4; ++page) {
        for (long i = 0; i < P; ++i) file_page[i] = 0x30 + page;
        abi_require(SC3(64, fd, file_page, P) == P);
    }
    long source = SC6(222, 0, 2 * P, 3, shared ? 1 : 2, fd, P), target = mapping(3, 0x22);
    abi_require(source > 0);
    ((unsigned char *)source)[0] = 0x77;
    long moved = SC5(216, source, 2 * P, 3 * P, 3, target);
    int good = moved >= 0 && moved == target && ((unsigned char *)target)[0] == 0x77 && pattern(target + P, 2, 2);
    if (moved >= 0) {
        ((unsigned char *)target)[1] = 0x78;
        abi_require(SC3(227, target, 3 * P, 4) == 0);
    }
    unsigned char bytes[2] = {0};
    abi_require(SC4(67, fd, bytes, 2, P) == 2);
    abi_record(shared ? "mremap.shared-file" : "mremap.private-file", moved < 0 ? moved : 0,
               good, abi_offset(fd) == 4 * P, 0, bytes, 2);
    abi_record(shared ? "mremap.shared-file-old" : "mremap.private-file-old", 0, -1, -1,
               moved < 0 ? 0 : access_status(source), 0, 0);
    unmap(source, 2); unmap(target, 3);
    abi_require(SC1(57, fd) == 0 && SC3(35, -100, "/mremap-file", 0) == 0);
}
void abi_mremap_cases(void)
{
    parameters(); resize(); fixed_subrange();
    fork_mapping(0); fork_mapping(1); file_mapping(0); file_mapping(1);
}
