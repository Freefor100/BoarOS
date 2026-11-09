#include "vfs_internal.h"
#include "open_file_internal.h"
#include "char_device_internal.h"
#include <kernel/errno.h>
#include <kernel/credentials.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_mount_namespace {
    struct kernel_heap *heap;
    struct kernel_vfs_path *top;
    uint32_t references;
    int copied_tree;
    uint64_t id;
};

struct kernel_fs_context_record {
    struct kernel_vfs_path *root;
    struct kernel_vfs_path *cwd;
    uint32_t umask;
    uint32_t references;
    uint32_t exec_users;
    struct kernel_mount_namespace *namespace;
};

static void mount_namespace_get_locked(struct kernel_mount_namespace *ns)
{
    if (!ns->references || ns->references==UINT32_MAX) __builtin_trap();
    ns->references++;
}

static void mount_namespace_put(struct kernel_mount_namespace *ns)
{
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        if (!ns->references) __builtin_trap();
        if (--ns->references) return;
    }
    struct kernel_vfs_mount *top=kernel_vfs_path_mount(ns->top);
    if (kernel_vfs_path_release(&ns->top)) __builtin_trap();
    if (ns->copied_tree) kernel_vfs_retire_mount_tree(top);
    kernel_heap_release(ns->heap,ns);
}

static struct kernel_mount_namespace *mount_namespace_create(struct kernel_heap *heap)
{
    static uint64_t sequence=UINT64_C(4026531840);
    struct kernel_mount_namespace *ns=NULL;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*ns),(void **)&ns)) return NULL;
    KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
    if (sequence==UINT64_MAX) __builtin_trap();
    ns->id=sequence++;
    ns->heap=heap;ns->references=1;
    return ns;
}

static int empty_context(const struct kernel_fs_context *fs)
{
    return fs->state == KERNEL_FS_CONTEXT_EMPTY && fs->heap == 0 &&
           fs->record == 0;
}

static void finish_context(struct kernel_fs_context *fs,
                           enum kernel_fs_context_state state)
{
    fs->heap = 0;
    fs->record = 0;
    fs->state = state;
}

struct kernel_fs_paths kernel_fs_context_snapshot(const struct kernel_fs_context *fs)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    struct kernel_fs_paths paths = {0};
    if (kernel_fs_context_is_live(fs)) {
        paths.root = fs->record->root;
        paths.cwd = fs->record->cwd;
        paths.umask = fs->record->umask;
        if (kernel_vfs_path_acquire(paths.root) || kernel_vfs_path_acquire(paths.cwd))
            __builtin_trap();
    }
    return paths;
}

void kernel_fs_paths_release(struct kernel_fs_paths *paths)
{
    if (paths->cwd && kernel_vfs_path_release(&paths->cwd)) __builtin_trap();
    if (paths->root && kernel_vfs_path_release(&paths->root)) __builtin_trap();
}

static enum kernel_fs_context_status create_context(
    struct kernel_fs_context *fs,
    struct kernel_vfs_path *root,
    struct kernel_vfs_path *cwd,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap,
    uint32_t umask,
    struct kernel_mount_namespace *ns)
{
    struct kernel_fs_context_record *record;
    enum kernel_heap_status heap_status;

    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*record),
                                              (void **)&record);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                   : KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    if (root == 0) {
        int result = kernel_vfs_path_root(root_mount, heap, &root);
        if (result != 0) {
            kernel_heap_release(heap, record);
            return result == -KERNEL_ENOMEM
                       ? KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                       : KERNEL_FS_CONTEXT_STATUS_STATE;
        }
    } else if (kernel_vfs_path_acquire(root) != 0) {
        kernel_heap_release(heap, record);
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    if (cwd == 0) cwd = root;
    if (kernel_vfs_path_acquire(cwd) != 0) {
        (void)kernel_vfs_path_release(&root);
        kernel_heap_release(heap, record);
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    record->root = root;
    record->cwd = cwd;
    record->umask = umask;
    record->references = 1U;
    if (!ns) {
        ns=mount_namespace_create(heap);
        if (!ns) {
            if (kernel_vfs_path_release(&cwd) || kernel_vfs_path_release(&root))
                __builtin_trap();
            kernel_heap_release(heap,record);
            return KERNEL_FS_CONTEXT_STATUS_NO_MEMORY;
        }
        if (kernel_vfs_path_acquire(root)) __builtin_trap();
        ns->top=root;
        record->namespace=ns;
    }
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!record->namespace) {
        mount_namespace_get_locked(ns);
        record->namespace=ns;
    }
    fs->heap = heap;
    fs->record = record;
    fs->state = KERNEL_FS_CONTEXT_LIVE;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap)
{
    if (fs == 0 || root_mount == 0 || heap == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_context(fs)) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    return create_context(fs, 0, 0, root_mount, heap, 0022U, NULL);
}

enum kernel_fs_context_status kernel_fs_context_acquire(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!empty_context(destination) || !kernel_fs_context_is_live(source) ||
        source->record->references == UINT32_MAX) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    if (source->record->exec_users) return KERNEL_FS_CONTEXT_STATUS_BUSY;
    source->record->references++;
    destination->heap = source->heap;
    destination->record = source->record;
    destination->state = KERNEL_FS_CONTEXT_LIVE;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

static enum kernel_fs_context_status fork_context(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source, int copy_namespace, int *copy_error)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    struct kernel_heap *heap;
    struct kernel_mount_namespace *ns;
    struct kernel_fs_paths paths __attribute__((cleanup(kernel_fs_paths_release))) = {0};
    {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        if (!empty_context(destination) || !kernel_fs_context_is_live(source))
            return KERNEL_FS_CONTEXT_STATUS_STATE;
        heap = source->heap;
        ns=source->record->namespace;
        mount_namespace_get_locked(ns);
        paths = (struct kernel_fs_paths){source->record->root, source->record->cwd,
                                         source->record->umask};
        if (kernel_vfs_path_acquire(paths.root) || kernel_vfs_path_acquire(paths.cwd))
            __builtin_trap();
    }
    enum kernel_fs_context_status status;
    if (!copy_namespace)
        status=create_context(destination,paths.root,paths.cwd,NULL,heap,paths.umask,ns);
    else {
        status=KERNEL_FS_CONTEXT_STATUS_NO_MEMORY;
        struct kernel_mount_namespace *copy=mount_namespace_create(heap);
        if (copy) {
            struct kernel_vfs_path *original[3]={ns->top,paths.root,paths.cwd},*cloned[3]={0};
            int result=kernel_vfs_copy_mount_tree(kernel_vfs_path_mount(ns->top),original,cloned);
            if (result) {
                *copy_error=result;
                kernel_heap_release(heap,copy);
            } else {
                copy->top=cloned[0];copy->copied_tree=1;
                status=create_context(destination,cloned[1],cloned[2],NULL,heap,paths.umask,copy);
                if (kernel_vfs_path_release(&cloned[2]) || kernel_vfs_path_release(&cloned[1]))
                    __builtin_trap();
                mount_namespace_put(copy);
            }
        }
    }
    mount_namespace_put(ns);
    return status;
}

enum kernel_fs_context_status kernel_fs_context_fork(struct kernel_fs_context *destination,
    const struct kernel_fs_context *source)
{ return fork_context(destination,source,0,NULL); }

int kernel_fs_context_fork_mount_namespace(struct kernel_fs_context *destination,
    const struct kernel_fs_context *source)
{
    int error=0;
    enum kernel_fs_context_status status=fork_context(destination,source,1,&error);
    return error?error:!status?0:status==KERNEL_FS_CONTEXT_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EINVAL;
}

static int replace_context(struct kernel_fs_context *fs,struct kernel_fs_context *next,int exclusive)
{
    struct kernel_fs_context old;
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        if (!kernel_fs_context_is_live(fs) ||
            (exclusive && fs->record->references!=1)) return -KERNEL_EINVAL;
        old=*fs;*fs=*next;
        finish_context(next,KERNEL_FS_CONTEXT_MOVED);
    }
    if (kernel_fs_context_release(&old)) __builtin_trap();
    return 0;
}

int kernel_fs_context_unshare(struct kernel_fs_context *fs,int new_mount_namespace)
{
    struct kernel_fs_context next={0};
    int result;
    if (new_mount_namespace) result=kernel_fs_context_fork_mount_namespace(&next,fs);
    else {
        {
            KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
            if (!kernel_fs_context_is_live(fs)) return -KERNEL_EINVAL;
            if (fs->record->references==1) return 0;
        }
        enum kernel_fs_context_status status=kernel_fs_context_fork(&next,fs);
        result=!status?0:status==KERNEL_FS_CONTEXT_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EINVAL;
    }
    if (result) return result;
    result=replace_context(fs,&next,0);
    if (result && kernel_fs_context_release(&next)) __builtin_trap();
    return result;
}

int kernel_fs_context_set_mount_namespace(struct kernel_fs_context *fs,struct kernel_mount_namespace *ns)
{
    if (!ns || !kernel_fs_context_is_live(fs)) return -KERNEL_EINVAL;
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        if (fs->record->references!=1) return -KERNEL_EINVAL;
    }
    struct kernel_vfs_path *root=NULL;
    int result=kernel_vfs_path_resolve(ns->top,ns->top,"/",1,&root);
    if (result) return result;
    struct kernel_fs_context next={0};
    enum kernel_fs_context_status status=create_context(&next,root,root,NULL,fs->heap,
        kernel_fs_context_umask(fs),ns);
    if (kernel_vfs_path_release(&root)) __builtin_trap();
    if (status) return status==KERNEL_FS_CONTEXT_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EINVAL;
    result=replace_context(fs,&next,1);
    if (result && kernel_fs_context_release(&next)) __builtin_trap();
    return result;
}

uint64_t kernel_mount_namespace_id(const struct kernel_mount_namespace *ns)
{ return ns->id; }

static void mount_namespace_file_release(void *instance)
{ mount_namespace_put(instance); }

static uint32_t mount_namespace_file_poll(void *instance,uint32_t flags,uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)instance;(void)requested;(void)queue;
    return flags&010000000U?KERNEL_POLLNVAL:
        KERNEL_POLLIN|KERNEL_POLLOUT|KERNEL_POLLRDNORM|KERNEL_POLLWRNORM;
}

static const struct kernel_char_device mount_namespace_ops={
    .release=mount_namespace_file_release,.poll=mount_namespace_file_poll,
};

int kernel_mount_namespace_open(struct kernel_mount_namespace *ns,struct kernel_heap *heap,
    uint32_t flags,struct kernel_open_file_description **owner)
{
    if (flags & 0200000U) return -KERNEL_ENOTDIR;
    if (!(flags&010000000U) && (flags&3U || flags&01000U)) return -KERNEL_EACCES;
    struct kernel_open_file_description *file=NULL;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*file),(void **)&file)) return -KERNEL_ENOMEM;
    file->heap=heap;
    file->references=1;
    file->kind=KERNEL_OPEN_FILE_KIND_MOUNT_NAMESPACE;
    file->proc_identity=ns->id;
    file->open_flags=flags;
    file->credentials=kernel_credentials_get(kernel_credentials_current());
    file->device=&mount_namespace_ops;
    file->device_instance=ns;
    file->device_opened=1;
    kernel_mutex_init(&file->offset_lock,10,(uintptr_t)file);
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        mount_namespace_get_locked(ns);
    }
    *owner=file;
    return 0;
}

struct kernel_mount_namespace *kernel_mount_namespace_file(const struct kernel_open_file_description *file)
{
    return file && file->kind==KERNEL_OPEN_FILE_KIND_MOUNT_NAMESPACE
        ? file->device_instance:NULL;
}

struct kernel_vfs_mount *kernel_fs_context_mount_root(const struct kernel_fs_context *fs)
{
    KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
    return kernel_fs_context_is_live(fs)?kernel_vfs_path_mount(fs->record->namespace->top):NULL;
}

struct kernel_fs_mounts kernel_fs_context_mounts_snapshot(const struct kernel_fs_context *fs)
{
    struct kernel_fs_mounts mounts={0};
    KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
    if (kernel_fs_context_is_live(fs)) {
        mounts.namespace=fs->record->namespace;
        mounts.root=fs->record->root;
        mounts.top=mounts.namespace->top;
        mount_namespace_get_locked(mounts.namespace);
        if (kernel_vfs_path_acquire(mounts.root)) __builtin_trap();
    }
    return mounts;
}

void kernel_fs_mounts_release(struct kernel_fs_mounts *mounts)
{
    if (!mounts->namespace) return;
    if (kernel_vfs_path_release(&mounts->root)) __builtin_trap();
    mount_namespace_put(mounts->namespace);
    *mounts=(struct kernel_fs_mounts){0};
}

int kernel_fs_context_exec_begin(const struct kernel_fs_context *fs, struct kernel_task *task)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!kernel_fs_context_is_live(fs)) __builtin_trap();
    unsigned local = kernel_task_fs_group_users(task, fs->record);
    if (fs->record->references > local) return 1;
    if (fs->record->exec_users == UINT32_MAX) __builtin_trap();
    ++fs->record->exec_users;
    return 0;
}

void kernel_fs_context_exec_end(const struct kernel_fs_context *fs)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!kernel_fs_context_is_live(fs) || !fs->record->exec_users) __builtin_trap();
    --fs->record->exec_users;
}

enum kernel_fs_context_status kernel_fs_context_move(
    struct kernel_fs_context *destination,
    struct kernel_fs_context *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!empty_context(destination) ||
        source->state != KERNEL_FS_CONTEXT_LIVE ||
        source->heap == 0 || source->record == 0) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    *destination = *source;
    finish_context(source, KERNEL_FS_CONTEXT_MOVED);
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

uint32_t kernel_fs_context_umask(const struct kernel_fs_context *fs)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    return kernel_fs_context_is_live(fs) ? fs->record->umask : 0U;
}

int kernel_fs_context_exchange_umask(const struct kernel_fs_context *fs,
                                    uint32_t requested, uint32_t *previous)
{
    KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
    if (!kernel_fs_context_is_live(fs) || previous == 0) return -KERNEL_EINVAL;
    *previous = fs->record->umask;
    fs->record->umask = requested & 0777U;
    return 0;
}

static int set_directory(const struct kernel_fs_context *fs,
                         struct kernel_vfs_path *path, int root)
{
    struct kernel_vfs_stat stat;
    if (!kernel_fs_context_is_live(fs) || !path) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_stat(path, &stat);
    if (result) return result;
    if ((stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_ENOTDIR;
    result = kernel_vfs_path_permission(path,KERNEL_VFS_MAY_EXEC);
    if (result) return result;
    if (root && !kernel_credentials_capable(kernel_credentials_current(),
                                            KERNEL_CAP_SYS_CHROOT))
        return -KERNEL_EPERM;
    result = kernel_vfs_path_acquire(path);
    if (result) return result;
    struct kernel_vfs_path *previous;
    {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        if (!kernel_fs_context_is_live(fs)) __builtin_trap();
        previous = root ? fs->record->root : fs->record->cwd;
        if (root) fs->record->root = path;
        else fs->record->cwd = path;
    }
    if (kernel_vfs_path_release(&previous)) __builtin_trap();
    return 0;
}

int kernel_fs_context_set_cwd(const struct kernel_fs_context *fs,
                              struct kernel_vfs_path *path)
{
    return set_directory(fs, path, 0);
}

int kernel_fs_context_set_root(const struct kernel_fs_context *fs,
                               struct kernel_vfs_path *path)
{
    /* root与cwd各自持有路径；chroot保留原cwd，包括它仍位于新根外的情况。 */
    return set_directory(fs, path, 1);
}

enum kernel_fs_context_status kernel_fs_context_resolve_user_path(
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result)
{
    size_t path_length;
    enum kernel_uaccess_status access_status;

    if (!kernel_fs_context_is_live(fs) || mm == 0 || buffer == 0 ||
        capacity != KERNEL_FS_PATH_MAX || mount == 0 ||
        linux_result == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    access_status = kernel_copy_string_from_user(mm,
                                                 buffer,
                                                 user_path,
                                                 capacity,
                                                 &path_length);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) {
        *linux_result = -KERNEL_ENAMETOOLONG;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    return kernel_fs_context_resolve_kernel_path(fs,
                                                  dirfd,
                                                  buffer,
                                                  path_length,
                                                  buffer,
                                                  capacity,
                                                  mount,
                                                  linux_result);
}

enum kernel_fs_context_status kernel_fs_context_resolve_kernel_path(
    const struct kernel_fs_context *fs,
    int64_t dirfd,
    const char *path,
    size_t path_length,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result)
{
    KERNEL_FS_PATHS(paths, fs);
    struct kernel_vfs_path *resolved = 0;
    if (!kernel_fs_context_is_live(fs) || !path || !buffer ||
        capacity != KERNEL_FS_PATH_MAX || !mount || !linux_result)
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    if (!path_length) *linux_result = -KERNEL_ENOENT;
    else if (path_length >= capacity) *linux_result = -KERNEL_ENAMETOOLONG;
    else if (path[0] != '/' && dirfd != KERNEL_FS_AT_FDCWD)
        *linux_result = -KERNEL_EBADF;
    else {
        *linux_result = kernel_vfs_path_resolve(paths.cwd,
                            paths.root, path, 1, &resolved);
        if (!*linux_result) {
            *mount = kernel_vfs_path_mount(resolved);
            *linux_result = kernel_vfs_path_string(resolved, paths.root,
                                                   buffer, capacity);
            if (*linux_result == -KERNEL_ERANGE)
                *linux_result = -KERNEL_ENAMETOOLONG;
            (void)kernel_vfs_path_release(&resolved);
        }
    }
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

enum kernel_fs_context_status kernel_fs_context_release(struct kernel_fs_context *fs)
{
    if (!fs) return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    struct kernel_fs_context_record *record;
    struct kernel_heap *heap;
    {
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        if (!kernel_fs_context_is_live(fs) || !fs->record->references || !fs->record->root)
            return KERNEL_FS_CONTEXT_STATUS_STATE;
        heap = fs->heap;
        record = fs->record;
        finish_context(fs, KERNEL_FS_CONTEXT_RELEASED);
        if (--record->references) return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (kernel_vfs_path_release(&record->cwd) || kernel_vfs_path_release(&record->root))
        __builtin_trap();
    mount_namespace_put(record->namespace);
    kernel_heap_release(heap, record);
    return KERNEL_FS_CONTEXT_STATUS_OK;
}
