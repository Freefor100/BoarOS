#include <kernel/errno.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

static int memory_read_at(void *context,
                          uint64_t offset,
                          void *buffer,
                          size_t size)
{
    const unsigned char *source = context;
    unsigned char *destination = buffer;
    size_t index;

    for (index = 0U; index < size; index++) {
        destination[index] = source[offset + index];
    }
    return 0;
}

int kernel_read_source_read_exact(const struct kernel_read_source *source,
                                  uint64_t offset,
                                  void *buffer,
                                  size_t size)
{
    if (source == 0 || source->read_at == 0 ||
        (buffer == 0 && size != 0U) || offset > source->size ||
        (uint64_t)size > source->size - offset) {
        return -KERNEL_EINVAL;
    }
    if (size == 0U) {
        return 0;
    }
    return source->read_at(source->context, offset, buffer, size);
}

int kernel_read_source_from_memory(const void *bytes,
                                   size_t size,
                                   struct kernel_read_source *source)
{
    if (bytes == 0 || source == 0) {
        return -KERNEL_EINVAL;
    }
    source->context = (void *)bytes;
    source->size = size;
    source->read_at = memory_read_at;
    return 0;
}
