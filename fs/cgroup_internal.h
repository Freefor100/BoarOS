#ifndef BOAROS_FS_CGROUP_INTERNAL_H
#define BOAROS_FS_CGROUP_INTERNAL_H
#include <kernel/vfs.h>
#include <stdint.h>
struct kernel_heap;
struct kernel_credentials;
struct kernel_open_file_description;
struct cgroup_node;
#define KERNEL_CGROUP_FILES 7
/* Topology, attributes, node aliases and membership are synchronized with
 * kernel_wait_domain; get is
 * safe under that lock, final put must run outside it. NULL task membership
 * denotes root and carries no reference. Each child owns its parent. */
struct kernel_cgroup {
    struct kernel_heap *heap;
    struct kernel_cgroup *parent, *children, *next, *all_next;
    struct cgroup_node *nodes;
    uint32_t refs;
    uint64_t members, id;
    unsigned linked;
    struct kernel_vfs_stat stat[KERNEL_CGROUP_FILES];
    char name[256];
};
struct kernel_cgroup *kernel_cgroup_root(void);
struct kernel_cgroup *kernel_cgroup_get(struct kernel_cgroup *);
void kernel_cgroup_put(struct kernel_cgroup *);
int kernel_cgroup_from_file(struct kernel_open_file_description *,struct kernel_cgroup **);
/* Both permission checks require kernel_wait_domain. */
int kernel_cgroup_permission(struct kernel_cgroup *,struct kernel_cgroup *,const struct kernel_credentials *);
int kernel_cgroup_migration_permission(struct kernel_cgroup *,struct kernel_cgroup *,const struct kernel_credentials *);
int kernel_cgroup_file(const struct kernel_vfs_file *);
int kernel_cgroup_shutdown(struct kernel_heap *);
int kernel_cgroup_mount(struct kernel_heap *,uint64_t,struct kernel_vfs_mount **);
/* Scheduler supplies these operations; they do not expose tasks to VFS. */
int kernel_cgroup_move(struct kernel_cgroup *,int32_t,int,const struct kernel_credentials *);
int kernel_cgroup_members_snapshot(struct kernel_cgroup *,int,struct kernel_heap *,char **,size_t *);
int kernel_cgroup_populated(struct kernel_cgroup *);
#endif
