#ifndef BOAROS_FS_MOUNT_CONTEXT_H
#define BOAROS_FS_MOUNT_CONTEXT_H
#include <kernel/fs_context.h>
#include <kernel/open_file.h>

#define KERNEL_MOUNT_OPTIONS_MAX 2048U
struct kernel_namespace;
struct kernel_pid_registry;
struct kernel_mount_parameters {
    struct kernel_namespace *ipc_namespace;
    struct kernel_pid_registry *pid_view;
    struct kernel_vfs_path *overlay_layers[3];
    char type[64],source[KERNEL_FS_PATH_MAX],options[KERNEL_MOUNT_OPTIONS_MAX];
    uint64_t flags,mask;
};
/* Creation returns a detached tree owner. Attachment does not consume the
 * caller's path; close_tree only retires a tree that never became attached. */
int kernel_mount_type_supported(const char *);
const char *kernel_mount_registered_type(size_t index, int *block);
int kernel_mount_create(const struct kernel_fs_context *,const struct kernel_mount_parameters *,
    const struct kernel_credentials *,int,struct kernel_vfs_path **);
void kernel_mount_discard(struct kernel_vfs_path **);
int kernel_mount_context_open(struct kernel_heap *,const char *,struct kernel_vfs_path *,struct kernel_open_file_description **);
int kernel_mount_context_configure(struct kernel_open_file_description *,const struct kernel_fs_context *,
    unsigned,const char *,const char *,char *);
int kernel_mount_context_mount(struct kernel_open_file_description *,const struct kernel_fs_context *,
    unsigned,unsigned,struct kernel_open_file_description **);
#endif
