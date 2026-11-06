#ifndef BOAROS_TCP_WRITE_SEGMENT_H
#define BOAROS_TCP_WRITE_SEGMENT_H

#include <stdint.h>

/* Sources stay pinned for one synchronous COPY admission. */
struct kernel_socket_write_segment {
    const void *data;
    uint32_t size;
};

#endif
