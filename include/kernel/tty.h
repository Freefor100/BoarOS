#ifndef BOAROS_KERNEL_TTY_H
#define BOAROS_KERNEL_TTY_H

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_tty;
struct kernel_char_device;
struct kernel_task;
struct kernel_vfs_file;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_mm;

enum kernel_tty_peer_state {
    KERNEL_TTY_PEER_LIVE,
    KERNEL_TTY_PEER_ABSENT, /* master仍可在slave重开后恢复；空读返回EIO。 */
    KERNEL_TTY_PEER_CLOSED
};

/* Linux RV64 TCGETS布局；libc termios的附加字段不属于该ioctl。 */
struct kernel_tty_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
_Static_assert(sizeof(struct kernel_tty_termios) == 36, "RV64 termios ABI");
struct kernel_tty_termios2 {
    struct kernel_tty_termios basic;
    uint32_t ispeed, ospeed;
};
_Static_assert(sizeof(struct kernel_tty_termios2) == 44, "RV64 termios2 ABI");
struct kernel_tty_rx { uint8_t character, status; };

struct kernel_tty_transport {
    /* 非睡眠、非分配；仅写当前可用硬件容量，返回实际接受字节。 */
    size_t (*transmit)(void *owner, const unsigned char *bytes, size_t size);
    int (*drained)(void *owner);
    int (*configure)(void *owner, struct kernel_tty_termios *settings);
    int (*configure2)(void *owner, struct kernel_tty_termios2 *settings);
    void (*kick)(void *owner);
    void (*last_close)(void *owner, uint32_t cflag);
    /* 内部base引用不调用此hook；OFD/ctty引用才保活配对。 */
    void (*external_ref)(void *owner, int delta);
    void (*opened)(void *owner);
    int (*open_check)(void *owner);
    enum kernel_tty_peer_state (*peer_state)(void *owner);
    void (*progress)(void *owner);
    void (*event)(void *owner, unsigned flags);
    struct kernel_tty *(*peer)(void *owner);
    int (*ioctl)(void *owner, struct kernel_task *caller,
        struct kernel_files *files, struct kernel_open_file_description **file,
        struct kernel_mm *mm, uint64_t command, uint64_t argument);
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
void kernel_tty_configure_identity(struct kernel_tty *tty, uint64_t rdev, int raw);
int kernel_tty_open(struct kernel_tty *tty, struct kernel_heap *heap,
    struct kernel_task *caller, uint32_t flags, int automatic_ctty, void **instance);
const struct kernel_char_device *kernel_tty_device_template(void);
void *kernel_tty_instance_owner(void *instance);
unsigned kernel_tty_open_count(struct kernel_tty *tty);
int kernel_tty_base_only(struct kernel_tty *tty);
/* 返回已经处理的前缀；raw容量或延后flush阻塞时保留后缀给transport。 */
size_t kernel_tty_receive_nonblocking(struct kernel_tty *tty,
    const struct kernel_tty_rx *input, size_t count);
void kernel_tty_packet_event(struct kernel_tty *tty, unsigned flags);
void kernel_tty_discard(struct kernel_tty *tty);

#endif
