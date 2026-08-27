#ifndef BOAROS_LIB_STDLIB_H
#define BOAROS_LIB_STDLIB_H

#include <stddef.h>

void qsort(void *base,
           size_t count,
           size_t size,
           int (*compare)(const void *left, const void *right));

#endif
