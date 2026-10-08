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

int main(void)
{
    const struct rlimit no_core = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
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
                assert(actual == ext4_crc32c(seeds[seed], oracle, size));
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
            ext4_crc32c(UINT32_MAX, oracle, size));
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
    printf("PASS: %u normalized CRC cases on read-only input, nonadjacent inode fields and 7 fatal layout boundaries\n", cases);
    return 0;
}
