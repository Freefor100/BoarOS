#ifndef BOAROS_KERNEL_NAMESPACE_H
#define BOAROS_KERNEL_NAMESPACE_H

#include <stdint.h>

#define KERNEL_NS_MNT UINT32_C(0x00020000)
#define KERNEL_NS_UTS UINT32_C(0x04000000)
#define KERNEL_NS_IPC UINT32_C(0x08000000)
#define KERNEL_NS_INITIAL_UTS_ID UINT64_C(4026531838)
#define KERNEL_NS_INITIAL_IPC_ID UINT64_C(4026531839)

struct kernel_heap;
struct kernel_open_file_description;

/* A task, an operation snapshot or a namespace FD holds one real reference.
 * Backends embed this first; the final owner calls destroy outside raw locks. */
struct kernel_namespace {
    uint32_t references;
    uint32_t type;
    uint64_t id;
    struct kernel_heap *heap;
    void (*destroy)(struct kernel_namespace *);
};

void kernel_namespace_init(struct kernel_namespace *,uint32_t,
    struct kernel_heap *,void (*destroy)(struct kernel_namespace *));
struct kernel_namespace *kernel_namespace_get(struct kernel_namespace *);
void kernel_namespace_put(struct kernel_namespace *);
int kernel_namespace_open(struct kernel_namespace *,struct kernel_heap *,uint32_t,
    struct kernel_open_file_description **);
/* Borrowed under an existing OFD owner. */
struct kernel_namespace *kernel_namespace_file(const struct kernel_open_file_description *);

#endif
