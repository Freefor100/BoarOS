#ifndef BOAROS_MM_USERFAULTFD_INTERNAL_H
#define BOAROS_MM_USERFAULTFD_INTERNAL_H

#include <kernel/mm.h>
#include <kernel/raw_lock.h>
#include <kernel/scheduler.h>
#include <kernel/wait_internal.h>
#include "../third_party/lwext4/include/misc/tree.h"

enum { UFFD_USER_MODE_ONLY = 1 };
struct uffd_message {
    uint8_t event, reserved[7];
    uint64_t flags, address, reserved_tail;
};
struct kernel_userfault_page;
RB_HEAD(userfault_pages, kernel_userfault_page);

struct kernel_userfault_ctx {
    struct kernel_mm_pagemap target;
    struct kernel_userfault_ctx *next;
    struct kernel_heap *heap;
    struct kernel_raw_lock lock;
    struct kernel_wait_queue ready;
    struct userfault_pages pages;
    struct kernel_userfault_page *pending, **pending_tail;
    struct kernel_userfault_page *spare;
    uint64_t zero_page;
    unsigned references;
    unsigned flags;
    int initialized, dead;
};

struct kernel_userfault_wait {
    struct kernel_userfault_page *page;
    struct kernel_wait_token token;
    struct kernel_wait_cursor ready;
};

enum kernel_userfault_origin {
    KERNEL_USERFAULT_INTERNAL,
    KERNEL_USERFAULT_COPY,
    KERNEL_USERFAULT_TRAP,
};

int kernel_mm_userfault_attach(struct kernel_mm *, struct kernel_userfault_ctx *);
void kernel_mm_userfault_detach(struct kernel_userfault_ctx *);
int kernel_mm_userfault_register(struct kernel_userfault_ctx *,
    uint64_t start, uint64_t end, int enable);
int kernel_mm_userfault_check_fill(struct kernel_userfault_ctx *, uint64_t start, uint64_t end,
    uint64_t *generation);
int kernel_mm_userfault_fill(struct kernel_userfault_ctx *, uint64_t destination, uint64_t end,
    uint64_t *generation, uint64_t physical, int cow);

void kernel_userfault_context_put(struct kernel_userfault_ctx *);
int kernel_userfault_next(struct kernel_userfault_ctx *, struct uffd_message *, int no_wait);
void kernel_userfault_wake(struct kernel_userfault_ctx *, uint64_t start, uint64_t end);

#endif
