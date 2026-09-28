#include "char_device_internal.h"

#include <arch/riscv/context.h>
#include <kernel/console.h>
#include <kernel/errno.h>

#include <string.h>

static struct kernel_char_device devices[3];
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
