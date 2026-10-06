#include "vfs_objects.h"
#include "vfs_internal.h"
#include "record_lock.h"
#include <arch/context.h>

#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/memory_object.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <string.h>

#include <stddef.h>
#include <stdint.h>

#define VFS_SYMLINK_MAX_FOLLOWS 40U

static int vfs_open_raw(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file);
static int vfs_create_raw(struct kernel_vfs_mount *mount,
                          const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file);
static int vfs_mkdir_raw(struct kernel_vfs_mount *mount,
                         const char *path,
                         uint32_t mode);
static int vfs_unlink_raw(struct kernel_vfs_mount *mount,
                          const char *path);
static int vfs_rmdir_raw(struct kernel_vfs_mount *mount,
                         const char *path);
static void kernel_vfs_try_release_orphan(struct kernel_vfs_node *node);

/* Mutation backends still take a pathname; derive it from the same held
 * parent/inode walk used by open and stat.  The pathname is only an adapter
 * argument, never the authority for deciding which object was found. */
static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          int follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256]);
static int path_to_backend_name(const struct kernel_vfs_path *path,
                                const char *missing_name,
                                char *buffer, size_t capacity);

static void release_path(struct kernel_heap *heap, char *work)
{
    if (kernel_heap_release(heap, work) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

uint64_t kernel_vfs_allocate_mount_id(void)
{
    static uint64_t next = 1;
    uintptr_t irq = arch_interrupt_save();
    if (next == UINT64_MAX) __builtin_trap();
    uint64_t id = next++;
    arch_interrupt_restore(irq);
    return id;
}

/* A mount edge is published after both paths are pinned. The short IRQ
 * section protects the edge and reference transfer; it never waits for I/O. */
int kernel_vfs_mount_attach(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_path *covered)
{
    struct kernel_vfs_path *new_root = 0;
    struct kernel_vfs_mount *parent;
    uintptr_t irq;
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
    irq = arch_interrupt_save();
    if (covered->detached ||
        ((struct kernel_vfs_node *)covered->file.private_data)->unlinked ||
        covered->mounted_here || parent->child_mounts == UINT32_MAX ||
        covered->references == UINT32_MAX) {
        arch_interrupt_restore(irq);
        kernel_lock_scope_release(&covered_guard);
        (void)kernel_vfs_path_release(&new_root);
        return -KERNEL_EBUSY;
    }
    covered->references++;
    parent->child_mounts++;
    mount->covered_path = covered;
    mount->root_path = new_root;
    mount->parent = parent;
    mount->next_sibling = parent->first_child;
    if (parent->first_child) parent->first_child->previous_sibling = mount;
    parent->first_child = mount;
    covered->mounted_here = mount;
    arch_interrupt_restore(irq);
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
    uintptr_t irq = arch_interrupt_save();
    uint32_t expected = named_root == mount->root_path ? 2U : 1U;
    if (mount->covered_path->mounted_here != mount || mount->child_mounts ||
        mount->root_path->references != expected || instance->external_files != 1U) {
        arch_interrupt_restore(irq);
        return -KERNEL_EBUSY;
    }
    /* 先关闭新提交入口，再放锁等待 worker；失败仍由可达的挂载持有。 */
    instance->quiescing = 1;
    arch_interrupt_restore(irq);
    kernel_lock_scope_release(&namespace_guard);
    return instance->ops->prepare_unmount ? instance->ops->prepare_unmount(mount) : 0;
}

int kernel_vfs_mount_detach(struct kernel_vfs_mount *mount,
                            const struct kernel_vfs_path *named_root)
{
    struct kernel_vfs_path *root, *covered;
    struct kernel_vfs_instance *instance;
    uintptr_t irq;

    if (!mount || !mount->private_data || !mount->covered_path ||
        !mount->root_path || !mount->parent)
        return -KERNEL_EINVAL;
    covered = mount->covered_path;
    root = mount->root_path;
    instance = mount->private_data;
    KERNEL_LOCK_SCOPE(covered_guard);
    kernel_vfs_namespace_lock(mount->parent, &covered_guard);
    irq = arch_interrupt_save();
    uint32_t expected_refs = named_root == root ? 2U : 1U;
    if (covered->mounted_here != mount || !mount->parent->child_mounts ||
        mount->child_mounts ||
        root->references != expected_refs || instance->external_files != 1U) {
        arch_interrupt_restore(irq);
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
    arch_interrupt_restore(irq);
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
        uintptr_t irq = arch_interrupt_save();
        struct kernel_vfs_mount *child = current->mounted_here;
        if (!child) {
            arch_interrupt_restore(irq);
            *owner = current;
            return 0;
        }
        next = child->root_path;
        if (!next || kernel_vfs_path_acquire(next)) {
            arch_interrupt_restore(irq);
            *owner = current;
            return -KERNEL_EBUSY;
        }
        arch_interrupt_restore(irq);
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

static void register_path(struct kernel_vfs_path *path)
{
    struct kernel_vfs_instance *adapter = path->file.mount->private_data;
    path->next = adapter->paths;
    path->previous = &adapter->paths;
    if (path->next) path->next->previous = &path->next;
    adapter->paths = path;
}

static void unregister_path(struct kernel_vfs_path *path)
{
    if (!path->previous || *path->previous != path) __builtin_trap();
    *path->previous = path->next;
    if (path->next) path->next->previous = path->previous;
    path->next = 0;
    path->previous = 0;
}

static int path_to_backend_name(const struct kernel_vfs_path *path,
                                const char *missing_name,
                                char *buffer, size_t capacity)
{
    const struct kernel_vfs_path *cursor = path;
    size_t position;
    size_t length;

    if (path == 0 || path->references == 0U || buffer == 0 ||
        capacity < 2U) return -KERNEL_EINVAL;
    position = capacity - 1U;
    buffer[position] = '\0';
    if (missing_name != 0 && missing_name[0] != '\0') {
        length = strlen(missing_name);
        if (length > 255U || length >= position)
            return -KERNEL_ENAMETOOLONG;
        position -= length;
        memcpy(buffer + position, missing_name, length);
        if (position == 0U) return -KERNEL_ENAMETOOLONG;
        buffer[--position] = '/';
    }
    while (cursor->parent != 0) {
        if (cursor->detached ||
            ((struct kernel_vfs_node *)cursor->file.private_data)->unlinked)
            return -KERNEL_ENOENT;
        length = strlen(cursor->name);
        if (length == 0U || length >= position)
            return -KERNEL_ENAMETOOLONG;
        position -= length;
        memcpy(buffer + position, cursor->name, length);
        if (position == 0U) return -KERNEL_ENAMETOOLONG;
        buffer[--position] = '/';
        cursor = cursor->parent;
    }
    if (position == capacity - 1U) buffer[--position] = '/';
    memmove(buffer, buffer + position, capacity - position);
    return 0;
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

int kernel_vfs_path_readlink(struct kernel_vfs_path *path, char *buffer,
                              size_t size, size_t *read)
{
    if (!path || !path->references) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(path->file.mount, &namespace_guard);
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    if ((kernel_vfs_path_mode(path) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK)
        return -KERNEL_EINVAL;
    return instance->ops->readlink(path->file.private_data, buffer, size, read);
}

static void release_path_pin(struct kernel_vfs_path **path)
{
    if (*path && kernel_vfs_path_release(path) != 0) __builtin_trap();
}
#define VFS_PATH_PIN(name, value) \
    struct kernel_vfs_path *name __attribute__((cleanup(release_path_pin))) = (struct kernel_vfs_path *)(value); \
    if (kernel_vfs_path_acquire(name) != 0) __builtin_trap()

int kernel_vfs_path_stat(const struct kernel_vfs_path *path,
                         struct kernel_vfs_stat *stat)
{
    if (path == 0 || path->references == 0U)
        return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    return kernel_vfs_fstat(&path->file, stat);
}

int kernel_vfs_path_lookup(struct kernel_vfs_path *parent,
                           const char *name, size_t name_length,
                           struct kernel_vfs_path **owner)
{
    if (!parent || !parent->file.mount || !parent->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, parent);
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(parent->file.mount, &namespace_guard);
    if (((struct kernel_vfs_instance *)parent->file.mount->private_data)->quiescing)
        return -KERNEL_EIO;

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
    if (name_length == 1U && name[0] == '.') {
        result = kernel_vfs_path_acquire(parent);
        if (result == 0) *owner = parent;
        return result;
    }
    if (name_length == 2U && name[0] == '.' && name[1] == '.') {
        uintptr_t irq = arch_interrupt_save();
        struct kernel_vfs_path *ancestor = parent;
        while (ancestor->parent == 0 && ancestor->file.mount->covered_path)
            ancestor = ancestor->file.mount->covered_path;
        if (ancestor->parent) ancestor = ancestor->parent;
        result = kernel_vfs_path_acquire(ancestor);
        arch_interrupt_restore(irq);
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
    for (child = adapter->paths; child != 0; child = child->next) {
        if (!child->detached && child->parent == parent &&
            strlen(child->name) == name_length &&
            !memcmp(child->name, name, name_length)) {
            if (kernel_vfs_path_inode(child) != inode) {
                child->detached = 1U;
                continue;
            }
            result = kernel_vfs_path_acquire(child);
            if (result) return result;
            *owner = child;
            result = enter_mounts(owner);
            if (result) (void)kernel_vfs_path_release(owner);
            return result;
        }
    }
    allocation = kernel_heap_allocate_zeroed(adapter->heap, 1U,
                    sizeof(*child) + name_length + 1U, (void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    result = kernel_vfs_path_acquire(parent);
    if (result != 0) {
        if (kernel_heap_release(adapter->heap, child) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
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
        if (kernel_heap_release(adapter->heap, child) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return result;
    }
    child->name = child->initial_name;
    memcpy(child->name, name, name_length);
    child->name[name_length] = '\0';
    register_path(child);
    *owner = child;
    result = enter_mounts(owner);
    if (result) (void)kernel_vfs_path_release(owner);
    return result;
}

static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          int follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256])
{
    struct kernel_vfs_path *current;
    struct kernel_vfs_instance *adapter;
    char *work = 0;
    char *pending, *target;
    size_t cursor = 0U;
    size_t length;
    uint32_t follows = 0U;
    enum kernel_heap_status allocation;
    int result = 0;

    if (start == 0 || root == 0 || path == 0 || owner == 0 ||
        *owner != 0 || start->file.mount == 0 ||
        start->file.mount->private_data == 0)
        return -KERNEL_EINVAL;
    if (path[0] == '\0') return -KERNEL_ENOENT;
    adapter = start->file.mount->private_data;
    length = strlen(path);
    if (length >= KERNEL_FS_PATH_MAX) return -KERNEL_ENAMETOOLONG;
    allocation = kernel_heap_allocate(adapter->heap,
                                      2U * KERNEL_FS_PATH_MAX,
                                      (void **)&work);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    pending = work;
    target = work + KERNEL_FS_PATH_MAX;
    memcpy(pending, path, length + 1U);
    current = pending[0] == '/' ? root : start;
    result = kernel_vfs_path_acquire(current);
    if (result != 0) {
        current = 0;
        goto Finish;
    }
    while (pending[cursor] != '\0') {
        struct kernel_vfs_path *next = 0;
        size_t begin, end, component_length;
        int suffix;
        int trailing_only;

        while (pending[cursor] == '/') cursor++;
        if (pending[cursor] == '\0') break;
        begin = cursor;
        while (pending[cursor] != '\0' && pending[cursor] != '/') cursor++;
        end = cursor;
        component_length = end - begin;
        suffix = pending[end] != '\0';
        {
            size_t remainder = end;
            while (pending[remainder] == '/') remainder++;
            trailing_only = pending[remainder] == '\0';
        }
        if (component_length > 255U) {
            result = -KERNEL_ENAMETOOLONG;
            goto Finish;
        }
        if (current == root && component_length == 2U &&
            pending[begin] == '.' && pending[begin + 1U] == '.') {
            result = kernel_vfs_path_acquire(root);
            if (!result) next = root;
        } else {
            result = kernel_vfs_path_lookup(current, pending + begin,
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
        if ((next->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK &&
            ((suffix && (!trailing_only || follow_trailing_final == 1)) ||
             follow_final)) {
            size_t target_length = 0U;
            size_t remainder = strlen(pending + end);
            int backend;
            if (++follows > VFS_SYMLINK_MAX_FOLLOWS) {
                result = -KERNEL_ELOOP;
                (void)kernel_vfs_path_release(&next);
                goto Finish;
            }
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
            memmove(pending + target_length, pending + end, remainder + 1U);
            memcpy(pending, target, target_length);
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
            !(trailing_only && follow_trailing_final == 2)) {
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
    if (current != 0) (void)kernel_vfs_path_release(&current);
    (void)kernel_heap_release(adapter->heap, work);
    return result;
}

int kernel_vfs_path_resolve(struct kernel_vfs_path *start,
                            struct kernel_vfs_path *root,
                            const char *path, int follow_final,
                            struct kernel_vfs_path **owner)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, start);
    return resolve_object(start, root, path, follow_final, 0, 0, 1,
                          owner, 0);
}

int kernel_vfs_path_root(struct kernel_vfs_mount *mount,
                         struct kernel_heap *heap,
                         struct kernel_vfs_path **owner)
{
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
    for (path = adapter->paths; path != 0; path = path->next) {
        if (path->parent == 0) {
            result = kernel_vfs_path_acquire(path);
            if (result == 0) *owner = path;
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
        if (kernel_heap_release(heap, path) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return result;
    }
    path->heap = heap;
    path->references = 1U;
    path->name = path->initial_name;
    path->name[0] = '/';
    register_path(path);
    *owner = path;
    return 0;
}

int kernel_vfs_path_acquire(struct kernel_vfs_path *path)
{
    if (path == 0 || path->references == 0U ||
        path->references == UINT32_MAX) return -KERNEL_EOVERFLOW;
    path->references++;
    return 0;
}

int kernel_vfs_path_release(struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *path;
    if (owner == 0 || *owner == 0) return -KERNEL_EINVAL;
    path = *owner;
    if (path->references == 0U) __builtin_trap();
    if (path->references > 1U) {
        path->references--;
        *owner = 0;
        return 0;
    }
    while (path != 0) {
        struct kernel_vfs_path *parent;
        if (path->references == 0U) __builtin_trap();
        if (path->references > 1U) {
            path->references--;
            *owner = 0;
            return 0;
        }
        /* Real backend close failures transfer to the mount cleanup list in
         * kernel_vfs_node_release.  Any remaining close error is an ownership
         * invariant violation, not a retryable path-release result. */
        unregister_path(path);
        if (kernel_vfs_close(&path->file) != 0) __builtin_trap();
        parent = path->parent;
        path->references = 0U;
        if (path->name != path->initial_name)
            (void)kernel_heap_release(path->heap, path->name);
        if (kernel_heap_release(path->heap, path) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
        path = parent;
        *owner = path;
    }
    *owner = 0;
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
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(path->file.mount, &namespace_guard);

    if (!path || !path->references) return -KERNEL_EINVAL;
    return kernel_vfs_file_set_times(&path->file, times);
}

int kernel_vfs_path_set_mode(struct kernel_vfs_path *path, uint32_t mode)
{
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(path->file.mount, &namespace_guard);

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
    VFS_PATH_PIN(path_pin, path);
    if (!root || !buffer || capacity < 2U) return -KERNEL_ERANGE;
    uintptr_t irq = arch_interrupt_save();
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
    arch_interrupt_restore(irq);
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

const char *kernel_vfs_path_name(const struct kernel_vfs_path *path)
{
    return path && path->references ? path->name : 0;
}

int kernel_vfs_path_open(struct kernel_vfs_path *path,
                         struct kernel_vfs_file *file)
{
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(path->file.mount, &namespace_guard);

    if (!path || !file || file->path) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_acquire(path);
    if (result) return result;
    result = vfs_open_inode_raw(path->file.mount,
              kernel_vfs_path_inode(path), path->file.mode, file);
    if (result) (void)kernel_vfs_path_release(&path);
    else file->path = path;
    return result;
}

int kernel_vfs_open_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       int follow_final, struct kernel_vfs_file *file)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, follow_final,
                                        &resolved);
    if (!result) {
        result = (resolved->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK
                     ? -KERNEL_ELOOP : kernel_vfs_path_open(resolved, file);
        (void)kernel_vfs_path_release(&resolved);
    }
    return result;
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
                             int follow_final)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_open_at(root, root, path, follow_final, file);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount, path, file, 1);
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
                          char missing_name[256], char **backend,
                          struct kernel_lock_guard *guard)
{
    if (!start || !root || !input) return -KERNEL_EINVAL;
    struct kernel_heap *heap = start->heap;
    int result = resolve_object(start, root, input, follow, missing, trailing,
                                follow_trailing, path, missing_name);
    if (result) return result;
    kernel_vfs_namespace_lock((*path)->file.mount, guard);
    if (((struct kernel_vfs_instance *)(*path)->file.mount->private_data)->quiescing) {
        (void)kernel_vfs_path_release(path);
        return -KERNEL_EIO;
    }
    /* PATH_MAX constrains the supplied path, not a dirfd's ancestry. The
     * pathname-only backend bridge needs the full current ancestor chain. */
    size_t capacity = 2U;
    for (const struct kernel_vfs_path *cursor = *path; cursor->parent;
         cursor = cursor->parent) {
        if (cursor->detached ||
            ((struct kernel_vfs_node *)cursor->file.private_data)->unlinked) {
            result = -KERNEL_ENOENT;
            goto Finish;
        }
        size_t length = strlen(cursor->name) + 1U;
        if (length > SIZE_MAX - capacity) { result = -KERNEL_ENOMEM; goto Finish; }
        capacity += length;
    }
    size_t length = strlen(missing_name) + 1U;
    if (length > SIZE_MAX - capacity) { result = -KERNEL_ENOMEM; goto Finish; }
    capacity += length;
    enum kernel_heap_status status = kernel_heap_allocate(heap, capacity,
                                                           (void **)backend);
    if (status != KERNEL_HEAP_STATUS_OK) {
        result = status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        goto Finish;
    }
    result = path_to_backend_name(*path, missing_name, *backend, capacity);
Finish:
    if (result) {
        (void)kernel_vfs_path_release(path);
        if (*backend) release_path(heap, *backend);
        *backend = 0;
    }
    return result;
}

int kernel_vfs_create_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         uint32_t mode, struct kernel_vfs_file *file)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);

    struct kernel_vfs_path *parent = 0, *child = 0;
    char name[256] = {0};
    char *backend = 0;
    if (!start || !file || file->state || file->private_data || file->path)
        return -KERNEL_EINVAL;
    int result = mutation_path(start, root, input, 1, 1, 0, 1,
                                &parent, name, &backend, &namespace_guard);
    if (result) {
        size_t length = input ? strlen(input) : 0U;
        return result == -KERNEL_ENOENT && length && input[length - 1U] == '/'
                   ? -KERNEL_EISDIR : result;
    }
    if (!name[0]) { result = -KERNEL_EEXIST; goto Finish; }
    struct kernel_vfs_mount *target_mount = parent->file.mount;
    struct kernel_vfs_instance *target_instance = target_mount->private_data;
    if (target_instance->read_only || !target_instance->ops->create) {
        result = -KERNEL_EROFS;
        goto Finish;
    }
    uint64_t present_inode;
    uint32_t present_mode;
    result = target_instance->ops->lookup(target_instance,
                                          kernel_vfs_path_inode(parent),
                                          name, strlen(name),
                                          &present_inode, &present_mode);
    if (result == 0) { result = -KERNEL_EEXIST; goto Finish; }
    if (result != -KERNEL_ENOENT) goto Finish;
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(
        start->heap, 1U, sizeof(*child) + strlen(name) + 1U, (void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    /* Reserve the path and its parent reference before creating disk state. */
    child->heap = start->heap;
    child->parent = parent;
    parent = 0;
    child->references = 1U;
    child->name = child->initial_name;
    memcpy(child->name, name, strlen(name) + 1U);
    result = vfs_create_raw(target_mount, backend, mode, file);
    if (!result) {
        struct kernel_vfs_node *node = file->private_data;
        struct kernel_vfs_instance *adapter = file->mount->private_data;
        child->file = *file;
        node->references++;
        node->open_files++;
        adapter->external_files++;
        file->path = child;
        register_path(child);
        child = 0;
    }
Finish:
    if (child) {
        (void)kernel_vfs_path_release(&child->parent);
        release_path(child->heap, (char *)child);
    }
    if (parent) (void)kernel_vfs_path_release(&parent);
    release_path(start->heap, backend);
    return result;
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
        if (node->write_openers > 0U) lease_error = -KERNEL_ETXTBSY;
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
    return finish_executable_open(file, kernel_vfs_open(mount, path, file));
}

int kernel_vfs_open_executable_at(struct kernel_vfs_path *start,
                                  struct kernel_vfs_path *root,
                                  const char *path, struct kernel_vfs_file *file)
{
    return finish_executable_open(file, kernel_vfs_open_at(start, root, path,
                                                           1, file));
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
    while (*bytes_read < size && offset < node->size) {
        uint64_t address;
        size_t valid, start = (size_t)(offset & (BOAROS_PAGE_SIZE - 1U));
        void *page;
        enum kernel_page_cache_status status = kernel_page_cache_get(
              cache, file, offset >> BOAROS_PAGE_SHIFT, &address, &valid);
        if (status != KERNEL_PAGE_CACHE_STATUS_OK)
            return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                       ? -KERNEL_ENOMEM : -KERNEL_EIO;
        if (physical_page_resolve(cache->allocator, address, &page) !=
                PHYSICAL_PAGE_STATUS_OK || valid <= start) __builtin_trap();
        size_t count = valid - start;
        if (count > size - *bytes_read) count = size - *bytes_read;
        memcpy((unsigned char *)buffer + *bytes_read,
                (unsigned char *)page + start, count);
        (void)physical_page_release(cache->allocator, address);
        *bytes_read += count;
        offset += count;
    }
    return 0;
}

static void apply_truncated_size(struct kernel_vfs_node *node,
                                 struct kernel_vfs_file *file, uint64_t size)
{
    uint64_t old_size = node->size;
    if (size > old_size && node->instance->page_cache)
        kernel_page_cache_extend(node->instance->page_cache, node, old_size, size);
    node->size = size;
    file->size = node->size;
    if (node->size < old_size) {
        for (struct kernel_file_mapping *mapping = node->mappings;
             mapping != 0; mapping = mapping->next) {
            mapping->truncate(mapping->owner, node, node->size);
        }
    }
    if (size < old_size && node->instance->page_cache)
        kernel_page_cache_truncate(node->instance->page_cache, node, size);
}

int kernel_vfs_pwrite(struct kernel_vfs_file *file,
                      uint64_t offset,
                      const void *buffer,
                      size_t size,
                      size_t *bytes_written)
{
    struct kernel_vfs_node *node;
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
    result = node->instance->ops->error(node->instance);
    if (result) return result;
    if (size == 0U) {
        *bytes_written = 0U;
        return 0;
    }

    if (offset >= node->max_size) return -KERNEL_EFBIG;
    if (size > node->max_size - offset)
        size = (size_t)(node->max_size - offset);
    result = node->memory
        ? node->instance->ops->memory_write(node, offset, buffer, size, &written)
        : kernel_page_cache_write(node->instance->page_cache, file, offset, buffer, size, &written);
    file->size = node->size;
    *bytes_written = written;
    if (written > size) {
        return -KERNEL_EIO;
    }
    if (result != 0 && written == 0U) {
        return result;
    }
    return 0;
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

int kernel_vfs_path_truncate(struct kernel_vfs_path *path, uint64_t size)
{
    if (!path || !path->references) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    if ((kernel_vfs_path_mode(path) & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR)
        return -KERNEL_EISDIR;
    return kernel_vfs_ftruncate(&path->file, size);
}

int kernel_vfs_ftruncate(struct kernel_vfs_file *file,
                         uint64_t size)
{
    struct kernel_vfs_node *node;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
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
    if (node->exec_users > 0U) {
        return -KERNEL_ETXTBSY;
    }

    /* Even a same-size ftruncate updates mtime/ctime. Reconciliation also
     * preserves visible inode mutations when the backend reports an error. */
    if (size > node->max_size) return -KERNEL_EFBIG;
    if (node->memory) apply_truncated_size(node, file, size);
    result = node->memory ? 0 : kernel_page_cache_writeback_before(node->instance->page_cache, node, size);
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
        (void)kernel_vfs_try_release_orphan(node);
    }

    file->state = VFS_FILE_STATE_CLEANUP;
    if (kernel_vfs_node_release(&node) != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->external_files == 0U) {
        return -KERNEL_EIO;
    }
    adapter->external_files--;
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

uint64_t kernel_vfs_file_size(const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 ||
        file->state != VFS_FILE_STATE_LIVE) {
        return 0U;
    }
    node = file->private_data;
    return node->size;
}

int kernel_vfs_stat_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       int follow_final, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, follow_final,
                                         &resolved);
    if (!result) {
        result = kernel_vfs_path_stat(resolved, stat);
        (void)kernel_vfs_path_release(&resolved);
    } else if (follow_final &&
               (result == -KERNEL_ENOENT || result == -KERNEL_ENXIO)) {
        int original = result;
        /* 跟随无路径对象失败时，只重查最终链接并向其后端索取元数据。 */
        if (!kernel_vfs_path_resolve(start, root, path, 0, &resolved)) {
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
    KERNEL_LOCK_SCOPE(namespace_guard);
    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &path, name, &backend, &namespace_guard);
    if (result) return result;
    if (!name[0]) {
        result = -KERNEL_EEXIST;
    } else {
        struct kernel_vfs_instance *instance = path->file.mount->private_data;
        result = instance->read_only ? -KERNEL_EROFS : !instance->ops->mknod
                     ? -KERNEL_ENOTSUP
                     : instance->ops->mknod(instance, backend, type, mode, device);
    }
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
    return result;
}

int kernel_vfs_mkdir_at(struct kernel_vfs_path *start,
                        struct kernel_vfs_path *root, const char *input,
                        uint32_t mode)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);

    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    int result = mutation_path(start, root, input, 0, 1, 1, 2,
                                &path, name, &backend, &namespace_guard);
    if (result) return result;
    result = !name[0] ? -KERNEL_EEXIST :
                 kernel_vfs_mount_is_readonly(path->file.mount) ? -KERNEL_EROFS :
                 vfs_mkdir_raw(path->file.mount, backend, mode);
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
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

int kernel_vfs_unlink_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         int directory)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);

    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    int special = special_last_component(input);
    if (directory && special) return special == 1 ? -KERNEL_EINVAL :
                          special == 2 ? -KERNEL_ENOTEMPTY : -KERNEL_EBUSY;
    int result = mutation_path(start, root, input, 0, 0, 0, 0,
                                &path, name, &backend, &namespace_guard);
    if (result) return result;
    if (path->parent == 0 && path->file.mount->covered_path)
        result = -KERNEL_EBUSY;
    else if (kernel_vfs_mount_is_readonly(path->file.mount))
        result = -KERNEL_EROFS;
    else
        result = directory ? vfs_rmdir_raw(path->file.mount, backend)
                           : vfs_unlink_raw(path->file.mount, backend);
    if (!result) path->detached = 1U;
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
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
    KERNEL_LOCK_SCOPE(namespace_guard);
    KERNEL_LOCK_SCOPE(inode_guard);
    struct kernel_vfs_path *parent = 0;
    char name[256] = {0}, *backend = 0;
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &parent, name, &backend, &namespace_guard);
    if (result) return result;
    struct kernel_vfs_instance *instance = parent->file.mount->private_data;
    /* 目标存在优先于跨挂载；NULL 源表示已 pin 的无路径 pipe/socket OFD。 */
    if (!name[0]) result = -KERNEL_EEXIST;
    else if (instance->read_only) result = -KERNEL_EROFS;
    else if (!source || source->file.mount != parent->file.mount)
        result = -KERNEL_EXDEV;
    else if ((source->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ||
             !instance->ops->link) result = -KERNEL_EPERM;
    else {
        struct kernel_vfs_node *node = source->file.private_data;
        kernel_vfs_node_lock(node, &inode_guard, 1);
        /* dentry 可已摘除但 inode 仍有别名；只有最后链接消失才禁止复活。 */
        result = node->unlinked ? -KERNEL_ENOENT :
            instance->ops->link(instance, node->inode,
                               kernel_vfs_path_inode(parent), name);
    }
    kernel_lock_scope_release(&inode_guard);
    (void)kernel_vfs_path_release(&parent);
    release_path(start->heap, backend);
    return result;
}

int kernel_vfs_symlink_at(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *target,
                          const char *input)
{
    if (!start || !start->file.mount || !start->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);

    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    if (!start || !target || !target[0]) return -KERNEL_EINVAL;
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &path, name, &backend, &namespace_guard);
    if (result) return result;
    struct kernel_vfs_instance *instance = path->file.mount->private_data;
    result = !name[0] ? -KERNEL_EEXIST :
             instance->read_only || !instance->ops->symlink ? -KERNEL_EROFS :
             instance->ops->symlink(instance, target, backend);
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
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
        if ((resolved->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK)
            result = -KERNEL_EINVAL;
        else
            result = ((struct kernel_vfs_instance *)resolved->file.mount->private_data)->ops->readlink(
                resolved->file.private_data, buffer, size, bytes_read);
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
                          int *special, int *trailing)
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
        if (!result) *parent = base;
        return result;
    }
    char *prefix;
    enum kernel_heap_status allocation = kernel_heap_allocate(start->heap,
                                                   begin + 1U, (void **)&prefix);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    memcpy(prefix, input, begin);
    prefix[begin] = 0;
    int result = kernel_vfs_path_resolve(start, root, prefix, 1, parent);
    release_path(start->heap, prefix);
    return result;
}

int kernel_vfs_rename_at(struct kernel_vfs_path *old_start,
                         struct kernel_vfs_path *new_start,
                         struct kernel_vfs_path *root,
                         const char *old_input, const char *new_input,
                         unsigned flags)
{
    if (!old_start || !old_start->file.mount || !old_start->file.mount->private_data) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(namespace_guard);

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
                             old_component, &old_special, &old_trailing);
    if (result) goto Finish;
    result = rename_parent(new_start, root, new_input, &new_parent,
                             missing, &new_special, &new_trailing);
    if (result) goto Finish;
    if (source_parent->file.mount != new_parent->file.mount) {
        result = -KERNEL_EXDEV;
        goto Finish;
    }
    kernel_vfs_namespace_lock(source_parent->file.mount, &namespace_guard);
    struct kernel_vfs_instance *adapter = source_parent->file.mount->private_data;
    if (old_special) { result = -KERNEL_EBUSY; goto Finish; }
    if (new_special) { result = flags & 1U ? -KERNEL_EEXIST : -KERNEL_EBUSY; goto Finish; }
    if (adapter->read_only || !adapter->ops->rename) {
        result = -KERNEL_EROFS;
        goto Finish;
    }
    result = kernel_vfs_path_lookup(source_parent, old_component,
                                    strlen(old_component), &source);
    if (result) goto Finish;
    if (source->file.mount != source_parent->file.mount) {
        result = -KERNEL_EBUSY;
        goto Finish;
    }
    result = kernel_vfs_path_lookup(new_parent, missing, strlen(missing), &target);
    if (result && result != -KERNEL_ENOENT) goto Finish;
    if (target && target->file.mount != new_parent->file.mount) {
        result = -KERNEL_EBUSY;
        goto Finish;
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
    enum kernel_heap_status allocation = kernel_heap_allocate(adapter->heap,
                                  strlen(missing) + 1U, (void **)&new_name);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    memcpy(new_name, missing, strlen(missing) + 1U);
    /* The namespace owner spans the disk transaction and publication of
     * dentry identity, including device sleeps. */
    result = adapter->ops->rename(adapter,
                 kernel_vfs_path_inode(source->parent), source->name,
                 kernel_vfs_path_inode(new_parent), new_name, flags, &renamed);
    if (result || !renamed.changed) goto Finish;
    if (target && renamed.replaced_inode !=
                      kernel_vfs_path_inode(target)) __builtin_trap();
    if (!target && renamed.replaced_inode) __builtin_trap();
    struct kernel_vfs_path *old_parent = source->parent;
    char *old_name = source->name;
    uintptr_t irq = arch_interrupt_save();
    if (target) {
        struct kernel_vfs_node *node = target->file.private_data;
        target->detached = 1U;
        if (renamed.replaced_last_link) node->unlinked = 1U;
    }
    source->parent = new_parent;
    source->name = new_name;
    arch_interrupt_restore(irq);
    new_parent = 0;
    new_name = 0;
    if (old_name != source->initial_name) release_path(adapter->heap, old_name);
    (void)kernel_vfs_path_release(&old_parent);
Finish:
    if (new_name) release_path(old_start->heap, new_name);
    if (new_parent) (void)kernel_vfs_path_release(&new_parent);
    if (target) (void)kernel_vfs_path_release(&target);
    if (source) (void)kernel_vfs_path_release(&source);
    if (source_parent) (void)kernel_vfs_path_release(&source_parent);
    return result;
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
    if (node == 0 || node->references == 0U || node->closed ||
        node->references == UINT32_MAX) {
        return -KERNEL_EINVAL;
    }
    node->references++;
    return 0;
}

static void release_node_pin(struct kernel_vfs_node **owner)
{
    if (*owner && kernel_vfs_node_release(owner)) __builtin_trap();
}

void kernel_file_mapping_register(struct kernel_file_mapping *mapping)
{
    if (mapping == 0 || mapping->node == 0 || mapping->owner == 0 ||
        mapping->truncate == 0 || mapping->previous != 0) {
        __builtin_trap();
    }
    mapping->next = mapping->node->mappings;
    mapping->previous = &mapping->node->mappings;
    if (mapping->next != 0) mapping->next->previous = &mapping->next;
    mapping->node->mappings = mapping;
}

void kernel_file_mapping_unregister(struct kernel_file_mapping *mapping)
{
    if (mapping->previous == 0) return;
    *mapping->previous = mapping->next;
    if (mapping->next != 0) mapping->next->previous = mapping->previous;
    mapping->next = 0;
    mapping->previous = 0;
}

int kernel_vfs_node_release(struct kernel_vfs_node **owner)
{
    struct kernel_vfs_node *node;
    struct kernel_vfs_node **link;
    int result;

    if (owner == 0 || *owner == 0) {
        return -KERNEL_EINVAL;
    }
    node = *owner;
    if (node->references > 1U) {
        node->references--;
        *owner = 0;
        return 0;
    }
    if (node->references == 1U) {
        if (node->mappings != 0 || node->fifo_pipe || node->cache_pages ||
            node->dirty_cache_pages.head || node->dirty_cache_pages.count) __builtin_trap();
        if (!kernel_record_lock_state_empty(&node->record_locks))
            __builtin_trap();
        node->references = 0U;
        link = &node->instance->nodes;
        while (*link != 0 && *link != node) {
            link = &(*link)->next;
        }
        if (*link != node) {
            return -KERNEL_EIO;
        }
        *link = node->next;
        node->next = 0;
        if (node->unlinked && !node->retired) {
            node->next = node->instance->cleanup_nodes;
            node->instance->cleanup_nodes = node;
            *owner = 0;
            return 0;
        }
    }
    /* 干净页回收期间也可能释放最后引用；close_node 不得等待存储 I/O。 */
    if (!node->closed) {
        result = node->instance->ops->close_node(node);
        if (result != 0) {
            node->next = node->instance->cleanup_nodes;
            node->instance->cleanup_nodes = node;
            *owner = 0;
            return 0;
        }
        node->closed = 1U;
    }
    (void)kernel_heap_release(node->instance->heap, node);
    *owner = 0;
    return 0;
}

uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node)
{
    return node != 0 && node->references != 0U && !node->closed
               ? node->size : 0U;
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
    if (end > node->size) node->size = end;
}

int kernel_vfs_node_writeback_allowed(const struct kernel_vfs_node *node)
{
    return node != 0 && node->instance->ops->writeback_allowed(node->instance);
}

void kernel_vfs_record_writeback_error(struct kernel_vfs_node *node, int error)
{
    uintptr_t irq = arch_interrupt_save();
    node->writeback_error = error;
    node->writeback_error_sequence++;
    if (node->writeback_error_sequence == 0) __builtin_trap();
    /* 内存不足不是已发生的设备写回失败，不污染后续独立打开的错误游标。 */
    if (error == -KERNEL_EIO || error == -KERNEL_ENOSPC) {
        node->instance->writeback_error = error;
        if (++node->instance->writeback_error_sequence == 0) __builtin_trap();
    }
    arch_interrupt_restore(irq);
}

uint64_t kernel_vfs_mount_error_sequence(const struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return 0;
    uintptr_t irq = arch_interrupt_save();
    uint64_t sequence = ((struct kernel_vfs_instance *)mount->private_data)->writeback_error_sequence;
    arch_interrupt_restore(irq);
    return sequence;
}

struct sync_nodes {
    struct kernel_heap *heap;
    struct kernel_vfs_node **nodes;
    size_t count;
};

static void release_sync_nodes(struct sync_nodes *snapshot)
{
    uintptr_t irq = arch_interrupt_save();
    for (size_t i = 0; i < snapshot->count; i++)
        if (kernel_vfs_node_release(&snapshot->nodes[i])) __builtin_trap();
    arch_interrupt_restore(irq);
    if (snapshot->nodes && kernel_heap_release(snapshot->heap, snapshot->nodes) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

static int snapshot_sync_nodes(struct kernel_vfs_instance *instance,
                                struct sync_nodes *snapshot)
{
    snapshot->heap = instance->heap;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        size_t capacity = 0;
        for (struct kernel_vfs_node *node = instance->nodes; node; node = node->next) capacity++;
        arch_interrupt_restore(irq);
        if (!capacity) return 0;
        if (capacity > SIZE_MAX / sizeof(*snapshot->nodes)) return -KERNEL_ENOMEM;
        enum kernel_heap_status allocation = kernel_heap_allocate(instance->heap,
                   capacity * sizeof(*snapshot->nodes), (void **)&snapshot->nodes);
        if (allocation != KERNEL_HEAP_STATUS_OK) {
            if (allocation != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            return -KERNEL_ENOMEM;
        }
        irq = arch_interrupt_save();
        size_t count = 0;
        for (struct kernel_vfs_node *node = instance->nodes; node; node = node->next) count++;
        if (count > capacity) {
            arch_interrupt_restore(irq);
            if (kernel_heap_release(instance->heap, snapshot->nodes) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
            snapshot->nodes = 0;
            continue;
        }
        for (struct kernel_vfs_node *node = instance->nodes; node; node = node->next) {
            int result = kernel_vfs_node_acquire(node);
            if (result) { arch_interrupt_restore(irq); return result; }
            snapshot->nodes[snapshot->count++] = node;
        }
        arch_interrupt_restore(irq);
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
        KERNEL_LOCK_SCOPE(node_guard);
        kernel_vfs_node_lock(node, &node_guard, 0);
        int error = kernel_page_cache_writeback(instance->page_cache, node);
        if (error && error != -KERNEL_ENOMEM && error != -KERNEL_EBUSY)
            kernel_vfs_record_writeback_error(node, error);
        if (error && !result) result = error;
    }
    /* 捕获此前数据交接与namespace修改的durable目标一次，不等待checkpoint。 */
    if (instance->ops->sync_filesystem) {
        int error = instance->ops->sync_filesystem(instance, root->file.private_data);
        if (error) {
            kernel_vfs_record_writeback_error(root->file.private_data, error);
            if (!result) result = error;
        }
    } else if (instance->page_cache) __builtin_trap();
observe:
    if (observed_error) {
        uintptr_t irq = arch_interrupt_save();
        if (*observed_error != instance->writeback_error_sequence) {
            if (!result) result = instance->writeback_error;
            *observed_error = instance->writeback_error_sequence;
        }
        arch_interrupt_restore(irq);
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
    uintptr_t irq = arch_interrupt_save();
    for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount))
        if (m->id > limit) limit = m->id;
    arch_interrupt_restore(irq);
    /* OOM时按启动时的身份上界逐个取得引用，跨等待不保存树中的裸指针。
     * 已卸载者由卸载路径排空；新挂载不能无限延长本次sync。 */
    for (;;) {
        irq = arch_interrupt_save();
        struct kernel_vfs_mount *selected = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) {
            if (m->id <= last || m->id > limit ||
                ((struct kernel_vfs_instance *)m->private_data)->quiescing) continue;
            if (!selected || m->id < selected->id) selected = m;
        }
        if (!selected) { arch_interrupt_restore(irq); return; }
        struct kernel_vfs_path *path = selected == mount ? root : selected->root_path;
        if (!path || kernel_vfs_path_acquire(path)) __builtin_trap();
        last = selected->id;
        arch_interrupt_restore(irq);
        (void)kernel_vfs_sync_mount(path->file.mount, 0);
        if (kernel_vfs_path_release(&path)) __builtin_trap();
    }
}

void kernel_vfs_sync_all(struct kernel_vfs_path *root)
{
    if (!root || !root->file.mount) return;
    struct kernel_vfs_mount *mount = root->file.mount;
    struct kernel_heap *heap = ((struct kernel_vfs_instance *)mount->private_data)->heap;
    struct kernel_vfs_path **paths = 0;
    size_t count = 0;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        size_t capacity = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) capacity++;
        arch_interrupt_restore(irq);
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
        irq = arch_interrupt_save();
        size_t needed = 0;
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) needed++;
        if (needed > capacity) {
            arch_interrupt_restore(irq);
            if (kernel_heap_release(heap, paths) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
            paths = 0;
            continue;
        }
        for (struct kernel_vfs_mount *m = mount; m; m = next_sync_mount(m, mount)) {
            if (((struct kernel_vfs_instance *)m->private_data)->quiescing) continue;
            struct kernel_vfs_path *path = m == mount ? root : m->root_path;
            if (!path || kernel_vfs_path_acquire(path)) __builtin_trap();
            paths[count++] = path;
        }
        arch_interrupt_restore(irq);
        break;
    }
    for (size_t i = 0; i < count; i++) (void)kernel_vfs_sync_mount(paths[i]->file.mount, 0);
    for (size_t i = 0; i < count; i++)
        if (kernel_vfs_path_release(&paths[i])) __builtin_trap();
    if (kernel_heap_release(heap, paths) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
}

uint64_t kernel_vfs_error_sequence(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node != 0 ? node->writeback_error_sequence : 0;
}

static int vfs_sync_range_mode(struct kernel_vfs_file *file,
    uint64_t start, uint64_t end, uint64_t *observed_error, int data_only)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    int result;
    if (node == 0 || observed_error == 0) return -KERNEL_EBADF;
    if (start >= end) return -KERNEL_EINVAL;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG &&
        (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    result = 0;
    if (node->instance->read_only) goto observe;
    result = !node->instance->page_cache ? 0 : start == 0U && end == UINT64_MAX
        ? kernel_page_cache_writeback(node->instance->page_cache, node)
        : kernel_page_cache_writeback_range(node->instance->page_cache,
                                            node, start, end);
    if (result == 0) {
        result = node->instance->ops->sync_metadata(node, data_only && (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR);
        if (result != 0) kernel_vfs_record_writeback_error(node, result);
    }
observe:
    if (*observed_error != node->writeback_error_sequence) {
        if (result == 0) result = node->writeback_error;
        *observed_error = node->writeback_error_sequence;
    }
    return result;
}

int kernel_vfs_sync(struct kernel_vfs_file *file, int datasync,
                    uint64_t *observed_error)
{
    return vfs_sync_range_mode(file, 0U, UINT64_MAX, observed_error, datasync);
}

int kernel_vfs_sync_range(struct kernel_vfs_file *file, uint64_t start,
    uint64_t end, uint64_t *observed_error)
{ return vfs_sync_range_mode(file, start, end, observed_error, 0); }

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
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);

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
    if (!node || !node->references) __builtin_trap();
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
    int result = instance->ops->unmount(mount);
    if (!result && release_owner) release_owner(mount);
    return result;
}

int kernel_vfs_mount_statfs(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_statfs *stat)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->statfs(mount, stat);
}

int kernel_vfs_file_set_times(struct kernel_vfs_file *file,
                              const struct kernel_vfs_timespec times[2])
{
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->set_times(file, times);
}

int kernel_vfs_file_set_mode(struct kernel_vfs_file *file, uint32_t mode)
{
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->set_mode(file, mode);
}

int kernel_vfs_file_set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid)
{
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    if (instance->read_only) return -KERNEL_EROFS;
    if (!instance->ops->set_owner) return -KERNEL_ENOTSUP;
    return instance->ops->set_owner(file, uid, gid);
}

int kernel_vfs_path_set_owner(struct kernel_vfs_path *path, uint32_t uid, uint32_t gid)
{
    if (!path || !path->file.mount || !path->file.mount->private_data) return -KERNEL_EINVAL;
    VFS_PATH_PIN(path_pin, path);
    KERNEL_LOCK_SCOPE(namespace_guard);
    kernel_vfs_namespace_lock(path->file.mount, &namespace_guard);
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
    kernel_vfs_namespace_lock(mount, &namespace_guard);
    if (!path) {
        int error = instance->ops->error ? instance->ops->error(instance) : 0;
        if (error) return error;
        for (struct kernel_vfs_node *node = instance->nodes; node; node = node->next) {
            if (node->inode != inode_number || node->retired || node->closed) continue;
            if (node->references == UINT32_MAX) return -KERNEL_EOVERFLOW;
            struct kernel_vfs_node *pin __attribute__((cleanup(release_node_pin))) = 0;
            int pinned = kernel_vfs_node_acquire(node);
            if (pinned) return pinned;
            pin = node;
            /* 等待锁之前独立持有节点，回收者可在锁交接后归还最后缓存引用。 */
            KERNEL_LOCK_SCOPE(node_guard);
            kernel_vfs_node_lock(node, &node_guard, 1);
            if (node->open_files == UINT32_MAX ||
                instance->external_files == UINT32_MAX) return -KERNEL_EOVERFLOW;
            /* 已有 owner 保证 inode 有效；新 file 只取得资格，不再临时打开后端。 */
            node->open_files++;
            instance->external_files++;
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
                          const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->create(mount, path, mode, file);
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
    if (!file || !file->mount || !file->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = file->mount->private_data;
    if (instance->quiescing) return -KERNEL_EIO;
    return instance->ops->stat(file, stat);
}

static int vfs_mkdir_raw(struct kernel_vfs_mount *mount,
                         const char *path,
                         uint32_t mode)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->mkdir(mount, path, mode);
}

static int vfs_unlink_raw(struct kernel_vfs_mount *mount,
                          const char *path)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->unlink(mount, path);
}

static int vfs_rmdir_raw(struct kernel_vfs_mount *mount,
                         const char *path)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    return instance->ops->rmdir(mount, path);
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
    struct kernel_vfs_instance *instance = file->mount->private_data;
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
    struct kernel_vfs_read_span *spans, size_t count)
{
    if (!node || !node->mount || !node->mount->private_data ||
        count > KERNEL_VFS_READ_BATCH_MAX || (count && !spans)) return -KERNEL_EINVAL;
    for (size_t i = 0; i < count; i++) { spans[i].completed = 0; spans[i].error = -KERNEL_EINVAL; }
    for (size_t i = 0; i < count; i++) {
        uintptr_t a = (uintptr_t)spans[i].buffer, b = (uintptr_t)spans;
        size_t n = spans[i].size;
        if (spans[i].offset > INT64_MAX || n > UINT64_MAX - spans[i].offset ||
            (n && (!a || n - 1 > UINTPTR_MAX - a ||
            (a < b ? b - a < n : a - b < count * sizeof(*spans))))) return spans[i].error = -KERNEL_EINVAL;
        for (size_t j = 0; j < i; j++) {
            b = (uintptr_t)spans[j].buffer;
            if (n && spans[j].size && (a < b ? b - a < n : a - b < spans[j].size)) return spans[i].error = -KERNEL_EINVAL;
        }
    }
    for (size_t i = 0; i < count; i++) spans[i].error = 0;
    struct kernel_vfs_instance *instance = node->mount->private_data;
    if (instance->ops->pread_batch) return instance->ops->pread_batch(node, spans, count);
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        spans[i].error = instance->ops->pread(node, spans[i].offset, spans[i].buffer, spans[i].size, &spans[i].completed);
        if (!result) result = spans[i].error;
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

int kernel_vfs_node_writeback(struct kernel_vfs_node *node, uint64_t offset,
                              const void *buffer, size_t size, size_t *written)
{
    if (!node || !node->mount || !node->mount->private_data) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = node->mount->private_data;
    return instance->ops->writeback(node, offset, buffer, size, written);
}

/* 后端已准备好私有 handle；同一实例的活 inode 只发布一个通用节点。 */
static int discard_unpublished_node(struct kernel_vfs_node *node)
{
    struct kernel_vfs_instance *instance = node->instance;
    int result = instance->ops->close_node(node);
    if (result) {
        node->next = instance->cleanup_nodes;
        instance->cleanup_nodes = node;
    } else (void)kernel_heap_release(instance->heap, node);
    return result;
}

int kernel_vfs_publish_node(struct kernel_vfs_mount *mount,
    struct kernel_vfs_node *node, struct kernel_vfs_file *file, int creating)
{
    struct kernel_vfs_instance *instance = mount->private_data;
    struct kernel_vfs_node *existing;
    node->instance = instance;
    for (existing = instance->nodes; existing; existing = existing->next)
        if (existing->inode == node->inode && !existing->retired && !existing->closed) break;
    if (existing) {
        int pin_error = existing->references == UINT32_MAX ? -KERNEL_EOVERFLOW :
                        kernel_vfs_node_acquire(existing);
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
        if (instance->external_files == UINT32_MAX) {
            int error = discard_unpublished_node(node);
            return error ? error : -KERNEL_EOVERFLOW;
        }
        node->mount = mount;
        node->references = 1;
        node->open_files = 1;
        kernel_mutex_init(&node->write_operation, 15, (uintptr_t)node);
        kernel_rwlock_init(&node->io_lock, 30, (uintptr_t)node);
        kernel_record_lock_state_init(&node->record_locks);
        node->next = instance->nodes;
        instance->nodes = node;
    }
    file->private_data = node;
    file->mount = mount;
    file->size = node->size;
    file->mode = node->mode;
    file->state = VFS_FILE_STATE_LIVE;
    file->write_lease = 0;
    file->exec_lease = 0;
    instance->external_files++;
    return 0;
}

struct kernel_memory_object *kernel_vfs_file_memory(const struct kernel_vfs_file *file)
{ struct kernel_vfs_node *n = kernel_vfs_file_node(file); return n ? n->memory : 0; }

#if BOAROS_COST_DIAGNOSTICS
int kernel_vfs_file_is_cost(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node && node->generated_diagnostic;
}
#endif
