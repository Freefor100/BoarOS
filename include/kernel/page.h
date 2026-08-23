#ifndef BOAROS_KERNEL_PAGE_H
#define BOAROS_KERNEL_PAGE_H

#include <stdint.h>

#ifndef BOAROS_PAGE_SHIFT
#error "the architecture build must define BOAROS_PAGE_SHIFT"
#endif

#if BOAROS_PAGE_SHIFT < 3 || BOAROS_PAGE_SHIFT >= 64
#error "BOAROS_PAGE_SHIFT must fit a uint64_t link in each page"
#endif

#define BOAROS_PAGE_SIZE (UINT64_C(1) << BOAROS_PAGE_SHIFT)
#define BOAROS_PAGE_MASK (BOAROS_PAGE_SIZE - UINT64_C(1))

#endif
