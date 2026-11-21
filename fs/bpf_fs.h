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
struct bpf_fs_attribute {
    uint64_t parent;
    const char *name;
    uint64_t key, generation;
    int (*snapshot)(uint64_t key, uint64_t generation, struct kernel_heap *,
        char **, size_t *);
};
int bpf_metadata_mount(struct kernel_heap *,uint64_t flags,uint64_t magic,
    const struct bpf_fs_entry *,size_t count,struct kernel_vfs_mount **);
int bpf_metadata_mount_attributes(struct kernel_heap *,uint64_t flags,uint64_t magic,
    const struct bpf_fs_entry *,size_t count,const struct bpf_fs_attribute *,size_t attributes,
    struct kernel_vfs_mount **);
#endif
