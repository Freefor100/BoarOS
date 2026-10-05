#define _GNU_SOURCE
/* Shared RV64/Linux serial probe. Fixed contract sources:
 * references/linux @ f4cdf7ca9a1fdcca413157df19753f388a5a224e:
 * drivers/tty/tty_ioctl.c, tty_baudrate.c and serial/8250/8250_port.c.
 * FD 0 is the real controlling serial TTY supplied by tests/tty/riscv.py. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define T1_GET UINT32_C(0x5401)
#define T1_SET UINT32_C(0x5402)
#define T2_GET UINT32_C(0x802c542a)
#define T2_SET UINT32_C(0x402c542b)
#define T2_WAIT UINT32_C(0x402c542c)
#define T2_FLUSH UINT32_C(0x402c542d)
#define CBAUD UINT32_C(0x100f)
#define CIBAUD UINT32_C(0x100f0000)
#define BOTHER UINT32_C(0x1000)
#define B57600 UINT32_C(0x1001)
#define B115200 UINT32_C(0x1002)
#define B9600 UINT32_C(0x000d)

struct tty_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
struct tty_termios2 {
    struct tty_termios basic;
    uint32_t ispeed, ospeed;
};
struct guarded_termios {
    uint64_t before;
    struct tty_termios value;
    unsigned char after[16];
};
struct guarded_termios2 {
    uint64_t before;
    struct tty_termios2 value;
    unsigned char after[16];
};
_Static_assert(sizeof(struct tty_termios) == 36, "RV64 TCGETS ABI");
_Static_assert(sizeof(struct tty_termios2) == 44, "RV64 TCGETS2 ABI");
_Static_assert(offsetof(struct tty_termios2, ispeed) == 36, "RV64 ispeed offset");
_Static_assert(offsetof(struct tty_termios2, ospeed) == 40, "RV64 ospeed offset");
_Static_assert(offsetof(struct guarded_termios, after) == 8 + 36, "TCGETS tail guard");
_Static_assert(offsetof(struct guarded_termios2, after) == 8 + 44, "TCGETS2 tail guard");

static struct tty_termios2 original;
static struct tty_termios2 baseline;
static int saved;

static void fail(const char *name, long actual, long expected)
{
    int error = errno;
    /* B0期间先恢复线路，才通过同一UART输出失败诊断。 */
    if (saved) (void)ioctl(0, T2_SET, &original);
    dprintf(1, "TTY_RECORD FAIL %s actual=%ld expected=%ld errno=%d\n",
            name, actual, expected, error);
    _exit(1);
}
static void check(long actual, long expected, const char *name)
{ if (actual != expected) fail(name, actual, expected); }
static void record(const char *name, long value)
{ dprintf(1, "TTY_RECORD %s %ld\n", name, value); }
static void guard_check(uint64_t before, const unsigned char after[16], const char *name)
{
    check(before == UINT64_C(0xa5a5a5a5a5a5a5a5), 1, name);
    for (unsigned i = 0; i < 16; i++) check(after[i], 0xa5, name);
}
static struct tty_termios2 get2(void)
{
    struct guarded_termios2 buffer;
    memset(&buffer, 0xa5, sizeof(buffer));
    check(ioctl(0, T2_GET, &buffer.value), 0, "TCGETS2");
    guard_check(buffer.before, buffer.after, "TCGETS2 guard");
    return buffer.value;
}
static void old_get_matches(const struct tty_termios2 *modern, const char *name)
{
    struct guarded_termios buffer;
    memset(&buffer, 0xa5, sizeof(buffer));
    check(ioctl(0, T1_GET, &buffer.value), 0, "TCGETS");
    guard_check(buffer.before, buffer.after, "TCGETS36 guard");
    check(memcmp(&buffer.value, &modern->basic, sizeof(buffer.value)), 0, name);
    record(name, 1);
}
static struct tty_termios2 with_speed(uint32_t output_code, uint32_t input_code,
                                    uint32_t output, uint32_t input)
{
    struct tty_termios2 requested = baseline;
    requested.basic.cflag = (requested.basic.cflag & ~(CBAUD | CIBAUD)) |
                            output_code | (input_code << 16);
    requested.ospeed = output;
    requested.ispeed = input;
    return requested;
}
static struct tty_termios2 set2(uint32_t command, const struct tty_termios2 *requested,
                               const char *name)
{
    struct guarded_termios2 buffer;
    memset(&buffer, 0xa5, sizeof(buffer));
    buffer.value = *requested;
    check(ioctl(0, command, &buffer.value), 0, name);
    guard_check(buffer.before, buffer.after, "TCSETS2 argument guard");
    check(memcmp(&buffer.value, requested, sizeof(*requested)), 0, "setter input unchanged");
    record(name, 0);
    return get2();
}
static void speed_check(const struct tty_termios2 *value, uint32_t rate,
                        uint32_t output_code, uint32_t input_code, const char *name)
{
    check(value->ispeed, rate, "input nominal speed");
    check(value->ospeed, rate, "output nominal speed");
    check(value->basic.cflag & CBAUD, output_code, "output baud code");
    check((value->basic.cflag & CIBAUD) >> 16, input_code, "input baud code");
    record(name, rate);
}
static void bad_pointer(uint32_t command, const char *name)
{
    errno = 0;
    int result = ioctl(0, command, (void *)(uintptr_t)1);
    int error = errno;
    check(result, -1, name);
    check(error, EFAULT, name);
    record(name, error);
}
int main(void)
{
    original = get2();
    saved = 1;
    old_get_matches(&original, "t2-initial-layout36-44");
    record("t2-get44-tail-guard", 1);
    /* 不依赖console启动参数的默认速度；两侧显式建立相同测试基线。 */
    struct tty_termios2 requested = original;
    requested.basic.cflag = (requested.basic.cflag & ~(CBAUD | CIBAUD)) | B115200;
    requested.ispeed = requested.ospeed = 115200;
    baseline = set2(T2_SET, &requested, "t2-baseline-set");
    speed_check(&baseline, 115200, B115200, 0, "t2-baseline-115200");

    /* 标准编码决定速度，44字节结构中的数字不能覆盖B57600。 */
    requested = with_speed(B57600, 0, 1, UINT32_MAX);
    struct tty_termios2 observed = set2(T2_SET, &requested, "t2-standard-set");
    speed_check(&observed, 57600, B57600, 0, "t2-standard-ignores-numeric");
    old_get_matches(&observed, "t2-standard-old-get");

    requested = with_speed(BOTHER, 0, 12345, 54321);
    observed = set2(T2_SET, &requested, "t2-bother12345-set");
    speed_check(&observed, 12345, BOTHER, 0, "t2-bother12345-nominal");
    old_get_matches(&observed, "t2-bother-old-get");
    /* 旧36字节setter保留先前BOTHER数字速度，而非强制退回默认速度。 */
    check(ioctl(0, T1_SET, &observed.basic), 0, "old TCSETS preserves BOTHER");
    observed = get2();
    speed_check(&observed, 12345, BOTHER, 0, "t2-old-set-preserves-bother");

    requested = with_speed(BOTHER, 0, 57600, 0);
    observed = set2(T2_SET, &requested, "t2-bother57600-set");
    speed_check(&observed, 57600, B57600, 0, "t2-bother-exact-standard-code");

    requested = with_speed(B9600, B115200, 1, 2);
    observed = set2(T2_SET, &requested, "t2-cibaud-split-set");
    speed_check(&observed, 9600, B9600, B9600, "t2-cibaud-shared-line");
    old_get_matches(&observed, "t2-cibaud-old-get");

    /* 之前的stdout已接受输出由TCSETSW2完成drain；无需注入输入字符。 */
    requested = with_speed(B57600, 0, 7, 9);
    observed = set2(T2_WAIT, &requested, "t2-wait-set");
    speed_check(&observed, 57600, B57600, 0, "t2-wait-readback");
    requested = with_speed(BOTHER, 0, 12345, 0);
    observed = set2(T2_FLUSH, &requested, "t2-flush-empty-input-set");
    speed_check(&observed, 12345, BOTHER, 0, "t2-flush-readback");

    bad_pointer(T2_GET, "t2-get-bad-pointer");
    bad_pointer(T2_SET, "t2-set-bad-pointer");
    struct tty_termios2 after_fault = get2();
    check(memcmp(&after_fault, &observed, sizeof(observed)), 0, "bad setter preserves mode");
    record("t2-bad-setter-state-unchanged", 1);
    old_get_matches(&after_fault, "t2-final-old-get-tail-guard");

    /* B0不写任何诊断/记录；先取得配置再立即恢复，才继续用UART输出。 */
    requested = with_speed(0, 0, 123, 456);
    int b0_set = ioctl(0, T2_SET, &requested);
    struct guarded_termios2 zero;
    memset(&zero, 0xa5, sizeof(zero));
    int b0_get = ioctl(0, T2_GET, &zero.value);
    int restored = ioctl(0, T2_SET, &baseline);
    check(restored, 0, "restore after B0");
    check(b0_set, 0, "B0 setter");
    check(b0_get, 0, "B0 getter");
    guard_check(zero.before, zero.after, "B0 getter guard");
    speed_check(&zero.value, 0, 0, 0, "t2-b0-immediate-restore");
    struct tty_termios2 final = get2();
    speed_check(&final, 115200, B115200, 0, "t2-restored-115200");
    check(memcmp(&final, &baseline, sizeof(final)), 0, "restore test baseline termios2");
    old_get_matches(&final, "t2-restored-baseline-old-get");
    check(ioctl(0, T2_SET, &original), 0, "restore original termios2");
    final = get2();
    check(memcmp(&final, &original, sizeof(final)), 0, "saved original state restored");
    record("t2-original-state-restored", 1);
    old_get_matches(&final, "t2-original-old-get");
    saved = 0;
    dprintf(1, "TTY_PROBE_PASS\n");
    return 0;
}
