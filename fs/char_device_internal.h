#ifndef BOAROS_FS_CHAR_DEVICE_INTERNAL_H
#define BOAROS_FS_CHAR_DEVICE_INTERNAL_H

#include <kernel/open_file.h>
#include <stddef.h>
#include <stdint.h>

struct kernel_wait_queue;
struct kernel_mm;

/* Built-in character backends are selected by st_rdev, not pathname. */
struct kernel_char_device {
    uint64_t rdev;
    enum kernel_open_file_kind kind;
    uint8_t discard_writes;
    uint8_t read_once;
    uint8_t positioned;
    uint8_t empty_range_fault;
    uint8_t interruptible_bulk;
    int (*open)(void);
    void (*release)(void);
    int (*read)(uint32_t flags, void *buffer, size_t size, size_t *bytes_read);
    int (*write)(const void *buffer, size_t size, size_t *bytes_written);
    int (*ioctl)(struct kernel_mm *mm, uint64_t command, uint64_t argument);
    uint32_t (*poll)(uint32_t requested,
                     struct kernel_wait_queue **out_queue);
};

const struct kernel_char_device *kernel_char_device_lookup(uint64_t rdev);
extern const struct kernel_char_device kernel_rtc_device;

#endif
