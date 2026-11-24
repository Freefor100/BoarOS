#ifndef BOAROS_KERNEL_IO_URING_ZCRX_H
#define BOAROS_KERNEL_IO_URING_ZCRX_H

#include <stdint.h>

#define KERNEL_IO_URING_ZCRX_MMAP_OFFSET UINT64_C(0x30000000)

struct kernel_heap;
struct kernel_mm;
struct kernel_uid_account;
struct kernel_io_uring_zcrx;
struct pbuf;

int kernel_io_uring_zcrx_register(struct kernel_heap *, struct kernel_mm *,
    struct kernel_uid_account *,uint64_t user_request,struct kernel_io_uring_zcrx **);
int kernel_io_uring_zcrx_release(struct kernel_io_uring_zcrx **);
unsigned kernel_io_uring_zcrx_refill(struct kernel_io_uring_zcrx *);
int kernel_io_uring_zcrx_capture(struct kernel_io_uring_zcrx *,const struct pbuf *,
    unsigned skip,unsigned count,uint64_t *offset);
int kernel_io_uring_zcrx_map(struct kernel_io_uring_zcrx *,uint64_t offset,uint64_t length);
int kernel_io_uring_zcrx_map_page(struct kernel_io_uring_zcrx *,uint64_t offset,uint64_t *physical);

struct kernel_open_file_description;
int kernel_socket_zcrx_receive(struct kernel_open_file_description **,
    struct kernel_io_uring_zcrx *, uint32_t count, uint64_t *offset);
#endif
