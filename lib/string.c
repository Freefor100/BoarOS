#include <stddef.h>

void *memset(void *destination, int value, size_t size)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = (unsigned char)value;
    }

    return destination;
}
