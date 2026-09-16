#include "abi.h"

static unsigned records;
static char line[2048];
static usize used;
long abi_call(long nr, long a, long b, long c, long d, long e, long f)
{
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
                      "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}
void abi_exit(long code) { SC1(93, code); for (;;) {} }
void abi_require(int condition)
{
    if (!condition) {
        static const char error[] = "ABI ERROR setup\n";
        SC3(64, 1, error, sizeof(error) - 1);
        abi_exit(99);
    }
}
static void text(const char *s) { while (*s) line[used++] = *s++; }
static void number(long value)
{
    char digits[24]; unsigned n = 0;
    unsigned long magnitude = (unsigned long)value;
    if (value < 0) { line[used++] = '-'; magnitude = 0 - magnitude; }
    do { digits[n++] = '0' + magnitude % 10; magnitude /= 10; } while (magnitude);
    while (n) line[used++] = digits[--n];
}
static void flush(void)
{
    line[used++] = '\n';
    abi_require(SC3(64, 1, line, used) == (long)used);
    used = 0;
}
void abi_record(const char *id, long ret, long size, long offset, long signal,
                const void *data, usize length)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *bytes = data;
    abi_require(length <= 512);
    text("ABI "); text(id); text(" "); number(ret); text(" ");
    number(ret < 0 && ret >= -4095 ? -ret : 0);
    text(" "); number(size); text(" "); number(offset);
    text(" "); number(signal); text(" ");
    if (!length) text("-");
    for (usize i = 0; i < length; ++i) {
        line[used++] = hex[bytes[i] >> 4]; line[used++] = hex[bytes[i] & 15];
    }
    flush(); ++records;
}
long abi_open(const char *path, long flags) { return SC4(56, -100, path, flags, 0600); }
long abi_size(long fd)
{
    struct abi_stat st;
    long result = SC2(80, fd, &st);
    abi_require(result == 0);
    return st.size;
}
long abi_offset(long fd) { return SC3(62, fd, 0, 1); }
static void close_fd(long fd) { abi_require(SC1(57, fd) == 0); }
static void mode_cases(void)
{
    const char *names[3][4] = {
        {"rd.open", "rd.read", "rd.pread", "rd.mmap"},
        {"wr.open", "wr.read", "wr.pread", "wr.mmap"},
        {"rw.open", "rw.read", "rw.pread", "rw.mmap"}
    };
    for (long mode = 0; mode < 3; ++mode) {
        long fd = abi_open("/data", mode); unsigned char data[8];
        abi_record(names[mode][0], fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
        abi_require(fd >= 0);
        long ret = SC3(63, fd, data, sizeof(data));
        abi_record(names[mode][1], ret, abi_size(fd), abi_offset(fd), 0, data, ret > 0 ? ret : 0);
        ret = SC4(67, fd, data, sizeof(data), 11);
        abi_record(names[mode][2], ret, abi_size(fd), abi_offset(fd), 0, data, ret > 0 ? ret : 0);
        long map = CALL(222, 0, 4096, 1, 2, fd, 0);
        abi_record(names[mode][3], map < 0 ? map : 0, abi_size(fd), abi_offset(fd), 0,
                   (void *)map, map >= 0 ? 8 : 0);
        if (map >= 0) abi_require(SC2(215, map, 4096) == 0);
        close_fd(fd);
    }
}
static void sparse_cases(void)
{
    long fd = abi_open("/sparse", 2 | 64 | 512); unsigned char data[80];
    abi_require(fd >= 0);
    abi_require(SC3(62, fd, 8195, 0) == 8195);
    long ret = SC3(64, fd, "end", 3);
    abi_record("sparse.write", ret, abi_size(fd), abi_offset(fd), 0, 0, 0);
    ret = SC4(67, fd, data, sizeof(data), 8150);
    abi_record("sparse.pread", ret, abi_size(fd), abi_offset(fd), 0, data, ret > 0 ? ret : 0);
    long map = CALL(222, 0, 16384, 1, 2, fd, 0);
    abi_require(map >= 0);
    abi_record("sparse.mmap", 0, abi_size(fd), abi_offset(fd), 0, (void *)(map + 8150), 80);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) { volatile unsigned char byte = *(volatile unsigned char *)(map + 12288); (void)byte; abi_exit(0); }
    int status = 0; abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("sparse.sigbus", 0, abi_size(fd), abi_offset(fd), status, 0, 0);
    abi_require(SC2(215, map, 16384) == 0); close_fd(fd);
}
static void partial_cases(void)
{
    static const int prefixes[] = {0, 1, 32, 63, 64, 65};
    static const char *names[4][6] = {
        {"write.0","write.1","write.32","write.63","write.64","write.65"},
        {"writev.0","writev.1","writev.32","writev.63","writev.64","writev.65"},
        {"append.0","append.1","append.32","append.63","append.64","append.65"},
        {"appendv.0","appendv.1","appendv.32","appendv.63","appendv.64","appendv.65"}
    };
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    abi_require(SC3(226, map + 4096, 4096, 0) == 0);
    for (int mode = 0; mode < 4; ++mode) for (int index = 0; index < 6; ++index) {
        int prefix = prefixes[index]; unsigned char *buf = (void *)(map + 4096 - prefix);
        for (int j = 0; j < prefix; ++j) buf[j] = 'a' + (j % 26);
        long fd = abi_open("/partial", 2 | 64 | 512);
        abi_require(fd >= 0);
        abi_require(SC3(64, fd, "seed", 4) == 4);
        close_fd(fd);
        fd = abi_open("/partial", 2 | (mode >= 2 ? 1024 : 0));
        abi_require(fd >= 0);
        abi_require(SC3(62, fd, 1, 0) == 1);
        struct abi_iovec vector = {buf, (usize)prefix + 1};
        long ret = mode & 1 ? SC3(66, fd, &vector, 1) : SC3(64, fd, buf, prefix + 1);
        long offset = abi_offset(fd), size = abi_size(fd);
        unsigned char content[256]; long count = SC4(67, fd, content, sizeof(content), 0);
        abi_require(count >= 0);
        abi_record(names[mode][index], ret, size, offset, 0, content, count);
        close_fd(fd);
    }
    abi_require(SC2(215, map, 8192) == 0);
}
void abi_main(void)
{
    text("ABI BEGIN 1"); flush();
    mode_cases(); sparse_cases(); partial_cases(); abi_truncate_cases();
    abi_timestamp_cases(); abi_readv_cases();
    text("ABI END "); number(records); flush();
    SC0(81); /* Linux sync; unsupported on BoarOS, outside observed cases. */
    CALL(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    abi_exit(42);
}
