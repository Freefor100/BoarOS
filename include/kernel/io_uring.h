#ifndef BOAROS_KERNEL_IO_URING_H
#define BOAROS_KERNEL_IO_URING_H
#include <kernel/syscall.h>
#include <stddef.h>

struct kernel_open_file_description;
struct kernel_files;
struct kernel_mm;
struct kernel_mm_long_pin;

struct uring_sqe {
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t offset, address;
    uint32_t length, op_flags;
    uint64_t data;
    uint16_t buffer, personality;
    uint32_t file_index;
    uint64_t extra[2];
};
/* Stats and path-operation adapters use the kernel's stable SQE snapshot.
 * Opposite-end process or pathname capture is the operation's responsibility. */
int kernel_io_uring_extra_supported(unsigned opcode);
int kernel_io_uring_extra_needs_file(unsigned opcode);
int kernel_io_uring_extra_validate(const struct uring_sqe *);
int64_t kernel_io_uring_extra_execute(struct kernel_files *, struct kernel_mm *,
    const struct uring_sqe *, struct kernel_open_file_description **);

enum kernel_syscall_status syscall_handle_io_uring(struct kernel_task *,
    const struct kernel_syscall_request *, struct kernel_syscall_result *);
/* The OFD owns every page in the region until the last mapping is released. */
int kernel_io_uring_mmap_begin(struct kernel_open_file_description *, uint64_t offset,
    uint64_t rounded_length);
int kernel_io_uring_mmap_page(struct kernel_open_file_description *, uint64_t offset,
    int writable, uint64_t *physical);
int kernel_io_uring_release_file(struct kernel_open_file_description *);
int kernel_io_uring_abort(struct kernel_task *);
void kernel_io_uring_clock_changed(void);
int64_t kernel_files_uring_rw(struct kernel_files *, struct kernel_mm *,
    struct kernel_open_file_description **, unsigned op, uint64_t buffer,
    uint64_t count, int64_t offset, uint32_t flags);
int64_t kernel_files_uring_fixed_rw(struct kernel_files *,
    struct kernel_open_file_description *, int writing, const struct kernel_mm_long_pin *,
    size_t first, size_t count, int64_t offset, uint32_t flags);

#endif
