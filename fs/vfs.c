#include "vfs_objects.h"
#include "vfs_internal.h"
#include "record_lock.h"
#include "write_source.h"
#include <arch/context.h>

#include <kernel/block.h>
#include <kernel/credentials.h>
#include <kernel/io_context.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/memory_object.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>

#include <string.h>

#include <stddef.h>
#include <stdint.h>

#define VFS_SYMLINK_MAX_FOLLOWS 40U

struct kernel_raw_lock kernel_vfs_state_lock = {
    .rank = KERNEL_RAW_RANK_OBJECT, .initialized = KERNEL_RAW_INITIALIZED
};

/* The live inode keeps the three DAC fields together. Callers retain the
 * namespace/inode qualification through mutations; reads take a small snapshot. */
int kernel_vfs_dac_permission(uint32_t mode, uint32_t uid, uint32_t gid,
    unsigned access, const struct kernel_credentials *cred)
{
    unsigned granted = cred->fsuid == uid ? mode >> 6 :
        kernel_credentials_in_group(cred, gid) ? mode >> 3 : mode;
    if (!(access & ~granted & 7U)) return 0;
    int directory = (mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if (kernel_credentials_capable(cred, KERNEL_CAP_DAC_OVERRIDE) &&
        (directory || !(access & 1U) || (mode & 0111U))) return 0;
    if (kernel_credentials_capable(cred, KERNEL_CAP_DAC_READ_SEARCH) &&
        (directory ? !(access & 2U) : access == 4U)) return 0;
    return -KERNEL_EACCES;
}
static int inode_permission(struct kernel_vfs_node *node, unsigned access)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    uint32_t type=node->mode & KERNEL_VFS_S_IFMT;
    if ((access & 2U) && node->instance->read_only &&
        (type==KERNEL_VFS_S_IFREG || type==KERNEL_VFS_S_IFDIR || type==KERNEL_VFS_S_IFLNK)) return -KERNEL_EROFS;
    /* Type is fixed for this inode identity. A DAC override needs no mutable
     * owner/mode snapshot except the execute-bit check on non-directories. */
    if (!access || (kernel_credentials_capable(cred,KERNEL_CAP_DAC_OVERRIDE) &&
        (!(access & 1U) || (node->mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR))) return 0;
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(node, &io, 0);
    return node->instance->ops->permission ? node->instance->ops->permission(node,cred,access) :
        kernel_vfs_dac_permission(node->mode,node->uid,node->gid,access,cred);
}
int kernel_vfs_path_permission(struct kernel_vfs_path *path, unsigned access)
{
    if (!path || !path->file.private_data) return -KERNEL_EINVAL;
    KERNEL_SERVICE_SCOPE_IF(service,kernel_vfs_namespace_requires_cpu0(path->file.mount));
    return inode_permission(path->file.private_data, access);
}
int kernel_vfs_chmod_allowed(uint32_t uid,uint32_t gid,uint32_t *mode)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    if (cred->fsuid != uid && !kernel_credentials_capable(cred,KERNEL_CAP_FOWNER))
        return -KERNEL_EPERM;
    if ((*mode & 02000U) && !kernel_credentials_in_group(cred,gid) &&
        !kernel_credentials_capable(cred,KERNEL_CAP_FSETID)) *mode &= ~02000U;
    return 0;
}
int kernel_vfs_chown_allowed(uint32_t old_uid,uint32_t old_gid,uint32_t mode,uint32_t uid,uint32_t gid)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    int privileged=kernel_credentials_capable(cred,KERNEL_CAP_CHOWN);
    if (uid!=UINT32_MAX && !(cred->fsuid==old_uid && uid==old_uid) && !privileged)
        return -KERNEL_EPERM;
    if (gid!=UINT32_MAX && !(cred->fsuid==old_uid &&
        (gid==old_gid || kernel_credentials_in_group(cred,gid))) && !privileged)
        return -KERNEL_EPERM;
    /* notify_change turns an actual set-ID removal into ATTR_MODE. Even an
     * otherwise empty chown must satisfy that attribute's owner check. */
    if (kernel_vfs_chown_mode(mode,old_gid,gid==UINT32_MAX?old_gid:gid)!=mode && cred->fsuid!=old_uid &&
        !kernel_credentials_capable(cred,KERNEL_CAP_FOWNER)) return -KERNEL_EPERM;
    return 0;
}

uint32_t kernel_vfs_chown_mode(uint32_t mode,uint32_t old_gid,uint32_t new_gid)
{
    if ((mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR) return mode;
    const struct kernel_credentials *cred=kernel_credentials_current();
    uint32_t original=mode;
    mode &= ~04000U;
    if ((mode & 0010U) || (!kernel_credentials_in_group(cred,old_gid) &&
        !kernel_credentials_capable(cred,KERNEL_CAP_FSETID))) mode &= ~02000U;
    /* Linux first removes set-ID against the old inode. Only an actual MODE
     * update passes the remaining SGID through the requested group's check. */
    if (mode!=original && !kernel_credentials_in_group(cred,new_gid) &&
        !kernel_credentials_capable(cred,KERNEL_CAP_FSETID)) mode &= ~02000U;
    return mode;
}
uint32_t kernel_vfs_write_mode(uint32_t mode,uint32_t gid)
{
    if (kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_FSETID)) return mode;
    return (mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFREG ? kernel_vfs_chown_mode(mode,gid,gid) : mode;
}
int kernel_vfs_inode_times_allowed(struct kernel_vfs_node *node,const struct kernel_vfs_timespec times[2])
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    if (cred->fsuid==node->uid || kernel_credentials_capable(cred,KERNEL_CAP_FOWNER)) return 0;
    if (!times || (times[0].nanoseconds==KERNEL_VFS_UTIME_NOW && times[1].nanoseconds==KERNEL_VFS_UTIME_NOW))
        return kernel_vfs_dac_permission(node->mode,node->uid,node->gid,2U,cred);
    return -KERNEL_EPERM;
}
void kernel_vfs_creation_identity(uint32_t parent_mode,uint32_t parent_gid,
    uint32_t *mode,uint32_t *uid,uint32_t *gid)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    *uid=cred->fsuid; *gid=cred->fsgid;
    if (parent_mode & 02000U) {
        *gid=parent_gid;
        if ((*mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR) *mode |= 02000U;
        else if ((mode[0] & 0010U) && !kernel_credentials_in_group(cred,parent_gid) &&
                 !kernel_credentials_capable(cred,KERNEL_CAP_FSETID)) *mode &= ~02000U;
    }
}
static int directory_mutation_allowed(struct kernel_vfs_path *parent,struct kernel_vfs_path *target)
{
    struct kernel_vfs_node *dir=parent->file.private_data;
    /* Parent namespace qualification also excludes chmod/chown publication. */
    int result=kernel_vfs_dac_permission(dir->mode,dir->uid,dir->gid,3U,kernel_credentials_current());
    if (result || !target) return result;
    const struct kernel_credentials *cred=kernel_credentials_current();
    struct kernel_vfs_node *node=target->file.private_data;
    if ((dir->mode & 01000U) && cred->fsuid!=dir->uid && cred->fsuid!=node->uid &&
        !kernel_credentials_capable(cred,KERNEL_CAP_FOWNER)) return -KERNEL_EPERM;
    return 0;
}
int kernel_vfs_exec_metadata(struct kernel_vfs_file *file,struct kernel_vfs_exec_metadata *out, int read_filecap)
{
    if (!file || !file->exec_lease || !file->private_data || !out) return -KERNEL_EINVAL;
    struct kernel_vfs_node *node=file->private_data;
    KERNEL_LOCK_SCOPE(io);kernel_vfs_node_lock(node,&io,0);
    *out=(struct kernel_vfs_exec_metadata){.mode=node->mode,.uid=node->uid,.gid=node->gid,
        .nosuid=file->mount->nosuid,
        .readable=!kernel_vfs_dac_permission(node->mode,node->uid,node->gid,4U,kernel_credentials_current())};
    return read_filecap && !out->nosuid && node->instance->ops->exec_metadata
        ? node->instance->ops->exec_metadata(node,out) : 0;
}

static int vfs_open_raw(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file);
static int vfs_create_raw(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file);
static int vfs_mkdir_raw(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path,
                         uint32_t mode);
static int vfs_unlink_raw(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path);
static int vfs_rmdir_raw(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path);
static void kernel_vfs_try_release_orphan(struct kernel_vfs_node *node);
static int finish_executable_open(struct kernel_vfs_file *file, int result);
static void release_path_pin(struct kernel_vfs_path **path);

enum vfs_trailing_policy {
    VFS_TRAILING_NOFOLLOW,
    VFS_TRAILING_FOLLOW,
    VFS_TRAILING_PRESERVE,
    VFS_TRAILING_CREATE,
};

struct vfs_path_walk {
    const struct kernel_vfs_path *start, *root;
    struct kernel_vfs_mount *mount;
    struct kernel_vfs_path *mount_owner;
    struct kernel_lock_guard guard;
    struct kernel_service_scope service;
    struct kernel_vfs_node *directory;
    unsigned final_write;
};
static void path_walk_release(struct vfs_path_walk *walk)
{
    kernel_lock_scope_release(&walk->guard);
    if (walk->mount_owner && kernel_vfs_path_release(&walk->mount_owner)) __builtin_trap();
    kernel_service_scope_end(&walk->service);
    walk->service.entered=0;
    walk->mount=NULL;
    walk->directory=NULL;
}
static int namespace_is_parallel(const struct kernel_vfs_instance *instance)
{ return __atomic_load_n(&instance->namespace_parallel,__ATOMIC_ACQUIRE); }
static void namespace_node_lock(struct kernel_vfs_node *node,
    struct kernel_lock_guard *guard, int write)
{
    if (!namespace_is_parallel(node->instance)) {
        kernel_vfs_namespace_lock(node->mount, guard);
        return;
    }
    if (kernel_lock_held(&node->namespace_lock, write)) return;
    if (write) kernel_rwlock_write(&node->namespace_lock, guard);
    else kernel_rwlock_read(&node->namespace_lock, guard);
}
struct namespace_guards {
    struct kernel_lock_guard guard[4];
    unsigned count;
};
static void namespace_guards_release(struct namespace_guards *guards)
{
    while (guards->count) kernel_lock_scope_release(&guards->guard[--guards->count]);
}
static int namespace_nodes_lock(struct namespace_guards *guards,
    struct kernel_vfs_node **nodes, unsigned count)
{
    if (guards->count || !count || count>4) __builtin_trap();
    for (unsigned i=1;i<count;i++) {
        struct kernel_vfs_node *node=nodes[i];unsigned j=i;
        while (j && nodes[j-1]->inode>node->inode) { nodes[j]=nodes[j-1];j--; }
        nodes[j]=node;
    }
    for (unsigned i=1;i<count;i++) if (nodes[i]->inode==nodes[i-1]->inode && nodes[i]!=nodes[i-1]) {
        /* A deleted dirfd can outlive disk inode reuse. Its old directory
         * is no longer a mutation endpoint, even though the object is alive. */
        if (nodes[i]->unlinked || nodes[i]->retired || nodes[i-1]->unlinked || nodes[i-1]->retired)
            return -KERNEL_ENOENT;
        __builtin_trap();
    }
    for (unsigned i=0;i<count;i++) {
        if (i && nodes[i]==nodes[i-1]) continue;
        namespace_node_lock(nodes[i],&guards->guard[guards->count],1);
        if (guards->guard[guards->count].lock) guards->count++;
    }
    return 0;
}
/* Link sources and removal/replacement targets share retirement state with
 * FD operations. Parent metadata and a rename source use backend inode locks. */
static void namespace_inode_lock(struct kernel_vfs_node *node,
    struct kernel_lock_guard *guard)
{
    if (namespace_is_parallel(node->instance) ||
        (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        kernel_vfs_node_lock(node, guard, 1);
}
static int path_walk_select_mode(struct vfs_path_walk *walk,
    struct kernel_vfs_path *path, int write)
{
    struct kernel_vfs_mount *mount=path->file.mount;
    struct kernel_vfs_instance *instance=mount->private_data;
    struct kernel_vfs_node *node=path->file.private_data;
    if (walk->mount==mount && (!namespace_is_parallel(instance) ||
        (walk->directory==node && (!write || walk->guard.write)))) return 0;
    path_walk_release(walk);
    /* 输入起点/根的caller owner覆盖整次操作；其他挂载保留一个实际路径，
     * 逐目录资格也独立pin目录，直到解锁再归还，避免循环前移先释放锁存储。 */
    if (namespace_is_parallel(instance) ||
       ((!walk->start || walk->start->file.mount!=mount) &&
        (!walk->root || walk->root->file.mount!=mount))) {
        int result=kernel_vfs_path_acquire(path);
        if (result) return result;
        walk->mount_owner=path;
    }
#if BOAROS_USER_SMP
    if (kernel_vfs_namespace_requires_cpu0(mount)) kernel_service_scope_begin(&walk->service);
#endif
    namespace_node_lock(node,&walk->guard,write);
    walk->mount=mount;
    walk->directory=node;
    return instance->quiescing ? -KERNEL_EIO : 0;
}
static int path_walk_select(struct vfs_path_walk *walk,struct kernel_vfs_path *path)
{ return path_walk_select_mode(walk,path,0); }
#define VFS_PATH_WALK(name, initial, boundary) \
    struct vfs_path_walk name __attribute__((cleanup(path_walk_release))) = { \
        .start=(initial), .root=(boundary) }

static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, size_t length, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          enum vfs_trailing_policy follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256], struct vfs_path_walk *walk);

static void release_path(struct kernel_heap *heap, char *work)
{
    kernel_heap_release(heap, work);
}

uint64_t kernel_vfs_allocate_mount_id(void)
{
    static uint64_t next = 1;
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    if (next == UINT64_MAX) __builtin_trap();
    uint64_t id = next++;
    kernel_task_lock_release(&irq);
    return id;
}

/* A mount edge is published after both paths are pinned. The short IRQ
 * section protects the edge and reference transfer; it never waits for I/O. */
int kernel_vfs_mount_attach(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_path *covered)
{
    struct kernel_vfs_path *new_root = 0;
    struct kernel_vfs_mount *parent;
    kernel_task_lock_guard irq = {0};
    int result;

    if (!mount || mount->state != VFS_MOUNT_STATE_LIVE || !mount->private_data ||
        mount->root_path || mount->covered_path || !covered ||
        mount->parent || mount->first_child || mount->next_sibling ||
        mount->previous_sibling || mount->child_mounts ||
        covered->references == 0 || !covered->file.mount ||
        (covered->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_EINVAL;
    parent = covered->file.mount;
    if (((struct kernel_vfs_instance *)parent->private_data)->quiescing) return -KERNEL_EIO;
    if (parent == mount) return -KERNEL_EINVAL;
    result = kernel_vfs_path_root(mount,
                    ((struct kernel_vfs_instance *)mount->private_data)->heap,
                    &new_root);
    if (result) return result;
    KERNEL_LOCK_SCOPE(covered_guard);
    kernel_vfs_namespace_lock(parent, &covered_guard);
    struct kernel_vfs_path *container __attribute__((cleanup(release_path_pin))) =
        kernel_vfs_path_parent_pin(covered);
    if (!container) {
        if (kernel_vfs_path_acquire(covered)) __builtin_trap();
        container=covered;
    }
    KERNEL_LOCK_SCOPE(directory_guard);
    namespace_node_lock(container->file.private_data,&directory_guard,1);
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    if (covered->detached ||
        ((struct kernel_vfs_node *)covered->file.private_data)->unlinked ||
        covered->mounted_here || parent->child_mounts == UINT32_MAX ||
        kernel_vfs_path_acquire(covered)) {
        kernel_task_lock_release(&irq);
        kernel_lock_scope_release(&directory_guard);
        kernel_lock_scope_release(&covered_guard);
        (void)kernel_vfs_path_release(&new_root);
        return -KERNEL_EBUSY;
    }
    parent->child_mounts++;
    mount->covered_path = covered;
    mount->root_path = new_root;
    mount->parent = parent;
    mount->next_sibling = parent->first_child;
    if (parent->first_child) parent->first_child->previous_sibling = mount;
    parent->first_child = mount;
    covered->mounted_here = mount;
    kernel_task_lock_release(&irq);
    return 0;
}

int kernel_vfs_mount_prepare_detach(struct kernel_vfs_mount *mount,
    const struct kernel_vfs_path *named_root)
{
    if (!mount || !mount->private_data || !mount->covered_path ||
        !mount->root_path || !mount->parent) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(mount, &namespace_guard);
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    uint32_t expected = named_root == mount->root_path ? 2U : 1U;
    if (mount->covered_path->mounted_here != mount || mount->child_mounts ||
        mount->root_path->references != expected || instance->external_files != 1U) {
        kernel_task_lock_release(&irq);
        return -KERNEL_EBUSY;
    }
    /* 先关闭新提交入口，再放锁等待 worker；失败仍由可达的挂载持有。 */
    instance->quiescing = 1;
    kernel_task_lock_release(&irq);
    kernel_lock_scope_release(&namespace_guard);
    return instance->ops->prepare_unmount ? instance->ops->prepare_unmount(mount) : 0;
}

int kernel_vfs_mount_detach(struct kernel_vfs_mount *mount,
                            const struct kernel_vfs_path *named_root)
{
    struct kernel_vfs_path *root, *covered;
    struct kernel_vfs_instance *instance;
    kernel_task_lock_guard irq = {0};

    if (!mount || !mount->private_data || !mount->covered_path ||
        !mount->root_path || !mount->parent)
        return -KERNEL_EINVAL;
    covered = mount->covered_path;
    root = mount->root_path;
    instance = mount->private_data;
    KERNEL_LOCK_SCOPE(covered_guard);
    kernel_vfs_namespace_lock(mount->parent, &covered_guard);
    struct kernel_vfs_path *container __attribute__((cleanup(release_path_pin))) =
        kernel_vfs_path_parent_pin(covered);
    if (!container) {
        if (kernel_vfs_path_acquire(covered)) __builtin_trap();
        container=covered;
    }
    KERNEL_LOCK_SCOPE(directory_guard);
    namespace_node_lock(container->file.private_data,&directory_guard,1);
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    uint32_t expected_refs = named_root == root ? 2U : 1U;
    if (covered->mounted_here != mount || !mount->parent->child_mounts ||
        mount->child_mounts ||
        root->references != expected_refs || instance->external_files != 1U) {
        kernel_task_lock_release(&irq);
        return -KERNEL_EBUSY;
    }
    if (mount->previous_sibling) {
        if (mount->previous_sibling->next_sibling != mount) __builtin_trap();
        mount->previous_sibling->next_sibling = mount->next_sibling;
    } else {
        if (mount->parent->first_child != mount) __builtin_trap();
        mount->parent->first_child = mount->next_sibling;
    }
    if (mount->next_sibling) {
        if (mount->next_sibling->previous_sibling != mount) __builtin_trap();
        mount->next_sibling->previous_sibling = mount->previous_sibling;
    }
    covered->mounted_here = 0;
    mount->parent->child_mounts--;
    mount->covered_path = 0;
    mount->root_path = 0;
    mount->parent = 0;
    mount->next_sibling = 0;
    mount->previous_sibling = 0;
    kernel_task_lock_release(&irq);
    kernel_lock_scope_release(&directory_guard);
    kernel_lock_scope_release(&covered_guard);
    if (kernel_vfs_path_release(&root) || kernel_vfs_path_release(&covered))
        __builtin_trap();
    return 0;
}

static int enter_mounts(struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *current = *owner;
    for (;;) {
        struct kernel_vfs_path *next;
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        struct kernel_vfs_mount *child = current->mounted_here;
        if (!child) {
            kernel_task_lock_release(&irq);
            *owner = current;
            return 0;
        }
        next = child->root_path;
        if (((struct kernel_vfs_instance *)child->private_data)->quiescing ||
            !next || kernel_vfs_path_acquire(next)) {
            kernel_task_lock_release(&irq);
            *owner = current;
            return -KERNEL_EBUSY;
        }
        kernel_task_lock_release(&irq);
        if (kernel_vfs_path_release(&current)) __builtin_trap();
        current = next;
        *owner = current;
    }
}

void kernel_vfs_namespace_lock(struct kernel_vfs_mount *mount, struct kernel_lock_guard *guard)
{
    struct kernel_vfs_instance *adapter = mount->private_data;
    if (!kernel_lock_held(&adapter->namespace_lock.lock, 1))
        kernel_mutex_lock(&adapter->namespace_lock, guard);
}

static int path_key_compare(const struct kernel_vfs_path *parent, const char *name,
                            const struct kernel_vfs_path *other)
{
    if (parent != other->parent) return (uintptr_t)parent < (uintptr_t)other->parent ? -1 : 1;
    return strcmp(name, other->name);
}
static unsigned path_height(const struct kernel_vfs_path *node)
{ return node ? node->index_height : 0; }
static void path_height_update(struct kernel_vfs_path *node)
{
    unsigned left = path_height(node->index_left), right = path_height(node->index_right);
    node->index_height = 1 + (left > right ? left : right);
}
static struct kernel_vfs_path *path_rotate_left(struct kernel_vfs_path *node)
{
    struct kernel_vfs_path *right = node->index_right;
    node->index_right = right->index_left; right->index_left = node;
    path_height_update(node); path_height_update(right); return right;
}
static struct kernel_vfs_path *path_rotate_right(struct kernel_vfs_path *node)
{
    struct kernel_vfs_path *left = node->index_left;
    node->index_left = left->index_right; left->index_right = node;
    path_height_update(node); path_height_update(left); return left;
}
static struct kernel_vfs_path *path_balance(struct kernel_vfs_path *node)
{
    path_height_update(node);
    int balance = (int)path_height(node->index_left) - (int)path_height(node->index_right);
    if (balance > 1) {
        if (path_height(node->index_left->index_left) < path_height(node->index_left->index_right))
            node->index_left = path_rotate_left(node->index_left);
        return path_rotate_right(node);
    }
    if (balance < -1) {
        if (path_height(node->index_right->index_right) < path_height(node->index_right->index_left))
            node->index_right = path_rotate_right(node->index_right);
        return path_rotate_left(node);
    }
    return node;
}
static struct kernel_vfs_path *path_index_insert(struct kernel_vfs_path *root,
                                                 struct kernel_vfs_path *node)
{
    if (!root) { node->index_height = 1; return node; }
    if (path_key_compare(node->parent, node->name, root) < 0) root->index_left = path_index_insert(root->index_left, node);
    else if (path_key_compare(node->parent, node->name, root) > 0) root->index_right = path_index_insert(root->index_right, node);
    else __builtin_trap();
    return path_balance(root);
}
static struct kernel_vfs_path *path_index_pop_min(struct kernel_vfs_path *root,
                                                  struct kernel_vfs_path **minimum)
{
    if (!root->index_left) { *minimum = root; return root->index_right; }
    root->index_left = path_index_pop_min(root->index_left, minimum);
    return path_balance(root);
}
static struct kernel_vfs_path *path_index_remove(struct kernel_vfs_path *root,
                                                 struct kernel_vfs_path *node)
{
    if (!root) return 0;
    if (path_key_compare(node->parent, node->name, root) < 0) root->index_left = path_index_remove(root->index_left, node);
    else if (path_key_compare(node->parent, node->name, root) > 0) root->index_right = path_index_remove(root->index_right, node);
    else {
        if (root != node) __builtin_trap();
        struct kernel_vfs_path *replacement;
        if (!root->index_left) replacement = root->index_right;
        else if (!root->index_right) replacement = root->index_left;
        else {
            struct kernel_vfs_path *right = path_index_pop_min(root->index_right, &replacement);
            replacement->index_left = root->index_left; replacement->index_right = right;
            replacement = path_balance(replacement);
        }
        node->index_left = node->index_right = 0; node->index_height = 0;
        return replacement;
    }
    return path_balance(root);
}
static struct kernel_vfs_path *find_path(struct kernel_vfs_instance *instance,
    struct kernel_vfs_path *parent, const char *name)
{
    struct kernel_vfs_path *path = instance->path_index;
    while (path) {
        int order = path_key_compare(parent, name, path);
        if (!order) return path;
        path = order < 0 ? path->index_left : path->index_right;
    }
    return 0;
}

static void detach_path(struct kernel_vfs_path *path)
{
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    if (path->index_height)
        instance->path_index = path_index_remove(instance->path_index, path);
    path->detached = 1;
}

static int register_path(struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *path=*owner,*existing;
    struct kernel_vfs_instance *adapter = path->file.mount->private_data;
    int result=0;
    {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        existing=find_path(adapter,path->parent,path->name);
        if (existing && kernel_vfs_path_inode(existing)!=kernel_vfs_path_inode(path)) {
            detach_path(existing);existing=NULL;
        }
        if (existing) result=kernel_vfs_path_acquire(existing);
        else {
            path->next = adapter->paths;
            path->previous = &adapter->paths;
            if (path->next) path->next->previous = &path->next;
            adapter->paths = path;
            adapter->path_index = path_index_insert(adapter->path_index, path);
        }
    }
    if (existing) {
        /* Shared lookups can prepare the same name concurrently. Only the
         * selected identity is published; candidate callbacks run lock-free. */
        if (kernel_vfs_close(&path->file)) __builtin_trap();
        if (path->parent && kernel_vfs_path_release(&path->parent)) __builtin_trap();
        kernel_heap_release(path->heap,path);
        *owner=result?NULL:existing;
    }
    return result;
}

static void unregister_path(struct kernel_vfs_path *path)
{
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    if (path->index_height)
        instance->path_index = path_index_remove(instance->path_index, path);
    if (!path->previous || *path->previous != path) __builtin_trap();
    *path->previous = path->next;
    if (path->next) path->next->previous = path->previous;
    path->next = 0;
    path->previous = 0;
}


static int vfs_open_inode_raw(struct kernel_vfs_mount *mount,
                              uint64_t inode, uint32_t mode,
                              struct kernel_vfs_file *file);

uint64_t kernel_vfs_path_inode(const struct kernel_vfs_path *path)
{
    const struct kernel_vfs_node *node;
    if (path == 0 || path->references == 0U ||
        path->file.private_data == 0) return 0;
    node = path->file.private_data;
    return node->inode;
}

uint32_t kernel_vfs_path_mode(const struct kernel_vfs_path *path)
{
    if (!path || !path->references || !path->file.private_data) return 0;
    const struct kernel_vfs_node *node = path->file.private_data;
    return node->mode;
}

struct kernel_vfs_path *kernel_vfs_path_parent_pin(const struct kernel_vfs_path *path)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    struct kernel_vfs_path *parent = path ? path->parent : NULL;
    if (parent && kernel_vfs_path_acquire(parent)) __builtin_trap();
    return parent;
}

int kernel_vfs_path_readlink(struct kernel_vfs_path *path, char *buffer,
                              size_t size, size_t *read)
{
    KERNEL_SERVICE_SCOPE_IF(service, path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->references) return -KERNEL_EINVAL;
    /* The owned symlink inode pins immutable link contents. Proc link queries
     * may close a foreign OFD, so they run without this namespace mutex. */
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    if ((kernel_vfs_path_mode(path) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK)
        return -KERNEL_EINVAL;
    return instance->ops->readlink(path->file.private_data, buffer, size, read);
}

static void release_path_pin(struct kernel_vfs_path **path)
{
    if (*path && kernel_vfs_path_release(path) != 0) __builtin_trap();
}
int kernel_vfs_path_stat(const struct kernel_vfs_path *path,
                         struct kernel_vfs_stat *stat)
{
    if (path == 0 || path->references == 0U)
        return -KERNEL_EINVAL;
    return kernel_vfs_fstat(&path->file, stat);
}

static int path_lookup_locked(struct kernel_vfs_path *parent,
                           const char *name, size_t name_length,
                           struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *child;
    struct kernel_vfs_mount *mount;
    struct kernel_vfs_instance *adapter;
    enum kernel_heap_status allocation;
    uint64_t inode;
    uint32_t mode;
    int result;

    if (parent == 0 || parent->references == 0U || name == 0 ||
        name_length == 0U || owner == 0 ||
        *owner != 0 || parent->file.private_data == 0)
        return -KERNEL_EINVAL;
    if (name_length > 255U) return -KERNEL_ENAMETOOLONG;
    if ((parent->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_ENOTDIR;
    result = inode_permission(parent->file.private_data, KERNEL_VFS_MAY_EXEC);
    if (result) return result;
    if (name_length == 1U && name[0] == '.') {
        result = kernel_vfs_path_acquire(parent);
        if (result == 0) *owner = parent;
        return result;
    }
    if (name_length == 2U && name[0] == '.' && name[1] == '.') {
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        struct kernel_vfs_path *ancestor = parent;
        while (ancestor->parent == 0 && ancestor->file.mount->covered_path)
            ancestor = ancestor->file.mount->covered_path;
        if (ancestor->parent) ancestor = ancestor->parent;
        result = kernel_vfs_path_acquire(ancestor);
        kernel_task_lock_release(&irq);
        if (result == 0) *owner = ancestor;
        return result;
    }
    if (((struct kernel_vfs_node *)parent->file.private_data)->unlinked)
        return -KERNEL_ENOENT;
    for (size_t i = 0; i < name_length; i++)
        if (name[i] == '/' || name[i] == '\0') return -KERNEL_EINVAL;
    mount = parent->file.mount;
    if (mount == 0 || mount->private_data == 0) return -KERNEL_EINVAL;
    adapter = mount->private_data;
    result = adapter->ops->lookup(adapter, kernel_vfs_path_inode(parent),
                                   name, name_length, &inode, &mode);
    if (result) return result;
    char canonical[256];
    if (adapter->ops->entry_name) {
        result = adapter->ops->entry_name(adapter, inode, canonical, sizeof(canonical));
        if (result) return result;
        name = canonical;
        name_length = strlen(name);
    }
    {
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    char key[256];
    memcpy(key, name, name_length); key[name_length] = 0;
    child = find_path(adapter, parent, key);
    if (child && kernel_vfs_path_inode(child) != inode) {
        detach_path(child);
        child = 0;
    }
    if (child) {
        result = kernel_vfs_path_acquire(child);
        if (result) return result;
        *owner = child;
    }
    }
    if (*owner) {
        result = enter_mounts(owner);
        if (result) (void)kernel_vfs_path_release(owner);
        return result;
    }
    allocation = kernel_heap_allocate_zeroed(adapter->heap, 1U,
                    sizeof(*child) + name_length + 1U, (void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    result = kernel_vfs_path_acquire(parent);
    if (result != 0) {
        kernel_heap_release(adapter->heap, child);
        return result;
    }
    child->heap = adapter->heap;
    child->parent = parent;
    child->references = 1U;
    result = vfs_open_inode_raw(mount, inode, mode, &child->file);
    if (result != 0) {
        struct kernel_vfs_path *parent_owner = child->parent;
        if (kernel_vfs_path_release(&parent_owner) != 0)
            __builtin_trap();
        kernel_heap_release(adapter->heap, child);
        return result;
    }
    child->name = child->initial_name;
    memcpy(child->name, name, name_length);
    child->name[name_length] = '\0';
    result=register_path(&child);
    if (result) return result;
    *owner = child;
    result = enter_mounts(owner);
    if (result) (void)kernel_vfs_path_release(owner);
    return result;
}

int kernel_vfs_path_lookup(struct kernel_vfs_path *parent,
                           const char *name,size_t length,struct kernel_vfs_path **owner)
{
    KERNEL_SERVICE_SCOPE_IF(service,parent && kernel_vfs_namespace_requires_cpu0(parent->file.mount));
    if (!parent || !parent->file.mount || !parent->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(guard);
    namespace_node_lock(parent->file.private_data,&guard,0);
    if (((struct kernel_vfs_instance *)parent->file.mount->private_data)->quiescing) return -KERNEL_EIO;
    return path_lookup_locked(parent,name,length,owner);
}

static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, size_t length, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          enum vfs_trailing_policy follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256], struct vfs_path_walk *walk)
{
    struct kernel_vfs_path *current;
    struct kernel_vfs_instance *adapter;
    char *work = 0;
    const char *pending=path;
    char *target=NULL;
    size_t cursor = 0U;
    uint32_t follows = 0U;
    enum kernel_heap_status allocation;
    int result = 0;

    if (start == 0 || root == 0 || path == 0 || owner == 0 ||
        *owner != 0 || start->file.mount == 0 ||
        start->file.mount->private_data == 0)
        return -KERNEL_EINVAL;
    if (!length) return -KERNEL_ENOENT;
    adapter = start->file.mount->private_data;
    if (length >= KERNEL_FS_PATH_MAX) return -KERNEL_ENAMETOOLONG;
    current = pending[0] == '/' ? root : start;
    result = kernel_vfs_path_acquire(current);
    if (result != 0) {
        current = 0;
        goto Finish;
    }
    while (cursor<length) {
        struct kernel_vfs_path *next = 0;
        size_t begin, end, component_length;
        int suffix;
        int trailing_only;

        while (cursor<length && pending[cursor] == '/') cursor++;
        if (cursor==length) break;
        begin = cursor;
        while (cursor<length && pending[cursor] != '/') cursor++;
        end = cursor;
        component_length = end - begin;
        suffix = end<length;
        {
            size_t remainder = end;
            while (remainder<length && pending[remainder] == '/') remainder++;
            trailing_only = remainder==length;
        }
        result=path_walk_select_mode(walk,current,
            walk->final_write && (!suffix || trailing_only));
        if (result) goto Finish;
        /* open(O_CREAT)拒绝最后的斜杠；此前父路径的错误仍先返回。 */
        if (suffix && trailing_only && follow_trailing_final==VFS_TRAILING_CREATE) {
            result=-KERNEL_EISDIR;
            goto Finish;
        }
        if (component_length > 255U) {
            result = -KERNEL_ENAMETOOLONG;
            goto Finish;
        }
        if (current == root && component_length == 2U &&
            pending[begin] == '.' && pending[begin + 1U] == '.') {
            result = inode_permission(current->file.private_data, KERNEL_VFS_MAY_EXEC);
            if (!result) result = kernel_vfs_path_acquire(root);
            if (!result) next = root;
        } else {
            result = path_lookup_locked(current, pending + begin,
                                            component_length, &next);
        }
        if (result == -KERNEL_ENOENT && allow_missing_final &&
            (!suffix || (allow_missing_trailing && trailing_only)) &&
            missing_name != 0) {
            memcpy(missing_name, pending + begin, component_length);
            missing_name[component_length] = '\0';
            *owner = current;
            current = 0;
            result = 0;
            goto Finish;
        }
        if (result != 0) goto Finish;
        if (next->file.mount!=current->file.mount) {
            /* 挂载根及向上的跨挂载目标是目录；先在旧执行域归还旧分量，
             * 下一轮或返回前再切换保护，切换时始终持有next。 */
            if (kernel_vfs_path_release(&current)) __builtin_trap();
            current=next;
            continue;
        }
        if ((next->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK &&
            ((suffix && (!trailing_only || follow_trailing_final == VFS_TRAILING_FOLLOW)) ||
             follow_final)) {
            size_t target_length = 0U;
            size_t remainder = length-end;
            int backend;
            if (++follows > VFS_SYMLINK_MAX_FOLLOWS) {
                result = -KERNEL_ELOOP;
                (void)kernel_vfs_path_release(&next);
                goto Finish;
            }
            /* next/current own their inodes while the callback acquires and
             * releases foreign objects. Drop names and the old execution
             * scope before choosing the callback's actual backend domain. */
            path_walk_release(walk);
            KERNEL_SERVICE_SCOPE_IF(link_service, kernel_vfs_namespace_requires_cpu0(next->file.mount));
            struct kernel_vfs_instance *target_instance =
                next->file.mount->private_data;
            if (target_instance->ops->follow_link) {
                struct kernel_vfs_path *linked = 0;
                backend = target_instance->ops->follow_link(
                    next->file.private_data, &linked);
                if (backend == 0) {
                    (void)kernel_vfs_path_release(&next);
                    (void)kernel_vfs_path_release(&current);
                    current = linked;
                    if (!current) { result = -KERNEL_EIO; goto Finish; }
                    continue;
                }
                if (backend != -KERNEL_ENOTSUP) {
                    result = backend;
                    (void)kernel_vfs_path_release(&next);
                    goto Finish;
                }
            }
            if (!work) {
                allocation=kernel_heap_allocate(adapter->heap,2U*KERNEL_FS_PATH_MAX,(void **)&work);
                if (allocation!=KERNEL_HEAP_STATUS_OK) {
                    result=allocation==KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
                    if (kernel_vfs_path_release(&next)) __builtin_trap();
                    goto Finish;
                }
                target=work+KERNEL_FS_PATH_MAX;
            }
            backend = target_instance->ops->readlink(next->file.private_data,
                         target, KERNEL_FS_PATH_MAX - 1U, &target_length);
            (void)kernel_vfs_path_release(&next);
            if (backend != 0) {
                result = backend;
                goto Finish;
            }
            if (target_length == 0U) {
                result = -KERNEL_ENOENT;
                goto Finish;
            }
            if (target_length + remainder >= KERNEL_FS_PATH_MAX) {
                result = -KERNEL_ENAMETOOLONG;
                goto Finish;
            }
            memmove(work+target_length,pending+end,remainder);
            memcpy(work,target,target_length);
            length=target_length+remainder;
            work[length]=0;
            pending=work;
            if (target[0] == '/') {
                (void)kernel_vfs_path_release(&current);
                current = root;
                result = kernel_vfs_path_acquire(current);
                if (result != 0) {
                    current = 0;
                    goto Finish;
                }
            }
            cursor = 0U;
            continue;
        }
        if (suffix && (next->file.mode & KERNEL_VFS_S_IFMT) !=
                          KERNEL_VFS_S_IFDIR &&
            !(trailing_only && follow_trailing_final == VFS_TRAILING_PRESERVE)) {
            result = -KERNEL_ENOTDIR;
            (void)kernel_vfs_path_release(&next);
            goto Finish;
        }
        (void)kernel_vfs_path_release(&current);
        current = next;
    }
    *owner = current;
    current = 0;
Finish:
    if (!result && *owner && (walk->mount!=(*owner)->file.mount || !walk->guard.lock)) {
        result=path_walk_select(walk,*owner);
        if (result && kernel_vfs_path_release(owner)) __builtin_trap();
    }
    if (current != 0) (void)kernel_vfs_path_release(&current);
    if (work) kernel_heap_release(adapter->heap, work);
    return result;
}

int kernel_vfs_path_resolve(struct kernel_vfs_path *start,
                            struct kernel_vfs_path *root,
                            const char *path, int follow_final,
                            struct kernel_vfs_path **owner)
{
    if (!path || !start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,start,root);
    return resolve_object(start, root, path, strlen(path), follow_final, 0, 0, VFS_TRAILING_FOLLOW,
                          owner, 0, &walk);
}

int kernel_vfs_path_root(struct kernel_vfs_mount *mount,
                         struct kernel_heap *heap,
                         struct kernel_vfs_path **owner)
{
    KERNEL_SERVICE_SCOPE_IF(service, mount && kernel_vfs_namespace_requires_cpu0(mount));
    struct kernel_vfs_path *path;
    enum kernel_heap_status allocation;
    uint32_t mode;
    uint64_t inode;
    int result;

    if (mount == 0 || mount->private_data == 0 ||
        heap == 0 || owner == 0 || *owner != 0)
        return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(mount, &namespace_guard);
    struct kernel_vfs_instance *adapter = mount->private_data;
    {
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    path = find_path(adapter, 0, "/");
    if (path) {
        result = kernel_vfs_path_acquire(path);
        if (!result) *owner = path;
        return result;
    }
    }
    allocation = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*path) + 2U,
                                              (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                     : -KERNEL_EIO;
    result = adapter->ops->root(adapter, &inode, &mode);
    if (result == 0)
        result = vfs_open_inode_raw(mount, inode,
                                    mode, &path->file);
    if (result != 0) {
        kernel_heap_release(heap, path);
        return result;
    }
    path->heap = heap;
    path->references = 1U;
    path->name = path->initial_name;
    path->name[0] = '/';
    result=register_path(&path);
    if (result) return result;
    *owner = path;
    return 0;
}

int kernel_vfs_path_acquire(struct kernel_vfs_path *path)
{
    if (!path) return -KERNEL_EINVAL;
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!path->references || path->references == UINT32_MAX) return -KERNEL_EOVERFLOW;
    path->references++;
    return 0;
#else
    uint32_t references = __atomic_load_n(&path->references, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!references || references == UINT32_MAX) return -KERNEL_EOVERFLOW;
        if (__atomic_compare_exchange_n(&path->references, &references, references + 1, 1,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
#endif
}

int kernel_vfs_path_release(struct kernel_vfs_path **owner)
{
    if (!owner || !*owner) return -KERNEL_EINVAL;
    struct kernel_vfs_path *path = *owner;
    *owner = 0;
    while (path) {
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
        {
            KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
            if (!path->references) __builtin_trap();
            if (--path->references) return 0;
            unregister_path(path);
        }
#else
        uint32_t references = __atomic_load_n(&path->references, __ATOMIC_ACQUIRE);
        if (!references) __builtin_trap();
        if (references > 1) {
            if (__atomic_compare_exchange_n(&path->references, &references, references - 1, 1,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
            continue;
        }
        {
            KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
            if (!__atomic_compare_exchange_n(&path->references, &references, 0, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            unregister_path(path);
        }
#endif
        KERNEL_SERVICE_SCOPE_IF(service, kernel_vfs_namespace_requires_cpu0(path->file.mount));
        struct kernel_vfs_path *parent = path->parent;
        /* Registry retirement precedes backend close; close failures belong
         * to the mount cleanup list, never to an unreachable path. */
        if (kernel_vfs_close(&path->file)) __builtin_trap();
        if (path->name != path->initial_name) kernel_heap_release(path->heap, path->name);
        kernel_heap_release(path->heap, path);
        path = parent;
    }
    return 0;
}

struct kernel_vfs_mount *kernel_vfs_path_mount(
    const struct kernel_vfs_path *path)
{
    return path != 0 && path->references != 0U ? path->file.mount : 0;
}

int kernel_vfs_mount_is_readonly(const struct kernel_vfs_mount *mount)
{
    if (mount == 0 || mount->private_data == 0) {
        return 0;
    }
    const struct kernel_vfs_instance *adapter = mount->private_data;
    return adapter->read_only != 0;
}

int kernel_vfs_path_set_times(struct kernel_vfs_path *path,
                              const struct kernel_vfs_timespec times[2])
{
    KERNEL_SERVICE_SCOPE_IF(service, path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    namespace_node_lock(path->file.private_data, &namespace_guard, 1);

    if (!path || !path->references) return -KERNEL_EINVAL;
    return kernel_vfs_file_set_times(&path->file, times);
}

int kernel_vfs_path_set_mode(struct kernel_vfs_path *path, uint32_t mode)
{
    KERNEL_SERVICE_SCOPE_IF(service, path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    if (!path || !path->references) return -KERNEL_EINVAL;
    return kernel_vfs_file_set_mode(&path->file, mode);
}

static int vfs_open_inode_raw(struct kernel_vfs_mount *mount,
                              uint64_t inode, uint32_t mode,
                              struct kernel_vfs_file *file)
{
    return vfs_open_raw(mount, 0, inode, mode, file);
}

static int path_string(const struct kernel_vfs_path *path,
                       const struct kernel_vfs_path *root,
                       char *buffer, size_t capacity, int allow_deleted)
{
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    if (!root || !buffer || capacity < 2U) return -KERNEL_ERANGE;
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    const struct kernel_vfs_path *cursor = path;
    size_t position = capacity - 1U;
    int result = 0;
    int deleted = 0;
    buffer[position] = '\0';
    while (cursor != root) {
        if (cursor->parent == 0) {
            if (!cursor->file.mount->covered_path) {
                result = -KERNEL_ENOENT;
                break;
            }
            cursor = cursor->file.mount->covered_path;
            continue;
        }
        if (cursor->detached ||
            ((struct kernel_vfs_node *)cursor->file.private_data)->unlinked) {
            if (!allow_deleted) { result = -KERNEL_ENOENT; break; }
            deleted = 1;
        }
        size_t length = strlen(cursor->name);
        if (!length || length >= position) {
            result = -KERNEL_ERANGE;
            break;
        }
        position -= length;
        memcpy(buffer + position, cursor->name, length);
        if (!position) {
            result = -KERNEL_ERANGE;
            break;
        }
        buffer[--position] = '/';
        cursor = cursor->parent;
    }
    if (!result) {
        if (position == capacity - 1U) buffer[--position] = '/';
        memmove(buffer, buffer + position, capacity - position);
        if (deleted) {
            static const char suffix[] = " (deleted)";
            size_t length = strlen(buffer);
            if (length + sizeof(suffix) > capacity) result = -KERNEL_ERANGE;
            else memcpy(buffer + length, suffix, sizeof(suffix));
        }
    }
    kernel_task_lock_release(&irq);
    return result;
}

int kernel_vfs_path_string(const struct kernel_vfs_path *path,
                           const struct kernel_vfs_path *root,
                           char *buffer, size_t capacity)
{
    return path_string(path, root, buffer, capacity, 0);
}

int kernel_vfs_path_link_string(const struct kernel_vfs_path *path,
                                const struct kernel_vfs_path *root,
                                char *buffer, size_t capacity)
{
    return path_string(path, root, buffer, capacity, 1);
}

int kernel_vfs_path_copy_name(const struct kernel_vfs_path *path, char *buffer, size_t capacity)
{
    if (!path || !buffer || !capacity) return -KERNEL_EINVAL;
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    size_t length = strlen(path->name);
    if (length >= capacity) length = capacity - 1U;
    memcpy(buffer, path->name, length);
    buffer[length] = '\0';
    return 0;
}

static int path_open_locked(struct kernel_vfs_path *path,
                         struct kernel_vfs_file *file)
{
    if (!path || !file || file->path) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_acquire(path);
    if (result) return result;
    result = vfs_open_inode_raw(path->file.mount,
              kernel_vfs_path_inode(path), path->file.mode, file);
    if (result) (void)kernel_vfs_path_release(&path);
    else file->path = path;
    return result;
}

int kernel_vfs_path_open(struct kernel_vfs_path *path,struct kernel_vfs_file *file)
{
    KERNEL_SERVICE_SCOPE_IF(service,path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(guard);
    namespace_node_lock(path->file.private_data,&guard,0);
    return path_open_locked(path,file);
}

static int create_at_parent(struct kernel_vfs_path **owner, const char *name,
                             uint32_t mode, struct kernel_vfs_file *file)
{
    struct kernel_vfs_path *parent=*owner,*child=NULL;
    struct kernel_vfs_mount *mount=parent->file.mount;
    struct kernel_vfs_instance *instance=mount->private_data;
    if (instance->read_only || !instance->ops->create) return -KERNEL_EROFS;
    if (((struct kernel_vfs_node *)parent->file.private_data)->unlinked)
        return -KERNEL_ENOENT;
    int allowed=directory_mutation_allowed(parent,NULL);
    if (allowed) return allowed;
    enum kernel_heap_status allocation=kernel_heap_allocate_zeroed(
        parent->heap,1U,sizeof(*child)+strlen(name)+1U,(void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation==KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    /* 目录发布前准备名称与父引用；成功后该引用随打开路径继续存活。 */
    child->heap=parent->heap;child->parent=parent;*owner=NULL;
    child->references=1U;child->name=child->initial_name;
    memcpy(child->name,name,strlen(name)+1U);
    int result=vfs_create_raw(mount,kernel_vfs_path_inode(parent),name,mode,file);
    if (!result) {
        struct kernel_vfs_node *node=file->private_data;
        child->file=*file;
        __atomic_fetch_add(&node->references,1,__ATOMIC_RELAXED);
        node->open_files++;
        __atomic_fetch_add(&instance->external_files,1,__ATOMIC_RELAXED);
        result=register_path(&child);
        if (result) { if (kernel_vfs_close(file)) __builtin_trap(); }
        else file->path=child;
    } else {
        if (kernel_vfs_path_release(&child->parent)) __builtin_trap();
        release_path(child->heap,(char *)child);
    }
    return result;
}

int kernel_vfs_open_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       const struct kernel_vfs_open_request *request,
                       struct kernel_vfs_file *file, int *created)
{
    if (created) *created=0;
    if (!start || !root || !path || !request || !file || file->state || file->private_data || file->path ||
        !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    unsigned flags=request->flags;
    int create=!!(flags & KERNEL_VFS_OPEN_CREATE);
    int exclusive=create && (flags & KERNEL_VFS_OPEN_EXCLUSIVE);
    int follow=!!(flags & KERNEL_VFS_OPEN_FOLLOW);
    int result;
    VFS_PATH_WALK(walk,start,root);
    walk.final_write=create;
    {
        struct kernel_vfs_path *resolved=NULL;
        char missing[256]={0};
        result=resolve_object(start,root,path,strlen(path),follow,create,1,
            create?VFS_TRAILING_CREATE:VFS_TRAILING_FOLLOW,&resolved,missing,&walk);
        if (result) goto Complete;
        if (((struct kernel_vfs_instance *)resolved->file.mount->private_data)->quiescing)
            result=-KERNEL_EIO;
        else if (!missing[0]) {
            if (create && path[strlen(path)-1U]=='/') result=-KERNEL_EISDIR;
            else if (exclusive) result=-KERNEL_EEXIST;
            else if ((flags & KERNEL_VFS_OPEN_DIRECTORY) &&
                (resolved->file.mode & KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR) result=-KERNEL_ENOTDIR;
            else if (flags & KERNEL_VFS_OPEN_PATH_ONLY) {
                file->path=resolved;resolved=NULL;
                file->mount=file->path->file.mount;file->mode=file->path->file.mode;
            } else if ((resolved->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFLNK)
                result=-KERNEL_ELOOP;
            else if (create && (resolved->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)
                result=-KERNEL_EISDIR;
            else if ((request->access & 2U) &&
                (resolved->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR) result=-KERNEL_EISDIR;
            else if ((flags & KERNEL_VFS_OPEN_EXECUTABLE) &&
                (resolved->file.mode & KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG) result=-KERNEL_EACCES;
            else {
                result=inode_permission(resolved->file.private_data,request->access |
                    ((flags & KERNEL_VFS_OPEN_EXECUTABLE) ? KERNEL_VFS_MAY_EXEC : 0));
                if (!result) result=path_open_locked(resolved,file);
            }
        } else {
            if (path[strlen(path)-1U]=='/') result=-KERNEL_EISDIR;
            else {
                result=create_at_parent(&resolved,missing,request->mode,file);
                if (!result && created) *created=1;
            }
        }
        if (resolved && kernel_vfs_path_release(&resolved)) __builtin_trap();
    }
Complete:
    path_walk_release(&walk);
    return flags & KERNEL_VFS_OPEN_EXECUTABLE ? finish_executable_open(file,result) : result;
}

int kernel_vfs_reopen_link_at(struct kernel_vfs_path *start,
    struct kernel_vfs_path *root, const char *path, struct kernel_heap *heap,
    uint32_t flags, struct kernel_open_file_description **owner)
{
    if (!start || !root || !path || !heap || !owner || *owner)
        return -KERNEL_EINVAL;
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, 0, &resolved);
    if (result) return result;
    KERNEL_SERVICE_SCOPE_IF(service, kernel_vfs_namespace_requires_cpu0(resolved->file.mount));
    struct kernel_vfs_instance *instance = resolved->file.mount->private_data;
    result = (resolved->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK &&
             instance->ops->reopen_link
        ? instance->ops->reopen_link(resolved->file.private_data, heap,
                                     flags, owner)
        : -KERNEL_ENOTSUP;
    if (kernel_vfs_path_release(&resolved)) __builtin_trap();
    return result;
}

static int mount_root_path(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_path **root)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    return kernel_vfs_path_root(mount,
               ((struct kernel_vfs_instance *)mount->private_data)->heap, root);
}

static int vfs_open_resolved(struct kernel_vfs_mount *mount,
                             const char *path,
                             struct kernel_vfs_file *file,
                             unsigned flags)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    const struct kernel_vfs_open_request request={.flags=flags};
    result = kernel_vfs_open_at(root, root, path, &request, file, NULL);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount, path, file, KERNEL_VFS_OPEN_FOLLOW);
}

int kernel_vfs_open_nofollow(struct kernel_vfs_mount *mount,
                             const char *path,
                             struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount, path, file, 0);
}

/* Keep the resolved identity alive until namespace mutation has committed. */
static int mutation_path(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *input,
                          int follow, int missing, int trailing,
                          int follow_trailing, struct kernel_vfs_path **path,
                          char missing_name[256],
                          struct vfs_path_walk *walk)
{
    if (!start || !root || !input) return -KERNEL_EINVAL;
    walk->final_write=1;
    return resolve_object(start, root, input, strlen(input), follow, missing, trailing,
                          follow_trailing, path, missing_name, walk);
}

int kernel_vfs_create_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         uint32_t mode, struct kernel_vfs_file *file)
{
    const struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW |
        KERNEL_VFS_OPEN_CREATE | KERNEL_VFS_OPEN_EXCLUSIVE,.mode=mode};
    return kernel_vfs_open_at(start,root,input,&request,file,NULL);
}

int kernel_vfs_create(struct kernel_vfs_mount *mount, const char *path,
                      uint32_t mode, struct kernel_vfs_file *file)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_create_at(root, root, path, mode, file);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int finish_executable_open(struct kernel_vfs_file *file, int result)
{
    if (result == -KERNEL_EISDIR) {
        return -KERNEL_EACCES;
    }
    if (result != 0) {
        return result;
    }
    if ((file->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        (file->mode & (KERNEL_VFS_S_IXUSR |
                       KERNEL_VFS_S_IXGRP |
                       KERNEL_VFS_S_IXOTH)) == 0U) {
        return kernel_vfs_close(file) == 0 ? -KERNEL_EACCES
                                           : -KERNEL_EIO;
    }
    struct kernel_vfs_node *node = file->private_data;
    int lease_error = 0;
    {
        KERNEL_LOCK_SCOPE(node_guard);
        kernel_vfs_node_lock(node, &node_guard, 1);
        lease_error=kernel_vfs_dac_permission(node->mode,node->uid,node->gid,1U,kernel_credentials_current());
        if (lease_error) {}
        else if (node->write_openers > 0U) lease_error = -KERNEL_ETXTBSY;
        else if (node->exec_users == UINT32_MAX) lease_error = -KERNEL_EOVERFLOW;
        else {
            node->exec_users++;
            file->exec_lease = 1U;
        }
    }
    if (lease_error) {
        (void)kernel_vfs_close(file);
        return lease_error;
    }
    return 0;
}

int kernel_vfs_open_executable(struct kernel_vfs_mount *mount,
                               const char *path, struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount,path,file,KERNEL_VFS_OPEN_FOLLOW | KERNEL_VFS_OPEN_EXECUTABLE);
}

int kernel_vfs_file_acquire_write(struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if (node->instance != 0 && node->instance->read_only) {
        return -KERNEL_EROFS;
    }
    if (node->exec_users > 0U) {
        return -KERNEL_ETXTBSY;
    }
    if (file->write_lease == 0U) {
        if (node->write_openers == UINT32_MAX) {
            return -KERNEL_EOVERFLOW;
        }
        node->write_openers++;
        file->write_lease = 1U;
    }
    return 0;
}

int kernel_vfs_pread(struct kernel_vfs_file *file,
                     uint64_t offset,
                     void *buffer,
                     size_t size,
                     size_t *bytes_read)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || bytes_read == 0 ||
        (buffer == 0 && size != 0U)) {
        return -KERNEL_EINVAL;
    }
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    struct kernel_page_cache *cache = node->instance->page_cache;
    *bytes_read = 0;
    if (offset > INT64_MAX) return -KERNEL_EOVERFLOW;
    if (node->memory) return node->instance->ops->pread(node, offset, buffer, size, bytes_read);
    if (node_guard.lock) kernel_lock_release(&node_guard);
    struct kernel_page_cache_read_page single, *pages = &single;
    size_t capacity = 1;
    int result = 0, done = 0;
    while (*bytes_read < size && offset < node->size && !done) {
        size_t start = offset & BOAROS_PAGE_MASK;
        size_t window = BOAROS_PAGE_SIZE * KERNEL_PAGE_CACHE_READ_BATCH_MAX - start;
        if (window > size - *bytes_read) window = size - *bytes_read;
        if (window > node->size - offset) window = node->size - offset;
        size_t count = (start + window + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
        if (count > capacity) {
            struct kernel_page_cache_read_page *larger;
            if (kernel_heap_allocate(node->instance->heap, count * sizeof(*pages), (void **)&larger)) {
                result = -KERNEL_ENOMEM;
                break;
            }
            if (pages != &single) kernel_heap_release(node->instance->heap, pages);
            pages = larger;
            capacity = count;
        }
        if (kernel_page_cache_get_batch(cache, file, offset >> BOAROS_PAGE_SHIFT, pages, count) ==
            KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT) __builtin_trap();
        int error = 0;
        for (size_t i = 0; i < count; i++) {
            if (pages[i].status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) { done=1; break; }
            if (pages[i].status) {
                error = pages[i].status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
                break;
            }
            void *page;
            size_t valid = pages[i].valid_bytes;
            if (valid <= start) { done=1; break; }
            if (physical_page_map_owned(cache->allocator, pages[i].physical_address, &page))
                __builtin_trap();
            size_t copied = valid - start;
            if (copied > size - *bytes_read) copied = size - *bytes_read;
            memcpy((unsigned char *)buffer + *bytes_read, (unsigned char *)page + start, copied);
            *bytes_read += copied; offset += copied; start = 0;
        }
        for (size_t i = 0; i < count; i++) if (!pages[i].status)
            if (physical_page_release(cache->allocator, pages[i].physical_address)) __builtin_trap();
        if (error) { result = error; break; }
    }
    if (pages != &single) kernel_heap_release(node->instance->heap, pages);
    return result;
}

static void apply_truncated_size(struct kernel_vfs_node *node,
                                 struct kernel_vfs_file *file, uint64_t size)
{
    uint64_t old_size = node->size;
    node->size = size;
    file->size = node->size;
    if (node->size < old_size && !node->memory) {
        kernel_file_mapping_notify(node, node->size);
    }
    if (size < old_size && node->instance->page_cache)
        kernel_page_cache_truncate(node->instance->page_cache, node, size);
}

static int memory_write_source(struct kernel_vfs_node *node,uint64_t offset,
    struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    struct physical_page_allocator *allocator=node->instance->heap->page_allocator;
    *written=0;
    while (*written<size) {
        size_t start=offset&BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if (count>size-*written) count=size-*written;
        count=kernel_vfs_write_source_span(source,count);
        if (!count) { source->access=KERNEL_UACCESS_STATUS_STATE; return 0; }
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node,&io,1);
        uint64_t address,index=offset>>BOAROS_PAGE_SHIFT; int created;
        enum kernel_memory_object_status status=kernel_memory_object_prepare_page(
            node->memory,index,&address,&created);
        if (status==KERNEL_MEMORY_OBJECT_NO_SPACE) return -KERNEL_ENOSPC;
        if (status==KERNEL_MEMORY_OBJECT_NO_MEMORY) return -KERNEL_ENOMEM;
        if (status) __builtin_trap();
        kernel_lock_release(&io);
        struct kernel_mm_user_pin pin={0};
        if (!kernel_vfs_write_source_pin(source,&pin)) {
            kernel_memory_object_finish_page(node->memory,index,address,0);
            return 0;
        }
        kernel_vfs_node_lock(node,&io,1);
        void *page;
        if (physical_page_resolve(allocator,address,&page)) __builtin_trap();
        kernel_vfs_write_source_copy(source,(unsigned char *)page+start,count);
        node->instance->ops->memory_written(node,offset+count);
        kernel_lock_release(&io);
        if (pin.registration) kernel_mm_unpin_user_page(&pin);
        kernel_memory_object_finish_page(node->memory,index,address,1);
        if (physical_page_release(allocator,address)) __builtin_trap();
        kernel_vfs_write_source_advance(source,count);
        offset+=count; *written+=count;
    }
    return 0;
}

int kernel_vfs_check_write_limit(struct kernel_vfs_size_limit *limit,
    uint64_t offset, size_t *size)
{
    if (!limit || limit->maximum == UINT64_MAX) return 0;
    if (offset >= limit->maximum) {
        limit->exceeded = 1;
        return -KERNEL_EFBIG;
    }
    if (*size > limit->maximum - offset) *size = (size_t)(limit->maximum - offset);
    return 0;
}

static int vfs_write_source(struct kernel_vfs_file *file,uint64_t offset,
    struct kernel_vfs_write_source *source,size_t size,
    struct kernel_vfs_size_limit *limit,size_t *bytes_written)
{
    struct kernel_vfs_node *node;
    size_t written = 0U;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || bytes_written == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    *bytes_written=0;
    KERNEL_LOCK_SCOPE(operation_guard);
    kernel_vfs_file_write_lock(file, &operation_guard);
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->instance == 0 || node->instance->read_only) {
        return -KERNEL_EROFS;
    }
    result = node->instance->ops->error(node->instance);
    if (result) return result;
    if (size == 0U) {
        *bytes_written = 0U;
        return 0;
    }

    /* 追加写的实际EOF已在写操作保护内确定；在时间戳、复制和分配前接纳范围。 */
    result = kernel_vfs_check_write_limit(limit, offset, &size);
    if (result) return result;
    if (offset >= node->max_size) return -KERNEL_EFBIG;
    if (size > node->max_size - offset)
        size = (size_t)(node->max_size - offset);
    if (node->instance->ops->killpriv) {
        result=node->instance->ops->killpriv(node,kernel_vfs_write_mode(node->mode,node->gid));
        if (result) return result;
    }
    if (source->mm) {
        /* 首字节用户fault仍保留修改时间；限额拒绝则没有这项副作用。 */
        result = kernel_vfs_file_modified(file, offset, 0);
        if (result) return result;
    }
    kernel_lock_release(&node_guard);
    result = node->memory
        ? memory_write_source(node,offset,source,size,&written)
        : kernel_page_cache_write_source(node->instance->page_cache,file,offset,source,size,&written);
    file->size = node->size;
    *bytes_written = written;
    if (written > size) {
        return -KERNEL_EIO;
    }
    return result;
}

int kernel_vfs_pwrite(struct kernel_vfs_file *file,uint64_t offset,
    const void *buffer,size_t size,size_t *bytes_written)
{
    if ((!buffer && size) || !bytes_written) return -KERNEL_EINVAL;
    struct kernel_vfs_write_source source={.data=buffer};
    size_t written=0;
    int error=vfs_write_source(file,offset,&source,size,NULL,&written);
    *bytes_written=written;
    return written?0:error;
}

int kernel_vfs_write_user(struct kernel_vfs_file *file,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,size_t count,
    uint64_t offset,int append,struct kernel_vfs_size_limit *limit,
    struct kernel_vfs_user_write_result *result)
{
    if (!file || file->state!=VFS_FILE_STATE_LIVE || !file->private_data ||
        !mm || !iov || !result) return -KERNEL_EINVAL;
    *result=(struct kernel_vfs_user_write_result){.end_offset=offset};
    KERNEL_LOCK_SCOPE(operation);
    kernel_vfs_file_write_lock(file,&operation);
    if (append) {
        KERNEL_LOCK_SCOPE(io);
        struct kernel_vfs_node *node=file->private_data;
        kernel_vfs_node_lock(node,&io,0);
        offset=node->size;
        result->end_offset=offset;
    }
    struct kernel_vfs_write_source source={.mm=mm,.user={iov,iov_count,0,0}};
    int error=vfs_write_source(file,offset,&source,count,limit,&result->written);
    result->end_offset=offset+result->written;
    result->access=source.access;
    result->chunks=source.chunks;
    return error;
}

int kernel_vfs_append(struct kernel_vfs_file *file,
                      const void *buffer,
                      size_t size,
                      uint64_t *written_offset,
                      size_t *bytes_written)
{
    struct kernel_vfs_node *node;
    uint64_t offset;
    size_t written = 0U;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || (buffer == 0 && size != 0U) ||
        bytes_written == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    KERNEL_LOCK_SCOPE(operation_guard);
    kernel_vfs_file_write_lock(file, &operation_guard);
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->instance == 0 || node->instance->read_only) {
        return -KERNEL_EROFS;
    }
    offset = node->size;
    if (size == 0U) {
        if (written_offset != 0) {
            *written_offset = offset;
        }
        *bytes_written = 0U;
        return 0;
    }
    kernel_lock_release(&node_guard);
    result = kernel_vfs_pwrite(file, offset, buffer, size, &written);
    if (written_offset != 0) {
        *written_offset = offset + (uint64_t)written;
    }
    *bytes_written = written;
    if (written > size) {
        return -KERNEL_EIO;
    }
    if (result != 0 && written == 0U) {
        return result;
    }
    return 0;
}

int kernel_vfs_path_truncate(struct kernel_vfs_path *path, uint64_t size,
    struct kernel_vfs_size_limit *limit)
{
    KERNEL_SERVICE_SCOPE_IF(service, path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->references) return -KERNEL_EINVAL;
    if ((kernel_vfs_path_mode(path) & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR)
        return -KERNEL_EISDIR;
    int allowed=kernel_vfs_path_permission(path,KERNEL_VFS_MAY_WRITE);
    return allowed ? allowed : kernel_vfs_ftruncate_borrowing(&path->file, size, NULL, limit);
}

struct kernel_block_device *kernel_vfs_file_block_device(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node || !node->instance || !node->instance->ops->block_device) return NULL;
    return node->instance->ops->block_device(node->instance);
}

int kernel_vfs_fallocate_borrowing(struct kernel_vfs_file *file, unsigned mode,
    uint64_t offset, uint64_t length, const struct kernel_lock_guard *borrowed,
    struct kernel_vfs_size_limit *limit)
{
    if (!file || file->state != VFS_FILE_STATE_LIVE || !file->private_data) return -KERNEL_EINVAL;
    if (!length || offset > INT64_MAX || length > INT64_MAX) return -KERNEL_EINVAL;
    if (length > INT64_MAX - offset) return -KERNEL_EFBIG;
    if (mode != 0 && mode != KERNEL_VFS_FALLOC_KEEP_SIZE &&
        mode != (KERNEL_VFS_FALLOC_KEEP_SIZE | KERNEL_VFS_FALLOC_PUNCH_HOLE))
        return -KERNEL_EOPNOTSUPP;
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(operation);
    if (borrowed) {
        if (borrowed->lock != &node->write_operation.lock || !borrowed->write || !borrowed->owner) __builtin_trap();
    } else kernel_vfs_file_write_lock(file,&operation);
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(node,&io,1);
    if ((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) return -KERNEL_EISDIR;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) return -KERNEL_ENODEV;
    if (!node->instance || node->instance->read_only) return -KERNEL_EROFS;
    if (node->exec_users) return -KERNEL_ETXTBSY;
    if (!node->instance->ops->fallocate) return -KERNEL_EOPNOTSUPP;
    uint64_t end = offset + length, old_size = node->size;
    /* vfs_fallocate先拒绝文件系统上限，后端的进程限额检查才可能发信号。 */
    if (end > node->max_size) return -KERNEL_EFBIG;
    /* ext4 file_modified precedes its new-size/FSIZE and allocation checks.
     * A later space failure preserves this already accepted privilege removal. */
    if (node->instance->ops->killpriv) {
        int privilege=node->instance->ops->killpriv(node,kernel_vfs_write_mode(node->mode,node->gid));
        if (privilege) return privilege;
    }
    int modified=kernel_vfs_file_modified(file,offset,0);
    if (modified) return modified;
    if (!mode && end > old_size && limit && end > limit->maximum) {
        limit->exceeded = 1;
        return -KERNEL_EFBIG;
    }
    struct kernel_page_cache *cache = node->instance->page_cache;
    int result = kernel_file_mapping_prepare(node);
    if (result) return result;
    kernel_file_mapping_close(node);
    kernel_lock_release(&io);
    int punch = mode & KERNEL_VFS_FALLOC_PUNCH_HOLE;
    uint64_t first = offset & ~BOAROS_PAGE_MASK;
    uint64_t last = (end + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (punch) kernel_file_mapping_invalidate_range(node,first,last);
    /* Buffered EOF can lead persistent EOF. Publish its dirty prefix before
     * the backend computes KEEP_SIZE or clamps a punch at i_size. */
    result = cache ? kernel_page_cache_writeback_range_borrowing(cache,node,0,UINT64_MAX,
        borrowed ? borrowed : &operation) : 0;
    struct kernel_page_cache_resize resize = {0};
    if (!result && cache && !mode && end > old_size)
        result = kernel_page_cache_resize_prepare(cache,node,old_size,end,&resize);
    kernel_vfs_node_lock(node,&io,1);
    if (!result) {
        uint64_t actual;
        int changed;
        result = node->instance->ops->fallocate(node,mode,offset,length,&actual,&changed);
        if (changed) {
            __atomic_store_n(&node->size,actual,__ATOMIC_RELEASE);
            file->size = actual;
            /* The same inode writer publishes disk mappings, cache retirement
             * and EOF; queued old writeback cannot capture between them. */
            if (punch && cache) kernel_page_cache_invalidate_range(cache,node,first,last);
        }
    }
    kernel_page_cache_resize_finish(&resize,node->size);
    kernel_file_mapping_open(node);
    return result;
}

int kernel_vfs_fallocate(struct kernel_vfs_file *file, unsigned mode,
    uint64_t offset, uint64_t length)
{ return kernel_vfs_fallocate_borrowing(file,mode,offset,length,NULL,NULL); }

int kernel_vfs_ftruncate_borrowing(struct kernel_vfs_file *file,
    uint64_t size, const struct kernel_lock_guard *borrowed,
    struct kernel_vfs_size_limit *limit)
{
    struct kernel_vfs_node *node;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    KERNEL_LOCK_SCOPE(operation_guard);
    if (borrowed) {
        if (borrowed->lock != &node->write_operation.lock || !borrowed->write || !borrowed->owner)
            __builtin_trap();
    } else kernel_vfs_file_write_lock(file, &operation_guard);
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->instance == 0 || node->instance->read_only) {
        return -KERNEL_EROFS;
    }
    if (node->exec_users > 0U) {
        return -KERNEL_ETXTBSY;
    }

    if (size > node->size && limit && size > limit->maximum) {
        limit->exceeded = 1;
        return -KERNEL_EFBIG;
    }
    /* Even a same-size ftruncate updates mtime/ctime. Reconciliation also
     * preserves visible inode mutations when the backend reports an error. */
    if (size > node->max_size) return -KERNEL_EFBIG;
    if (node->memory) {
        /* 关闭旧候选的发布，释放inode锁后才逐MM撤销。登记/OFD覆盖回调
         * 寿命；缺页最终校验取得IO读锁，因此关闭与提交有唯一先后关系。 */
        uint64_t old_size = node->size;
        /* 首次状态发布同样取得 I/O 读保护。没有状态时保持写保护直到
         * 后备提交，禁止在旧尺寸和新登记之间形成未关闭的窗口。 */
        if (!__atomic_load_n(&node->mapping_state, __ATOMIC_ACQUIRE)) {
            uint64_t actual;
            int changed;
            result = node->instance->ops->truncate(node, size, &actual, &changed);
            if (changed) apply_truncated_size(node, file, actual);
            return result;
        }
        kernel_file_mapping_close(node);
        kernel_lock_release(&node_guard);
        if (size < old_size) kernel_file_mapping_notify(node, size);
        kernel_vfs_node_lock(node, &node_guard, 1);
        uint64_t actual;
        int changed;
        result = node->instance->ops->truncate(node, size, &actual, &changed);
        if (changed) apply_truncated_size(node, file, actual);
        kernel_file_mapping_open(node);
        return result;
    }
    if (node->instance->page_cache != NULL) {
        if (node->instance->ops->file_metadata_execution == KERNEL_VFS_EXECUTION_CPU0)
            kernel_assert_service_context();
        struct kernel_page_cache *cache = node->instance->page_cache;
        uint64_t old_size = node->size;
        result = kernel_file_mapping_prepare(node);
        if (result) return result;
        /* 写操作门保留，inode保护必须在MM回调、pin排空和磁盘等待前释放。 */
        kernel_lock_release(&node_guard);
        result = size ? kernel_page_cache_writeback_range_borrowing(cache,node,0,size,
            borrowed ? borrowed : &operation_guard) : 0;
        if (result) return result;
        kernel_vfs_node_lock(node, &node_guard, 1);
        kernel_file_mapping_close(node);
        kernel_lock_release(&node_guard);
        struct kernel_page_cache_resize resize = {0};
        result = kernel_page_cache_resize_prepare(cache,node,old_size,size,&resize);
        if (result) {
            kernel_vfs_node_lock(node,&node_guard,1);
            kernel_file_mapping_open(node);
            return result;
        }
        if (size < old_size) kernel_file_mapping_notify(node,size);
        uint64_t actual;
        int changed;
        kernel_vfs_node_lock(node,&node_guard,1);
        result = node->instance->ops->truncate(node,size,&actual,&changed);
        /* 后端映射、缓存退休和尺寸必须在同一次 inode 写保护内发布。
         * MM 关闭代次不排斥写回；提前解锁会按旧 EOF 重建刚删除的映射。 */
        if (changed) {
            __atomic_store_n(&node->size,actual,__ATOMIC_RELEASE); file->size=actual;
            if (actual < old_size) kernel_page_cache_truncate(cache,node,actual);
        }
        if (changed && actual != size) {
            kernel_page_cache_resize_finish(&resize,old_size);
            kernel_lock_release(&node_guard);
            if (actual < size) kernel_file_mapping_notify(node,actual);
            if (kernel_page_cache_resize_prepare(cache,node,old_size,actual,&resize))
                __builtin_trap();
            kernel_vfs_node_lock(node,&node_guard,1);
        }
        /* 无变化失败保留原脏页；部分变化已先发布实际 EOF，再重取尾页资格。 */
        kernel_page_cache_resize_finish(&resize,changed ? actual : old_size);
        kernel_file_mapping_open(node);
        return result;
    }
    result = node->memory || !size ? 0 : kernel_page_cache_writeback_range_borrowing(
        node->instance->page_cache,node,0,size,borrowed ? borrowed : &operation_guard);
    if (result != 0) return result;
    uint64_t actual;
    int changed;
    result = node->instance->ops->truncate(node, size, &actual, &changed);
    if (changed) apply_truncated_size(node, file, actual);
    if (result) return result;
    return 0;
}

static int vfs_source_read_at(void *context,
                              uint64_t offset,
                              void *buffer,
                              size_t size)
{
    struct kernel_vfs_file *file = context;
    size_t bytes_read = 0U;
    int result = kernel_vfs_pread(file,
                                  offset,
                                  buffer,
                                  size,
                                  &bytes_read);

    if (result != 0) {
        return result;
    }
    return bytes_read == size ? 0 : -KERNEL_EIO;
}

int kernel_vfs_file_read_source(struct kernel_vfs_file *file,
                                struct kernel_read_source *source)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || source == 0) {
        return -KERNEL_EINVAL;
    }
    source->context = file;
    source->size = file->size;
    source->read_at = vfs_source_read_at;
    return 0;
}

int kernel_vfs_ftruncate(struct kernel_vfs_file *file, uint64_t size)
{ return kernel_vfs_ftruncate_borrowing(file, size, NULL, NULL); }

int kernel_vfs_close(struct kernel_vfs_file *file)
{
    struct kernel_vfs_instance *adapter;
    struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 || file->mount == 0 ||
        (file->state != VFS_FILE_STATE_LIVE &&
         file->state != VFS_FILE_STATE_CLEANUP)) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if (file->path && kernel_vfs_path_release(&file->path) != 0)
        __builtin_trap();
    {
        KERNEL_LOCK_SCOPE(node_guard);
        kernel_vfs_node_lock(node, &node_guard, 1);
        if (file->write_lease != 0U) {
            if (node->write_openers == 0U) __builtin_trap();
            node->write_openers--;
            file->write_lease = 0U;
        }
        if (file->exec_lease != 0U) {
            if (node->exec_users == 0U) __builtin_trap();
            node->exec_users--;
            file->exec_lease = 0U;
        }
        if (node->open_files == 0U) __builtin_trap();
        node->open_files--;
    }
    adapter = file->mount->private_data;
    if (adapter == 0) {
        return -KERNEL_EIO;
    }

    if (node->unlinked) {
        KERNEL_LOCK_SCOPE(namespace_guard);
        if (!namespace_is_parallel(node->instance))
            kernel_vfs_namespace_lock(node->mount, &namespace_guard);
        (void)kernel_vfs_try_release_orphan(node);
    }

    file->state = VFS_FILE_STATE_CLEANUP;
    if (kernel_vfs_node_release(&node) != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->external_files == 0U) {
        return -KERNEL_EIO;
    }
    __atomic_fetch_sub(&adapter->external_files, 1, __ATOMIC_RELEASE);
    file->private_data = 0;
    file->mount = 0;
    file->size = 0U;
    file->mode = 0U;
    file->state = VFS_FILE_STATE_EMPTY;
    return 0;
}

uint64_t kernel_vfs_file_inode(const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 ||
        file->state != VFS_FILE_STATE_LIVE) {
        return 0U;
    }
    node = file->private_data;
    return node->inode;
}

int kernel_vfs_namespace_requires_cpu0(const struct kernel_vfs_mount *mount)
{
    const struct kernel_vfs_instance *instance = mount ? mount->private_data : 0;
    return instance && instance->ops->namespace_execution == KERNEL_VFS_EXECUTION_CPU0;
}

int kernel_vfs_file_metadata_requires_cpu0(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node && node->instance->ops->file_metadata_execution == KERNEL_VFS_EXECUTION_CPU0 &&
        kernel_page_cache_needs_cpu0(node->instance->page_cache);
}

uint64_t kernel_vfs_file_size(const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 ||
        file->state != VFS_FILE_STATE_LIVE) {
        return 0U;
    }
    node = file->private_data;
    return __atomic_load_n(&node->size,__ATOMIC_ACQUIRE);
}

/* A real target stays pinned through its attribute and mount checks. Pathless
 * link metadata has no target mount; the link's mount must not stand in for it. */
static int stat_at_resolved(struct kernel_vfs_path *start,
                            struct kernel_vfs_path *root, const char *path,
                            int follow_final, struct kernel_vfs_stat *stat,
                            struct kernel_vfs_path **target)
{
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, follow_final,
                                         &resolved);
    if (!result) {
        result = kernel_vfs_path_stat(resolved, stat);
        *target = resolved;
    } else if (follow_final &&
               (result == -KERNEL_ENOENT || result == -KERNEL_ENXIO)) {
        int original = result;
        /* 跟随无路径对象失败时，只重查最终链接并向其后端索取元数据。 */
        if (!kernel_vfs_path_resolve(start, root, path, 0, &resolved)) {
            KERNEL_SERVICE_SCOPE_IF(service, kernel_vfs_namespace_requires_cpu0(resolved->file.mount));
            struct kernel_vfs_instance *instance =
                resolved->file.mount->private_data;
            if ((resolved->file.mode & KERNEL_VFS_S_IFMT) ==
                    KERNEL_VFS_S_IFLNK && instance->ops->stat_link)
                result = instance->ops->stat_link(resolved->file.private_data,
                                                  stat);
            else result = -KERNEL_ENOTSUP;
            (void)kernel_vfs_path_release(&resolved);
            if (result == -KERNEL_ENOTSUP) result = original;
        }
    }
    return result;
}

int kernel_vfs_stat_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       int follow_final, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_path *target __attribute__((cleanup(release_path_pin))) = 0;
    return stat_at_resolved(start, root, path, follow_final, stat, &target);
}

int kernel_vfs_access_check(const struct kernel_vfs_stat *stat,
                             const struct kernel_vfs_mount *mount, unsigned mode)
{
    if (mode & ~7U) return -KERNEL_EINVAL;
    int result=kernel_vfs_dac_permission(stat->mode,stat->uid,stat->gid,mode,
                                        kernel_credentials_current());
    if (result) return result;
    uint32_t type=stat->mode & KERNEL_VFS_S_IFMT;
    int special=type==KERNEL_VFS_S_IFCHR || type==KERNEL_VFS_S_IFBLK ||
                type==KERNEL_VFS_S_IFIFO || type==KERNEL_VFS_S_IFSOCK;
    if ((mode & 2U) && mount && !special &&
        kernel_vfs_mount_is_readonly(mount)) return -KERNEL_EROFS;
    return 0;
}

int kernel_vfs_access_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *path,
                         unsigned mode, int follow_final)
{
    struct kernel_vfs_path *target __attribute__((cleanup(release_path_pin))) = 0;
    struct kernel_vfs_stat stat;
    int result=stat_at_resolved(start,root,path,follow_final,&stat,&target);
    return result ? result : kernel_vfs_access_check(&stat,
                                      target ? target->file.mount : NULL,mode);
}

int kernel_vfs_stat_path(struct kernel_vfs_mount *mount, const char *path,
                         int follow_final, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_stat_at(root, root, path, follow_final, stat);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_mknod_at(struct kernel_vfs_path *start,
                        struct kernel_vfs_path *root, const char *input,
                        uint32_t mode, uint32_t device)
{
    if (!start || !start->file.mount || !start->file.mount->private_data)
        return -KERNEL_EINVAL;
    uint32_t type = mode & KERNEL_VFS_S_IFMT;
    if (type == KERNEL_VFS_S_IFDIR) return -KERNEL_EPERM;
    if (type == KERNEL_VFS_S_IFSOCK)
        return -KERNEL_ENOTSUP;
    if (type != 0 && type != KERNEL_VFS_S_IFREG && type != KERNEL_VFS_S_IFCHR &&
        type != KERNEL_VFS_S_IFBLK && type != KERNEL_VFS_S_IFIFO)
        return -KERNEL_EINVAL;
    if (!type) type = KERNEL_VFS_S_IFREG;
    VFS_PATH_WALK(walk,start,root);
    struct kernel_vfs_path *path = 0;
    char name[256] = {0};
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &path, name, &walk);
    if (result) return result;
    if (!name[0]) {
        result = -KERNEL_EEXIST;
    } else {
        struct kernel_vfs_instance *instance = path->file.mount->private_data;
        result = instance->read_only ? -KERNEL_EROFS : directory_mutation_allowed(path,NULL);
        if (!result && (type==KERNEL_VFS_S_IFCHR || type==KERNEL_VFS_S_IFBLK) &&
            !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_MKNOD)) result=-KERNEL_EPERM;
        if (!result) result = !instance->ops->mknod
                     ? -KERNEL_ENOTSUP
                     : instance->ops->mknod(instance, kernel_vfs_path_inode(path), name, type, mode, device);
    }
    (void)kernel_vfs_path_release(&path);
    return result;
}

int kernel_vfs_mkdir_at(struct kernel_vfs_path *start,
                        struct kernel_vfs_path *root, const char *input,
                        uint32_t mode)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,start,root);

    struct kernel_vfs_path *path = 0;
    char name[256] = {0};
    int result = mutation_path(start, root, input, 0, 1, 1, 2,
                                &path, name, &walk);
    if (result) return result;
    result = !name[0] ? -KERNEL_EEXIST :
                 kernel_vfs_mount_is_readonly(path->file.mount) ? -KERNEL_EROFS :
                 directory_mutation_allowed(path,NULL);
    if (!result) result=vfs_mkdir_raw(path->file.mount,kernel_vfs_path_inode(path),name,mode);
    (void)kernel_vfs_path_release(&path);
    return result;
}

int kernel_vfs_mkdir(struct kernel_vfs_mount *mount, const char *path,
                     uint32_t mode)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_mkdir_at(root, root, path, mode);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int special_last_component(const char *path)
{
    if (!path) return 0;
    size_t end = strlen(path), begin;
    while (end && path[end - 1U] == '/') end--;
    if (!end && path[0] == '/') return 3;
    begin = end;
    while (begin && path[begin - 1U] != '/') begin--;
    if (end - begin == 1U && path[begin] == '.') return 1;
    if (end - begin == 2U && path[begin] == '.' && path[begin + 1U] == '.')
        return 2;
    return 0;
}

static int rename_parent(struct kernel_vfs_path *, struct kernel_vfs_path *,
    const char *, struct kernel_vfs_path **, char [256], int *, int *, struct vfs_path_walk *);

int kernel_vfs_unlink_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         int directory)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,start,root);
    struct namespace_guards directories __attribute__((cleanup(namespace_guards_release)))={0};
    KERNEL_LOCK_SCOPE(inode_guard);
    struct kernel_vfs_path *parent = 0, *path = 0;
    char name[256];
    int special = special_last_component(input), trailing;
    if (special == 3) return directory ? -KERNEL_EBUSY : -KERNEL_EISDIR;
    int result = rename_parent(start, root, input, &parent, name, &special, &trailing, &walk);
    if (result) return result;
    if (special) {
        (void)kernel_vfs_path_release(&parent);
        return !directory ? -KERNEL_EISDIR : special == 1 ? -KERNEL_EINVAL : -KERNEL_ENOTEMPTY;
    }
    result=path_walk_select_mode(&walk,parent,1);
    if (result) goto Finish;
    /* Keep basename lookup and removal in one namespace operation; a racing
     * rename must not redirect unlink to the object's new name. */
    result = path_lookup_locked(parent, name, strlen(name), &path);
    if (result) goto Finish;
    if (directory && namespace_is_parallel(parent->file.mount->private_data) &&
        path->file.mount==parent->file.mount) {
        for (;;) {
            path_walk_release(&walk);
            struct kernel_vfs_node *nodes[2]={parent->file.private_data,path->file.private_data};
            result=namespace_nodes_lock(&directories,nodes,2);
            if (result) goto Finish;
            struct kernel_vfs_path *verified=NULL;
            result=path_lookup_locked(parent,name,strlen(name),&verified);
            if (result) goto Finish;
            if (verified==path) { (void)kernel_vfs_path_release(&verified);break; }
            namespace_guards_release(&directories);
            (void)kernel_vfs_path_release(&path);path=verified;
            if (path->file.mount!=parent->file.mount) { result=-KERNEL_EBUSY;goto Finish; }
        }
    }
    if (path->file.mount != parent->file.mount) result = -KERNEL_EBUSY;
    else if (trailing && (path->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        result = -KERNEL_ENOTDIR;
    else if (kernel_vfs_mount_is_readonly(parent->file.mount)) result = -KERNEL_EROFS;
    else {
        namespace_inode_lock(path->file.private_data, &inode_guard);
        result = directory_mutation_allowed(parent,path);
        if (!result) result = directory ? vfs_rmdir_raw(parent->file.mount, kernel_vfs_path_inode(parent), name)
                           : vfs_unlink_raw(parent->file.mount, kernel_vfs_path_inode(parent), name);
    }
    if (!result) {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        detach_path(path);
    }
Finish:
    kernel_lock_scope_release(&inode_guard);
    namespace_guards_release(&directories);
    if (path) (void)kernel_vfs_path_release(&path);
    (void)kernel_vfs_path_release(&parent);
    return result;
}

static int unlink_root(struct kernel_vfs_mount *mount, const char *path,
                        int directory)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_unlink_at(root, root, path, directory);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_unlink(struct kernel_vfs_mount *mount, const char *path)
{
    return unlink_root(mount, path, 0);
}

int kernel_vfs_rmdir(struct kernel_vfs_mount *mount, const char *path)
{
    return unlink_root(mount, path, 1);
}

int kernel_vfs_link_at(struct kernel_vfs_path *source,
                       struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *input)
{
    if (!start || !start->file.mount || !start->file.mount->private_data)
        return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,start,root);
    KERNEL_LOCK_SCOPE(inode_guard);
    struct kernel_vfs_path *parent = 0;
    char name[256] = {0};
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &parent, name, &walk);
    if (result) return result;
    struct kernel_vfs_instance *instance = parent->file.mount->private_data;
    /* 目标存在优先于跨挂载；NULL 源表示已 pin 的无路径 pipe/socket OFD。 */
    if (!name[0]) result = -KERNEL_EEXIST;
    else if (instance->read_only) result = -KERNEL_EROFS;
    else if ((result=directory_mutation_allowed(parent,NULL))) {}
    else if (!source || source->file.mount != parent->file.mount)
        result = -KERNEL_EXDEV;
    else if ((source->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ||
             !instance->ops->link) result = -KERNEL_EPERM;
    else {
        struct kernel_vfs_node *node = source->file.private_data;
        namespace_inode_lock(node, &inode_guard);
        /* dentry 可已摘除但 inode 仍有别名；只有最后链接消失才禁止复活。 */
        result = node->unlinked ? -KERNEL_ENOENT :
            instance->ops->link(instance, node->inode,
                               kernel_vfs_path_inode(parent), name);
    }
    kernel_lock_scope_release(&inode_guard);
    (void)kernel_vfs_path_release(&parent);
    return result;
}

int kernel_vfs_symlink_at(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *target,
                          const char *input)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,start,root);

    struct kernel_vfs_path *path = 0;
    char name[256] = {0};
    if (!start || !target || !target[0]) return -KERNEL_EINVAL;
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &path, name, &walk);
    if (result) return result;
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    result = !name[0] ? -KERNEL_EEXIST :
             instance->read_only || !instance->ops->symlink ? -KERNEL_EROFS :
             directory_mutation_allowed(path,NULL);
    if (!result) result=instance->ops->symlink(instance,kernel_vfs_path_inode(path),target,name);
    (void)kernel_vfs_path_release(&path);
    return result;
}

int kernel_vfs_symlink(struct kernel_vfs_mount *mount, const char *target,
                       const char *path)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_symlink_at(root, root, target, path);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_readlink_at(struct kernel_vfs_path *start,
                           struct kernel_vfs_path *root, const char *path,
                           char *buffer, size_t size, size_t *bytes_read)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_path *resolved = 0;
    if (!buffer || !bytes_read || !size) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_resolve(start, root, path, 0, &resolved);
    if (!result) {
        result = kernel_vfs_path_readlink(resolved, buffer, size, bytes_read);
        (void)kernel_vfs_path_release(&resolved);
    }
    return result;
}

int kernel_vfs_readlink(struct kernel_vfs_mount *mount, const char *path,
                        char *buffer, size_t size, size_t *bytes_read)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_readlink_at(root, root, path, buffer, size, bytes_read);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int rename_parent(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *input,
                          struct kernel_vfs_path **parent, char name[256],
                          int *special, int *trailing, struct vfs_path_walk *walk)
{
    if (!input[0]) return -KERNEL_ENOENT;
    size_t length = strlen(input), end = length, begin;
    if (length >= KERNEL_FS_PATH_MAX) return -KERNEL_ENAMETOOLONG;
    *trailing = input[length - 1U] == '/';
    while (end && input[end - 1U] == '/') end--;
    begin = end;
    while (begin && input[begin - 1U] != '/') begin--;
    if (end - begin > 255U) return -KERNEL_ENAMETOOLONG;
    memcpy(name, input + begin, end - begin);
    name[end - begin] = 0;
    *special = special_last_component(input);
    if (!begin) {
        struct kernel_vfs_path *base = end ? start : root;
        int result = kernel_vfs_path_acquire(base);
        if (!result) {
            *parent=base;
            result=path_walk_select(walk,base);
            if (result && kernel_vfs_path_release(parent)) __builtin_trap();
        }
        return result;
    }
    return resolve_object(start,root,input,begin,1,0,0,VFS_TRAILING_FOLLOW,parent,NULL,walk);
}

int kernel_vfs_rename_at(struct kernel_vfs_path *old_start,
                         struct kernel_vfs_path *new_start,
                         struct kernel_vfs_path *root,
                         const char *old_input, const char *new_input,
                         unsigned flags)
{
    if (!old_start || !old_start->file.mount || !old_start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_WALK(walk,old_start,root);
    KERNEL_LOCK_SCOPE(topology_guard);
    struct namespace_guards directories __attribute__((cleanup(namespace_guards_release)))={0};
    KERNEL_LOCK_SCOPE(inode_guard);
    KERNEL_LOCK_SCOPE(source_inode_guard);

    struct kernel_vfs_path *source = 0, *target = 0, *new_parent = 0;
    struct kernel_vfs_path *source_parent = 0;
    char missing[256] = {0}, old_component[256];
    char *new_name = 0;
    struct kernel_vfs_rename_result renamed = {0};
    int old_special, new_special, old_trailing, new_trailing;
    int result;
    if ((flags & ~7U) || ((flags & 2U) && (flags & 5U))) return -KERNEL_EINVAL;
    if (flags & 6U) return -KERNEL_ENOTSUP;
    if (!old_start || !new_start || !root || !old_input || !new_input)
        return -KERNEL_EINVAL;
    result = rename_parent(old_start, root, old_input, &source_parent,
                             old_component, &old_special, &old_trailing, &walk);
    if (result) goto Finish;
    result = rename_parent(new_start, root, new_input, &new_parent,
                             missing, &new_special, &new_trailing, &walk);
    if (result) goto Finish;
    if (source_parent->file.mount != new_parent->file.mount) {
        result = -KERNEL_EXDEV;
        goto Finish;
    }
    struct kernel_vfs_instance *adapter = source_parent->file.mount->private_data;
    if (old_special) { result = -KERNEL_EBUSY; goto Finish; }
    if (new_special) { result = flags & 1U ? -KERNEL_EEXIST : -KERNEL_EBUSY; goto Finish; }
    if (adapter->read_only || !adapter->ops->rename) {
        result = -KERNEL_EROFS;
        goto Finish;
    }
    if (namespace_is_parallel(adapter)) {
        /* No directory guard crosses the topology acquisition. The stable
         * parent owners survive this handoff and are rechecked below. */
        path_walk_release(&walk);
        kernel_vfs_namespace_lock(source_parent->file.mount,&topology_guard);
        struct kernel_vfs_node *nodes[2]={source_parent->file.private_data,new_parent->file.private_data};
        result=namespace_nodes_lock(&directories,nodes,2);
        if (result) goto Finish;
    }
LookupNames:
    result = path_lookup_locked(source_parent, old_component,
                                    strlen(old_component), &source);
    if (result) goto Finish;
    if (source->file.mount != source_parent->file.mount) {
        result = -KERNEL_EBUSY;
        goto Finish;
    }
    result = path_lookup_locked(new_parent, missing, strlen(missing), &target);
    if (result && result != -KERNEL_ENOENT) goto Finish;
    if (target && target->file.mount != new_parent->file.mount) {
        result = -KERNEL_EBUSY;
        goto Finish;
    }
    if (namespace_is_parallel(adapter) &&
        (((source->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR) ||
         (target && (target->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR))) {
        struct kernel_vfs_node *nodes[4]={source_parent->file.private_data,new_parent->file.private_data};
        unsigned count=2;
        if ((source->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)
            nodes[count++]=source->file.private_data;
        if (target && (target->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)
            nodes[count++]=target->file.private_data;
        namespace_guards_release(&directories);
        result=namespace_nodes_lock(&directories,nodes,count);
        if (result) goto Finish;
        struct kernel_vfs_path *verified_source=NULL,*verified_target=NULL;
        result=path_lookup_locked(source_parent,old_component,strlen(old_component),&verified_source);
        if (!result) {
            result=path_lookup_locked(new_parent,missing,strlen(missing),&verified_target);
            if (result==-KERNEL_ENOENT) result=0;
        }
        int same=!result && verified_source==source && verified_target==target;
        if (verified_target) (void)kernel_vfs_path_release(&verified_target);
        if (verified_source) (void)kernel_vfs_path_release(&verified_source);
        if (result) goto Finish;
        if (!same) {
            namespace_guards_release(&directories);
            if (target) (void)kernel_vfs_path_release(&target);
            (void)kernel_vfs_path_release(&source);
            struct kernel_vfs_node *parents[2]={source_parent->file.private_data,new_parent->file.private_data};
            result=namespace_nodes_lock(&directories,parents,2);
            if (result) goto Finish;
            goto LookupNames;
        }
    }
    if ((flags & 1U) && target) { result = -KERNEL_EEXIST; goto Finish; }
    if ((old_trailing || new_trailing) &&
        (source->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        result = -KERNEL_ENOTDIR;
        goto Finish;
    }
    if (((struct kernel_vfs_node *)new_parent->file.private_data)->unlinked) {
        result = -KERNEL_ENOENT;
        goto Finish;
    }
    /* Moving a name preserves source EOF and mapping. Its path reference
     * retains the object; the backend owns ctime. Only a replaced inode can
     * become unlinked and race the last FD's data/cache cleanup. */
    struct kernel_vfs_node *source_node=source->file.private_data;
    struct kernel_vfs_node *target_node=target ? target->file.private_data : NULL;
    const struct kernel_credentials *actor=kernel_credentials_current();
    int source_identity=((struct kernel_vfs_node *)source_parent->file.private_data)->mode & 01000U;
    source_identity=source_identity && !kernel_credentials_capable(actor,KERNEL_CAP_FOWNER);
    if (source_identity && target_node && target_node!=source_node && target_node->inode<source_node->inode) {
        namespace_inode_lock(target_node,&inode_guard);
        namespace_inode_lock(source_node,&source_inode_guard);
    } else {
        if (source_identity) namespace_inode_lock(source_node,&source_inode_guard);
        if (target_node && target_node!=source_node) namespace_inode_lock(target_node,&inode_guard);
    }
    result=directory_mutation_allowed(source_parent,source);
    if (!result) result=directory_mutation_allowed(new_parent,target);
    if (!result && source_parent!=new_parent &&
        (source_node->mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)
        result=kernel_vfs_dac_permission(source_node->mode,source_node->uid,source_node->gid,2U,actor);
    if (result) goto Finish;
    enum kernel_heap_status allocation = kernel_heap_allocate(source->heap,
                                  strlen(missing) + 1U, (void **)&new_name);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    memcpy(new_name, missing, strlen(missing) + 1U);
    /* Parent and moved-directory qualifications span the backend transaction
     * and publication of dentry identity, including device sleeps. */
    result = adapter->ops->rename(adapter,
                 kernel_vfs_path_inode(source_parent), old_component,
                 kernel_vfs_path_inode(new_parent), new_name, flags, &renamed);
    if (result || !renamed.changed) goto Finish;
    if (target && target != source && renamed.replaced_inode !=
                      kernel_vfs_path_inode(target)) __builtin_trap();
    if ((!target || target == source) && renamed.replaced_inode) __builtin_trap();
    struct kernel_vfs_path *old_parent = source->parent;
    char *old_name = source->name;
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    if (target && target != source) {
        struct kernel_vfs_node *node = target->file.private_data;
        detach_path(target);
        if (renamed.replaced_last_link) node->unlinked = 1U;
    }
    adapter->path_index = path_index_remove(adapter->path_index, source);
    source->parent = new_parent;
    source->name = new_name;
    adapter->path_index = path_index_insert(adapter->path_index, source);
    kernel_task_lock_release(&irq);
    new_parent = 0;
    new_name = 0;
    if (old_name != source->initial_name) release_path(source->heap, old_name);
    (void)kernel_vfs_path_release(&old_parent);
Finish:
    if (inode_guard.lock && source_inode_guard.lock && inode_guard.previous==&source_inode_guard) {
        kernel_lock_scope_release(&inode_guard);kernel_lock_scope_release(&source_inode_guard);
    } else {
        kernel_lock_scope_release(&source_inode_guard);kernel_lock_scope_release(&inode_guard);
    }
    namespace_guards_release(&directories);
    kernel_lock_scope_release(&topology_guard);
    if (source_parent && walk.mount!=source_parent->file.mount) path_walk_release(&walk);
    if (new_name) release_path(source->heap, new_name);
    if (new_parent) (void)kernel_vfs_path_release(&new_parent);
    if (target) (void)kernel_vfs_path_release(&target);
    if (source) (void)kernel_vfs_path_release(&source);
    if (source_parent) (void)kernel_vfs_path_release(&source_parent);
    return result;
}

/* The identity index borrows registered nodes. Its embedded links require no
 * allocation; the list remains the bounded full-instance enumeration order. */
static unsigned node_height(const struct kernel_vfs_node *node)
{ return node ? node->index_height : 0; }
static void node_height_update(struct kernel_vfs_node *node)
{
    unsigned left = node_height(node->index_left), right = node_height(node->index_right);
    node->index_height = 1 + (left > right ? left : right);
}
static struct kernel_vfs_node *node_rotate_left(struct kernel_vfs_node *node)
{
    struct kernel_vfs_node *right = node->index_right;
    node->index_right = right->index_left; right->index_left = node;
    node_height_update(node); node_height_update(right); return right;
}
static struct kernel_vfs_node *node_rotate_right(struct kernel_vfs_node *node)
{
    struct kernel_vfs_node *left = node->index_left;
    node->index_left = left->index_right; left->index_right = node;
    node_height_update(node); node_height_update(left); return left;
}
static struct kernel_vfs_node *node_balance(struct kernel_vfs_node *node)
{
    node_height_update(node);
    int balance = (int)node_height(node->index_left) - (int)node_height(node->index_right);
    if (balance > 1) {
        if (node_height(node->index_left->index_left) < node_height(node->index_left->index_right))
            node->index_left = node_rotate_left(node->index_left);
        return node_rotate_right(node);
    }
    if (balance < -1) {
        if (node_height(node->index_right->index_right) < node_height(node->index_right->index_left))
            node->index_right = node_rotate_right(node->index_right);
        return node_rotate_left(node);
    }
    return node;
}
static struct kernel_vfs_node *node_index_insert(struct kernel_vfs_node *root,
                                                 struct kernel_vfs_node *node)
{
    if (!root) { node->index_height = 1; return node; }
    if (node->inode < root->inode) root->index_left = node_index_insert(root->index_left, node);
    else if (node->inode > root->inode) root->index_right = node_index_insert(root->index_right, node);
    else {
        if (!root->retired && !root->closed) __builtin_trap();
        /* Disk inode reuse replaces only the index entry. The retired node
         * keeps its actual file/cache owners and enumeration link. */
        node->index_left = root->index_left; node->index_right = root->index_right;
        root->index_left = root->index_right = 0; root->index_height = 0;
        return node_balance(node);
    }
    return node_balance(root);
}
static struct kernel_vfs_node *node_index_pop_min(struct kernel_vfs_node *root,
                                                  struct kernel_vfs_node **minimum)
{
    if (!root->index_left) { *minimum = root; return root->index_right; }
    root->index_left = node_index_pop_min(root->index_left, minimum);
    return node_balance(root);
}
static struct kernel_vfs_node *node_index_remove(struct kernel_vfs_node *root,
                                                 struct kernel_vfs_node *node)
{
    if (!root) return 0;
    if (node->inode < root->inode) root->index_left = node_index_remove(root->index_left, node);
    else if (node->inode > root->inode) root->index_right = node_index_remove(root->index_right, node);
    else {
        if (root != node) return root; /* A newer incarnation owns this key. */
        struct kernel_vfs_node *replacement;
        if (!root->index_left) replacement = root->index_right;
        else if (!root->index_right) replacement = root->index_left;
        else {
            struct kernel_vfs_node *right = node_index_pop_min(root->index_right, &replacement);
            replacement->index_left = root->index_left; replacement->index_right = right;
            replacement = node_balance(replacement);
        }
        node->index_left = node->index_right = 0; node->index_height = 0;
        return replacement;
    }
    return node_balance(root);
}
struct kernel_vfs_node *kernel_vfs_node_find(struct kernel_vfs_instance *instance, uint64_t inode)
{
    struct kernel_vfs_node *node = instance->node_index;
    while (node) {
        if (inode == node->inode) return node->retired || node->closed ? 0 : node;
        node = inode < node->inode ? node->index_left : node->index_right;
    }
    return 0;
}

struct kernel_vfs_node *kernel_vfs_node_pin(struct kernel_vfs_instance *instance, uint64_t inode)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    struct kernel_vfs_node *node = kernel_vfs_node_find(instance, inode);
    if (node && kernel_vfs_node_acquire(node)) __builtin_trap();
    return node;
}

void kernel_vfs_cleanup_node(struct kernel_vfs_node *node)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    node->next = node->instance->cleanup_nodes;
    node->instance->cleanup_nodes = node;
}

int kernel_vfs_nodes_idle(struct kernel_vfs_instance *instance, struct kernel_vfs_node *root)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    return !instance->retiring_nodes && instance->nodes == root && (!root || !root->next);
}

static void node_retirement_end(struct kernel_vfs_instance *instance)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!instance->retiring_nodes) __builtin_trap();
    instance->retiring_nodes--;
}

struct kernel_vfs_node *kernel_vfs_file_node(
    const struct kernel_vfs_file *file)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return 0;
    }
    return file->private_data;
}

struct kernel_record_lock_state *kernel_vfs_node_record_locks(
    struct kernel_vfs_node *node)
{
    return node ? &node->record_locks : 0;
}

int kernel_vfs_node_acquire(struct kernel_vfs_node *node)
{
    if (!node || node->closed) return -KERNEL_EINVAL;
    uint32_t references=__atomic_load_n(&node->references,__ATOMIC_ACQUIRE);
    for (;;) {
        if (!references || references==UINT32_MAX) return -KERNEL_EINVAL;
        if (__atomic_compare_exchange_n(&node->references,&references,references+1,1,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return 0;
    }
}

static void release_node_pin(struct kernel_vfs_node **owner)
{
    if (*owner && kernel_vfs_node_release(owner)) __builtin_trap();
}

int kernel_vfs_node_release(struct kernel_vfs_node **owner)
{
    struct kernel_vfs_node *node;
    int result;

    if (owner == 0 || *owner == 0) {
        return -KERNEL_EINVAL;
    }
    node = *owner;
    for (;;) {
        uint32_t references = __atomic_load_n(&node->references, __ATOMIC_ACQUIRE);
        if (references > 1U) {
            if (__atomic_compare_exchange_n(&node->references, &references, references - 1, 1,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { *owner = 0; return 0; }
            continue;
        }
        if (references != 1U) __builtin_trap();
        /* Weak lookup+pin and the final transition share the same protection. */
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        if (!__atomic_compare_exchange_n(&node->references, &references, 0, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { kernel_task_lock_release(&irq); continue; }
        if (!kernel_file_mapping_empty(node) || node->fifo_pipe || node->cache_pages ||
            node->dirty_cache_pages.tree.rbh_root || node->dirty_cache_pages.count ||
            node->dirty_cache_pages.queue_next || node->dirty_cache_pages.queue_previous ||
            !kernel_record_lock_state_empty(&node->record_locks)) __builtin_trap();
        if (!node->previous || *node->previous != node || !node->instance->node_count)
            __builtin_trap();
        node->instance->node_index = node_index_remove(node->instance->node_index, node);
        *node->previous = node->next;
        if (node->next) node->next->previous = node->previous;
        node->previous = 0; node->next = 0;
        node->instance->node_count--;
        if (node->unlinked && !node->retired) {
            node->next = node->instance->cleanup_nodes;
            node->instance->cleanup_nodes = node;
            *owner = 0;
            kernel_task_lock_release(&irq);
            return 0;
        }
        if (node->instance->retiring_nodes == SIZE_MAX) __builtin_trap();
        node->instance->retiring_nodes++;
        kernel_task_lock_release(&irq);
        break;
    }
    struct kernel_vfs_instance *instance = node->instance;
    /* 摘出弱索引后，close 和 heap 释放仍持有实例；卸载必须看到这个 owner。 */
    if (!node->closed) {
        result = node->instance->ops->close_node(node);
        if (result != 0) {
            kernel_vfs_cleanup_node(node);
            node_retirement_end(instance);
            *owner = 0;
            return 0;
        }
        node->closed = 1U;
    }
    kernel_file_mapping_destroy(node);
    kernel_heap_release(node->instance->heap, node);
    node_retirement_end(instance);
    *owner = 0;
    return 0;
}

uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node)
{
    return __atomic_load_n(&node->size,__ATOMIC_ACQUIRE);
}

struct kernel_page_cache_entry **kernel_vfs_node_cache_pages(
    struct kernel_vfs_node *node)
{
    return &node->cache_pages;
}

struct kernel_page_cache_dirty *kernel_vfs_node_dirty_pages(struct kernel_vfs_node *node)
{ return &node->dirty_cache_pages; }

void kernel_vfs_node_written(struct kernel_vfs_node *node, uint64_t end)
{
    if (end > node->size) __atomic_store_n(&node->size,end,__ATOMIC_RELEASE);
}

int kernel_vfs_node_writeback_allowed(const struct kernel_vfs_node *node)
{
    return node != 0 && node->instance->ops->writeback_allowed(node->instance);
}

void kernel_vfs_record_writeback_error(struct kernel_vfs_node *node, int error)
{
    KERNEL_TASK_LOCK_SCOPE(guard, &node->instance->error_lock);
    node->writeback_error = error;
    node->writeback_error_sequence++;
    if (node->writeback_error_sequence == 0) __builtin_trap();
    /* 内存不足不是已发生的设备写回失败，不污染后续独立打开的错误游标。 */
    if (error == -KERNEL_EIO || error == -KERNEL_ENOSPC) {
        node->instance->writeback_error = error;
        if (++node->instance->writeback_error_sequence == 0) __builtin_trap();
    }
}

uint64_t kernel_vfs_mount_error_sequence(const struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return 0;
    struct kernel_vfs_instance *instance = mount->private_data;
    KERNEL_TASK_LOCK_SCOPE(guard, &instance->error_lock);
    return instance->writeback_error_sequence;
}

struct sync_nodes {
    struct kernel_heap *heap;
    struct kernel_vfs_node **nodes;
    size_t count;
};

static void release_sync_nodes(struct sync_nodes *snapshot)
{
    for (size_t i = 0; i < snapshot->count; i++)
        if (kernel_vfs_node_release(&snapshot->nodes[i])) __builtin_trap();
    if (snapshot->nodes) kernel_heap_release(snapshot->heap, snapshot->nodes);
}

static int snapshot_sync_nodes(struct kernel_vfs_instance *instance,
                                struct sync_nodes *snapshot)
{
    snapshot->heap = instance->heap;
    for (;;) {
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        size_t capacity = instance->node_count;
        kernel_task_lock_release(&irq);
        if (!capacity) return 0;
        if (capacity > SIZE_MAX / sizeof(*snapshot->nodes)) return -KERNEL_ENOMEM;
        enum kernel_heap_status allocation = kernel_heap_allocate(instance->heap,
                   capacity * sizeof(*snapshot->nodes), (void **)&snapshot->nodes);
        if (allocation != KERNEL_HEAP_STATUS_OK) {
            if (allocation != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            return -KERNEL_ENOMEM;
        }
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        if (instance->node_count > capacity) {
            kernel_task_lock_release(&irq);
            kernel_heap_release(instance->heap, snapshot->nodes);
            snapshot->nodes = 0;
            continue;
        }
        for (struct kernel_vfs_node *node = instance->nodes; node; node = node->next) {
            int result = kernel_vfs_node_acquire(node);
            if (result) { kernel_task_lock_release(&irq); return result; }
            snapshot->nodes[snapshot->count++] = node;
        }
        kernel_task_lock_release(&irq);
        return 0;
    }
}

int kernel_vfs_sync_mount(struct kernel_vfs_mount *mount, uint64_t *observed_error)
{
    if (!mount || mount->state != VFS_MOUNT_STATE_LIVE || !mount->private_data)
        return -KERNEL_EBADF;
    struct kernel_vfs_instance *instance = mount->private_data;
    struct kernel_vfs_path *root __attribute__((cleanup(release_path_pin))) = 0;
    struct sync_nodes snapshot __attribute__((cleanup(release_sync_nodes))) = {0};
    int result = kernel_vfs_path_root(mount, instance->heap, &root);
    if (result) goto observe;
    if (instance->read_only) goto observe;
    result = snapshot_sync_nodes(instance, &snapshot);
    if (result) goto observe;
    /* 等待任何inode或设备之前，完整节点集合已经各持有一份引用。 */
    for (size_t i = 0; i < snapshot.count; i++) {
        struct kernel_vfs_node *node = snapshot.nodes[i];
        if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG || !instance->page_cache)
            continue;
        int error = kernel_page_cache_writeback(instance->page_cache, node);
        if (error && !result) result = error;
    }
    /* 捕获此前数据交接与namespace修改的durable目标一次，不等待checkpoint。 */
    if (instance->ops->sync_filesystem) {
        int error = instance->ops->sync_filesystem(instance, root->file.private_data);
        if (error && !result) result = error;
    } else if (instance->page_cache) __builtin_trap();
observe:
    if (observed_error) {
        KERNEL_TASK_LOCK_SCOPE(guard, &instance->error_lock);
        if (*observed_error != instance->writeback_error_sequence) {
            if (!result) result = instance->writeback_error;
            *observed_error = instance->writeback_error_sequence;
        }
    }
    return result;
}

static struct kernel_vfs_mount *next_sync_mount(struct kernel_vfs_mount *mount,
                                               struct kernel_vfs_mount *root)
{
    if (mount->first_child) return mount->first_child;
    while (mount != root) {
        if (mount->next_sibling) return mount->next_sibling;
        mount = mount->parent;
    }
    return 0;
}

static void sync_mounts_without_array(struct kernel_vfs_path *root)
{
    struct kernel_vfs_mount *mount = root->file.mount;
    uint64_t last = 0, limit = 0;
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount))
        if (m->id > limit) limit = m->id;
    kernel_task_lock_release(&irq);
    /* OOM时按启动时的身份上界逐个取得引用，跨等待不保存树中的裸指针。
     * 已卸载者由卸载路径排空；新挂载不能无限延长本次sync。 */
    for (;;) {
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        struct kernel_vfs_mount *selected = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) {
            if (m->id <= last || m->id > limit ||
                ((struct kernel_vfs_instance *)m->private_data)->quiescing) continue;
            if (!selected || m->id < selected->id) selected = m;
        }
        if (!selected) { kernel_task_lock_release(&irq); return; }
        struct kernel_vfs_path *path = selected == mount ? root : selected->root_path;
        if (!path || kernel_vfs_path_acquire(path)) __builtin_trap();
        last = selected->id;
        kernel_task_lock_release(&irq);
        (void)kernel_vfs_sync_mount(path->file.mount, 0);
        if (kernel_vfs_path_release(&path)) __builtin_trap();
    }
}

void kernel_vfs_sync_all(struct kernel_vfs_path *root)
{
    KERNEL_SERVICE_SCOPE(service);
    if (!root || !root->file.mount) return;
    struct kernel_vfs_mount *mount = root->file.mount;
    struct kernel_heap *heap = ((struct kernel_vfs_instance *)mount->private_data)->heap;
    struct kernel_vfs_path **paths = 0;
    size_t count = 0;
    for (;;) {
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        size_t capacity = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) capacity++;
        kernel_task_lock_release(&irq);
        if (capacity > SIZE_MAX / sizeof(*paths)) {
            sync_mounts_without_array(root);
            return;
        }
        enum kernel_heap_status allocation = kernel_heap_allocate(heap,
                                        capacity * sizeof(*paths), (void **)&paths);
        if (allocation != KERNEL_HEAP_STATUS_OK) {
            if (allocation != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            sync_mounts_without_array(root);
            return;
        }
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        size_t needed = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) needed++;
        if (needed > capacity) {
            kernel_task_lock_release(&irq);
            kernel_heap_release(heap, paths);
            paths = 0;
            continue;
        }
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) {
            if (((struct kernel_vfs_instance *)m->private_data)->quiescing) continue;
            struct kernel_vfs_path *path = m == mount ? root : m->root_path;
            if (!path || kernel_vfs_path_acquire(path)) __builtin_trap();
            paths[count++] = path;
        }
        kernel_task_lock_release(&irq);
        break;
    }
    for (size_t i = 0; i < count; i++) (void)kernel_vfs_sync_mount(paths[i]->file.mount, 0);
    for (size_t i = 0; i < count; i++)
        if (kernel_vfs_path_release(&paths[i])) __builtin_trap();
    kernel_heap_release(heap, paths);
}

uint64_t kernel_vfs_error_sequence(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node) return 0;
    KERNEL_TASK_LOCK_SCOPE(guard, &node->instance->error_lock);
    return node->writeback_error_sequence;
}

static void vfs_sync_observe_complete(struct kernel_vfs_sync_request *request, int result)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(request->file);
    /* 完成可来自任一CPU；错误与共享OFD游标在通知前成对消费。 */
    {
        KERNEL_TASK_LOCK_SCOPE(error, &node->instance->error_lock);
        if (*request->observed_error != node->writeback_error_sequence) {
            if (!result) result = node->writeback_error;
            *request->observed_error = node->writeback_error_sequence;
        }
    }
    {
        KERNEL_RAW_SCOPE(guard, &request->metadata);
        if (request->done || request->index_height) __builtin_trap();
        request->result = result;
        request->done = 1;
        if (request->changed.initialized)
            (void)kernel_wait_queue_wake_all(&request->changed);
        /* request can retire once this raw lock is released. */
    }
}

void kernel_vfs_sync_complete(struct kernel_vfs_sync_request *request, int result)
{
    if (result) kernel_vfs_record_writeback_error(kernel_vfs_file_node(request->file), result);
    vfs_sync_observe_complete(request, result);
}

static int vfs_sync_wait(struct kernel_vfs_sync_request *request)
{
    int result;
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_SCOPE(guard, &request->metadata);
            if (request->done) { result = request->result; break; }
            if (!request->changed.initialized) kernel_wait_queue_init(&request->changed);
            if (kernel_wait_prepare(&request->changed, 0, 0, &token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    if (request->changed.initialized && (kernel_wait_queue_close(&request->changed) ||
        kernel_wait_queue_destroy(&request->changed))) __builtin_trap();
    return result;
}

struct vfs_sync_call {
    struct kernel_vfs_sync_request *request;
    int result, data_only;
};

static void vfs_sync_prepare(void *argument)
{
    struct vfs_sync_call *call = argument;
    struct kernel_vfs_sync_request *request = call->request;
    struct kernel_vfs_node *node = kernel_vfs_file_node(request->file);
    if (call->result || node->instance->read_only) {
        /* 数据交接失败已由写回路径记录；这里只观察，不能再递增序号。 */
        vfs_sync_observe_complete(request, call->result);
        return;
    }
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    if (node->instance->ops->sync_prepare)
        node->instance->ops->sync_prepare(node, call->data_only, request);
    else
        kernel_vfs_sync_complete(request, 0);
}

static int vfs_sync_range_mode(struct kernel_vfs_file *file,
    uint64_t start, uint64_t end, uint64_t *observed_error, int data_only,
    const struct kernel_lock_guard *borrowed)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node || !observed_error) return -KERNEL_EBADF;
    if (start >= end) return -KERNEL_EINVAL;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG &&
        (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_EINVAL;
    struct kernel_page_cache *cache = node->instance->page_cache;
    struct kernel_vfs_sync_request request = {.file = file, .observed_error = observed_error};
    struct vfs_sync_call call = {.request = &request,
        .data_only = data_only && (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR};
    if (!node->instance->read_only)
        call.result = cache ? kernel_page_cache_writeback_range_borrowing(
            cache, node, start, end, borrowed) : 0;
    kernel_raw_lock_init(&request.metadata, KERNEL_RAW_RANK_OBJECT);
    if (kernel_vfs_file_metadata_requires_cpu0(file)) {
        enum kernel_page_cache_status status = kernel_page_cache_run_cpu0_borrowing(
            cache, vfs_sync_prepare, &call, borrowed);
        if (status) return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    } else vfs_sync_prepare(&call);
    return vfs_sync_wait(&request);
}

int kernel_vfs_sync(struct kernel_vfs_file *file, int datasync,
                    uint64_t *observed_error)
{
    return kernel_vfs_sync_borrowing(file, datasync, observed_error, NULL);
}

int kernel_vfs_sync_borrowing(struct kernel_vfs_file *file, int datasync,
    uint64_t *observed_error, const struct kernel_lock_guard *borrowed)
{ return vfs_sync_range_mode(file, 0U, UINT64_MAX, observed_error, datasync, borrowed); }

int kernel_vfs_sync_range(struct kernel_vfs_file *file, uint64_t start,
    uint64_t end, uint64_t *observed_error)
{ return vfs_sync_range_mode(file, start, end, observed_error, 0, NULL); }

const struct kernel_vfs_mount *kernel_vfs_node_mount(
    const struct kernel_vfs_node *node)
{
    return node != 0 ? node->mount : 0;
}

int kernel_vfs_files_share_node(const struct kernel_vfs_file *left,
                                const struct kernel_vfs_file *right)
{
    struct kernel_vfs_node *left_node = kernel_vfs_file_node(left);

    return left_node != 0 && left_node == kernel_vfs_file_node(right);
}

uint64_t kernel_vfs_file_max_size(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node != 0 ? node->max_size : 0U;
}

struct kernel_page_cache *kernel_vfs_file_page_cache(
    const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node = file ? file->private_data : NULL;

    return node != 0 ? node->instance->page_cache : 0;
}

int kernel_vfs_node_try_read(struct kernel_vfs_node *node, struct kernel_lock_guard *guard)
{ return kernel_rwlock_try_read(&node->io_lock, guard); }

void kernel_vfs_file_write_lock(struct kernel_vfs_file *file, struct kernel_lock_guard *guard)
{
    if (!file || file->state != VFS_FILE_STATE_LIVE || !file->private_data) __builtin_trap();
    struct kernel_vfs_node *node = file->private_data;
    /* writev 外层已持有门闩，内部 pwrite/append 不递归取得同一锁。 */
    if (kernel_lock_held(&node->write_operation.lock, 1)) return;
    kernel_mutex_lock(&node->write_operation, guard);
}

void kernel_vfs_node_lock(struct kernel_vfs_node *node, struct kernel_lock_guard *guard, int write)
{
    if (!node || !__atomic_load_n(&node->references,__ATOMIC_ACQUIRE)) __builtin_trap();
    if (kernel_lock_held(&node->io_lock, write)) return;
    if (write) kernel_rwlock_write(&node->io_lock, guard);
    else kernel_rwlock_read(&node->io_lock, guard);
}

int kernel_vfs_unmount(struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    if (mount->covered_path || mount->child_mounts) return -KERNEL_EBUSY;
    struct kernel_vfs_instance *instance = mount->private_data;
    void (*release_owner)(struct kernel_vfs_mount *) = mount->release_owner;
    {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        if (instance->retiring_nodes) return -KERNEL_EBUSY;
    }
    int result = instance->ops->unmount(mount);
    if (!result && release_owner) release_owner(mount);
    return result;
}

int kernel_vfs_mount_statfs(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_statfs *stat)
{
    KERNEL_SERVICE_SCOPE_IF(service, kernel_vfs_namespace_requires_cpu0(mount));
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    int result=instance->ops->statfs(mount, stat);
    if (!result && mount->nosuid) stat->flags |= 2U;
    return result;
}

int kernel_vfs_file_set_times(struct kernel_vfs_file *file,
                              const struct kernel_vfs_timespec times[2])
{
    KERNEL_SERVICE_SCOPE_IF(service, file && kernel_vfs_namespace_requires_cpu0(file->mount));
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    namespace_node_lock(file->private_data,&namespace_guard,1);
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->set_times(file, times);
}

int kernel_vfs_file_set_mode(struct kernel_vfs_file *file, uint32_t mode)
{
    KERNEL_SERVICE_SCOPE_IF(service, file && kernel_vfs_namespace_requires_cpu0(file->mount));
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(operation);
    kernel_vfs_file_write_lock(file,&operation);
    KERNEL_LOCK_SCOPE(namespace_guard);
    namespace_node_lock(file->private_data,&namespace_guard,1);
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->set_mode(file, mode);
}

int kernel_vfs_file_set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid)
{
    KERNEL_SERVICE_SCOPE_IF(service, file && kernel_vfs_namespace_requires_cpu0(file->mount));
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(operation);
    kernel_vfs_file_write_lock(file,&operation);
    KERNEL_LOCK_SCOPE(namespace_guard);
    namespace_node_lock(file->private_data,&namespace_guard,1);
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    if (instance->read_only) return -KERNEL_EROFS;
    if (!instance->ops->set_owner) return -KERNEL_ENOTSUP;
    return instance->ops->set_owner(file, uid, gid);
}

int kernel_vfs_path_set_owner(struct kernel_vfs_path *path, uint32_t uid, uint32_t gid)
{
    KERNEL_SERVICE_SCOPE_IF(service, path && kernel_vfs_namespace_requires_cpu0(path->file.mount));
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    return kernel_vfs_file_set_owner(&path->file, uid, gid);
}

static int vfs_open_raw(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    if (!file || file->state != VFS_FILE_STATE_EMPTY || file->private_data)
        return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    if (!namespace_is_parallel(instance))
        kernel_vfs_namespace_lock(mount, &namespace_guard);
    if (!path) {
        int error = instance->ops->error ? instance->ops->error(instance) : 0;
        if (error) return error;
        kernel_task_lock_guard irq = {0};
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        struct kernel_vfs_node *node = kernel_vfs_node_find(instance, inode_number);
        int pinned = node ? (__atomic_load_n(&node->references, __ATOMIC_ACQUIRE) == UINT32_MAX
            ? -KERNEL_EOVERFLOW : kernel_vfs_node_acquire(node)) : 0;
        kernel_task_lock_release(&irq);
        if (pinned) return pinned;
        if (node) {
            struct kernel_vfs_node *pin __attribute__((cleanup(release_node_pin))) = node;
            /* 等待锁之前独立持有节点，回收者可在锁交接后归还最后缓存引用。 */
            KERNEL_LOCK_SCOPE(node_guard);
            kernel_vfs_node_lock(node, &node_guard, 1);
            if (node->open_files == UINT32_MAX ||
                instance->external_files == UINT32_MAX) return -KERNEL_EOVERFLOW;
            /* 已有 owner 保证 inode 有效；新 file 只取得资格，不再临时打开后端。 */
            node->open_files++;
            __atomic_fetch_add(&instance->external_files, 1, __ATOMIC_RELAXED);
            file->private_data = node;
            file->mount = mount;
            file->size = node->size;
            file->mode = node->mode;
            file->state = VFS_FILE_STATE_LIVE;
            file->write_lease = file->exec_lease = 0;
            pin = 0; /* 临时 pin 转为 file owner。 */
            return 0;
        }
    }
    return instance->ops->open(mount, path, inode_number, inode_mode, file);
}

static int vfs_create_raw(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->create(mount, parent_inode, path, mode, file);
}

void kernel_vfs_file_accessed(struct kernel_vfs_file *file)
{
    if (!file || !file->mount || !file->mount->private_data) return;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    instance->ops->accessed(file);
}

int kernel_vfs_file_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append)
{
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    return instance->ops->modified(file, offset, append);
}

int kernel_vfs_fstat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat)
{
    KERNEL_SERVICE_SCOPE_IF(service, file && kernel_vfs_namespace_requires_cpu0(file->mount));
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->stat(file, stat);
}

static int vfs_mkdir_raw(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path,
                         uint32_t mode)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->mkdir(mount, parent_inode, path, mode);
}

static int vfs_unlink_raw(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->unlink(mount, parent_inode, path);
}

static int vfs_rmdir_raw(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->rmdir(mount, parent_inode, path);
}

static void kernel_vfs_try_release_orphan(struct kernel_vfs_node *node)
{
    if (!node || !node->mount || !node->mount->private_data) return;
    struct kernel_vfs_instance *instance = node->mount->private_data;
    instance->ops->release_unlinked(node);
}

int kernel_vfs_dir_entry(struct kernel_vfs_file *file,
                         uint64_t position,
                         uint64_t *next_position,
                         uint64_t *inode,
                         uint8_t *type,
                         char *name,
                         size_t name_size)
{
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    namespace_node_lock(file->private_data,&namespace_guard,0);
    struct kernel_vfs_instance *instance = file->mount->private_data;
    *next_position = position;
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (node && node->unlinked && (node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR)
        return -KERNEL_ENOENT;
    return instance->ops->dir_entry(file, position, next_position, inode, type, name, name_size);
}

int kernel_vfs_node_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read)
{
    if (!node || !node->mount || !node->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = node->mount->private_data;
    return instance->ops->pread(node, offset, buffer, size, bytes_read);
}

int kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,
    struct file_read_span *spans, size_t count, struct file_read_workspace *workspace, const struct file_read_completion *completion)
{
    /* 私有调用者已固定独立的缓存页；后端边界负责自己的参数校验和结果初始化。 */
    struct kernel_vfs_instance *instance = node->mount->private_data;
    if (instance->ops->pread_batch) return instance->ops->pread_batch(node, spans, count, workspace, completion);
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        spans[i].completed = 0;
        spans[i].error = instance->ops->pread(node, spans[i].offset, spans[i].buffer, spans[i].size, &spans[i].completed);
        if (!result) result = spans[i].error;
        if (completion && i + 1 < count) completion->complete(completion->context, i, &spans[i]);
    }
    return result;
}

int kernel_vfs_file_is_control(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node && node->generated_control;
}

int kernel_vfs_file_control(const struct kernel_vfs_file *file, int write,
    uint64_t offset, char *buffer, size_t size, size_t *count)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node || !node->generated_control || !count || (!buffer && size))
        return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = node->instance;
    if (instance->quiescing) return -KERNEL_EIO;
    if (write && instance->read_only) return -KERNEL_EROFS;
    return instance->ops->control(node, write, offset, buffer, size, count);
}

int kernel_vfs_file_generated(const struct kernel_vfs_file *file)
{
    if (!file || !file->mount || !file->mount->private_data) return 0;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    return instance->ops->snapshot != 0 &&
           (file->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG;
}

int kernel_vfs_file_snapshot(const struct kernel_vfs_file *file,
                             struct kernel_heap *heap,
                             char **buffer, size_t *length)
{
    if (!kernel_vfs_file_generated(file) || !heap || !buffer || *buffer ||
        !length) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    return instance->ops->snapshot(file->private_data, heap, buffer, length);
}

int kernel_vfs_node_writeback(struct kernel_vfs_node *node,
    struct file_write_span *spans, size_t count)
{
    if (!node || !node->mount || !node->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = node->mount->private_data;
    int result = instance->ops->writeback(node, spans, count);
    for (size_t i = 0; i < count; i++) {
        if (spans[i].written > spans[i].size) __builtin_trap();
        if (!result && spans[i].written != spans[i].size) result = -KERNEL_EIO;
    }
    /* One backend submission publishes one error before its cache completion;
     * foreground, background and mount callers only propagate this result. */
    if (result) kernel_vfs_record_writeback_error(node, result);
    return result;
}

/* 后端已准备好私有 handle；同一实例的活 inode 只发布一个通用节点。 */
static int discard_unpublished_node(struct kernel_vfs_node *node)
{
    struct kernel_vfs_instance *instance = node->instance;
    int result = instance->ops->close_node(node);
    if (result) {
        kernel_vfs_cleanup_node(node);
    } else kernel_heap_release(instance->heap, node);
    return result;
}

int kernel_vfs_publish_node(struct kernel_vfs_mount *mount,
    struct kernel_vfs_node *node, struct kernel_vfs_file *file, int creating)
{
    struct kernel_vfs_instance *instance = mount->private_data;
    struct kernel_vfs_node *existing;
    node->instance = instance;
    node->mount = mount;
    __atomic_store_n(&node->references,1,__ATOMIC_RELEASE);
    node->open_files = 1;
    kernel_mutex_init(&node->write_operation, 15, (uintptr_t)node);
    kernel_rwlock_init(&node->namespace_lock, 21, (uintptr_t)node->inode);
    kernel_rwlock_init(&node->io_lock, 30, (uintptr_t)node);
    kernel_file_mapping_init(node);
    kernel_record_lock_state_init(&node->record_locks);
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    existing = kernel_vfs_node_find(instance, node->inode);
    int pin_error = existing ? (existing->references == UINT32_MAX ? -KERNEL_EOVERFLOW :
                               kernel_vfs_node_acquire(existing)) : 0;
    if (!existing && instance->external_files != UINT32_MAX) {
        /* Prepare outside protection, then find and publish together. */
        node->next = instance->nodes;
        node->previous = &instance->nodes;
        if (node->next) node->next->previous = &node->next;
        instance->nodes = node;
        instance->node_index = node_index_insert(instance->node_index, node);
        instance->node_count++;
    } else if (!existing) pin_error=-KERNEL_EOVERFLOW;
    kernel_task_lock_release(&irq);
    if (existing) {
        struct kernel_vfs_node *pin __attribute__((cleanup(release_node_pin))) =
            pin_error ? 0 : existing;
        if (pin_error) {
            int error = discard_unpublished_node(node);
            return error ? error : pin_error;
        }
        KERNEL_LOCK_SCOPE(existing_guard);
        kernel_vfs_node_lock(existing, &existing_guard, 1);
        int busy = creating && existing->exec_users;
        int error = discard_unpublished_node(node);
        if (error) return busy ? -KERNEL_ETXTBSY : error;
        if (busy) return -KERNEL_ETXTBSY;
        if (existing->open_files == UINT32_MAX || instance->external_files == UINT32_MAX)
            return -KERNEL_EOVERFLOW;
        existing->open_files++;
        node = existing;
        pin = 0;
    } else {
        if (pin_error) {
            int error = discard_unpublished_node(node);
            return error ? error : pin_error;
        }
    }
    file->private_data = node;
    file->mount = mount;
    file->size = node->size;
    file->mode = node->mode;
    file->state = VFS_FILE_STATE_LIVE;
    file->write_lease = 0;
    file->exec_lease = 0;
    __atomic_fetch_add(&instance->external_files, 1, __ATOMIC_RELAXED);
    return 0;
}

struct kernel_memory_object *kernel_vfs_file_memory(const struct kernel_vfs_file *file)
{ struct kernel_vfs_node *n = kernel_vfs_file_node(file); return n ? n->memory : 0; }

int kernel_vfs_check_io_domain(const struct kernel_vfs_node *node, const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if (!node || !node->instance || !node->instance->ops->check_domain) return -KERNEL_EINVAL;
    return node->instance->ops->check_domain(node, heap, allocator);
}
int kernel_vfs_check_mapping_domain(const struct kernel_vfs_node *node, const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    int r = kernel_vfs_check_io_domain(node, heap, allocator);
    if (r || (!node->memory && !__atomic_load_n(&node->mapping_state, __ATOMIC_ACQUIRE))) return -KERNEL_EINVAL;
    return 0;
}

#if BOAROS_COST_DIAGNOSTICS
int kernel_vfs_file_is_cost(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node && node->generated_diagnostic;
}
#endif
