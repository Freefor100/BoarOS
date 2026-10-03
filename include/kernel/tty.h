#ifndef BOAROS_KERNEL_TTY_H
#define BOAROS_KERNEL_TTY_H

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_tty;
struct kernel_char_device;

/* Linux RV64 TCGETS布局；libc termios的附加字段不属于该ioctl。 */
struct kernel_tty_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
_Static_assert(sizeof(struct kernel_tty_termios) == 36, "RV64 termios ABI");
struct kernel_tty_rx { uint8_t character, status; };

struct kernel_tty_transport {
    /* 非睡眠、非分配；仅写当前可用硬件容量，返回实际接受字节。 */
    size_t (*transmit)(void *owner, const unsigned char *bytes, size_t size);
    int (*drained)(void *owner);
    int (*configure)(void *owner, struct kernel_tty_termios *settings);
    void (*kick)(void *owner);
    void (*last_close)(void *owner, uint32_t cflag);
};

int kernel_tty_create(struct kernel_heap *heap,
    const struct kernel_tty_transport *transport, void *transport_owner,
    struct kernel_tty **owner);
int kernel_tty_destroy(struct kernel_tty **owner);
void kernel_tty_publish_serial(struct kernel_tty *tty);
const struct kernel_char_device *kernel_tty_device_lookup(uint64_t rdev);
/* Worker上下文；receive的status使用8250 LSR的OE/PE/FE/BI位。 */
void kernel_tty_receive(struct kernel_tty *tty,
                        const struct kernel_tty_rx *input, size_t count);
size_t kernel_tty_service_output(struct kernel_tty *tty, size_t budget);
int kernel_tty_output_pending(struct kernel_tty *tty);
void kernel_tty_transport_ready(struct kernel_tty *tty);
void kernel_tty_shutdown(struct kernel_tty *tty);

#endif
