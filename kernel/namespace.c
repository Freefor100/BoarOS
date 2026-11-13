#include <kernel/namespace.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include "../fs/open_file_internal.h"
#include "../fs/char_device_internal.h"

void kernel_namespace_init(struct kernel_namespace *ns,uint32_t type,
    struct kernel_heap *heap,void (*destroy)(struct kernel_namespace *))
{
    static uint64_t next_id=UINT64_C(4026531840);
    uint64_t id=__atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED);
    if (!id || id==UINT64_MAX || !heap || !destroy ||
        (type!=KERNEL_NS_MNT && type!=KERNEL_NS_UTS && type!=KERNEL_NS_IPC))
        __builtin_trap();
    *ns=(struct kernel_namespace){.references=1,.type=type,.id=id,.heap=heap,
        .destroy=destroy};
}

struct kernel_namespace *kernel_namespace_get(struct kernel_namespace *ns)
{
    if (!ns) return NULL;
    uint32_t references=__atomic_load_n(&ns->references,__ATOMIC_RELAXED);
    do {
        if (!references || references==UINT32_MAX) __builtin_trap();
    } while (!__atomic_compare_exchange_n(&ns->references,&references,references+1,
        1,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    return ns;
}

void kernel_namespace_put(struct kernel_namespace *ns)
{
    if (!ns) return;
    uint32_t old=__atomic_fetch_sub(&ns->references,1,__ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    if (old!=1) return;
    if (!ns->destroy) __builtin_trap(); /* Initial namespaces retain a boot owner. */
    ns->destroy(ns);
}

static void namespace_file_release(void *instance)
{ kernel_namespace_put(instance); }

static uint32_t namespace_file_poll(void *instance,uint32_t flags,uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)instance;(void)requested;(void)queue;
    return flags&010000000U?KERNEL_POLLNVAL:
        KERNEL_POLLIN|KERNEL_POLLOUT|KERNEL_POLLRDNORM|KERNEL_POLLWRNORM;
}

static const struct kernel_char_device namespace_ops={
    .release=namespace_file_release,.poll=namespace_file_poll,
};

int kernel_namespace_open(struct kernel_namespace *ns,struct kernel_heap *heap,
    uint32_t flags,struct kernel_open_file_description **owner)
{
    if (flags&0200000U) return -KERNEL_ENOTDIR;
    /* nsfs stashed inodes are immutable, so write/truncate admission fails
     * with EPERM before ordinary mode permissions, even for root. */
    if (!(flags&010000000U) && ((flags&3U) || (flags&01000U))) return -KERNEL_EPERM;
    struct kernel_open_file_description *file=NULL;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*file),(void **)&file)) return -KERNEL_ENOMEM;
    file->heap=heap;
    file->references=1;
    file->kind=KERNEL_OPEN_FILE_KIND_NAMESPACE;
    file->proc_identity=ns->id;
    file->open_flags=flags;
    file->credentials=kernel_credentials_get(kernel_credentials_current());
    file->device=&namespace_ops;
    file->device_instance=kernel_namespace_get(ns);
    file->device_opened=1;
    kernel_mutex_init(&file->offset_lock,10,(uintptr_t)file);
    *owner=file;
    return 0;
}

struct kernel_namespace *kernel_namespace_file(const struct kernel_open_file_description *file)
{
    return file && file->kind==KERNEL_OPEN_FILE_KIND_NAMESPACE?file->device_instance:NULL;
}
