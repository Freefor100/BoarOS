#ifndef BOAROS_LWIP_STDIO_H
#define BOAROS_LWIP_STDIO_H

#include <stddef.h>

/* lwIP's disabled memory-overflow diagnostic still includes this header. */
int snprintf(char *buffer, size_t size, const char *format, ...);

#endif
