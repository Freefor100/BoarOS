#include "abi.h"

static unsigned char file_page[ABI_PAGE_SIZE];
static unsigned char two_pages[(2 * ABI_PAGE_SIZE)];

void abi_shared_file_truncate_cases(void)
{
    for (usize i = 0; i < sizeof(two_pages); i++) two_pages[i] = 'E';
    long fd = abi_open("/shared-file-truncate", 2 | 64 | 512);
    abi_require(fd >= 0 &&
        SC3(64, fd, two_pages, sizeof(two_pages)) == (long)sizeof(two_pages));
    long map = CALL(222, 0, sizeof(two_pages), 3, 1, fd, 0);
    abi_require(map >= 0);
    volatile unsigned char *bytes = (void *)map;
    abi_require(bytes[ABI_PAGE_SIZE] == 'E');
    int ready[2], release[2];
    abi_require(SC2(59, ready, 0) == 0 && SC2(59, release, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        char signal;
        SC1(57, ready[0]); SC1(57, release[1]);
        if (bytes[ABI_PAGE_SIZE] != 'E' ||
            SC3(64, ready[1], "r", 1) != 1 ||
            SC3(63, release[0], &signal, 1) != 1 || signal != 't')
            abi_exit(90);
        volatile unsigned char beyond = bytes[ABI_PAGE_SIZE];
        (void)beyond;
        abi_exit(91);
    }
    SC1(57, ready[1]); SC1(57, release[0]);
    char signal;
    abi_require(SC3(63, ready[0], &signal, 1) == 1 && signal == 'r');
    abi_require(SC2(46, fd, ABI_PAGE_SIZE) == 0);
    abi_require(SC3(64, release[1], "t", 1) == 1);
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("shared-file.truncate-sigbus", 0, abi_size(fd),
               abi_offset(fd), status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0 && SC1(57, release[1]) == 0);
    abi_require(SC2(215, map, sizeof(two_pages)) == 0);
    abi_require(SC2(46, fd, 100) == 0);
    map = CALL(222, 0, ABI_PAGE_SIZE, 3, 1, fd, 0);
    abi_require(map >= 0);
    bytes = (void *)map;
    bytes[150] = 'X';
    abi_require(SC2(46, fd, 200) == 0);
    unsigned char grew = bytes[150];
    abi_record("shared-file.grow-tail", 0, abi_size(fd),
               abi_offset(fd), 0, &grew, 1);
    bytes[250] = 'X';
    abi_require(SC3(62, fd, 300, 0) == 300);
    abi_require(SC3(64, fd, "Z", 1) == 1);
    unsigned char write_gap[] = {bytes[250], bytes[300]};
    abi_record("shared-file.write-grow-gap", 0, abi_size(fd),
               abi_offset(fd), 0, write_gap, sizeof(write_gap));
    long truncated = abi_open("/shared-file-truncate", 2 | 512);
    abi_require(truncated >= 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        volatile unsigned char beyond = bytes[0];
        (void)beyond;
        abi_exit(92);
    }
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("shared-file.open-trunc-sigbus", 0, abi_size(truncated),
               abi_offset(truncated), status, 0, 0);
    abi_require(SC1(57, truncated) == 0);
    abi_require(SC2(215, map, ABI_PAGE_SIZE) == 0 && SC1(57, fd) == 0);
}

void abi_shared_file_mapping_cases(void)
{
    long first = abi_open("/shared-file", 2 | 64 | 512);
    abi_require(first >= 0);
    for (usize i = 0; i < sizeof(file_page); i++) file_page[i] = 'A';
    abi_require(SC3(64, first, file_page, sizeof(file_page)) ==
                (long)sizeof(file_page));
    long second = abi_open("/shared-file", 2);
    abi_require(second >= 0);
    long left = CALL(222, 0, ABI_PAGE_SIZE, 3, 1, first, 0);
    long right = CALL(222, 0, ABI_PAGE_SIZE, 3, 1, second, 0);
    abi_require(left >= 0 && right >= 0);
    volatile unsigned char *a = (void *)left;
    volatile unsigned char *b = (void *)right;
    abi_require(a[19] == 'A' && b[19] == 'A');
    a[19] = 'B';
    unsigned char observed[] = {b[19], 0, 0};
    abi_require(SC3(62, second, 19, 0) == 19);
    abi_require(SC3(63, second, &observed[1], 1) == 1);
    abi_require(SC3(227, left, ABI_PAGE_SIZE, 4) == 0);
    abi_require(SC3(62, first, 19, 0) == 19);
    abi_require(SC3(63, first, &observed[2], 1) == 1);
    abi_record("shared-file.alias-write-sync", 0, abi_size(first),
               abi_offset(second), 0, observed, sizeof(observed));
    abi_record("shared-file.msync-empty", SC3(227, left, 0, 4),
               -1, -1, 0, 0, 0);
    abi_record("shared-file.msync-unaligned", SC3(227, left + 1, 0, 4),
               -1, -1, 0, 0, 0);
    abi_record("shared-file.msync-flags", SC3(227, left, ABI_PAGE_SIZE, 5),
               -1, -1, 0, 0, 0);
    abi_record("shared-file.msync-hole", SC3(227, ABI_PAGE_SIZE, ABI_PAGE_SIZE, 4),
               -1, -1, 0, 0, 0);
    long readonly = abi_open("/shared-file", 0);
    abi_require(readonly >= 0);
    long ro_map = CALL(222, 0, ABI_PAGE_SIZE, 1, 1, readonly, 0);
    abi_require(ro_map >= 0);
    long upgrade = SC3(226, ro_map, ABI_PAGE_SIZE, 3);
    abi_record("shared-file.readonly-upgrade", upgrade,
               abi_size(readonly), abi_offset(readonly), 0, 0, 0);
    abi_require(SC2(215, ro_map, ABI_PAGE_SIZE) == 0);
    abi_require(SC1(57, readonly) == 0);

    long protected_map = CALL(222, 0, ABI_PAGE_SIZE, 1, 1, first, 0);
    abi_require(protected_map >= 0);
    abi_require(SC3(226, protected_map, ABI_PAGE_SIZE, 3) == 0);
    volatile unsigned char *protected_bytes = (void *)protected_map;
    protected_bytes[37] = 'C';
    abi_require(SC1(57, first) == 0);
    abi_require(SC3(227, protected_map, ABI_PAGE_SIZE, 4) == 0);
    unsigned char upgraded = b[37];
    abi_record("shared-file.protect-close-sync", 0, abi_size(second),
               abi_offset(second), 0, &upgraded, 1);
    abi_require(SC2(215, protected_map, ABI_PAGE_SIZE) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        if (b[19] != 'B' || b[37] != 'C') abi_exit(9);
        b[55] = 'D';
        abi_exit(0);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    unsigned char fork_value = a[55];
    abi_record("shared-file.fork", 0, abi_size(second),
               abi_offset(second), status, &fork_value, 1);
    abi_require(SC3(227, left, ABI_PAGE_SIZE, 4) == 0);
    abi_require(SC3(62, second, 55, 0) == 55);
    unsigned char fork_written = 0;
    abi_require(SC3(63, second, &fork_written, 1) == 1);
    abi_record("shared-file.fork-sync", 0, abi_size(second),
               abi_offset(second), 0, &fork_written, 1);
    long private_map = CALL(222, 0, ABI_PAGE_SIZE, 3, 2, second, 0);
    abi_require(private_map >= 0);
    volatile unsigned char *private_bytes = (void *)private_map;
    private_bytes[80] = 'P';
    unsigned char isolation[] = {private_bytes[80], a[80]};
    abi_record("shared-file.private-isolation", 0, abi_size(second),
               abi_offset(second), 0, isolation, sizeof(isolation));
    abi_require(SC2(215, private_map, ABI_PAGE_SIZE) == 0);
    abi_require(SC3(35, -100, "/shared-file", 0) == 0);
    abi_require(SC1(57, second) == 0);
    a[75] = 'E';
    long synced_unlinked = SC3(227, left, ABI_PAGE_SIZE, 4);
    unsigned char unlinked = b[75];
    abi_record("shared-file.unlinked-sync", synced_unlinked,
               -1, -1, 0, &unlinked, 1);
    abi_require(SC2(215, left, ABI_PAGE_SIZE) == 0 &&
                SC2(215, right, ABI_PAGE_SIZE) == 0);
}

void abi_shared_mapping_cases(void)
{
    volatile unsigned char *pages = (void *)CALL(222, 0, 3 * ABI_PAGE_SIZE, 3,
                                                 0x21, -1, 0);
    int child_to_parent[2], parent_to_child[2], status = 0;
    char signal;
    long child;
    unsigned char values[3];

    abi_require((long)pages >= 0);
    pages[0] = 0x11U;
    abi_require(SC2(59, child_to_parent, 0) == 0);
    abi_require(SC2(59, parent_to_child, 0) == 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        SC1(57, child_to_parent[0]);
        SC1(57, parent_to_child[1]);
        if (pages[0] != 0x11U) abi_exit(11);
        pages[0] = 0x22U;
        pages[(2 * ABI_PAGE_SIZE)] = 0x44U;
        if (SC3(64, child_to_parent[1], "r", 1) != 1 ||
            SC3(63, parent_to_child[0], &signal, 1) != 1 ||
            pages[ABI_PAGE_SIZE] != 0x33U) abi_exit(12);
        abi_exit(0);
    }
    SC1(57, child_to_parent[1]);
    SC1(57, parent_to_child[0]);
    abi_require(SC3(63, child_to_parent[0], &signal, 1) == 1);
    values[0] = pages[0];
    values[2] = pages[(2 * ABI_PAGE_SIZE)];
    pages[ABI_PAGE_SIZE] = 0x33U;
    values[1] = pages[ABI_PAGE_SIZE];
    abi_require(SC3(64, parent_to_child[1], "a", 1) == 1);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("shared-anon.fork", 0, -1, -1, status, values,
               sizeof(values));
    abi_require(SC1(57, child_to_parent[0]) == 0);
    abi_require(SC1(57, parent_to_child[1]) == 0);
    abi_require(SC2(215, pages, 3 * ABI_PAGE_SIZE) == 0);
}
