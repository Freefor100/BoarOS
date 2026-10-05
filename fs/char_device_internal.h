#ifndef BOAROS_FS_CHAR_DEVICE_INTERNAL_H
#define BOAROS_FS_CHAR_DEVICE_INTERNAL_H

#include <kernel/open_file.h>
#include <kernel/files.h>
#include <kernel/uaccess.h>
#include <stddef.h>
#include <stdint.h>

struct kernel_wait_queue;
struct kernel_mm;
struct kernel_task;
struct kernel_heap;
struct kernel_vfs_file;

/* Built-in character backends are selected by st_rdev, not pathname. */
struct kernel_char_device {
    uint64_t rdev;
    enum kernel_open_file_kind kind;
    uint8_t discard_writes;
    uint8_t read_once;
    uint8_t positioned;
    uint8_t empty_range_fault;
    uint8_t interruptible_bulk;
    /* 一份OFD持有一个实例；dup/fork与阻塞pin不重复打开设备。 */
    int (*open)(struct kernel_heap *heap, struct kernel_task *caller,
                uint32_t flags, const struct kernel_vfs_file *path, void **instance);
    void (*release)(void *instance);
    int (*read)(void *instance, struct kernel_task *caller, uint32_t flags,
                void *buffer, size_t size, size_t *bytes_read);
    int (*write)(void *instance, struct kernel_task *caller, uint32_t flags,
                 const void *buffer, size_t size, size_t *bytes_written);
    int (*ioctl)(void *instance, struct kernel_task *caller,
                 struct kernel_files *files, struct kernel_open_file_description **owner,
                 struct kernel_mm *mm, uint64_t command, uint64_t argument);
    uint32_t (*poll)(void *instance, uint32_t flags, uint32_t requested,
                     struct kernel_wait_queue **out_queue);
    /* 行规程的等待/continuation归完整请求，不能每个staging块重新开始。
     * owner/iov_owner移交给栈上活调用；正常恢复，强制取消消费以免栈退出泄漏。 */
    enum kernel_files_status (*readv)(void *instance, struct kernel_task *caller,
        struct kernel_files *files, struct kernel_open_file_description **owner, void **iov_owner,
        struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
        size_t iov_count, uint64_t count, uint32_t flags, int64_t *result);
    enum kernel_files_status (*writev)(void *instance, struct kernel_task *caller,
        struct kernel_files *files, struct kernel_open_file_description **owner, void **iov_owner,
        struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
        size_t iov_count, uint64_t count, uint32_t flags, int64_t *result);
};

const struct kernel_char_device *kernel_char_device_lookup(uint64_t rdev);
extern const struct kernel_char_device kernel_rtc_device;
int kernel_char_device_open(struct kernel_open_file_description *file,
                            uint32_t flags);

#endif
