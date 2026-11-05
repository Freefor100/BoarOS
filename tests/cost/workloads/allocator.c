#define COST_END_WAIT_ASYNC 1
#include "common.h"
#include <stdint.h>
#include <sys/mman.h>
#include <termios.h>

int main(void)
{
    const unsigned pages[] = {1, 64, 4096};
    const unsigned repetitions[] = {1024, 64, 4};
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    cost_init();
    /* 每阶段先预热同类操作；测量体只有固定页数/次数，不依赖耗时停止。 */
    for (unsigned phase = 0; phase < 3; phase++)
    {
        size_t bytes = (size_t)pages[phase] * (size_t)page_size;
        unsigned char *warm = mmap(0, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(warm != MAP_FAILED);
        for (size_t p = 0; p < bytes; p += page_size) warm[p] = 0x5a;
        CHECK(munmap(warm, bytes) == 0);
        char name[64];
        snprintf(name, sizeof(name), "allocator-%u-%u", pages[phase], repetitions[phase]);
        uint64_t checksum = 0;
        cost_begin();
        for (unsigned iteration = 0; iteration < repetitions[phase]; iteration++)
        {
            unsigned char *mapped = mmap(0, bytes, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            CHECK(mapped != MAP_FAILED);
            for (unsigned p = 0; p < pages[phase]; p++)
            {
                CHECK(mapped[(size_t)p * page_size] == 0);
                mapped[(size_t)p * page_size] = 0x5a;
            }
            for (unsigned p = 0; p < pages[phase]; p++)
            {
                CHECK(mapped[(size_t)p * page_size] == 0x5a);
                checksum += mapped[(size_t)p * page_size];
            }
            CHECK(munmap(mapped, bytes) == 0);
        }
        cost_end(name, 0, 0, 0, 0);
        CHECK(checksum == (uint64_t)pages[phase] * repetitions[phase] * 90);
        printf("COST ALLOCATOR WORK %s %u %u %llu\n", name, pages[phase],
               repetitions[phase], (unsigned long long)checksum);
    }
    puts("COST PASS allocator");
    fflush(stdout);
    CHECK(tcdrain(STDOUT_FILENO) == 0);
    return 0;
}
