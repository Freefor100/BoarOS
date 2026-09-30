#include "char_device_internal.h"

#include <arch/riscv/context.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/random.h>
#include <kernel/files.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <string.h>

static struct kernel_char_device devices[5];
static uint8_t devices_ready;

static int empty_read(uint32_t flags, void *buffer, size_t size,
                      size_t *bytes_read)
{
    (void)flags;
    (void)buffer;
    (void)size;
    *bytes_read = 0U;
    return 0;
}

static int zero_read(uint32_t flags, void *buffer, size_t size,
                     size_t *bytes_read)
{
    (void)flags;
    memset(buffer, 0, size);
    *bytes_read = size;
    return 0;
}

static int console_read(uint32_t flags, void *buffer, size_t size,
                        size_t *bytes_read)
{
    return kernel_console_read_buffer(flags, buffer, size, bytes_read);
}

static int discard_write(const void *buffer, size_t size,
                         size_t *bytes_written)
{
    (void)buffer;
    *bytes_written = size;
    return 0;
}

static int console_write(const void *buffer, size_t size,
                         size_t *bytes_written)
{
    const unsigned char *bytes = buffer;
    for (size_t i = 0U; i < size; i++) kernel_console_putc((char)bytes[i]);
    *bytes_written = size;
    return 0;
}

static uint32_t memory_poll(uint32_t requested,
                            struct kernel_wait_queue **out_queue)
{
    (void)requested;
    if (out_queue) *out_queue = 0;
    return KERNEL_POLLIN | KERNEL_POLLOUT | KERNEL_POLLRDNORM |
           KERNEL_POLLWRNORM;
}

static int urandom_read(uint32_t flags, void *buffer, size_t size,
                        size_t *bytes_read)
{
    (void)flags;
    (void)kernel_random_fill(buffer, size);
    *bytes_read = size;
    return 0;
}

static int random_read(uint32_t flags, void *buffer, size_t size,
                       size_t *bytes_read)
{
    int result = kernel_random_wait_ready(flags & KERNEL_FILES_O_NONBLOCK);
    if (result == -KERNEL_EINTR) {
        kernel_signal_note_syscall_restart(kernel_task_current());
        result = -KERNEL_ERESTARTSYS;
    }
    if (result) { *bytes_read = 0; return result; }
    return urandom_read(flags, buffer, size, bytes_read);
}

static int random_write(const void *buffer, size_t size, size_t *bytes_written)
{
    kernel_random_mix(buffer, size, 0);
    *bytes_written = size;
    return 0;
}

static uint32_t random_poll(uint32_t requested,
                            struct kernel_wait_queue **out_queue)
{
    (void)requested;
    if (out_queue) *out_queue = kernel_random_wait_queue();
    return kernel_random_ready() ? KERNEL_POLLIN | KERNEL_POLLRDNORM
                                 : KERNEL_POLLOUT | KERNEL_POLLWRNORM;
}

static int random_ioctl(struct kernel_mm *mm, uint64_t command, uint64_t argument)
{
    switch ((unsigned)command) {
    case 0x80045200U: {
        int bits = (int)kernel_random_credited_bits();
        size_t copied = 0;
        return kernel_copy_to_user(mm, argument, &bits, sizeof(bits), &copied) ==
                   KERNEL_UACCESS_STATUS_OK && copied == sizeof(bits)
                   ? 0 : -KERNEL_EFAULT;
    }
    case 0x40045201U: /* RNDADDTOENTCNT */
    case 0x40085203U: /* RNDADDENTROPY */
    case 0x5204U: /* RNDZAPENTCNT */
    case 0x5206U: /* RNDCLEARPOOL */
    case 0x5207U: /* RNDRESEEDCRNG */
        return -KERNEL_ENOTSUP;
    default:
        return -KERNEL_EINVAL;
    }
}

static void initialize_devices(void)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!devices_ready) {
        volatile struct kernel_char_device *null = &devices[0];
        null->rdev = UINT64_C(0x103);
        null->kind = KERNEL_OPEN_FILE_KIND_NULL;
        null->discard_writes = 1U;
        null->positioned = 1U;
        null->empty_range_fault = 1U;
        null->read = empty_read;
        null->write = discard_write;
        null->poll = memory_poll;
        volatile struct kernel_char_device *zero = &devices[1];
        zero->rdev = UINT64_C(0x105);
        zero->kind = KERNEL_OPEN_FILE_KIND_ZERO;
        zero->discard_writes = 1U;
        zero->positioned = 1U;
        zero->empty_range_fault = 1U;
        zero->read = zero_read;
        zero->write = discard_write;
        zero->poll = memory_poll;
        volatile struct kernel_char_device *console = &devices[2];
        console->rdev = UINT64_C(0x501);
        console->kind = KERNEL_OPEN_FILE_KIND_CONSOLE;
        console->read_once = 1U;
        console->read = console_read;
        console->write = console_write;
        console->poll = kernel_console_poll;
        for (unsigned i = 3; i < 5; i++) {
            volatile struct kernel_char_device *rng = &devices[i];
            rng->rdev = i == 3 ? UINT64_C(0x108) : UINT64_C(0x109);
            rng->kind = i == 3 ? KERNEL_OPEN_FILE_KIND_RANDOM
                              : KERNEL_OPEN_FILE_KIND_URANDOM;
            rng->positioned = 1U;
            rng->empty_range_fault = 1U;
            rng->ioctl = random_ioctl;
            rng->interruptible_bulk = 1U;
            rng->read = i == 3 ? random_read : urandom_read;
            rng->write = random_write;
            rng->poll = i == 3 ? random_poll : memory_poll;
        }
        devices_ready = 1U;
    }
    riscv_interrupt_restore(irq);
}

const struct kernel_char_device *kernel_char_device_lookup(uint64_t rdev)
{
    initialize_devices();
    for (size_t i = 0U; i < sizeof(devices) / sizeof(devices[0]); i++)
        if (devices[i].rdev == rdev) return &devices[i];
    return 0;
}
