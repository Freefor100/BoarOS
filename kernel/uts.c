#include <kernel/uts.h>
#include <kernel/raw_lock.h>

#include <string.h>

#ifndef BOAROS_UTS_MACHINE
#error "BOAROS_UTS_MACHINE must name the Linux architecture"
#endif

static struct kernel_utsname initial_utsname = {
    .sysname = "Linux",
    .nodename = "boaros",
    /* glibc checks this ABI baseline before main; it is not feature coverage. */
    .release = "5.19.0-boaros",
    .version = "#1 BoarOS 0.1.0",
    .machine = BOAROS_UTS_MACHINE,
    .domainname = "(none)",
};
static struct kernel_raw_lock uts_lock = {
    .rank = KERNEL_RAW_RANK_OBJECT, .initialized = KERNEL_RAW_INITIALIZED,
};

_Static_assert(sizeof(struct kernel_utsname) == 390U,
               "Linux new_utsname ABI size must remain 390 bytes");

static char *uts_name(enum kernel_uts_name which)
{
    switch (which) {
    case KERNEL_UTS_HOSTNAME: return initial_utsname.nodename;
    case KERNEL_UTS_DOMAINNAME: return initial_utsname.domainname;
    }
    __builtin_trap();
}

void kernel_uts_snapshot(struct kernel_utsname *name)
{
    KERNEL_TASK_LOCK_SCOPE(guard, &uts_lock);
    memcpy(name, &initial_utsname, sizeof(*name));
}

void kernel_uts_name_set(enum kernel_uts_name which, const char *name, size_t length)
{
    if (length > KERNEL_UTS_NAME_MAX) __builtin_trap();
    KERNEL_TASK_LOCK_SCOPE(guard, &uts_lock);
    char *field = uts_name(which);
    if (length) memcpy(field, name, length);
    memset(field + length, 0, KERNEL_UTS_FIELD_SIZE - length);
}

int kernel_uts_name_write(enum kernel_uts_name which, uint64_t offset,
                          const char *name, size_t length)
{
    KERNEL_TASK_LOCK_SCOPE(guard, &uts_lock);
    char *field = uts_name(which);
    if (offset > strlen(field)) return 0;
    size_t available = KERNEL_UTS_NAME_MAX - (size_t)offset;
    if (length > available) length = available;
    if (length) memcpy(field + offset, name, length);
    /* proc_dostring keeps bytes beyond its new NUL; uname reports the full
     * fixed field, including that unchanged tail. */
    field[offset + length] = 0;
    return 1;
}
