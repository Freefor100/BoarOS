/* Independent copy-and-zero oracle for a non-mutating normalized CRC. */
#include <ext4_crc32.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>

/* Reflected Castagnoli bit recurrence, independent of the production tables. */
static uint32_t bitwise_crc32c(uint32_t crc, const void *buffer, uint32_t size)
{
    const unsigned char *bytes = buffer;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
    return crc;
}

static void raw_crc32c(void)
{
    unsigned char bytes[16400];
    const uint32_t seeds[] = {0, UINT32_MAX, 0x1234abcd};
    const uint32_t lengths[] = {511, 512, 513, 1023, 1024, 1025,
        4095, 4096, 4097, 8191, 8192, 8193, 16383, 16384};
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(i * 17 + i / 251);
    assert((ext4_crc32c(UINT32_MAX, "123456789", 9) ^ UINT32_MAX) == 0xe3069283);
    assert((ext4_crc32(UINT32_MAX, "123456789", 9) ^ UINT32_MAX) == 0xcbf43926);
    for (unsigned seed = 0; seed < 3; seed++) for (unsigned offset = 0; offset < 16; offset++) {
        for (uint32_t size = 0; size <= 257; size++)
            assert(ext4_crc32c(seeds[seed], bytes + offset, size) ==
                bitwise_crc32c(seeds[seed], bytes + offset, size));
        for (unsigned size = 0; size < sizeof(lengths) / sizeof(lengths[0]); size++)
            assert(ext4_crc32c(seeds[seed], bytes + offset, lengths[size]) ==
                bitwise_crc32c(seeds[seed], bytes + offset, lengths[size]));
        for (uint32_t split = 0; split <= 257; split++)
            assert(ext4_crc32c(ext4_crc32c(seeds[seed], bytes + offset, split),
                bytes + offset + split, 257 - split) ==
                bitwise_crc32c(seeds[seed], bytes + offset, 257));
    }
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    size_t page = (size_t)page_size;
    unsigned char *area = mmap(NULL, 3 * page, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(area != MAP_FAILED && !mprotect(area + page, page, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < page; i++) area[page + i] = (unsigned char)(i * 43);
    assert(!mprotect(area + page, page, PROT_READ));
    for (uint32_t size = 0; size <= page; size++) {
        assert(ext4_crc32c(UINT32_MAX, area + page, size) ==
            bitwise_crc32c(UINT32_MAX, area + page, size));
        assert(ext4_crc32c(UINT32_MAX, area + 2 * page - size, size) ==
            bitwise_crc32c(UINT32_MAX, area + 2 * page - size, size));
    }
    assert(!munmap(area, 3 * page));
}

int main(void)
{
    const struct rlimit no_core = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
    raw_crc32c();
    unsigned char original[16384], oracle[16384];
    unsigned char *data = mmap(NULL, sizeof(original), PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(data != MAP_FAILED);
    const uint32_t sizes[] = {32, 64, 128, 256, 1024, 4096, 16384};
    const uint32_t seeds[] = {0, UINT32_MAX, 0x1234abcd};
    unsigned cases = 0;
    for (unsigned i = 0; i < sizeof(original); i++) original[i] = (unsigned char)(i * 13 + i / 251);
    memcpy(data, original, sizeof(original));
    assert(!mprotect(data, sizeof(original), PROT_READ));
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint32_t size = sizes[s];
        const uint32_t firsts[] = {0, 2, size / 2, size - 2};
        for (unsigned f = 0; f < 4; f++) for (unsigned two = 0; two < 3; two++) {
            uint32_t first = firsts[f], second = size;
            if (two == 1 && first + 4 <= size) second = first + 2;
            if (two == 2 && first + 4 <= size) second = size - 2;
            for (unsigned seed = 0; seed < 3; seed++) {
                memcpy(oracle, data, size);
                memset(oracle + first, 0, 2);
                if (second < size) memset(oracle + second, 0, 2);
                uint32_t actual = ext4_crc32c_zeroed(seeds[seed], data, size, first, second);
                assert(actual == bitwise_crc32c(seeds[seed], oracle, size));
                assert(!memcmp(data, original, sizeof(original)));
                cases++;
            }
        }
    }
    for (uint32_t size = 128; size <= 16384; size *= 2) {
        uint32_t first = 124, second = size > 128 ? 130 : size;
        memcpy(oracle, data, size);
        memset(oracle + first, 0, 2);
        if (second < size) memset(oracle + second, 0, 2);
        assert(ext4_crc32c_zeroed(UINT32_MAX, data, size, first, second) ==
            bitwise_crc32c(UINT32_MAX, oracle, size));
        cases++;
    }
    const uint32_t invalid[][3] = {{0,0,0},{1,0,1},{32,UINT32_MAX,32},{32,31,32},
        {32,30,31},{32,4,5},{32,4,UINT32_MAX}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) { (void)ext4_crc32c_zeroed(0, data, invalid[i][0], invalid[i][1], invalid[i][2]); _exit(0); }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP));
    }
    assert(!munmap(data, sizeof(original)));
    printf("PASS: CRC32C bitwise oracle, alignments, splits, guard pages; %u normalized cases on read-only input and 7 fatal layout boundaries\n", cases);
    return 0;
}
