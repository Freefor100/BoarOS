#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/fanotify.h>
#include <kernel/fs_context.h>
#include <kernel/pid.h>
#include <kernel/wait_internal.h>
#include "char_device_internal.h"
#include "fanotify_internal.h"
#include "files/private.h"
#include "open_file_internal.h"
#include "uaccess_iov_internal.h"
#include "vfs_notify.h"
#include "vfs_objects.h"
#include "../third_party/lwext4/include/misc/tree.h"
#include <string.h>

enum {
    FAN_CLOEXEC = 1, FAN_NONBLOCK = 2, FAN_REPORT_FID = 0x200,
    FAN_MARK_ADD = 1, FAN_MARK_REMOVE = 2, FAN_MARK_DONT_FOLLOW = 4,
    FAN_MARK_ONLYDIR = 8, FAN_MARK_MOUNT = 0x10, FAN_MARK_IGNORED_MASK = 0x20,
    FAN_MARK_IGNORED_SURV_MODIFY = 0x40, FAN_MARK_FLUSH = 0x80,
    FAN_MARK_FILESYSTEM = 0x100, FAN_MARK_TYPE = 0x110,
    FAN_Q_OVERFLOW = 0x4000, FAN_EVENT_ON_CHILD = 0x08000000,
    FAN_ONDIR = VFS_NOTIFY_ISDIR, FAN_NOFD = -1,
    FAN_MAX_GROUPS = 128, FAN_MAX_MARKS = 8192, FAN_MAX_EVENTS = 16384,
    FAN_HASH_BUCKETS = 128, FAN_MERGE_LIMIT = 128,
    LINUX_O_CLOEXEC = 02000000, LINUX_O_NONBLOCK = 04000,
};

#define FAN_PATH_EVENTS (VFS_NOTIFY_ACCESS | VFS_NOTIFY_MODIFY | VFS_NOTIFY_OPEN | \
                         VFS_NOTIFY_CLOSE_WRITE | VFS_NOTIFY_CLOSE_NOWRITE)
#define FAN_EVENT_FLAGS (FAN_EVENT_ON_CHILD | FAN_ONDIR)
#define FAN_SUPPORTED_MASK (FAN_PATH_EVENTS | VFS_NOTIFY_ATTRIB | FAN_EVENT_FLAGS)
#define FAN_KNOWN_MASK UINT64_C(0x7b17bfff)
#define FAN_KNOWN_FLAGS 0x7fffU
#define FAN_KNOWN_MARK_FLAGS 0x7ffU
#define FAN_ADMIN_INIT 0x21bcU
#define FAN_EVENT_FILE_FLAGS (3U | 02000U | LINUX_O_NONBLOCK | 04010000U | \
                              LINUX_O_CLOEXEC | 00100000U | 01000000U)

struct fan_metadata {
    uint32_t event_len;
    uint8_t version, reserved;
    uint16_t metadata_len;
    uint64_t mask;
    int32_t fd, pid;
};
struct fan_fid_info {
    uint8_t type, pad;
    uint16_t len;
    int32_t fsid[2];
    uint32_t handle_bytes;
    int32_t handle_type;
    unsigned char handle[sizeof(struct kernel_vfs_handle)];
};
_Static_assert(sizeof(struct fan_metadata) == 24, "fanotify metadata ABI");
_Static_assert(offsetof(struct fan_fid_info, handle) == 20, "fanotify FID ABI");
struct fan_record {
    struct fan_metadata metadata;
    struct fan_fid_info fid;
};
_Static_assert(offsetof(struct fan_record, fid) == 24, "fanotify event ABI");

struct fan_group;
struct fan_key {
    struct kernel_vfs_instance *instance;
    uintptr_t object;
    uint64_t serial;
    unsigned type;
};
struct fan_mark {
    struct fan_key key;
    struct fan_group *group;
    uint64_t fsid;
    uint32_t mask, ignored;
    int survive_modify;
    struct fan_mark *group_next, **group_previous;
    RB_ENTRY(fan_mark) link;
};
struct fan_event {
    struct fan_event *next, *hash_next, **hash_previous;
    struct kernel_vfs_path *path;
    struct kernel_pid *sender;
    uint32_t mask;
    int32_t handle_type;
    uint64_t fsid;
    struct kernel_vfs_handle handle;
};
struct fan_group {
    struct kernel_heap *heap;
    struct kernel_uid_account *account;
    uint64_t serial;
    unsigned references, event_flags;
    bool fid, unprivileged;
    struct fan_mark *marks[3];
    struct kernel_raw_lock queue_lock;
    struct kernel_wait_queue ready;
    struct fan_event *first, **tail;
    struct fan_event *merge[FAN_HASH_BUCKETS];
    struct fan_event overflow;
    unsigned queued;
    bool overflow_queued, closed;
};
struct kernel_fanotify_read {
    struct fan_group *group;
    struct kernel_files *files;
    struct kernel_open_file_description *owner, *opened;
    void *vectors;
    struct fan_event *event;
    struct kernel_files_fd_reservation reservation;
};

static struct kernel_raw_lock mark_lock = {
    .rank = KERNEL_RAW_RANK_OBJECT, .initialized = KERNEL_RAW_INITIALIZED,
};
static uint64_t last_group;
static const struct kernel_fanotify_ops fan_ops;
static const struct kernel_char_device fan_file_ops;

static int mark_order(const struct fan_mark *a, const struct fan_mark *b)
{
    uintptr_t left = (uintptr_t)a->key.instance, right = (uintptr_t)b->key.instance;
    if (left != right) return (left > right) - (left < right);
    if (a->key.type != b->key.type) return (a->key.type > b->key.type) - (a->key.type < b->key.type);
    if (a->key.object != b->key.object) return (a->key.object > b->key.object) - (a->key.object < b->key.object);
    return (a->key.serial > b->key.serial) - (a->key.serial < b->key.serial);
}

RB_HEAD(fan_marks, fan_mark);
RB_GENERATE_INTERNAL(fan_marks, fan_mark, link, mark_order, static inline)
static struct fan_marks marks;

static struct fan_mark *first_mark(struct fan_key key)
{
    struct fan_mark search = {.key = key};
    return RB_NFIND(fan_marks, &marks, &search);
}

static bool match_object(const struct fan_mark *mark, struct fan_key key)
{
    return mark && mark->key.instance == key.instance &&
        mark->key.type == key.type && mark->key.object == key.object;
}

static void unlink_mark(struct fan_mark *mark)
{
    RB_REMOVE(fan_marks, &marks, mark);
    *mark->group_previous = mark->group_next;
    if (mark->group_next) mark->group_next->group_previous = mark->group_previous;
    struct kernel_vfs_instance *instance = mark->key.instance;
    if (!--instance->fanotify_marks)
        __atomic_store_n(&instance->fanotify_ops, NULL, __ATOMIC_RELEASE);
    mark->group->account->fanotify_marks--;
}

/* Drop storage outside mark_lock. The group owner covers every mark's heap
 * until removal; retirement captures the heap before releasing the lock. */
static void retire_marks(struct fan_key key, bool whole_instance)
{
    for (;;) {
        struct fan_mark *mark;
        struct kernel_heap *heap;
        {
            KERNEL_RAW_SCOPE(guard, &mark_lock);
            mark = first_mark(key);
            if (whole_instance ? !mark || mark->key.instance != key.instance : !match_object(mark, key))
                return;
            heap = mark->group->heap;
            unlink_mark(mark);
        }
        kernel_heap_release(heap, mark);
    }
}

static void retire_inode(struct kernel_vfs_instance *instance, uint64_t inode)
{
    retire_marks((struct fan_key){.instance = instance, .object = inode}, false);
}

static void retire_mount(struct kernel_vfs_mount *mount)
{
    retire_marks((struct fan_key){.instance = mount->private_data,
        .object = (uintptr_t)mount, .type = FAN_MARK_MOUNT}, false);
}

static void retire_instance(struct kernel_vfs_instance *instance)
{
    retire_marks((struct fan_key){.instance = instance}, true);
}

static unsigned mark_slot(unsigned type)
{
    if (type == FAN_MARK_MOUNT) return 1;
    return type == FAN_MARK_FILESYSTEM ? 2 : 0;
}

static void clear_group_marks(struct fan_group *group, unsigned slot)
{
    for (;;) {
        struct fan_mark *mark;
        {
            KERNEL_RAW_SCOPE(guard, &mark_lock);
            mark = group->marks[slot];
            if (!mark) return;
            unlink_mark(mark);
        }
        kernel_heap_release(group->heap, mark);
    }
}

static void group_put(struct fan_group *group)
{
    if (__atomic_sub_fetch(&group->references, 1U, __ATOMIC_ACQ_REL)) return;
    if (kernel_wait_queue_destroy(&group->ready)) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &mark_lock);
        group->account->fanotify_groups--;
    }
    kernel_uid_account_put(group->account);
    kernel_heap_release(group->heap, group);
}

static void event_destroy(struct fan_group *group, struct fan_event *event)
{
    if (event == &group->overflow) return;
    if (event->path && kernel_vfs_path_release(&event->path)) __builtin_trap();
    if (event->sender) kernel_task_identity_put(event->sender);
    kernel_heap_release(group->heap, event);
}

static void discard_opened_event(struct kernel_fanotify_read *read)
{
    kernel_files_cancel_reservation(&read->reservation);
    if (read->opened)
        (void)kernel_files_release_uninstalled_description(read->files, &read->opened);
}

void kernel_fanotify_read_abort(struct kernel_fanotify_read *read)
{
    discard_opened_event(read);
    if (read->event) event_destroy(read->group, read->event);
    if (read->vectors) kernel_heap_release(read->files->heap, read->vectors);
    (void)kernel_files_release_uninstalled_description(read->files, &read->owner);
}

static void release_group(void *object)
{
    struct fan_group *group = object;
    struct fan_event *event;
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        group->closed = true;
        event = group->first;
        group->first = NULL;
    }
    for (unsigned i = 0; i < sizeof(group->marks) / sizeof(group->marks[0]); i++)
        clear_group_marks(group, i);
    if (kernel_wait_queue_close(&group->ready)) __builtin_trap();
    while (event) {
        struct fan_event *next = event->next;
        event_destroy(group, event);
        event = next;
    }
    group_put(group);
}

static size_t event_size(const struct fan_event *event)
{
    return sizeof(struct fan_metadata) + (event->handle_type ? sizeof(struct fan_fid_info) : 0);
}

static unsigned event_bucket(const struct fan_event *event)
{
    uint64_t key = (uintptr_t)event->sender >> 6;
    if (event->path) key ^= ((uintptr_t)event->path->dentry >> 4) ^ (uintptr_t)event->path->file.mount;
    else key ^= event->fsid ^ event->handle.inode ^ event->handle.generation;
    key ^= key >> 17;
    return (unsigned)key & (FAN_HASH_BUCKETS - 1);
}

static bool merge_event(struct fan_group *group, const struct fan_event *event)
{
    unsigned count = FAN_MERGE_LIMIT;
    for (struct fan_event *old = group->merge[event_bucket(event)]; old && count; old = old->hash_next, count--) {
        if (old->sender != event->sender || !!old->path != !!event->path ||
            ((old->mask ^ event->mask) & FAN_ONDIR)) continue;
        if (old->path) {
            if (old->path->dentry != event->path->dentry ||
                old->path->file.mount != event->path->file.mount) continue;
        } else if (old->fsid != event->fsid || old->handle_type != event->handle_type ||
                   memcmp(&old->handle, &event->handle, sizeof(old->handle))) continue;
        old->mask |= event->mask;
        return true;
    }
    return false;
}

/* Insertion transfers the event owner. Waking readers is outside queue_lock. */
static void append_event(struct fan_group *group, struct fan_event *event,
                         struct kernel_wait_cursor *cursor)
{
    bool was_empty = !group->first;
    event->next = NULL;
    *group->tail = event;
    group->tail = &event->next;
    if (was_empty) kernel_wait_cursor_prepare(&group->ready, cursor);
}

static void queue_notification(struct fan_group *group, const struct fan_event *identity)
{
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        if (group->closed || merge_event(group, identity)) return;
    }

    struct fan_event *candidate = NULL;
    if (!kernel_heap_allocate_nosleep(group->heap, sizeof(*candidate), (void **)&candidate)) {
        *candidate = *identity;
        if (candidate->sender) kernel_task_identity_retain(candidate->sender);
        if (identity->path) {
            candidate->path = NULL;
            if (kernel_vfs_fanotify_acquire_path(identity->path, &candidate->path)) {
                event_destroy(group, candidate);
                candidate = NULL;
            }
        }
    }
    struct kernel_wait_cursor wake = {0};
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        if (!group->closed && !merge_event(group, identity)) {
            if (candidate && group->queued < FAN_MAX_EVENTS) {
                struct fan_event **slot = &group->merge[event_bucket(candidate)];
                candidate->hash_next = *slot;
                candidate->hash_previous = slot;
                if (*slot) (*slot)->hash_previous = &candidate->hash_next;
                *slot = candidate;
                group->queued++;
                append_event(group, candidate, &wake);
                candidate = NULL;
            } else if (!group->overflow_queued) {
                group->overflow_queued = true;
                append_event(group, &group->overflow, &wake);
            }
        }
    }
    if (wake.queue) kernel_wait_cursor_wake(&wake, 1);
    if (candidate) event_destroy(group, candidate);
}

static bool applicable(uint32_t mask, bool directory, bool parent)
{
    return (!directory || (mask & FAN_ONDIR)) && (!parent || (mask & FAN_EVENT_ON_CHILD));
}

/* A lower-bound search in the mark index selects the next group for up to
 * four object keys. No lock interval scans a list of other groups or marks. */
static struct fan_group *next_group(struct fan_key *keys, unsigned count,
    uint64_t *after, uint64_t limit, uint32_t event, uint32_t *mask, uint64_t *fsid)
{
    KERNEL_RAW_SCOPE(guard, &mark_lock);
    struct fan_mark *selected[4];
    struct fan_group *group = NULL;
    for (unsigned i = 0; i < count; i++) {
        keys[i].serial = *after + 1;
        struct fan_mark *mark = first_mark(keys[i]);
        if (!match_object(mark, keys[i]) || mark->key.serial > limit) mark = NULL;
        selected[i] = mark;
        if (mark && (!group || mark->key.serial < group->serial)) group = mark->group;
    }
    if (!group) return NULL;
    *after = group->serial;
    uint32_t include = 0, ignore = 0;
    bool directory = !!(event & FAN_ONDIR);
    for (unsigned i = 0; i < count; i++) {
        struct fan_mark *mark = selected[i];
        if (!mark || mark->group != group) continue;
        bool parent = i == 3;
        /* The legacy ignore flags follow the selection flags of the mark. */
        if (parent && !(mark->mask & FAN_EVENT_ON_CHILD)) continue;
        if ((event & VFS_NOTIFY_MODIFY) && !mark->survive_modify) mark->ignored = 0;
        ignore |= mark->ignored;
        if (!applicable(mark->mask, directory, parent)) continue;
        include |= mark->mask;
        *fsid = mark->fsid;
    }
    *mask = event & include & ~ignore & (FAN_PATH_EVENTS | VFS_NOTIFY_ATTRIB);
    if (*mask) *mask |= event & FAN_ONDIR;
    __atomic_add_fetch(&group->references, 1U, __ATOMIC_RELAXED);
    return group;
}

static void notify(const struct kernel_vfs_file *file, const struct kernel_vfs_path *path,
                   uint64_t parent_inode, uint32_t event)
{
    if (!(event & (FAN_PATH_EVENTS | VFS_NOTIFY_ATTRIB))) return;
    const struct kernel_vfs_node *node = file->private_data;
    struct kernel_vfs_instance *instance = node->instance;
    struct fan_key keys[4] = {
        {.instance = instance, .object = node->inode},
        {.instance = instance, .object = (uintptr_t)file->mount, .type = FAN_MARK_MOUNT},
        {.instance = instance, .type = FAN_MARK_FILESYSTEM},
        {.instance = instance, .object = parent_inode},
    };
    uint64_t after = 0, limit = __atomic_load_n(&last_group, __ATOMIC_RELAXED);
    struct kernel_pid *sender = NULL;
    bool sender_sampled = false;
    int handle_type = 0;
    struct kernel_vfs_handle handle;
    unsigned count = parent_inode && parent_inode != node->inode ? 4U : 3U;
    unsigned file_type = file->mode & KERNEL_VFS_S_IFMT;
    if (file_type != KERNEL_VFS_S_IFREG && file_type != KERNEL_VFS_S_IFDIR) path = NULL;
    for (;;) {
        uint32_t mask = 0;
        uint64_t fsid = 0;
        struct fan_group *group = next_group(keys, count, &after, limit, event, &mask, &fsid);
        if (!group) break;
        if (mask) {
            if (!sender_sampled) {
                (void)kernel_task_identity_get(0, &sender);
                sender_sampled = true;
            }
            struct fan_event identity = {.sender = sender, .mask = mask};
            if (group->fid) {
                if (!handle_type) handle_type = kernel_vfs_fanotify_encode_handle(file, &handle);
                if (handle_type > 0) {
                    identity.handle = handle;
                    identity.handle_type = handle_type;
                    identity.fsid = fsid;
                    queue_notification(group, &identity);
                }
            } else if (path) {
                identity.path = (struct kernel_vfs_path *)path;
                queue_notification(group, &identity);
            }
        }
        group_put(group);
    }
    if (sender) kernel_task_identity_put(sender);
}

static const struct kernel_fanotify_ops fan_ops = {
    .notify = notify, .retire_inode = retire_inode, .retire_mount = retire_mount,
    .retire_instance = retire_instance,
};

static uint32_t poll_events(void *object, uint32_t flags, uint32_t requested,
                            struct kernel_wait_queue **queue)
{
    (void)flags;
    (void)requested;
    struct fan_group *group = object;
    KERNEL_RAW_SCOPE(guard, &group->queue_lock);
    if (queue) *queue = &group->ready;
    return group->first ? KERNEL_POLLIN | KERNEL_POLLRDNORM : 0;
}

static int fan_ioctl(void *object, struct kernel_task *task, struct kernel_files *files,
    struct kernel_open_file_description **owner, struct kernel_mm *mm, uint64_t command,
    uint64_t argument)
{
    (void)task;
    (void)files;
    (void)owner;
    if (command != 0x541b) return -KERNEL_ENOTTY;
    struct fan_group *group = object;
    uint32_t bytes;
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        bytes = (group->queued + group->overflow_queued) * sizeof(struct fan_metadata);
    }
    size_t copied = 0;
    return kernel_copy_to_user(mm, argument, &bytes, sizeof(bytes), &copied) ||
        copied != sizeof(bytes) ? -KERNEL_EFAULT : 0;
}

/* Prepare both the descriptor number and its owner before copying metadata.
 * A fault discards the event and returns its unused descriptor to the table. */
static int output_event(struct kernel_fanotify_read *read, struct kernel_mm *mm,
    struct kernel_uaccess_iov_cursor *cursor)
{
    struct fan_group *group = read->group;
    struct fan_event *event = read->event;
    const size_t size = event_size(event);
    struct fan_record record = {0};
    record.metadata = (struct fan_metadata){
        .event_len = (uint32_t)size, .version = 3, .metadata_len = sizeof(record.metadata),
        .mask = group->fid ? event->mask : event->mask & ~FAN_ONDIR, .fd = FAN_NOFD,
    };
    if (event->sender) {
        bool visible = !group->unprivileged;
        if (!visible) {
            struct kernel_pid *reader = NULL;
            (void)kernel_task_identity_get(0, &reader);
            visible = reader == event->sender;
            if (reader) kernel_task_identity_put(reader);
        }
        if (visible) record.metadata.pid = kernel_task_identity_user_number(event->sender);
    }
    if (event->path) {
        unsigned fd;
        int error = 0;
        if (kernel_files_find_free_fd(read->files, &fd, &error)) return -KERNEL_EIO;
        if (error) return error;
        read->reservation = (struct kernel_files_fd_reservation){read->files, fd};
        error = kernel_files_open_fanotify_event(read->files, event->path,
            group->event_flags, &read->opened);
        if (error) return error;
        record.metadata.fd = (int32_t)fd;
    } else if (event->handle_type) {
        record.fid.type = 1;
        record.fid.len = sizeof(record.fid);
        record.fid.fsid[0] = (int32_t)(uint32_t)event->fsid;
        record.fid.fsid[1] = (int32_t)(uint32_t)(event->fsid >> 32);
        record.fid.handle_bytes = sizeof(event->handle);
        record.fid.handle_type = event->handle_type;
        memcpy(record.fid.handle, &event->handle, sizeof(event->handle));
    }
    size_t copied = 0;
    if (kernel_copy_to_user_iov(mm, cursor, &record, size, &copied) || copied != size)
        return -KERNEL_EFAULT;
    if (read->opened) {
        KERNEL_FILES_SCOPE(guard, read->files);
        if (kernel_files_install_owned_locked(read->files, read->reservation.fd,
                group->event_flags & LINUX_O_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, &read->opened))
            __builtin_trap();
        read->reservation.files = NULL;
    }
    return (int)size;
}

static int64_t read_records(struct kernel_fanotify_read *read,
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor, uint64_t bytes, uint32_t flags)
{
    struct fan_group *group = read->group;
    uint64_t done = 0;
    for (;;) {
        struct fan_event *event;
        struct kernel_wait_token token;
        {
            KERNEL_RAW_SCOPE(guard, &group->queue_lock);
            event = group->first;
            if (event) {
                if (event_size(event) > bytes - done) return done ? (int64_t)done : -KERNEL_EINVAL;
                group->first = event->next;
                if (!group->first) group->tail = &group->first;
                if (event == &group->overflow) group->overflow_queued = false;
                else {
                    *event->hash_previous = event->hash_next;
                    if (event->hash_next) event->hash_next->hash_previous = event->hash_previous;
                    group->queued--;
                }
                read->event = event;
            } else if (done || (flags & LINUX_O_NONBLOCK)) {
                return done ? (int64_t)done : -KERNEL_EAGAIN;
            } else if (kernel_wait_prepare(&group->ready, 0, 1, &token)) {
                return -KERNEL_EIO;
            }
        }
        if (event) {
            int result = output_event(read, mm, cursor);
            if (result < 0) discard_opened_event(read);
            event_destroy(group, event);
            read->event = NULL;
            if (result < 0) return done && result != -KERNEL_EFAULT ? (int64_t)done : result;
            done += (unsigned)result;
        } else {
            enum kernel_wait_wake_reason reason;
            enum kernel_scheduler_status status = kernel_wait_park(&token, &reason);
            if (kernel_wait_finish(&token) || status) return -KERNEL_EIO;
            if (reason == KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
        }
    }
}

static enum kernel_files_status fan_readv(void *object, struct kernel_task *task,
    struct kernel_files *files, struct kernel_open_file_description **owner, void **iov_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov, size_t count,
    uint64_t bytes, uint32_t flags, int64_t *result)
{
    struct kernel_fanotify_read read = {
        .group = object, .files = files, .owner = *owner, .vectors = iov_owner ? *iov_owner : NULL,
    };
    *owner = NULL;
    if (iov_owner) *iov_owner = NULL;
    kernel_task_fanotify_read_set(task, &read);
    *result = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t length = iov[i].length < bytes ? iov[i].length : bytes;
        if (!length && iov_owner) continue;
        struct kernel_uaccess_iov_cursor cursor = {iov + i, 1, 0, 0};
        int64_t got = read_records(&read, mm, &cursor, length, flags);
        if (got < 0) {
            if (!*result) *result = got;
            break;
        }
        *result += got;
        bytes -= got;
        if ((uint64_t)got != length || !bytes) break;
    }
    kernel_task_fanotify_read_clear(task, &read);
    *owner = read.owner;
    if (iov_owner) *iov_owner = read.vectors;
    return KERNEL_FILES_STATUS_OK;
}

static const struct kernel_char_device fan_file_ops = {
    .kind = KERNEL_OPEN_FILE_KIND_FANOTIFY, .release = release_group, .stateless_io = 1,
    .readv = fan_readv, .poll = poll_events, .ioctl = fan_ioctl,
};

static enum kernel_files_status create_group(struct kernel_files *files, unsigned flags,
    unsigned event_flags, int64_t *result)
{
    const struct kernel_credentials *cred = kernel_credentials_current();
    bool admin = kernel_credentials_capable(cred, KERNEL_CAP_SYS_ADMIN);
    if (!admin && (!(flags & (FAN_REPORT_FID | 0x1c00U | 0x4000U)) || (flags & FAN_ADMIN_INIT))) {
        *result = -KERNEL_EPERM;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((flags & ~FAN_KNOWN_FLAGS) || (flags & 12U) == 12U ||
        (event_flags & ~FAN_EVENT_FILE_FLAGS) || (event_flags & 3U) == 3U) {
        *result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (flags & ~(FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_FID)) {
        *result = -KERNEL_EOPNOTSUPP;
        return KERNEL_FILES_STATUS_OK;
    }
    unsigned fd;
    int error = 0;
    enum kernel_files_status status = kernel_files_find_free_fd(files, &fd, &error);
    if (status || error) {
        *result = error;
        return status;
    }
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation = (struct kernel_files_fd_reservation){files, fd};
    struct kernel_uid_account *account = kernel_uid_account_get(files->heap, cred->uid);
    if (!account) {
        *result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }

    struct fan_group *group = NULL;
    struct kernel_open_file_description *file = NULL;
    if (kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*group), (void **)&group) ||
        kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*file), (void **)&file)) {
        error = -KERNEL_ENOMEM;
        goto fail;
    }
    {
        KERNEL_RAW_SCOPE(guard, &mark_lock);
        if (account->fanotify_groups == FAN_MAX_GROUPS) error = -KERNEL_EMFILE;
        else if (last_group == UINT64_MAX - 1) error = -KERNEL_EOVERFLOW;
        else {
            account->fanotify_groups++;
            group->serial = last_group + 1;
            __atomic_store_n(&last_group, group->serial, __ATOMIC_RELAXED);
        }
    }
    if (error) goto fail;
    group->heap = files->heap;
    group->account = account;
    group->references = 1;
    group->event_flags = event_flags;
    group->fid = !!(flags & FAN_REPORT_FID);
    group->unprivileged = !admin;
    group->tail = &group->first;
    group->overflow.mask = FAN_Q_OVERFLOW;
    kernel_raw_lock_init(&group->queue_lock, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&group->ready);

    file->heap = files->heap;
    file->references = 1;
    file->kind = KERNEL_OPEN_FILE_KIND_FANOTIFY;
    file->open_flags = flags & FAN_NONBLOCK ? LINUX_O_NONBLOCK : 0;
    file->device = &fan_file_ops;
    file->device_instance = group;
    file->credentials = kernel_credentials_get(cred);
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    status = kernel_files_install_new_owned_at(files, fd,
        flags & FAN_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, &file);
    if (status) {
        (void)kernel_files_release_uninstalled_description(files, &file);
        return status;
    }
    reservation.files = NULL;
    *result = fd;
    return KERNEL_FILES_STATUS_OK;

fail:
    if (group) kernel_heap_release(files->heap, group);
    if (file) kernel_heap_release(files->heap, file);
    kernel_uid_account_put(account);
    *result = error;
    return KERNEL_FILES_STATUS_OK;
}

static int resolve_mark(struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int32_t dirfd, uint64_t user_path, unsigned flags,
    struct kernel_vfs_path **path)
{
    if (!user_path) {
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *file = kernel_files_hold_fd(files, dirfd, &pin);
        if (!file) return -KERNEL_EBADF;
        struct kernel_vfs_path *selected = kernel_open_file_path(file);
        if (!selected) return -KERNEL_EOPNOTSUPP;
        int error = kernel_vfs_path_acquire(selected);
        if (!error) *path = selected;
        return error;
    }

    char *name;
    if (kernel_heap_allocate(files->heap, KERNEL_FS_PATH_MAX, (void **)&name))
        return -KERNEL_ENOMEM;
    size_t length = 0;
    enum kernel_uaccess_status access =
        kernel_copy_string_from_user(mm, name, user_path, KERNEL_FS_PATH_MAX, &length);
    int error;
    if (access) error = access == KERNEL_UACCESS_STATUS_TOO_LONG ? -KERNEL_ENAMETOOLONG : -KERNEL_EFAULT;
    else {
        KERNEL_FS_PATHS(paths, fs);
        KERNEL_FILES_PATH_SCOPE(start);
        error = kernel_files_path_start(files, &paths, dirfd, name, &start);
        if (!error) error = kernel_vfs_path_resolve(start, paths.root, name,
            !(flags & FAN_MARK_DONT_FOLLOW), path);
    }
    kernel_heap_release(files->heap, name);
    return error;
}

static int update_mark(struct fan_group *group, struct fan_key key, unsigned flags,
                       uint32_t mask, uint64_t fsid)
{
    struct fan_mark *candidate = NULL, *retired = NULL;
    if (flags & FAN_MARK_ADD)
        (void)kernel_heap_allocate_zeroed(group->heap, 1, sizeof(*candidate), (void **)&candidate);
    int error = 0;
    {
        KERNEL_RAW_SCOPE(guard, &mark_lock);
        struct fan_mark *mark = first_mark(key);
        if (!match_object(mark, key) || mark->key.serial != key.serial) mark = NULL;
        if (flags & FAN_MARK_ADD) {
            if (!mark && !candidate) error = -KERNEL_ENOMEM;
            else if (!mark && group->account->fanotify_marks == FAN_MAX_MARKS) error = -KERNEL_ENOSPC;
            else {
                if (!mark) {
                    mark = candidate;
                    candidate = NULL;
                    mark->key = key;
                    mark->group = group;
                    mark->fsid = fsid;
                    struct fan_mark **list = &group->marks[mark_slot(key.type)];
                    mark->group_next = *list;
                    mark->group_previous = list;
                    if (*list) (*list)->group_previous = &mark->group_next;
                    *list = mark;
                    RB_INSERT(fan_marks, &marks, mark);
                    if (!key.instance->fanotify_marks++)
                        __atomic_store_n(&key.instance->fanotify_ops, &fan_ops, __ATOMIC_RELEASE);
                    group->account->fanotify_marks++;
                }
                if (flags & FAN_MARK_IGNORED_MASK) mark->ignored |= mask;
                else mark->mask |= mask;
                if (flags & FAN_MARK_IGNORED_SURV_MODIFY) mark->survive_modify = true;
            }
        } else if (!mark) error = -KERNEL_ENOENT;
        else {
            if (flags & FAN_MARK_IGNORED_MASK) mark->ignored &= ~mask;
            else mark->mask &= ~mask;
            if (!(mark->mask | mark->ignored)) {
                unlink_mark(mark);
                retired = mark;
            }
        }
    }
    if (candidate) kernel_heap_release(group->heap, candidate);
    if (retired) kernel_heap_release(group->heap, retired);
    return error;
}

static int modify_mark(struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int fd, unsigned flags, uint64_t mask, int32_t dirfd, uint64_t user_path)
{
    unsigned type = flags & FAN_MARK_TYPE;
    unsigned command = flags & (FAN_MARK_ADD | FAN_MARK_REMOVE | FAN_MARK_FLUSH);
    if (flags & ~FAN_KNOWN_MARK_FLAGS || mask & ~FAN_KNOWN_MASK) return -KERNEL_EINVAL;
    if (command == FAN_MARK_FLUSH) {
        if (flags & ~(FAN_MARK_FLUSH | FAN_MARK_TYPE)) return -KERNEL_EINVAL;
    } else if ((command != FAN_MARK_ADD && command != FAN_MARK_REMOVE) || !mask) {
        return -KERNEL_EINVAL;
    }

    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file = kernel_files_hold_fd(files, fd, &pin);
    if (!file) return -KERNEL_EBADF;
    if (file->kind != KERNEL_OPEN_FILE_KIND_FANOTIFY) return -KERNEL_EINVAL;
    struct fan_group *group = file->device_instance;
    if (type && !kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_SYS_ADMIN))
        return -KERNEL_EPERM;
    if (type == FAN_MARK_TYPE || flags & 0x600U) return -KERNEL_EOPNOTSUPP;
    if (mask & 0x170000U) return -KERNEL_EINVAL; /* Permission events require a content group. */
    if ((mask & VFS_NOTIFY_ATTRIB) && (!group->fid || type == FAN_MARK_MOUNT))
        return -KERNEL_EINVAL;
    if (mask & ~FAN_SUPPORTED_MASK) return -KERNEL_EOPNOTSUPP;
    if (command == FAN_MARK_FLUSH) {
        clear_group_marks(group, mark_slot(type));
        return 0;
    }

    KERNEL_FILES_PATH_SCOPE(path);
    int error = resolve_mark(files, fs, mm, dirfd, user_path, flags, &path);
    if (error) return error;
    struct kernel_vfs_node *node = path->file.private_data;
    if (!node) return -KERNEL_EOPNOTSUPP;
    bool dir = (path->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if ((flags & FAN_MARK_ONLYDIR) && !dir) return -KERNEL_ENOTDIR;
    error = kernel_vfs_path_permission(path, KERNEL_VFS_MAY_READ);
    if (error) return error;
    uint64_t fsid = 0;
    if (group->fid) {
        if (kernel_vfs_handle_type(path,!type) < 0) return -KERNEL_EOPNOTSUPP;
        if (command == FAN_MARK_ADD) {
            struct kernel_vfs_statfs stat;
            error = kernel_vfs_mount_statfs(path->file.mount, &stat);
            if (error) return error;
            if (!stat.fsid) return -KERNEL_ENODEV;
            fsid = stat.fsid;
        }
    }
    if ((flags & FAN_MARK_IGNORED_MASK) && command == FAN_MARK_ADD && !type &&
        !(flags & FAN_MARK_IGNORED_SURV_MODIFY) &&
        __atomic_load_n(&node->write_openers, __ATOMIC_RELAXED)) return 0;
    if (flags & FAN_MARK_IGNORED_MASK) mask &= ~FAN_EVENT_FLAGS;
    if (type || !dir) mask &= ~FAN_EVENT_ON_CHILD;

    struct fan_key key = {.instance = node->instance, .type = type, .serial = group->serial};
    if (type == FAN_MARK_MOUNT) key.object = (uintptr_t)path->file.mount;
    else if (!type) key.object = node->inode;
    return update_mark(group, key, flags, (uint32_t)mask, fsid);
}

enum kernel_files_status kernel_files_fanotify(struct kernel_files *files,
    const struct kernel_fs_context *fs, struct kernel_mm *mm, unsigned syscall,
    int32_t fd_or_flags, uint32_t flags, uint64_t mask, int32_t dirfd,
    uint64_t path, int64_t *result)
{
    if (syscall == 262U) return create_group(files, (unsigned)fd_or_flags, flags, result);
    *result = modify_mark(files, fs, mm, fd_or_flags, flags, mask, dirfd, path);
    return KERNEL_FILES_STATUS_OK;
}
