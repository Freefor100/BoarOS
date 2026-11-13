#include <kernel/uts.h>
#include <kernel/raw_lock.h>
#include <kernel/namespace.h>
#include <kernel/heap.h>
#include <kernel/errno.h>

#include <string.h>

#ifndef BOAROS_UTS_MACHINE
#error "BOAROS_UTS_MACHINE must name the Linux architecture"
#endif

struct kernel_uts_namespace {
    struct kernel_namespace base;
    struct kernel_raw_lock lock;
    struct kernel_utsname name;
};
static struct kernel_uts_namespace initial_uts = {
    .base={.references=1,.type=KERNEL_NS_UTS,.id=KERNEL_NS_INITIAL_UTS_ID},
    .lock={.rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED},
    .name={
        .sysname = "Linux",
        .nodename = "boaros",
        /* glibc checks this ABI baseline before main; it is not feature coverage. */
        .release = "5.19.0-boaros",
        .version = "#1 BoarOS 0.1.0",
        .machine = BOAROS_UTS_MACHINE,
        .domainname = "(none)",
    },
};

_Static_assert(sizeof(struct kernel_utsname) == 390U,
               "Linux new_utsname ABI size must remain 390 bytes");

static struct kernel_uts_namespace *uts_namespace(struct kernel_namespace *base)
{
    if (!base || base->type!=KERNEL_NS_UTS) __builtin_trap();
    return (struct kernel_uts_namespace *)base;
}

struct kernel_namespace *kernel_uts_initial(void)
{ return &initial_uts.base; }

static void uts_destroy(struct kernel_namespace *base)
{ kernel_heap_release(base->heap,base); }

int kernel_uts_clone(struct kernel_namespace *source,struct kernel_heap *heap,
    struct kernel_namespace **owner)
{
    struct kernel_uts_namespace *copy=NULL;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*copy),(void **)&copy)) return -KERNEL_ENOMEM;
    kernel_namespace_init(&copy->base,KERNEL_NS_UTS,heap,uts_destroy);
    kernel_raw_lock_init(&copy->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_uts_snapshot(source,&copy->name);
    *owner=&copy->base;
    return 0;
}

static char *uts_name(struct kernel_uts_namespace *ns,enum kernel_uts_name which)
{
    switch (which) {
    case KERNEL_UTS_HOSTNAME: return ns->name.nodename;
    case KERNEL_UTS_DOMAINNAME: return ns->name.domainname;
    }
    __builtin_trap();
}

void kernel_uts_snapshot(struct kernel_namespace *base,struct kernel_utsname *name)
{
    struct kernel_uts_namespace *ns=uts_namespace(base);
    KERNEL_TASK_LOCK_SCOPE(guard,&ns->lock);
    memcpy(name,&ns->name,sizeof(*name));
}

void kernel_uts_name_set(struct kernel_namespace *base,enum kernel_uts_name which,
    const char *name,size_t length)
{
    if (length > KERNEL_UTS_NAME_MAX) __builtin_trap();
    struct kernel_uts_namespace *ns=uts_namespace(base);
    KERNEL_TASK_LOCK_SCOPE(guard,&ns->lock);
    char *field=uts_name(ns,which);
    if (length) memcpy(field, name, length);
    memset(field + length, 0, KERNEL_UTS_FIELD_SIZE - length);
}

int kernel_uts_name_write(struct kernel_namespace *base,enum kernel_uts_name which, uint64_t offset,
                          const char *name, size_t length)
{
    struct kernel_uts_namespace *ns=uts_namespace(base);
    KERNEL_TASK_LOCK_SCOPE(guard,&ns->lock);
    char *field=uts_name(ns,which);
    if (offset > strlen(field)) return 0;
    size_t available = KERNEL_UTS_NAME_MAX - (size_t)offset;
    if (length > available) length = available;
    if (length) memcpy(field + offset, name, length);
    /* proc_dostring keeps bytes beyond its new NUL; uname reports the full
     * fixed field, including that unchanged tail. */
    field[offset + length] = 0;
    return 1;
}
