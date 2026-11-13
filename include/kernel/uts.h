#ifndef BOAROS_KERNEL_UTS_H
#define BOAROS_KERNEL_UTS_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_UTS_NAME_MAX 64U
#define KERNEL_UTS_FIELD_SIZE (KERNEL_UTS_NAME_MAX + 1U)

struct kernel_utsname {
    char sysname[KERNEL_UTS_FIELD_SIZE];
    char nodename[KERNEL_UTS_FIELD_SIZE];
    char release[KERNEL_UTS_FIELD_SIZE];
    char version[KERNEL_UTS_FIELD_SIZE];
    char machine[KERNEL_UTS_FIELD_SIZE];
    char domainname[KERNEL_UTS_FIELD_SIZE];
};

enum kernel_uts_name {
    KERNEL_UTS_HOSTNAME,
    KERNEL_UTS_DOMAINNAME,
};

/* The initial UTS namespace is shared by all tasks. No pointer into its
 * mutable fields escapes the state lock. */
void kernel_uts_snapshot(struct kernel_utsname *name);
void kernel_uts_name_set(enum kernel_uts_name which, const char *name, size_t length);
/* A proc string write replaces the suffix at offset and terminates it. Return
 * zero when offset is beyond the current string and the position must stay. */
int kernel_uts_name_write(enum kernel_uts_name which, uint64_t offset,
                          const char *name, size_t length);

#endif
