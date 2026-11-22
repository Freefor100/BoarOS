#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/fanotify.h>
#include <kernel/fs_context.h>
#include <kernel/pid.h>
#include <kernel/wait_internal.h>
#include "char_device_internal.h"
#include "fanotify_internal.h"
#include "files/private.h"
#include "notify_fdinfo.h"
#include "open_file_internal.h"
#include "uaccess_iov_internal.h"
#include "vfs_notify.h"
#include "vfs_objects.h"
#include "../third_party/lwext4/include/misc/tree.h"
#include <string.h>

enum {
    FAN_CLOEXEC = 1, FAN_NONBLOCK = 2, FAN_REPORT_FID = 0x200,
    FAN_REPORT_DIR_FID = 0x400, FAN_REPORT_NAME = 0x800, FAN_REPORT_TARGET_FID = 0x1000,
    FAN_MARK_ADD = 1, FAN_MARK_REMOVE = 2, FAN_MARK_DONT_FOLLOW = 4,
    FAN_MARK_ONLYDIR = 8, FAN_MARK_MOUNT = 0x10, FAN_MARK_IGNORED_MASK = 0x20,
    FAN_MARK_IGNORED_SURV_MODIFY = 0x40, FAN_MARK_FLUSH = 0x80,
    FAN_MARK_FILESYSTEM = 0x100, FAN_MARK_TYPE = 0x110,
    FAN_MARK_EVICTABLE = 0x200, FAN_MARK_IGNORE = 0x400,
    FAN_Q_OVERFLOW = 0x4000, FAN_EVENT_ON_CHILD = 0x08000000,
    FAN_RENAME = 0x10000000, FAN_ONDIR = VFS_NOTIFY_ISDIR, FAN_NOFD = -1,
    FAN_INFO_FID = 1, FAN_INFO_DFID_NAME = 2, FAN_INFO_DFID = 3,
    FAN_INFO_OLD_DFID_NAME = 10, FAN_INFO_NEW_DFID_NAME = 12,
    FAN_MAX_GROUPS = 128, FAN_MAX_MARKS = 8192, FAN_MAX_EVENTS = 16384,
    FAN_HASH_BUCKETS = 128, FAN_MERGE_LIMIT = 128,
    LINUX_O_CLOEXEC = 02000000, LINUX_O_NONBLOCK = 04000,
    FAN_CLASS_CONTENT = 4, FAN_CLASS_PRE_CONTENT = 8,
    FAN_ALLOW = 1, FAN_DENY = 2,
};

#define FAN_PATH_EVENTS (VFS_NOTIFY_ACCESS | VFS_NOTIFY_MODIFY | VFS_NOTIFY_OPEN | \
                         VFS_NOTIFY_CLOSE_WRITE | VFS_NOTIFY_CLOSE_NOWRITE | VFS_NOTIFY_OPEN_EXEC)
#define FAN_FID_FLAGS (FAN_REPORT_FID | FAN_REPORT_DIR_FID | FAN_REPORT_NAME | FAN_REPORT_TARGET_FID)
#define FAN_IGNORE_FLAGS (FAN_MARK_IGNORED_MASK | FAN_MARK_IGNORE)
#define FAN_DIRENT_EVENTS (VFS_NOTIFY_MOVED_FROM | VFS_NOTIFY_MOVED_TO | VFS_NOTIFY_CREATE | \
                           VFS_NOTIFY_DELETE | FAN_RENAME)
#define FAN_INODE_EVENTS (VFS_NOTIFY_ATTRIB | VFS_NOTIFY_DELETE_SELF | VFS_NOTIFY_MOVE_SELF)
#define FAN_EVENT_FLAGS (FAN_EVENT_ON_CHILD | FAN_ONDIR)
#define FAN_SUPPORTED_MASK (FAN_PATH_EVENTS | FAN_INODE_EVENTS | FAN_DIRENT_EVENTS | \
                            VFS_NOTIFY_PERMISSION_EVENTS | FAN_EVENT_FLAGS)
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
    struct kernel_vfs_node *node;
    uintptr_t object;
    uint64_t serial;
    unsigned type;
};
struct fan_mark {
    struct fan_key key;
    struct fan_group *group;
    uint64_t fsid;
    uint32_t mask, ignored;
    bool survive_modify, ignore_has_flags, evictable;
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
    uint16_t info_bytes;
};

enum fan_permission_state {
    FAN_PERMISSION_QUEUED, FAN_PERMISSION_READING,
    FAN_PERMISSION_REPORTED, FAN_PERMISSION_DONE,
};

struct fan_permission {
    struct fan_event event;
    struct kernel_wait_queue reply;
    struct fan_event **queue_previous;
    RB_ENTRY(fan_permission) link;
    uint64_t report_order;
    unsigned references;
    enum fan_permission_state state;
    int fd, result;
};

static int permission_order(const struct fan_permission *a, const struct fan_permission *b)
{
    if (a->fd != b->fd) return (a->fd > b->fd) - (a->fd < b->fd);
    return (a->report_order > b->report_order) - (a->report_order < b->report_order);
}

RB_HEAD(fan_permissions, fan_permission);
RB_GENERATE_INTERNAL(fan_permissions, fan_permission, link, permission_order, static inline)

struct fan_fdinfo_cursor {
    struct fan_fdinfo_cursor *next, **previous;
    struct fan_mark *mark[3];
};

struct fan_group {
    struct kernel_heap *heap;
    struct kernel_uid_account *account;
    uint64_t serial;
    unsigned references, event_flags;
    uint16_t report, init_flags;
    bool fid, unprivileged;
    struct fan_mark *marks[3];
    struct fan_fdinfo_cursor *fdinfo;
    unsigned mark_count;
    struct kernel_raw_lock queue_lock;
    struct kernel_wait_queue ready;
    struct fan_event *first, **tail;
    struct fan_event *merge[FAN_HASH_BUCKETS];
    struct fan_event overflow;
    struct fan_permissions reported;
    uint64_t last_report;
    unsigned queued;
    uint8_t priority;
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

static struct fan_permission *permission_of(struct fan_event *event)
{
    return event && (event->mask & VFS_NOTIFY_PERMISSION_EVENTS) ?
        (struct fan_permission *)event : NULL;
}

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

static uint32_t mark_interest(const struct fan_mark *mark)
{
    uint32_t mask = mark->mask & ~FAN_EVENT_FLAGS;
    if (!mark->survive_modify && (mark->ignored & ~FAN_EVENT_FLAGS))
        mask |= VFS_NOTIFY_MODIFY;
    return mask;
}

static uint32_t parent_interest(const struct fan_mark *mark)
{
    uint32_t interest = mark_interest(mark);
    if (mark->mask & FAN_EVENT_ON_CHILD ||
        (mark->ignore_has_flags && (mark->ignored & FAN_EVENT_ON_CHILD)))
        return interest;
    return (mark->group->report & FAN_REPORT_DIR_FID) ? interest & FAN_PATH_EVENTS : 0;
}

static void update_interest(uint32_t *summary, unsigned counts[32], uint32_t old, uint32_t new)
{
    uint32_t changed = old ^ new;
    if (!changed) return;
    uint32_t result = __atomic_load_n(summary, __ATOMIC_RELAXED);
    for (unsigned i = 0; changed; ++i, changed >>= 1) {
        if (!(changed & 1U)) continue;
        uint32_t bit = UINT32_C(1) << i;
        if (new & bit) {
            ++counts[i];
            result |= bit;
        } else if (!--counts[i]) result &= ~bit;
    }
    __atomic_store_n(summary, result, __ATOMIC_RELEASE);
}

static void summary_change(const struct fan_mark *mark, uint32_t old, uint32_t old_parent,
    uint32_t new, uint32_t new_parent)
{
    struct kernel_vfs_instance *instance = mark->key.instance;
    update_interest(&instance->fanotify_parent_interest, instance->fanotify_parent_count,
                    old_parent, new_parent);
    update_interest(&instance->fanotify_interest, instance->fanotify_interest_count, old, new);
    if ((mark->group->report & FAN_REPORT_TARGET_FID) && ((old ^ new) & VFS_NOTIFY_CREATE)) {
        if (new & VFS_NOTIFY_CREATE)
            __atomic_fetch_add(&instance->fanotify_create_targets, 1U, __ATOMIC_RELEASE);
        else
            __atomic_fetch_sub(&instance->fanotify_create_targets, 1U, __ATOMIC_RELEASE);
    }
}

static void unlink_mark(struct fan_mark *mark)
{
    summary_change(mark, mark_interest(mark), parent_interest(mark), 0, 0);
    RB_REMOVE(fan_marks, &marks, mark);
    *mark->group_previous = mark->group_next;
    if (mark->group_next) mark->group_next->group_previous = mark->group_previous;
    for (struct fan_fdinfo_cursor *cursor = mark->group->fdinfo; cursor; cursor = cursor->next)
        for (unsigned i = 0; i < 3; ++i)
            if (cursor->mark[i] == mark) cursor->mark[i] = mark->group_next;
    struct kernel_vfs_instance *instance = mark->key.instance;
    if (!--instance->fanotify_marks)
        __atomic_store_n(&instance->fanotify_ops, NULL, __ATOMIC_RELEASE);
    mark->group->account->fanotify_marks--;
    mark->group->mark_count--;
}

static void destroy_mark(struct kernel_heap *heap, struct fan_mark *mark)
{
    struct kernel_vfs_node *node = mark->key.node;
    if (node && !mark->evictable) {
        __atomic_fetch_sub(&node->fanotify_pins, 1U, __ATOMIC_RELEASE);
        if (kernel_vfs_node_release(&node)) __builtin_trap();
    }
    kernel_heap_release(heap, mark);
}

/* Drop storage outside mark_lock. The group owner covers every mark's heap
 * until removal; retirement captures the heap before releasing the lock. */
static void retire_marks(struct fan_key key, bool whole_instance, bool evicted)
{
    for (;;) {
        struct fan_mark *mark;
        struct kernel_heap *heap;
        {
            KERNEL_RAW_SCOPE(guard, &mark_lock);
            mark = first_mark(key);
            if (whole_instance ? !mark || mark->key.instance != key.instance : !match_object(mark, key))
                return;
            if (key.node && (mark->key.node != key.node || (evicted && !mark->evictable))) {
                key.serial = mark->key.serial + 1;
                continue;
            }
            heap = mark->group->heap;
            unlink_mark(mark);
        }
        destroy_mark(heap, mark);
    }
}

static void retire_inode(struct kernel_vfs_node *node, bool evicted)
{
    retire_marks((struct fan_key){.instance = node->instance, .object = node->inode,
        .node = node}, false, evicted);
}

static void retire_mount(struct kernel_vfs_mount *mount)
{
    retire_marks((struct fan_key){.instance = mount->private_data,
        .object = (uintptr_t)mount, .type = FAN_MARK_MOUNT}, false, false);
}

static void retire_instance(struct kernel_vfs_instance *instance)
{
    retire_marks((struct fan_key){.instance = instance}, true, false);
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
        destroy_mark(group->heap, mark);
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
    struct fan_permission *permission = permission_of(event);
    if (permission) {
        if (__atomic_sub_fetch(&permission->references, 1U, __ATOMIC_ACQ_REL)) return;
        if (kernel_wait_queue_close(&permission->reply) ||
            kernel_wait_queue_destroy(&permission->reply)) __builtin_trap();
    }
    if (event->path && kernel_vfs_path_release(&event->path)) __builtin_trap();
    if (event->sender) kernel_task_identity_put(event->sender);
    kernel_heap_release(group->heap, event);
}

static void unlink_queued_event(struct fan_group *group, struct fan_event **link)
{
    struct fan_event *event = *link;
    *link = event->next;
    if (!event->next) group->tail = link;
    struct fan_permission *next = permission_of(event->next);
    if (next) next->queue_previous = link;

    struct fan_permission *permission = permission_of(event);
    if (permission) permission->queue_previous = NULL;
    if (event == &group->overflow) {
        group->overflow_queued = false;
    } else {
        if (event->hash_previous) {
            *event->hash_previous = event->hash_next;
            if (event->hash_next) event->hash_next->hash_previous = event->hash_previous;
        }
        group->queued--;
    }
}

static void answer_permission(struct fan_permission *permission, int result,
                              struct kernel_wait_cursor *wake)
{
    permission->result = result;
    permission->state = FAN_PERMISSION_DONE;
    kernel_wait_cursor_prepare(&permission->reply, wake);
}

static void deny_unreadable_permission(struct fan_group *group, struct fan_event *event)
{
    struct fan_permission *permission = permission_of(event);
    if (!permission) return;
    struct kernel_wait_cursor wake = {0};
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        answer_permission(permission, -KERNEL_EPERM, &wake);
    }
    if (wake.queue) kernel_wait_cursor_wake(&wake, 1);
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
    if (read->event) {
        deny_unreadable_permission(read->group, read->event);
        event_destroy(read->group, read->event);
    }
    if (read->vectors) kernel_heap_release(read->files->heap, read->vectors);
    (void)kernel_files_release_uninstalled_description(read->files, &read->owner);
}

static void release_group(void *object)
{
    struct fan_group *group = object;
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        group->closed = true;
    }
    for (unsigned i = 0; i < sizeof(group->marks) / sizeof(group->marks[0]); i++)
        clear_group_marks(group, i);
    if (kernel_wait_queue_close(&group->ready)) __builtin_trap();
    for (;;) {
        struct fan_event *event;
        struct kernel_wait_cursor wake = {0};
        {
            KERNEL_RAW_SCOPE(guard, &group->queue_lock);
            event = group->first;
            if (event) {
                unlink_queued_event(group, &group->first);
                struct fan_permission *permission = permission_of(event);
                if (permission) answer_permission(permission, 0, &wake);
            } else {
                struct fan_permission *permission = RB_MIN(fan_permissions, &group->reported);
                if (!permission) break;
                RB_REMOVE(fan_permissions, &group->reported, permission);
                answer_permission(permission, 0, &wake);
                event = &permission->event;
            }
        }
        if (wake.queue) kernel_wait_cursor_wake(&wake, 1);
        event_destroy(group, event);
    }
    group_put(group);
}

static size_t event_size(const struct fan_event *event)
{
    return sizeof(struct fan_metadata) + event->info_bytes +
        (event->handle_type ? sizeof(struct fan_fid_info) : 0);
}

struct fan_file_id {
    struct kernel_vfs_handle handle;
    int type;
};

struct fan_info {
    const struct fan_file_id *id;
    const char *name;
    uint16_t name_bytes;
    uint8_t type;
};

static unsigned info_size(const struct fan_info *info)
{
    return (sizeof(struct fan_fid_info) + info->name_bytes + 3U) & ~3U;
}

static bool equal_info(const struct fan_event *event, const struct fan_info *info, unsigned count)
{
    const unsigned char *data = (const void *)(event + 1);
    for (unsigned i = 0; i < count; ++i) {
        const struct fan_fid_info *record = (const void *)data;
        if (record->type != info[i].type || record->len != info_size(info + i) ||
            record->handle_type != info[i].id->type ||
            memcmp(record->handle, &info[i].id->handle, sizeof(record->handle)) ||
            (info[i].name_bytes &&
             memcmp(data + sizeof(*record), info[i].name, info[i].name_bytes))) return false;
        /* A length can differ only by trailing alignment. Compare the
         * terminating NUL too so aliases with a common prefix stay distinct. */
        data += record->len;
    }
    return data == (const unsigned char *)(event + 1) + event->info_bytes;
}

static void encode_info(struct fan_event *event, const struct fan_info *info, unsigned count)
{
    unsigned char *data = (void *)(event + 1);
    for (unsigned i = 0; i < count; ++i) {
        unsigned length = info_size(info + i);
        struct fan_fid_info record = {
            .type = info[i].type, .len = length,
            .fsid = {(int32_t)event->fsid, (int32_t)(event->fsid >> 32)},
            .handle_bytes = sizeof(info[i].id->handle), .handle_type = info[i].id->type,
        };
        memcpy(record.handle, &info[i].id->handle, sizeof(record.handle));
        memcpy(data, &record, sizeof(record));
        if (info[i].name_bytes)
            memcpy(data + sizeof(record), info[i].name, info[i].name_bytes);
        unsigned used = sizeof(record) + info[i].name_bytes;
        memset(data + used, 0, length - used);
        data += length;
    }
}

static unsigned event_bucket(const struct fan_event *event)
{
    uint64_t key = (uintptr_t)event->sender >> 6;
    if (event->path) key ^= ((uintptr_t)event->path->dentry >> 4) ^ (uintptr_t)event->path->file.mount;
    else key ^= event->fsid ^ event->handle.inode ^ event->handle.generation;
    key ^= key >> 17;
    return (unsigned)key & (FAN_HASH_BUCKETS - 1);
}

static bool merge_event(struct fan_group *group, const struct fan_event *event,
                        const struct fan_info *info, unsigned count)
{
    unsigned remaining = FAN_MERGE_LIMIT;
    for (struct fan_event *old = group->merge[event_bucket(event)]; old && remaining;
         old = old->hash_next, remaining--) {
        if (old->sender != event->sender || !!old->path != !!event->path ||
            ((old->mask ^ event->mask) & FAN_ONDIR) ||
            old->info_bytes != event->info_bytes) continue;
        if (old->path) {
            if (old->path->dentry != event->path->dentry ||
                old->path->file.mount != event->path->file.mount) continue;
        } else if (old->fsid != event->fsid || old->handle_type != event->handle_type ||
                   memcmp(&old->handle, &event->handle, sizeof(old->handle))) continue;
        if (count && !equal_info(old, info, count)) continue;
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
    struct fan_permission *permission = permission_of(event);
    if (permission) permission->queue_previous = group->tail;
    event->next = NULL;
    *group->tail = event;
    group->tail = &event->next;
    if (was_empty) kernel_wait_cursor_prepare(&group->ready, cursor);
}

static void queue_records(struct fan_group *group, const struct fan_event *identity,
                          const struct fan_info *info, unsigned count)
{
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        if (group->closed || merge_event(group, identity, info, count)) return;
    }

    struct fan_event *candidate = NULL;
    if (!kernel_heap_allocate_nosleep(group->heap, sizeof(*candidate) + identity->info_bytes,
                                      (void **)&candidate)) {
        *candidate = *identity;
        if (count) encode_info(candidate, info, count);
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
        if (!group->closed && !merge_event(group, identity, info, count)) {
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

static void queue_notification(struct fan_group *group, const struct fan_event *identity)
{
    queue_records(group, identity, NULL, 0);
}

static bool applicable(uint32_t mask, bool directory, bool parent)
{
    return (!directory || (mask & FAN_ONDIR)) && (!parent || (mask & FAN_EVENT_ON_CHILD));
}

/* Find one group at a time so a mark update does not wait behind
 * another group's allocation, queue operations or permission response. */
static struct fan_group *select_group(struct fan_key *keys, unsigned count,
    unsigned parent_keys, uint64_t *after, uint64_t limit, uint32_t event, int priority,
    uint32_t *mask, uint64_t *fsid, unsigned *matched)
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
    *matched = 0;
    if (priority < 0 || priority == group->priority) {
        for (unsigned i = 0; i < count; i++) {
            struct fan_mark *mark = selected[i];
            if (!mark || mark->group != group ||
                (keys[i].node && mark->key.node && keys[i].node != mark->key.node)) continue;
            bool parent = !!(parent_keys & (1U << i));
            if ((event & VFS_NOTIFY_MODIFY) && !mark->survive_modify && mark->ignored) {
                uint32_t old = mark_interest(mark), old_parent = parent_interest(mark);
                mark->ignored = 0;
                summary_change(mark, old, old_parent, mark_interest(mark), parent_interest(mark));
            }
            uint32_t ignored = mark->ignored;
            if (!mark->ignore_has_flags)
                ignored |= FAN_ONDIR | (mark->mask & FAN_EVENT_ON_CHILD);
            if (applicable(ignored, directory, parent)) ignore |= ignored;
            if (!applicable(mark->mask, directory, parent)) continue;
            include |= mark->mask;
            *matched |= 1U << i;
            *fsid = mark->fsid;
        }
    }
    *mask = event & include & ~ignore & ~FAN_EVENT_FLAGS;
    if (*mask) *mask |= event & FAN_ONDIR;
    __atomic_add_fetch(&group->references, 1U, __ATOMIC_RELAXED);
    return group;
}

static struct fan_key inode_key(struct kernel_vfs_node *node)
{
    return (struct fan_key){.instance = node->instance, .object = node->inode, .node = node};
}

struct fan_subject {
    struct fan_file_id id;
    struct kernel_vfs_node *node;
};

struct fan_delivery {
    struct fan_subject object, directory, destination;
    const struct kernel_vfs_path *path;
    const char *name, *new_name;
    uint16_t name_bytes, new_name_bytes;
    struct kernel_pid *sender;
    bool sender_sampled;
};

static struct fan_file_id *file_id(struct fan_subject *subject)
{
    if (!subject->id.type && subject->node) {
        struct kernel_vfs_file file = {.private_data = subject->node};
        subject->id.type = kernel_vfs_fanotify_encode_handle(&file, &subject->id.handle);
    }
    return subject->id.type > 0 ? &subject->id : NULL;
}

static void add_info(struct fan_info *info, unsigned *count, struct fan_subject *subject,
    unsigned type, const char *name, uint16_t bytes)
{
    struct fan_file_id *id = file_id(subject);
    if (id) info[(*count)++] = (struct fan_info){.id = id, .type = type,
        .name = name, .name_bytes = bytes};
}

static void deliver_fid(struct fan_group *group, struct fan_delivery *delivery,
    struct fan_event *identity, uint32_t mask, unsigned sides)
{
    bool directory = !!(mask & FAN_ONDIR), dirent = !!(mask & FAN_DIRENT_EVENTS);
    bool report_name = !!(group->report & FAN_REPORT_NAME);
    struct fan_subject *parent = !dirent && directory ? &delivery->object : &delivery->directory;
    if (!(group->report & FAN_REPORT_DIR_FID)) {
        struct fan_file_id *id = file_id(dirent ? parent : &delivery->object);
        if (id) {
            identity->handle = id->handle;
            identity->handle_type = id->type;
            queue_notification(group, identity);
        }
        return;
    }

    struct fan_info info[3];
    unsigned count = 0;
    if (mask & FAN_RENAME) {
        if (sides & 1U)
            add_info(info, &count, parent, FAN_INFO_OLD_DFID_NAME, delivery->name,
                     delivery->name_bytes);
        if (sides & 2U)
            add_info(info, &count, &delivery->destination, FAN_INFO_NEW_DFID_NAME,
                     delivery->new_name, delivery->new_name_bytes);
    } else if (parent->node) {
        const char *name = directory && !dirent ? "." : delivery->name;
        uint16_t bytes = directory && !dirent ? 2U : delivery->name_bytes;
        if (report_name && name) add_info(info, &count, parent, FAN_INFO_DFID_NAME, name, bytes);
        else add_info(info, &count, parent, FAN_INFO_DFID, NULL, 0);
    }

    /* Entry events describe the containing directory by default. Its child
     * handle is requested separately, unlike the ordinary non-directory FID. */
    if ((dirent && (group->report & FAN_REPORT_TARGET_FID)) ||
        (!dirent && !directory && (group->report & FAN_REPORT_FID)))
        add_info(info, &count, &delivery->object, FAN_INFO_FID, NULL, 0);
    if (!count) return;
    identity->handle = info[0].id->handle;
    for (unsigned i = 0; i < count; ++i) identity->info_bytes += info_size(info + i);
    queue_records(group, identity, info, count);
}

static void dispatch_delivery(struct fan_delivery *delivery, struct fan_key *keys,
    unsigned count, unsigned parent_keys, uint32_t event)
{
    uint64_t after = 0, limit = __atomic_load_n(&last_group, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t mask = 0;
        uint64_t fsid = 0;
        unsigned matched;
        struct fan_group *group = select_group(keys, count, parent_keys, &after, limit,
            event, -1, &mask, &fsid, &matched);
        if (!group) break;
        if (mask && (group->fid || delivery->path)) {
            if (!delivery->sender_sampled) {
                (void)kernel_task_identity_get(0, &delivery->sender);
                delivery->sender_sampled = true;
            }
            struct fan_event identity = {.sender = delivery->sender, .mask = mask, .fsid = fsid};
            if (group->fid) {
                /* Rename keys are old directory, new directory, filesystem.
                 * A filesystem mark represents both names. */
                unsigned sides = matched & 4U ? 3U : matched & 3U;
                deliver_fid(group, delivery, &identity, mask, sides);
            } else {
                identity.path = (struct kernel_vfs_path *)delivery->path;
                queue_notification(group, &identity);
            }
        }
        group_put(group);
    }
}

static void notify(const struct kernel_vfs_file *file, const struct kernel_vfs_path *path,
                   struct kernel_vfs_node *parent, const char *name, uint32_t event)
{
    const struct kernel_vfs_node *node = file->private_data;
    struct kernel_vfs_instance *instance = node->instance;
    struct fan_key keys[4] = {
        inode_key(file->private_data),
        {.instance = instance, .object = (uintptr_t)file->mount, .type = FAN_MARK_MOUNT},
        {.instance = instance, .type = FAN_MARK_FILESYSTEM},
    };
    unsigned count = 3;
    if (parent && parent != node) keys[count++] = inode_key(parent);
    unsigned file_type = file->mode & KERNEL_VFS_S_IFMT;
    struct fan_delivery delivery = {
        .object.node = file->private_data, .directory.node = parent,
        .path = file_type == KERNEL_VFS_S_IFREG || file_type == KERNEL_VFS_S_IFDIR ? path : NULL,
        .name = name, .name_bytes = name ? strlen(name) + 1 : 0,
    };
    dispatch_delivery(&delivery, keys, count, 1U << 3, event);
    if (delivery.sender) kernel_task_identity_put(delivery.sender);
}

static void notify_entry(const struct kernel_vfs_path *parent, const char *name,
    const struct kernel_vfs_file *file, const struct kernel_vfs_handle *handle,
    const struct kernel_vfs_path *new_parent, const char *new_name, uint32_t event)
{
    struct kernel_vfs_node *dir = parent->file.private_data;
    struct kernel_vfs_instance *instance = dir->instance;
    struct fan_key fs = {.instance = instance, .type = FAN_MARK_FILESYSTEM};
    struct fan_key keys[4] = {inode_key(dir), fs};
    struct fan_delivery delivery = {
        .object.node = file ? file->private_data : NULL, .directory.node = dir,
        .name = name, .name_bytes = strlen(name) + 1,
    };
    if (handle)
        delivery.object.id = (struct fan_file_id){*handle, instance->ops->export->type};
    if (new_parent) {
        struct kernel_vfs_node *destination = new_parent->file.private_data;
        uint32_t flag = event & FAN_ONDIR;
        uint32_t interest = __atomic_load_n(&instance->fanotify_interest, __ATOMIC_ACQUIRE);
        delivery.destination.node = destination;
        delivery.new_name = new_name;
        delivery.new_name_bytes = strlen(new_name) + 1;
        if (interest & FAN_RENAME) {
            keys[1] = inode_key(destination);
            keys[2] = fs;
            dispatch_delivery(&delivery, keys, 3, 0, FAN_RENAME | flag);
        }
        keys[1] = fs;
        if (interest & VFS_NOTIFY_MOVED_FROM)
            dispatch_delivery(&delivery, keys, 2, 0, VFS_NOTIFY_MOVED_FROM | flag);
        keys[0] = inode_key(destination);
        delivery.directory = delivery.destination;
        delivery.name = new_name;
        delivery.name_bytes = delivery.new_name_bytes;
        if (interest & VFS_NOTIFY_MOVED_TO)
            dispatch_delivery(&delivery, keys, 2, 0, VFS_NOTIFY_MOVED_TO | flag);
    } else {
        dispatch_delivery(&delivery, keys, 2, 0, event);
    }
    if (delivery.sender) kernel_task_identity_put(delivery.sender);
}

static int request_permission(struct fan_group *group, const struct kernel_vfs_path *path,
                              struct kernel_pid *sender, uint32_t mask)
{
    struct fan_permission *permission;
    if (kernel_heap_allocate_zeroed(group->heap, 1, sizeof(*permission), (void **)&permission))
        return -KERNEL_ENOMEM;
    permission->references = 1;
    permission->event.mask = mask;
    kernel_wait_queue_init(&permission->reply);
    int error = kernel_vfs_fanotify_acquire_path(path, &permission->event.path);
    if (error) {
        event_destroy(group, &permission->event);
        return error;
    }
    if (sender) {
        kernel_task_identity_retain(sender);
        permission->event.sender = sender;
    }

    struct kernel_wait_token token = {0};
    struct kernel_wait_cursor ready = {0};
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        if (!group->closed) {
            if (group->queued >= FAN_MAX_EVENTS) {
                if (!group->overflow_queued) {
                    group->overflow_queued = true;
                    append_event(group, &group->overflow, &ready);
                }
            } else if (kernel_wait_prepare(&permission->reply, 0, KERNEL_WAIT_KILLABLE, &token)) {
                error = -KERNEL_EIO;
            } else {
                /* The second reference travels through the read queue and the
                 * response index. The request itself holds the first reference. */
                permission->references++;
                group->queued++;
                append_event(group, &permission->event, &ready);
            }
        }
    }
    if (ready.queue) kernel_wait_cursor_wake(&ready, 1);

    if (token.task) {
        enum kernel_wait_wake_reason reason = KERNEL_WAIT_SIGNALLED;
        enum kernel_scheduler_status parked = kernel_wait_park(&token, &reason);
        enum kernel_scheduler_status finished = kernel_wait_finish(&token);
        bool withdrew = false;
        {
            KERNEL_RAW_SCOPE(guard, &group->queue_lock);
            if (parked || finished || reason == KERNEL_WAIT_SIGNALLED) {
                error = (parked || finished) ? -KERNEL_EIO : -KERNEL_ERESTARTSYS;
                if (permission->state == FAN_PERMISSION_QUEUED) {
                    unlink_queued_event(group, permission->queue_previous);
                    withdrew = true;
                }
            } else {
                error = permission->result;
            }
        }
        if (withdrew) event_destroy(group, &permission->event);
    }
    event_destroy(group, &permission->event);
    return error;
}

static bool watch_access(struct fan_key *keys, unsigned count, unsigned parents, bool directory)
{
    KERNEL_RAW_SCOPE(guard, &mark_lock);
    for (unsigned i = 0; i < count; i++) {
        struct fan_key key = keys[i];
        key.serial = 0;
        bool parent = !!(parents & (1U << i));
        for (struct fan_mark *mark = first_mark(key); match_object(mark, key);
                mark = RB_NEXT(fan_marks, &marks, mark)) {
            if (key.node && mark->key.node && key.node != mark->key.node) continue;
            if ((mark->mask & VFS_NOTIFY_ACCESS_PERM) && applicable(mark->mask, directory, parent))
                return true;
        }
    }
    return false;
}

static int check_permission(const struct kernel_vfs_file *file, const struct kernel_vfs_path *path,
                            struct kernel_vfs_node *parent, uint32_t event, uint8_t *track_access)
{
    struct kernel_vfs_node *node = file->private_data;
    struct kernel_vfs_instance *instance = node->instance;
    struct fan_key keys[4] = {
        inode_key(node),
        {.instance = instance, .object = (uintptr_t)file->mount, .type = FAN_MARK_MOUNT},
        {.instance = instance, .type = FAN_MARK_FILESYSTEM},
        {0},
    };
    if (parent) keys[3] = inode_key(parent);
    unsigned count = parent ? 4U : 3U;
    unsigned parents = parent ? 1U << 3 : 0U;
    if (track_access && !*track_access && watch_access(keys, count, parents, !!(event & FAN_ONDIR)))
        *track_access = true;
    if (!(event & VFS_NOTIFY_PERMISSION_EVENTS)) return 0;

    uint64_t limit = __atomic_load_n(&last_group, __ATOMIC_RELAXED);
    struct kernel_pid *sender = NULL;
    bool sender_sampled = false;
    int result = 0;
    for (int priority = FAN_CLASS_PRE_CONTENT; priority >= FAN_CLASS_CONTENT; priority -= 4) {
        uint64_t after = 0;
        for (;;) {
            uint32_t mask;
            uint64_t fsid;
            unsigned matched;
            struct fan_group *group = select_group(keys, count, parents, &after, limit,
                event, priority, &mask, &fsid, &matched);
            if (!group) break;
            if (mask) {
                if (!sender_sampled) {
                    (void)kernel_task_identity_get(0, &sender);
                    sender_sampled = true;
                }
                result = request_permission(group, path, sender, mask & ~FAN_ONDIR);
            }
            group_put(group);
            if (result) break;
        }
        if (result) break;
    }
    if (sender) kernel_task_identity_put(sender);
    return result;
}

static const struct kernel_fanotify_ops fan_ops = {
    .permission = check_permission,
    .notify = notify, .notify_entry = notify_entry, .retire_inode = retire_inode, .retire_mount = retire_mount,
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

struct fan_fdinfo_row {
    struct kernel_vfs_handle handle;
    uint64_t device, mount;
    uint32_t mask, ignored, flags;
    unsigned type, handle_type;
};

static void fdinfo_row(const struct fan_mark *mark, struct fan_fdinfo_row *row)
{
    const struct kernel_vfs_instance *instance = mark->key.instance;
    *row = (struct fan_fdinfo_row){
        .device = notify_fdinfo_device(instance->device_id),
        .handle.inode = mark->key.object,
        .mask = mark->mask, .ignored = mark->ignored, .type = mark->key.type,
        .flags = (mark->survive_modify ? FAN_MARK_IGNORED_SURV_MODIFY : 0) |
            (mark->evictable ? FAN_MARK_EVICTABLE : 0) |
            (mark->ignore_has_flags ? FAN_MARK_IGNORE : 0),
    };
    if (mark->key.type == FAN_MARK_MOUNT)
        row->mount = ((const struct kernel_vfs_mount *)mark->key.object)->id;
    const struct kernel_vfs_export_ops *export = instance->ops ? instance->ops->export : NULL;
    if (mark->key.node && export) {
        row->handle.generation = export->generation(mark->key.node);
        export->filesystem(mark->key.instance, row->handle.filesystem);
        row->handle_type = export->type;
    }
}

int kernel_fanotify_fdinfo(const struct kernel_open_file_description *file,
                         struct kernel_heap *heap, char **buffer, size_t *length)
{
    struct fan_group *group = file->device_instance;
    struct fan_fdinfo_cursor cursor;
    char *text;
    for (;;) {
        unsigned capacity;
        {
            KERNEL_RAW_SCOPE(guard, &mark_lock);
            capacity = group->mark_count;
        }
        enum kernel_heap_status allocation = kernel_heap_allocate(heap,
            *length + 64 + (size_t)capacity * 224, (void **)&text);
        if (allocation)
            return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        {
            KERNEL_RAW_SCOPE(guard, &mark_lock);
            if (group->mark_count <= capacity) {
                cursor = (struct fan_fdinfo_cursor){.next = group->fdinfo, .previous = &group->fdinfo};
                memcpy(cursor.mark, group->marks, sizeof(cursor.mark));
                if (cursor.next) cursor.next->previous = &cursor.next;
                group->fdinfo = &cursor;
                break;
            }
        }
        kernel_heap_release(heap, text);
    }

    memcpy(text, *buffer, *length);
    char *out = notify_fdinfo_field(text + *length, "fanotify flags:", group->init_flags);
    out = notify_fdinfo_field(out, " event-flags:", group->event_flags);
    *out++ = '\n';
    for (unsigned type = 0; type < 3; ++type) {
        for (;;) {
            struct fan_fdinfo_row rows[4];
            unsigned count = 0;
            {
                KERNEL_RAW_SCOPE(guard, &mark_lock);
                while (count < sizeof(rows) / sizeof(rows[0]) && cursor.mark[type]) {
                    struct fan_mark *mark = cursor.mark[type];
                    fdinfo_row(mark, &rows[count++]);
                    cursor.mark[type] = mark->group_next;
                }
            }
            if (!count) break;
            for (unsigned i = 0; i < count; ++i) {
                const struct fan_fdinfo_row *row = &rows[i];
                if (row->type == FAN_MARK_MOUNT) out = notify_fdinfo_field(out, "fanotify mnt_id:", row->mount);
                else {
                    if (!row->type) out = notify_fdinfo_field(out, "fanotify ino:", row->handle.inode);
                    out = notify_fdinfo_field(out, row->type ? "fanotify sdev:" : " sdev:", row->device);
                }
                out = notify_fdinfo_field(out, " mflags:", row->flags);
                out = notify_fdinfo_field(out, " mask:", row->mask);
                out = notify_fdinfo_field(out, " ignored_mask:", row->ignored);
                if (row->handle_type)
                    out = notify_fdinfo_handle(out, &row->handle, row->handle_type);
                *out++ = '\n';
            }
        }
    }
    {
        KERNEL_RAW_SCOPE(guard, &mark_lock);
        *cursor.previous = cursor.next;
        if (cursor.next) cursor.next->previous = cursor.previous;
    }
    *out = 0;
    kernel_heap_release(heap, *buffer);
    *buffer = text;
    *length = (size_t)(out - text);
    return 0;
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
    size_t prefix = size - event->info_bytes;
    if (kernel_copy_to_user_iov(mm, cursor, &record, prefix, &copied) || copied != prefix)
        return -KERNEL_EFAULT;
    if (event->info_bytes &&
        (kernel_copy_to_user_iov(mm, cursor, event + 1, event->info_bytes, &copied) ||
         copied != event->info_bytes)) return -KERNEL_EFAULT;
    if (read->opened) {
        KERNEL_FILES_SCOPE(guard, read->files);
        if (kernel_files_install_owned_locked(read->files, read->reservation.fd,
                group->event_flags & LINUX_O_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, &read->opened))
            __builtin_trap();
        read->reservation.files = NULL;
    }
    struct fan_permission *permission = permission_of(event);
    if (permission) permission->fd = record.metadata.fd;
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
                unlink_queued_event(group, &group->first);
                struct fan_permission *permission = permission_of(event);
                if (permission) permission->state = FAN_PERMISSION_READING;
                read->event = event;
            } else if (done || (flags & LINUX_O_NONBLOCK)) {
                return done ? (int64_t)done : -KERNEL_EAGAIN;
            } else if (kernel_wait_prepare(&group->ready, 0, 1, &token)) {
                return -KERNEL_EIO;
            }
        }
        if (event) {
            int result = output_event(read, mm, cursor);
            struct fan_permission *permission = permission_of(event);
            if (result < 0) {
                discard_opened_event(read);
                deny_unreadable_permission(group, event);
            }
            if (permission && result > 0) {
                KERNEL_RAW_SCOPE(guard, &group->queue_lock);
                if (RB_EMPTY(&group->reported)) group->last_report = 0;
                permission->report_order = ++group->last_report;
                permission->state = FAN_PERMISSION_REPORTED;
                RB_INSERT(fan_permissions, &group->reported, permission);
            } else {
                event_destroy(group, event);
            }
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

static bool permitted_errno(unsigned error)
{
    switch (error) {
    case KERNEL_EIO: case KERNEL_EPERM: case KERNEL_EBUSY: case KERNEL_ETXTBSY:
    case KERNEL_EAGAIN: case KERNEL_ENOSPC: case KERNEL_EDQUOT:
        return true;
    default:
        return false;
    }
}

static enum kernel_files_status fan_writev(void *object, struct kernel_task *task,
    struct kernel_files *files, struct kernel_open_file_description **owner, void **iov_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov, size_t count,
    uint64_t bytes, uint32_t flags, int64_t *result)
{
    (void)task;
    (void)files;
    (void)owner;
    (void)iov_owner;
    (void)flags;
    struct fan_group *group = object;
    struct { int32_t fd; uint32_t answer; } response;
    *result = -KERNEL_EINVAL;
    if (bytes < sizeof(response)) return KERNEL_FILES_STATUS_OK;
    struct kernel_uaccess_iov_cursor cursor = {iov, count, 0, 0};
    size_t copied;
    if (kernel_copy_from_user_iov(mm, &cursor, &response, sizeof(response), &copied) ||
        copied != sizeof(response)) {
        *result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    unsigned answer = response.answer & 0x00ffffffU;
    unsigned error = response.answer >> 24;
    if (response.fd < 0 || (answer != FAN_ALLOW && answer != FAN_DENY))
        return KERNEL_FILES_STATUS_OK;
    if (error && (answer != FAN_DENY || group->priority != FAN_CLASS_PRE_CONTENT ||
                  !permitted_errno(error)))
        return KERNEL_FILES_STATUS_OK;

    struct fan_permission *permission;
    struct kernel_wait_cursor wake = {0};
    {
        KERNEL_RAW_SCOPE(guard, &group->queue_lock);
        struct fan_permission key = {.fd = response.fd};
        permission = RB_NFIND(fan_permissions, &group->reported, &key);
        if (!permission || permission->fd != response.fd) {
            *result = -KERNEL_ENOENT;
            return KERNEL_FILES_STATUS_OK;
        }
        RB_REMOVE(fan_permissions, &group->reported, permission);
        answer_permission(permission, answer == FAN_ALLOW ? 0 : -(int)(error ? error : KERNEL_EPERM), &wake);
    }
    if (wake.queue) kernel_wait_cursor_wake(&wake, 1);
    event_destroy(group, &permission->event);
    *result = sizeof(response);
    return KERNEL_FILES_STATUS_OK;
}

static const struct kernel_char_device fan_file_ops = {
    .kind = KERNEL_OPEN_FILE_KIND_FANOTIFY, .release = release_group, .stateless_io = 1,
    .readv = fan_readv, .writev = fan_writev, .poll = poll_events, .ioctl = fan_ioctl,
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
    if ((flags & (FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT)) && (flags & FAN_FID_FLAGS)) {
        *result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (((flags & FAN_REPORT_NAME) && !(flags & FAN_REPORT_DIR_FID)) ||
        ((flags & FAN_REPORT_TARGET_FID) && (flags & FAN_FID_FLAGS) != FAN_FID_FLAGS)) {
        *result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (flags & ~(FAN_CLOEXEC | FAN_NONBLOCK | FAN_FID_FLAGS | FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT)) {
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
    group->priority = (uint8_t)(flags & (FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT));
    group->report = flags & FAN_FID_FLAGS;
    group->init_flags = flags;
    group->fid = group->report != 0;
    group->unprivileged = !admin;
    group->tail = &group->first;
    group->overflow.mask = FAN_Q_OVERFLOW;
    kernel_raw_lock_init(&group->queue_lock, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&group->ready);

    file->heap = files->heap;
    file->references = 1;
    file->kind = KERNEL_OPEN_FILE_KIND_FANOTIFY;
    file->open_flags = 2U | (flags & FAN_NONBLOCK ? LINUX_O_NONBLOCK : 0);
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
        if (mark && key.node && mark->key.node != key.node) {
            unlink_mark(mark);
            retired = mark;
            mark = NULL;
        }
        if (flags & FAN_MARK_ADD) {
            if (!mark && !candidate) error = -KERNEL_ENOMEM;
            else if (!mark && group->account->fanotify_marks == FAN_MAX_MARKS) error = -KERNEL_ENOSPC;
            else if (mark && (((flags & FAN_MARK_EVICTABLE) && !mark->evictable) ||
                     ((flags & FAN_MARK_IGNORED_MASK) && mark->ignore_has_flags) ||
                     ((flags & FAN_MARK_IGNORE) && !(flags & FAN_MARK_IGNORED_SURV_MODIFY) &&
                      mark->survive_modify))) error = -KERNEL_EEXIST;
            else {
                uint32_t old = mark ? mark_interest(mark) : 0;
                uint32_t old_parent = mark ? parent_interest(mark) : 0;
                bool add_pin = key.node && !(flags & FAN_MARK_EVICTABLE) &&
                    (!mark || mark->evictable);
                if (add_pin) {
                    if (kernel_vfs_node_acquire(key.node)) __builtin_trap();
                    __atomic_fetch_add(&key.node->fanotify_pins, 1U, __ATOMIC_RELEASE);
                }
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
                    group->mark_count++;
                }
                mark->evictable = !!(flags & FAN_MARK_EVICTABLE);
                if (flags & FAN_IGNORE_FLAGS) mark->ignored |= mask;
                else mark->mask |= mask;
                if (flags & FAN_MARK_IGNORE) mark->ignore_has_flags = true;
                if ((flags & FAN_IGNORE_FLAGS) && (flags & FAN_MARK_IGNORED_SURV_MODIFY))
                    mark->survive_modify = true;
                summary_change(mark, old, old_parent, mark_interest(mark), parent_interest(mark));
            }
        } else if (!mark) error = -KERNEL_ENOENT;
        else {
            uint32_t old = mark_interest(mark), old_parent = parent_interest(mark);
            if (flags & FAN_IGNORE_FLAGS) mark->ignored &= ~mask;
            else mark->mask &= ~mask;
            summary_change(mark, old, old_parent, mark_interest(mark), parent_interest(mark));
            if (!((mark->mask | mark->ignored) & ~FAN_EVENT_FLAGS)) {
                unlink_mark(mark);
                retired = mark;
            }
        }
    }
    if (candidate) kernel_heap_release(group->heap, candidate);
    if (retired) destroy_mark(group->heap, retired);
    return error;
}

static int modify_mark(struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int fd, unsigned flags, uint64_t mask, int32_t dirfd, uint64_t user_path)
{
    unsigned type = flags & FAN_MARK_TYPE;
    unsigned command = flags & (FAN_MARK_ADD | FAN_MARK_REMOVE | FAN_MARK_FLUSH);
    if (flags & ~FAN_KNOWN_MARK_FLAGS || mask & ~FAN_KNOWN_MASK) return -KERNEL_EINVAL;
    if ((flags & FAN_IGNORE_FLAGS) == FAN_IGNORE_FLAGS ||
        ((flags & FAN_MARK_EVICTABLE) && type)) return -KERNEL_EINVAL;
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
    if (type == FAN_MARK_TYPE) return -KERNEL_EOPNOTSUPP;
    if ((mask & VFS_NOTIFY_PERMISSION_EVENTS) && !group->priority) return -KERNEL_EINVAL;
    if ((mask & (FAN_INODE_EVENTS | FAN_DIRENT_EVENTS)) &&
        (!group->fid || type == FAN_MARK_MOUNT))
        return -KERNEL_EINVAL;
    if ((mask & FAN_RENAME) && !(group->report & FAN_REPORT_NAME))
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
    if (command == FAN_MARK_ADD && (flags & FAN_MARK_IGNORE) &&
        !(flags & FAN_MARK_IGNORED_SURV_MODIFY) && (type || dir))
        return type ? -KERNEL_EINVAL : -KERNEL_EISDIR;
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
    if ((flags & FAN_IGNORE_FLAGS) && command == FAN_MARK_ADD && !type &&
        !(flags & FAN_MARK_IGNORED_SURV_MODIFY) &&
        __atomic_load_n(&node->write_openers, __ATOMIC_RELAXED)) return 0;
    if (flags & FAN_MARK_IGNORED_MASK) mask &= ~FAN_EVENT_FLAGS;
    if (type || !dir) mask &= ~FAN_EVENT_ON_CHILD;

    struct fan_key key = {.instance = node->instance, .type = type, .serial = group->serial};
    if (type == FAN_MARK_MOUNT) key.object = (uintptr_t)path->file.mount;
    else if (!type) {
        key.object = node->inode;
        key.node = node;
    }
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
