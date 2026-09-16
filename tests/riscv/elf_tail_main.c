#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* A writable PT_LOAD with several file pages, a partial last file page,
 * and BSS. The ELF section table leaves nonzero bytes after p_filesz. */
static volatile unsigned char file_bytes[3 * 4096 + 137] = {
    [0] = 0x71,
    [4095] = 0x62,
    [4096] = 0x53,
    [3 * 4096 + 136] = 0x44,
};
static volatile unsigned char zero_bytes[3 * 4096 + 257];

int main(void)
{
    static const size_t positions[] = {0, 1, 256, 4095, 4096, 8191,
                                        8192, 12287, 12288, sizeof(zero_bytes) - 1};
    const char *error = "ELF BSS FAIL\n";

    if (file_bytes[0] != 0x71 || file_bytes[4095] != 0x62 ||
        file_bytes[4096] != 0x53 ||
        file_bytes[sizeof(file_bytes) - 1] != 0x44) {
        (void)write(1, error, 13);
        return 1;
    }
    for (size_t index = 0; index < sizeof(positions) / sizeof(positions[0]); index++) {
        if (zero_bytes[positions[index]] != 0) {
            (void)write(1, error, 13);
            return 1;
        }
    }
    for (size_t index = 0; index < sizeof(zero_bytes); index++) {
        if (zero_bytes[index] != 0) {
            (void)write(1, error, 13);
            return 1;
        }
    }
    (void)write(1, "ELF BSS PASS\n", 13);
    return 0;
}
