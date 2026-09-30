#ifndef BOAROS_KERNEL_BLAKE2S_H
#define BOAROS_KERNEL_BLAKE2S_H
#include <stddef.h>
#include <stdint.h>
void kernel_blake2s(const void *data, size_t size, uint8_t output[32]);
#endif
