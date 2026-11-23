#ifndef BOAROS_KERNEL_USERFAULTFD_H
#define BOAROS_KERNEL_USERFAULTFD_H

#include <kernel/files.h>

enum kernel_files_status kernel_files_userfaultfd_create(struct kernel_files *,
    struct kernel_mm *, uint32_t flags, int64_t *linux_result);

#endif
