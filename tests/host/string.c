#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void *boaros_memcpy(void *, const void *, size_t);
void *boaros_memmove(void *, const void *, size_t);
void *boaros_memset(void *, int, size_t);
int boaros_memcmp(const void *, const void *, size_t);
static void fill(unsigned char *, size_t);

static int sign(int value) { return (value > 0) - (value < 0); }
static void comparisons(void)
{
    unsigned char left[4160], right[4160];
    const size_t large[] = {255, 256, 257, 4095, 4096, 4097};
    fill(left, sizeof(left));
    for (size_t a = 0; a < 2 * sizeof(uintptr_t); a++)
        for (size_t b = 0; b < 2 * sizeof(uintptr_t); b++)
            for (size_t which = 0; which < 130 + sizeof(large) / sizeof(large[0]); which++) {
                size_t size = which < 130 ? which : large[which - 130];
                memcpy(right + b, left + a, size);
                assert(!boaros_memcmp(left + a, right + b, size));
                if (!size) continue;
                size_t positions[] = {0, size / 2, size - 1};
                for (unsigned i = 0; i < 3; i++) {
                    size_t at = positions[i]; unsigned char saved = right[b + at];
                    right[b + at] ^= 0x80;
                    assert(sign(boaros_memcmp(left + a, right + b, size)) ==
                           sign(memcmp(left + a, right + b, size)));
                    assert(sign(boaros_memcmp(right + b, left + a, size)) ==
                           sign(memcmp(right + b, left + a, size)));
                    right[b + at] = saved;
                }
            }
}

static void fill(unsigned char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) bytes[i] = (unsigned char)(i * 37 + i / 251);
}

static void alignments(void)
{
    enum { CAPACITY = 4608 };
    unsigned char source[CAPACITY], actual[CAPACITY], expected[CAPACITY];
    const size_t large[] = {255, 256, 257, 511, 512, 513, 4095, 4096, 4097};
    const int values[] = {-257, -1, 0, 1, 0xa5, 0x1234};
    fill(source, sizeof(source));
    for (size_t from = 0; from < 2 * sizeof(uintptr_t); from++) {
        for (size_t to = 0; to < 2 * sizeof(uintptr_t); to++) {
            for (size_t which = 0; which < 130 + sizeof(large) / sizeof(large[0]); which++) {
                size_t size = which < 130 ? which : large[which - 130];
                memset(actual, 0x93, sizeof(actual));
                memset(expected, 0x93, sizeof(expected));
                memcpy(expected + to, source + from, size);
                assert(boaros_memcpy(actual + to, source + from, size) == actual + to);
                assert(!memcmp(actual, expected, sizeof(actual)));
            }
        }
        for (size_t size = 0; size <= 256; size++) {
            for (size_t value = 0; value < sizeof(values) / sizeof(values[0]); value++) {
                fill(actual, sizeof(actual));
                memcpy(expected, actual, sizeof(expected));
                memset(expected + from, values[value], size);
                assert(boaros_memset(actual + from, values[value], size) == actual + from);
                assert(!memcmp(actual, expected, sizeof(actual)));
            }
        }
    }
}

static void overlaps(void)
{
    unsigned char original[768], actual[768], expected[768];
    fill(original, sizeof(original));
    for (size_t from = 64; from < 64 + 2 * sizeof(uintptr_t); from++) {
        for (int distance = -33; distance <= 33; distance++) {
            size_t to = (size_t)((int)from + distance);
            for (size_t size = 0; size <= 257; size++) {
                memcpy(actual, original, sizeof(actual));
                memcpy(expected, original, sizeof(expected));
                memmove(expected + to, expected + from, size);
                assert(boaros_memmove(actual + to, actual + from, size) == actual + to);
                assert(!memcmp(actual, expected, sizeof(actual)));
            }
        }
    }
}

static unsigned char *guarded(size_t page)
{
    unsigned char *area = mmap(NULL, 3 * page, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(area != MAP_FAILED);
    assert(!mprotect(area + page, page, PROT_READ | PROT_WRITE));
    return area + page;
}

static void boundaries(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    size_t page = (size_t)page_size;
    unsigned char *source = guarded(page), *actual = guarded(page);
    unsigned char *expected = malloc(page);
    assert(expected);
    fill(source, page);
    assert(!mprotect(source, page, PROT_READ));
    for (size_t size = 0; size <= page; size++) {
        size_t starts[] = {0, page - size};
        for (unsigned from = 0; from < 2; from++) for (unsigned to = 0; to < 2; to++) {
            memset(actual, 0x7c, page);
            memset(expected, 0x7c, page);
            memcpy(expected + starts[to], source + starts[from], size);
            assert(boaros_memcpy(actual + starts[to], source + starts[from], size) ==
                   actual + starts[to]);
            assert(!memcmp(actual, expected, page));
            assert(!boaros_memcmp(source + starts[from], actual + starts[to], size));
        }
        for (unsigned end = 0; end < 2; end++) {
            fill(actual, page);
            memcpy(expected, actual, page);
            memset(expected + starts[end], 0xb6, size);
            assert(boaros_memset(actual + starts[end], 0xb6, size) == actual + starts[end]);
            assert(!memcmp(actual, expected, page));
            fill(actual, page);
            memcpy(expected, actual, page);
            memmove(expected + starts[end], expected + starts[1 - end], size);
            assert(boaros_memmove(actual + starts[end], actual + starts[1 - end], size) ==
                   actual + starts[end]);
            assert(!memcmp(actual, expected, page));
        }
    }
    free(expected);
    assert(!munmap(source - page, 3 * page));
    assert(!munmap(actual - page, 3 * page));
}

int main(void)
{
    alignments();
    comparisons();
    overlaps();
    boundaries();
    puts("RAM compare/copy/fill alignments, ordering, overlap directions and guard-page boundaries verified");
    return 0;
}
