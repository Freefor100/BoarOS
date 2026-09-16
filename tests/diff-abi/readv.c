#include "abi.h"

static void close_fd(long fd) { abi_require(SC1(57, fd) == 0); }

static void readv_file_edges(void)
{
    static struct abi_iovec many[1025];
    static const int prefixes[6] = {0, 1, 32, 63, 64, 65};
    static const char *names[6] = {"readv.file-prefix-0",
        "readv.file-prefix-1", "readv.file-prefix-32", "readv.file-prefix-63",
        "readv.file-prefix-64", "readv.file-prefix-65"};
    unsigned char result[66], last = 0xa5;
    long fd = abi_open("/data", 0);
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(fd >= 0 && map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);

    long ret = SC3(65, fd, (void *)-1, 0);
    abi_record("readv.empty-invalid-vector", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    ret = SC3(65, fd, (void *)-1, 1);
    abi_record("readv.invalid-vector", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    ret = SC3(65, fd, many, 1024);
    abi_record("readv.1024-empty", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    ret = SC3(65, fd, many, 1025);
    abi_record("readv.1025", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    many[0] = (struct abi_iovec){(void *)-1, 0};
    ret = SC3(65, fd, many, 1);
    abi_record("readv.invalid-zero-base", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    many[0] = (struct abi_iovec){result, (usize)-1};
    ret = SC3(65, fd, many, 1);
    abi_record("readv.negative-length", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    many[0] = (struct abi_iovec){(void *)map, 0x7ffff000UL};
    many[1] = (struct abi_iovec){(void *)-1, 1};
    abi_require(SC3(62, fd, 9000, 0) == 9000);
    ret = SC3(65, fd, many, 2);
    abi_record("readv.after-cap-invalid", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    for (int index = 0; index < 6; index++) {
        int prefix = prefixes[index];
        struct abi_iovec vector[2] = {
            {(void *)(map + 4096 - prefix), (usize)prefix + 1},
            {&last, 1}
        };
        abi_require(SC3(62, fd, 0, 0) == 0);
        for (int j = 0; j < prefix; j++)
            ((unsigned char *)vector[0].base)[j] = 0xa5;
        last = 0xa5;
        ret = SC3(65, fd, vector, 2);
        for (int j = 0; j < prefix; j++)
            result[j] = ((unsigned char *)vector[0].base)[j];
        result[prefix] = last;
        abi_record(names[index], ret, abi_size(fd), abi_offset(fd), 0,
                   result, (usize)prefix + 1);
    }
    abi_require(SC2(215, map, 8192) == 0);
    close_fd(fd);

    fd = abi_open("/data", 1);
    abi_require(fd >= 0);
    ret = SC3(65, fd, (void *)-1, 1025);
    abi_record("readv.write-only-order", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    close_fd(fd);
    ret = SC3(65, -1, (void *)-1, 1025);
    abi_record("readv.bad-fd-order", ret, -1, -1, 0, 0, 0);
    fd = abi_open("/", 0);
    abi_require(fd >= 0);
    many[0] = (struct abi_iovec){result, 1};
    ret = SC3(65, fd, many, 1);
    abi_record("readv.directory", ret, abi_size(fd), abi_offset(fd),
               0, result, 1);
    ret = SC3(65, fd, 0, 0);
    abi_record("readv.directory-zero", ret, abi_size(fd),
               abi_offset(fd), 0, 0, 0);
    close_fd(fd);
}

static void readv_pipe_edges(void)
{
    static const int prefixes[6] = {0, 1, 32, 63, 64, 65};
    static const char *names[6] = {"readv.pipe-prefix-0",
        "readv.pipe-prefix-1", "readv.pipe-prefix-32", "readv.pipe-prefix-63",
        "readv.pipe-prefix-64", "readv.pipe-prefix-65"};
    static const char *retry_names[6] = {"readv.pipe-retry-0",
        "readv.pipe-retry-1", "readv.pipe-retry-32", "readv.pipe-retry-63",
        "readv.pipe-retry-64", "readv.pipe-retry-65"};
    unsigned char payload[128], evidence[66], retry[128], last = 0xa5;
    int pipefd[2];
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    for (int j = 0; j < 128; j++) payload[j] = 'A' + j % 26;
    for (int index = 0; index < 6; index++) {
        int prefix = prefixes[index];
        struct abi_iovec vector[2] = {
            {(void *)(map + 4096 - prefix), (usize)prefix + 1},
            {&last, 1}
        };
        abi_require(SC2(59, pipefd, 0) == 0);
        abi_require(SC3(64, pipefd[1], payload, 128) == 128);
        for (int j = 0; j < prefix; j++)
            ((unsigned char *)vector[0].base)[j] = 0xa5;
        last = 0xa5;
        long ret = SC3(65, pipefd[0], vector, 2);
        for (int j = 0; j < prefix; j++)
            evidence[j] = ((unsigned char *)vector[0].base)[j];
        evidence[prefix] = last;
        abi_record(names[index], ret, -1, -1, 0, evidence,
                   (usize)prefix + 1);
        ret = SC3(63, pipefd[0], retry, 128);
        abi_record(retry_names[index], ret, -1, -1, 0, retry,
                   ret > 0 ? (usize)ret : 0);
        close_fd(pipefd[0]); close_fd(pipefd[1]);
    }
    abi_require(SC2(215, map, 8192) == 0);
}

static void pipe_write_fragment_fault(void)
{
    int pipefd[2];
    unsigned char first[32], observed[64];
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    for (int index = 0; index < 32; index++) first[index] = 'a' + index % 26;
    struct abi_iovec vector[2] = {{first, sizeof first},
                                   {(void *)(map + 4096), 32}};
    abi_require(SC2(59, pipefd, 0) == 0);
    long ret = SC3(66, pipefd[1], vector, 2);
    abi_record("readv.pipe-write-fault", ret, -1, -1, 0, 0, 0);
    close_fd(pipefd[1]);
    ret = SC3(63, pipefd[0], observed, sizeof observed);
    abi_record("readv.pipe-after-write-fault", ret, -1, -1, 0,
               observed, ret > 0 ? (usize)ret : 0);
    close_fd(pipefd[0]);
    abi_require(SC2(215, map, 8192) == 0);
}

static void readv_more_edges(void)
{
    static unsigned char page[4096], ring[65536];
    unsigned char first[3] = {0}, second[5] = {0}, sample[42];
    struct abi_iovec vector[2] = {{first, 3}, {second, 5}};
    long fd = abi_open("/data", 0);
    abi_require(fd >= 0);
    abi_require(SC3(62, fd, 4094, 0) == 4094);
    long ret = SC3(65, fd, vector, 2);
    for (int j = 0; j < 3; j++) sample[j] = first[j];
    for (int j = 0; j < 5; j++) sample[3 + j] = second[j];
    abi_record("readv.file-cross-page", ret, abi_size(fd),
               abi_offset(fd), 0, sample, 8);
    long shared = SC1(23, fd);
    abi_require(shared >= 0);
    vector[0].length = 2;
    ret = SC3(65, shared, vector, 1);
    abi_record("readv.shared-ofd", ret, abi_size(fd),
               abi_offset(fd), 0, first, 2);
    close_fd(shared);
    abi_require(SC3(62, fd, 9000, 0) == 9000);
    first[0] = 0xa5;
    ret = SC3(65, fd, vector, 1);
    abi_record("readv.eof", ret, abi_size(fd), abi_offset(fd),
               0, first, 1);
    close_fd(fd);

    int pipefd[2];
    abi_require(SC2(59, pipefd, 04000) == 0);
    ret = SC3(65, pipefd[1], (void *)-1, 1025);
    abi_record("readv.pipe-write-only-order", ret, -1, -1, 0, 0, 0);
    ret = SC3(65, pipefd[0], vector, 1);
    abi_record("readv.pipe-nonblock-empty", ret, -1, -1, 0,
               first, 1);
    close_fd(pipefd[1]);
    ret = SC3(65, pipefd[0], vector, 1);
    abi_record("readv.pipe-eof", ret, -1, -1, 0, first, 1);
    close_fd(pipefd[0]);

    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    abi_require(SC2(59, pipefd, 0) == 0);
    for (int j = 0; j < 4096; j++) page[j] = 'x';
    abi_require(SC3(64, pipefd[1], page, 4096) == 4096);
    abi_require(SC3(64, pipefd[1], "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy", 64) == 64);
    vector[0] = (struct abi_iovec){page, 4096};
    vector[1] = (struct abi_iovec){(void *)(map + 4096 - 32), 64};
    ret = SC3(65, pipefd[0], vector, 2);
    for (int j = 0; j < 8; j++) sample[j] = page[j];
    sample[8] = page[4095];
    for (int j = 0; j < 32; j++)
        sample[9 + j] = ((unsigned char *)vector[1].base)[j];
    abi_record("readv.pipe-fragment-fault", ret, -1, -1, 0, sample, 41);
    close_fd(pipefd[1]);
    ret = SC3(63, pipefd[0], ring, 64);
    abi_record("readv.pipe-fragment-retry", ret, -1, -1, 0,
               ring, ret > 0 ? (usize)ret : 0);
    close_fd(pipefd[0]);
    abi_require(SC2(215, map, 8192) == 0);

    abi_require(SC2(59, pipefd, 0) == 0);
    for (int slot = 0; slot < 16; slot++) {
        for (int j = 0; j < 4096; j++) page[j] = 'A' + slot;
        abi_require(SC3(64, pipefd[1], page, 4096) == 4096);
    }
    abi_require(SC3(63, pipefd[0], ring, 15 * 4096) == 15 * 4096);
    for (int slot = 0; slot < 15; slot++) {
        for (int j = 0; j < 4096; j++) page[j] = 'a' + slot;
        abi_require(SC3(64, pipefd[1], page, 4096) == 4096);
    }
    struct abi_iovec wrap[2] = {{ring, 4096}, {ring + 4096, 15 * 4096}};
    ret = SC3(65, pipefd[0], wrap, 2);
    sample[0] = ring[0]; sample[1] = ring[4095];
    sample[2] = ring[4096]; sample[3] = ring[65535];
    abi_record("readv.pipe-wrap", ret, -1, -1, 0, sample, 4);
    close_fd(pipefd[0]); close_fd(pipefd[1]);
}

static void pipe_merge_boundary(void)
{
    static unsigned char data[5000], retry[6000];
    unsigned char sample[3];
    int pipefd[2];
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    abi_require(SC2(59, pipefd, 0) == 0);
    for (int j = 0; j < 5000; j++) data[j] = 'b';
    abi_require(SC3(64, pipefd[1], data, 500) == 500);
    abi_require(SC3(64, pipefd[1], data, 5000) == 5000);
    struct abi_iovec vector = {(void *)(map + 4096 - 2000), 5500};
    long ret = SC3(65, pipefd[0], &vector, 1);
    sample[0] = ((unsigned char *)vector.base)[0];
    sample[1] = ((unsigned char *)vector.base)[1399];
    sample[2] = ((unsigned char *)vector.base)[1999];
    abi_record("readv.pipe-merge-fault", ret, -1, -1, 0, sample, 3);
    close_fd(pipefd[1]);
    ret = SC3(63, pipefd[0], retry, sizeof retry);
    sample[0] = retry[0];
    sample[1] = ret > 0 ? retry[ret - 1] : 0;
    abi_record("readv.pipe-merge-retry", ret, -1, -1, 0, sample, 2);
    close_fd(pipefd[0]);
    abi_require(SC2(215, map, 8192) == 0);
}

static void pipe_partial_tail_poll(void)
{
    static unsigned char page[4096];
    struct { int fd; short events, revents; } pollfd;
    struct { long sec, nsec; } zero = {0, 0};
    int pipefd[2];
    abi_require(SC2(59, pipefd, 04000) == 0);
    for (int j = 0; j < 4096; j++) page[j] = 'p';
    for (int slot = 0; slot < 15; slot++)
        abi_require(SC3(64, pipefd[1], page, 4096) == 4096);
    abi_require(SC3(64, pipefd[1], page, 1) == 1);
    pollfd = (typeof(pollfd)){pipefd[1], 4, 0};
    long ret = CALL(73, &pollfd, 1, &zero, 0, 0, 0);
    unsigned char revents = (unsigned char)pollfd.revents;
    abi_record("readv.pipe-partial-tail-poll", ret, -1, -1, 0,
               &revents, 1);
    close_fd(pipefd[0]); close_fd(pipefd[1]);
}

void abi_readv_cases(void)
{
    unsigned char first[3] = {0}, second[5] = {0}, combined[8] = {0};
    struct abi_iovec vector[2] = {{first, sizeof first}, {second, sizeof second}};
    long fd = abi_open("/data", 0);
    abi_require(fd >= 0);
    long ret = SC3(65, fd, vector, 2);
    for (int i = 0; i < 3; i++) combined[i] = first[i];
    for (int i = 0; i < 5; i++) combined[3 + i] = second[i];
    abi_record("readv.file", ret, abi_size(fd), abi_offset(fd), 0,
               combined, sizeof combined);
    ret = SC3(65, fd, 0, 0);
    abi_record("readv.zero", ret, abi_size(fd), abi_offset(fd), 0, 0, 0);
    ret = SC3(65, fd, vector, 1025);
    abi_record("readv.count", ret, abi_size(fd), abi_offset(fd), 0, 0, 0);

    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    abi_require(SC3(62, fd, 0, 0) == 0);
    unsigned char untouched = 0xa5;
    struct abi_iovec fault[2] = {{(void *)(map + 4096 - 32), 64},
                                  {&untouched, 1}};
    ret = SC3(65, fd, fault, 2);
    unsigned char evidence[33];
    for (int i = 0; i < 32; i++) evidence[i] = ((unsigned char *)fault[0].base)[i];
    evidence[32] = untouched;
    abi_record("readv.file-fault", ret, abi_size(fd), abi_offset(fd), 0,
               evidence, sizeof evidence);
    abi_require(SC2(215, map, 8192) == 0);
    close_fd(fd);

    int pipefd[2] = {-1, -1};
    abi_require(SC2(59, pipefd, 0) == 0);
    abi_require(SC3(64, pipefd[1], "abc", 3) == 3);
    first[0] = first[1] = second[0] = second[1] = 0xa5;
    vector[0].length = 2;
    vector[1].length = 2;
    ret = SC3(65, pipefd[0], vector, 2);
    unsigned char pipe_data[4] = {first[0], first[1], second[0], second[1]};
    abi_record("readv.pipe", ret, -1, -1, 0, pipe_data, sizeof pipe_data);
    close_fd(pipefd[0]); close_fd(pipefd[1]);

    abi_require(SC2(59, pipefd, 0) == 0);
    unsigned char payload[64], retry[64];
    for (int i = 0; i < 64; i++) payload[i] = 'A' + (i % 26);
    abi_require(SC3(64, pipefd[1], payload, sizeof payload) == 64);
    map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    untouched = 0xa5;
    fault[0] = (struct abi_iovec){(void *)(map + 4096 - 32), 64};
    fault[1] = (struct abi_iovec){&untouched, 1};
    ret = SC3(65, pipefd[0], fault, 2);
    for (int i = 0; i < 32; i++) evidence[i] = ((unsigned char *)fault[0].base)[i];
    evidence[32] = untouched;
    abi_record("readv.pipe-fault", ret, -1, -1, 0,
               evidence, sizeof evidence);
    ret = SC3(63, pipefd[0], retry, sizeof retry);
    abi_record("readv.pipe-retry", ret, -1, -1, 0,
               retry, ret > 0 ? (usize)ret : 0);
    abi_require(SC2(215, map, 8192) == 0);
    close_fd(pipefd[0]); close_fd(pipefd[1]);

    readv_file_edges();
    readv_pipe_edges();
    pipe_write_fragment_fault();
    readv_more_edges();
    pipe_merge_boundary();
    pipe_partial_tail_poll();
}
