#include <stdlib.h>

#include <stddef.h>

static void swap_elements(unsigned char *left,
                          unsigned char *right,
                          size_t size)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        unsigned char temporary = left[index];

        left[index] = right[index];
        right[index] = temporary;
    }
}

static void sift_down(unsigned char *bytes,
                      size_t root,
                      size_t count,
                      size_t size,
                      int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child;
        size_t candidate = root;

        if (root > (count - 1U) / 2U) {
            return;
        }
        child = root * 2U + 1U;
        if (child < count &&
            compare(bytes + candidate * size, bytes + child * size) < 0) {
            candidate = child;
        }
        if (child + 1U < count &&
            compare(bytes + candidate * size,
                    bytes + (child + 1U) * size) < 0) {
            candidate = child + 1U;
        }
        if (candidate == root) {
            return;
        }

        swap_elements(bytes + root * size, bytes + candidate * size, size);
        root = candidate;
    }
}

void qsort(void *base,
           size_t count,
           size_t size,
           int (*compare)(const void *left, const void *right))
{
    unsigned char *bytes = base;
    size_t index;

    if (count < 2U || size == 0U || compare == 0) {
        return;
    }

    index = count / 2U;
    while (index != 0U) {
        index--;
        sift_down(bytes, index, count, size, compare);
    }
    index = count;
    while (index > 1U) {
        index--;
        swap_elements(bytes, bytes + index * size, size);
        sift_down(bytes, 0U, index, size, compare);
    }
}
