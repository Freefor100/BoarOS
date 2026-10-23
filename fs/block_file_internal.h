#ifndef BOAROS_FS_BLOCK_FILE_INTERNAL_H
#define BOAROS_FS_BLOCK_FILE_INTERNAL_H
#include <kernel/block.h>
#include <kernel/files.h>
struct kernel_open_file_description;
struct kernel_mm;
struct kernel_uaccess_iovec;
int kernel_block_file_open(struct kernel_open_file_description *, uint64_t number, uint32_t flags);
void kernel_block_file_close(struct kernel_open_file_description *);
int kernel_block_file_sync(struct kernel_open_file_description *);
int kernel_block_file_pwrite(struct kernel_open_file_description *, uint64_t, const void *, size_t, size_t *);
int kernel_block_file_pread(struct kernel_open_file_description *, uint64_t, void *, size_t, size_t *);
/* The caller validates numeric user ranges before dispatching this view. */
enum kernel_files_status kernel_block_file_io(struct kernel_open_file_description *,
    struct kernel_mm *, const struct kernel_uaccess_iovec *, size_t, uint64_t,
    int writing, int positioned, uint64_t offset, int64_t *result);
int64_t kernel_block_file_ioctl(struct kernel_files *, struct kernel_open_file_description **,
    struct kernel_mm *, uint64_t command, uint64_t argument);
#endif
