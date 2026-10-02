#ifndef BOAROS_HOST_LWIP_REASSEMBLY_CC_H
#define BOAROS_HOST_LWIP_REASSEMBLY_CC_H

#include <stdio.h>
#include <stdlib.h>

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(message) do { printf message; } while (0)
#define LWIP_PLATFORM_ASSERT(message) do { \
    fprintf(stderr, "lwIP assertion: %s\n", message); abort(); \
} while (0)

#endif
