#include <stddef.h>
#include <stdint.h>

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
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    for (index = 0U; index < size; index++) {
        output[index] = input[index];
    }

    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *output = destination;
    const unsigned char *input = source;

    if ((uintptr_t)output < (uintptr_t)input) {
        size_t index;

        for (index = 0U; index < size; index++) {
            output[index] = input[index];
        }
    } else if ((uintptr_t)output > (uintptr_t)input) {
        while (size != 0U) {
            size--;
            output[size] = input[size];
        }
    }

    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = (unsigned char)value;
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
