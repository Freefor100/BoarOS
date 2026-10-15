#include <stddef.h>
#include <stdint.h>

typedef uintptr_t memory_word __attribute__((__may_alias__));

/* 只用于普通RAM；整字读写保持对齐，首尾不借读相邻对象。 */
static inline __attribute__((always_inline)) void *copy_forward(
    void *destination, const void *source, size_t size)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    if (size == 0U) return destination;
    const size_t width = sizeof(memory_word);
    while (size != 0U && ((uintptr_t)output & (width - 1U)) != 0U) {
        *output++ = *input++;
        size--;
    }
    if (size == 0U) return destination;
    if (((uintptr_t)input & (width - 1U)) == 0U) {
        memory_word *words_out = (memory_word *)(void *)output;
        const memory_word *words_in = (const memory_word *)(const void *)input;
        while (size >= 4U * width) {
            words_out[0] = words_in[0];
            words_out[1] = words_in[1];
            words_out[2] = words_in[2];
            words_out[3] = words_in[3];
            words_out += 4;
            words_in += 4;
            size -= 4U * width;
        }
        while (size >= width) {
            *words_out++ = *words_in++;
            size -= width;
        }
        output = (unsigned char *)words_out;
        input = (const unsigned char *)words_in;
    } else if (size >= 2U * width) {
        size_t shift = (uintptr_t)input & (width - 1U);
        size_t prefix = width - shift;
        memory_word low = 0;
        /* RV/LA均为小端。只读区间内的对齐源字，合成对齐目标字；
         * 首尾不用向对象外取字，memmove前向重叠也保留已预读的边界。 */
        for (size_t i=0;i<prefix;i++) low |= (memory_word)input[i] << (8U*i);
        const memory_word *words_in=(const void *)(input+prefix);
        memory_word *words_out=(void *)output;
        while (size >= width+prefix) {
            memory_word high=*words_in++;
            *words_out++=low | (high << (8U*prefix));
            low=high >> (8U*shift);
            input+=width; size-=width;
        }
        output=(void *)words_out;
    }
    while (size != 0U) {
        *output++ = *input++;
        size--;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *left_bytes = left;
    const unsigned char *right_bytes = right;
    size_t index;

    for (index = 0U; index < size; index++) {
        if (left_bytes[index] != right_bytes[index]) {
            return (int)left_bytes[index] - (int)right_bytes[index];
        }
    }

    return 0;
}

void *memcpy(void *destination, const void *source, size_t size)
{
    return copy_forward(destination, source, size);
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *output = destination;
    const unsigned char *input = source;

    if (size == 0U || output == input) return destination;
    if ((uintptr_t)output < (uintptr_t)input ||
        (uintptr_t)output - (uintptr_t)input >= size) {
        return copy_forward(output, input, size);
    } else {
        const size_t width = sizeof(memory_word);
        output += size;
        input += size;
        while (size != 0U && ((uintptr_t)output & (width - 1U)) != 0U) {
            *--output = *--input;
            size--;
        }
        if (size == 0U) return destination;
        if (((uintptr_t)input & (width - 1U)) == 0U) {
            memory_word *words_out = (memory_word *)(void *)output;
            const memory_word *words_in = (const memory_word *)(const void *)input;
            /* 从高地址逐字提交，距离小于展开块时也不覆盖待读源字。 */
            while (size >= 4U * width) {
                words_out[-1] = words_in[-1];
                words_out[-2] = words_in[-2];
                words_out[-3] = words_in[-3];
                words_out[-4] = words_in[-4];
                words_out -= 4;
                words_in -= 4;
                size -= 4U * width;
            }
            while (size >= width) {
                *--words_out = *--words_in;
                size -= width;
            }
            output = (unsigned char *)words_out;
            input = (const unsigned char *)words_in;
        } else if (size >= 2U * width) {
            size_t suffix=(uintptr_t)input & (width-1U);
            memory_word high=0;
            for (size_t i=0;i<suffix;i++)
                high |= (memory_word)(input-suffix)[i] << (8U*(width-suffix+i));
            const memory_word *words_in=(const void *)(input-suffix);
            memory_word *words_out=(void *)output;
            while (size >= width+suffix) {
                memory_word low=*--words_in;
                *--words_out=high | (low >> (8U*suffix));
                high=low << (8U*(width-suffix));
                input-=width; size-=width;
            }
            output=(void *)words_out;
        }
        while (size != 0U) {
            *--output = *--input;
            size--;
        }
    }

    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    unsigned char *bytes = destination;
    const size_t width = sizeof(memory_word);
    const unsigned char byte = (unsigned char)value;
    while (size != 0U && ((uintptr_t)bytes & (width - 1U)) != 0U) {
        *bytes++ = byte;
        size--;
    }
    if (size == 0U) return destination;
    memory_word pattern = (UINTPTR_MAX / UINT8_MAX) * byte;
    memory_word *words = (memory_word *)(void *)bytes;
    while (size >= 4U * width) {
        words[0] = pattern;
        words[1] = pattern;
        words[2] = pattern;
        words[3] = pattern;
        words += 4;
        size -= 4U * width;
    }
    while (size >= width) {
        *words++ = pattern;
        size -= width;
    }
    bytes = (unsigned char *)words;
    while (size != 0U) {
        *bytes++ = byte;
        size--;
    }

    return destination;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strcpy(char *destination, const char *source)
{
    char *result = destination;

    do {
        *destination = *source;
        destination++;
    } while (*source++ != '\0');

    return result;
}

size_t strlen(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

int strncmp(const char *left, const char *right, size_t size)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        unsigned char left_byte = (unsigned char)left[index];
        unsigned char right_byte = (unsigned char)right[index];

        if (left_byte != right_byte) {
            return (int)left_byte - (int)right_byte;
        }
        if (left_byte == '\0') {
            return 0;
        }
    }

    return 0;
}

char *strncpy(char *destination, const char *source, size_t size)
{
    char *result = destination;
    size_t index = 0U;

    while (index < size && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    while (index < size) {
        destination[index] = '\0';
        index++;
    }

    return result;
}
