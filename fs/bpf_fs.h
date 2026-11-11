#ifndef BOAROS_BPF_FS_H
#define BOAROS_BPF_FS_H
#include "vfs_objects.h"
struct bpf_fs_entry {
    uint64_t parent;
    const char *name;
    const void *data;
    size_t size;
    uint32_t mode;
};
int bpf_metadata_mount(struct kernel_heap *,uint64_t flags,uint64_t magic,
    const struct bpf_fs_entry *,size_t count,struct kernel_vfs_mount **);
#endif
