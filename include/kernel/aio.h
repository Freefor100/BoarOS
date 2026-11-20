#ifndef BOAROS_KERNEL_AIO_H
#define BOAROS_KERNEL_AIO_H
#include <stdint.h>
struct kernel_mm;
struct kernel_memory_object;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_aio_context;
struct kernel_aio_direct;
struct kernel_mm_record;
int kernel_mm_aio_destination_begin(struct kernel_mm *);
void kernel_mm_aio_destination_end(struct kernel_mm_record *);
struct kernel_aio_context **kernel_mm_aio_contexts(struct kernel_mm *);
int kernel_mm_aio_map(struct kernel_mm *, uint64_t bytes, uint64_t *address,
    struct kernel_memory_object **);
void kernel_mm_aio_unmap(struct kernel_mm *, uint64_t address, uint64_t bytes,
    struct kernel_memory_object *);
int kernel_aio_ring_move(struct kernel_mm *, struct kernel_memory_object *, uint64_t, uint64_t);
void kernel_aio_exit(struct kernel_mm *);
uint64_t kernel_aio_quota(int maximum);
void kernel_aio_set_max(uint64_t maximum);
/* Return admission errors directly; completed request results may also be negative. */
int kernel_files_aio_rw(struct kernel_files *, struct kernel_mm *,
    struct kernel_open_file_description **, unsigned op, uint64_t buf,
    uint64_t count, int64_t offset, uint32_t flags, int64_t *result,
    struct kernel_aio_direct **);
int64_t kernel_files_aio_direct_run(struct kernel_open_file_description *, struct kernel_aio_direct *);
void kernel_files_aio_direct_free(struct kernel_aio_direct *);
int64_t kernel_files_aio_sync(struct kernel_open_file_description *, int data_only);
void kernel_event_fd_signal(struct kernel_open_file_description *);
#endif
