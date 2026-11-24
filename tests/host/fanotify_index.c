/* Exercise the production mark index, merge queue and owners using host boundaries. */
#include "../../fs/fanotify.c"
#include "../../kernel/sched/private.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/time.h>

_Thread_local uintptr_t sync_test_irq = 2;
static _Thread_local struct kernel_cpu cpu = {.initialized = KERNEL_CPU_INITIALIZED};
static struct kernel_heap heap;
static struct kernel_uid_account account;
static struct kernel_pid sender_a, sender_b;
static unsigned allocations, wakes;
static bool fail_heap_allocation;
static unsigned path_references, reserved, installed, discarded;
static unsigned pidfd_reserved, pidfd_installed, pidfd_discarded;
static int fd_error, pidfd_error;
static uint64_t fault_address;
static struct kernel_vfs_dentry watched;
static struct kernel_open_file_description legacy_file, watch_file, pidfd_file;

void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return NULL; }
void kernel_cpu_flush_notifications(void) { assert(!cpu.outbound); }
void kernel_assert_can_block(void) { assert(!cpu.raw_locks); }
void kernel_preempt_disable(void) { assert(!cpu.raw_locks); cpu.preempt_depth++; }
void kernel_preempt_enable(void) { assert(!cpu.raw_locks && cpu.preempt_depth); cpu.preempt_depth--; }
void kernel_heap_release(struct kernel_heap *h, void *p)
{
    assert(h == &heap && p && !cpu.raw_locks && allocations);
    allocations--;
    free(p);
}
enum kernel_heap_status kernel_heap_allocate_nosleep(struct kernel_heap *h, size_t size, void **out)
{
    assert(h == &heap && !cpu.raw_locks);
    if (fail_heap_allocation) {
        *out = NULL;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    *out = calloc(1, size);
    assert(*out);
    allocations++;
    return KERNEL_HEAP_STATUS_OK;
}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,
    size_t count, size_t size, void **out)
{
    return kernel_heap_allocate_nosleep(h, count * size, out);
}
void kernel_uid_account_put(struct kernel_uid_account *a) { assert(a == &account); }
void kernel_task_identity_retain(struct kernel_pid *id) { assert(!cpu.raw_locks); id->references++; }
void kernel_task_identity_put(struct kernel_pid *id)
{
    assert(!cpu.raw_locks && id->references);
    id->references--;
}
int kernel_task_identity_get(int32_t number, struct kernel_pid **out)
{
    assert(!number && !cpu.raw_locks);
    *out = &sender_a;
    kernel_task_identity_retain(*out);
    return 0;
}
struct kernel_pid *kernel_task_identity_get_current(enum kernel_pid_role role)
{
    assert(role == KERNEL_PID_TID || role == KERNEL_PID_TGID);
    struct kernel_pid *id = role == KERNEL_PID_TID ? &sender_b : &sender_a;
    kernel_task_identity_retain(id);
    return id;
}
kernel_pid_t kernel_task_identity_user_number(const struct kernel_pid *id)
{ return id == &sender_a ? 17 : 18; }
int kernel_vfs_path_release(struct kernel_vfs_path **path)
{
    assert(!cpu.raw_locks && (*path)->dentry == &watched && path_references);
    path_references--;
    *path = NULL;
    return 0;
}
int kernel_vfs_fanotify_acquire_path(const struct kernel_vfs_path *path, struct kernel_vfs_path **owner)
{
    assert(!cpu.raw_locks && path->dentry == &watched);
    path_references++;
    *owner = (struct kernel_vfs_path *)path;
    return 0;
}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm, uint64_t to,
    const void *from, size_t size, size_t *copied)
{
    (void)mm;
    assert(!cpu.raw_locks);
    if (to == fault_address) {
        *copied = 0;
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    memcpy((void *)(uintptr_t)to, from, size);
    *copied = size;
    return KERNEL_UACCESS_STATUS_OK;
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm, void *to,
    uint64_t from, size_t size, size_t *copied)
{
    (void)mm;
    assert(!cpu.raw_locks);
    if (from == fault_address) {
        *copied = 0;
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    memcpy(to, (const void *)(uintptr_t)from, size);
    *copied = size;
    return KERNEL_UACCESS_STATUS_OK;
}
enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(struct kernel_mm *mm,
    struct kernel_uaccess_write_cursor *cursor, uint64_t to, const void *from,
    size_t size, size_t *copied)
{
    (void)cursor;
    return kernel_copy_to_user(mm, to, from, size, copied);
}
void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *cursor)
{ (void)cursor; }
enum kernel_files_status kernel_files_find_free_fd(struct kernel_files *files, uint32_t *fd, int *result)
{
    (void)files;
    assert(!cpu.raw_locks && !reserved);
    *result = fd_error;
    if (!fd_error) {
        reserved++;
        *fd = 9;
    }
    return KERNEL_FILES_STATUS_OK;
}
void kernel_files_cancel_reservation(struct kernel_files_fd_reservation *reservation)
{
    if (!reservation->files) return;
    assert(!cpu.raw_locks && reservation->fd == 9 && reserved == 1);
    reserved--;
    reservation->files = NULL;
}
int kernel_pidfd_prepare_event(struct kernel_files *files, struct kernel_pid *id,
                              struct kernel_pidfd_prepared *prepared)
{
    assert(!cpu.raw_locks && !prepared->files && !pidfd_reserved && id == &sender_a);
    if (pidfd_error == -KERNEL_ESRCH) return pidfd_error;
    pidfd_reserved++;
    *prepared = (struct kernel_pidfd_prepared){.files = files, .fd = 10};
    if (!pidfd_error) prepared->file = &pidfd_file;
    return pidfd_error;
}
void kernel_pidfd_cancel_prepared(struct kernel_pidfd_prepared *prepared)
{
    if (!prepared->files) return;
    assert(!cpu.raw_locks && pidfd_reserved == 1 && prepared->fd == 10);
    pidfd_discarded += !!prepared->file;
    pidfd_reserved--;
    *prepared = (struct kernel_pidfd_prepared){0};
}
void kernel_pidfd_install_prepared_locked(struct kernel_pidfd_prepared *prepared)
{
    assert(kernel_raw_lock_owned(&prepared->files->record->metadata_lock) &&
           prepared->fd == 10 && prepared->file == &pidfd_file && pidfd_reserved == 1);
    pidfd_installed++;
    pidfd_reserved--;
    *prepared = (struct kernel_pidfd_prepared){0};
}
int kernel_files_open_fanotify_event(struct kernel_files *files, struct kernel_vfs_path *path,
    unsigned flags, struct kernel_open_file_description **file)
{
    (void)files;
    assert(!cpu.raw_locks && path->dentry == &watched && path_references && flags == LINUX_O_CLOEXEC);
    *file = &legacy_file;
    return 0;
}
enum kernel_files_status kernel_files_install_owned_locked(struct kernel_files *files, uint32_t fd,
    uint32_t flags, struct kernel_open_file_description **file)
{
    assert(kernel_raw_lock_owned(&files->record->metadata_lock) &&
           fd == 9 && flags == KERNEL_FILES_FD_CLOEXEC &&
           reserved == 1 && *file == &legacy_file);
    reserved--;
    installed++;
    *file = NULL;
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_release_uninstalled_description(struct kernel_files *files,
    struct kernel_open_file_description **file)
{
    (void)files;
    assert(!cpu.raw_locks);
    if (*file == &watch_file) release_group(watch_file.device_instance);
    else {
        assert(*file == &legacy_file);
        discarded++;
    }
    *file = NULL;
    return KERNEL_FILES_STATUS_OK;
}
int kernel_vfs_fanotify_encode_handle(const struct kernel_vfs_file *file,
    struct kernel_vfs_handle *handle)
{
    const struct kernel_vfs_node *node = file->private_data;
    assert(!cpu.raw_locks);
    *handle = (struct kernel_vfs_handle){.inode = node->inode, .generation = 13};
    return 1;
}
int kernel_vfs_node_acquire(struct kernel_vfs_node *node)
{ node->references++; return 0; }
int kernel_vfs_node_release(struct kernel_vfs_node **owner)
{ assert((*owner)->references); (*owner)->references--; *owner = NULL; return 0; }
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t) { (void)t; abort(); }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t) { (void)t; abort(); }
struct kernel_task *kernel_wait_current_task(void) { return NULL; }
int kernel_wait_backend_initialized(void) { return 1; }
void kernel_wait_backend_notify(struct kernel_task *t, uint32_t reason) { (void)t; (void)reason; abort(); }
void kernel_wait_backend_ready(struct kernel_task *t) { (void)t; abort(); }
void kernel_wait_backend_quiesce(void) { sched_yield(); }
void kernel_task_fanotify_read_set(struct kernel_task *t, struct kernel_fanotify_read *r) { (void)t; (void)r; abort(); }
void kernel_task_fanotify_read_clear(struct kernel_task *t, struct kernel_fanotify_read *r) { (void)t; (void)r; abort(); }
void kernel_wait_backend_commit(struct kernel_task *t, uint64_t d, int i)
{ (void)t; (void)d; (void)i; abort(); }
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *t,
    struct kernel_raw_guard *g) { (void)t; (void)g; abort(); }
uint64_t kernel_wait_backend_time(void) { return 0; }
int kernel_wait_backend_signal(struct kernel_task *t) { (void)t; return 0; }
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns, uint64_t *deadline)
{ *deadline = ns; return 0; }

static struct fan_group *group_create(void)
{
    struct fan_group *group;
    kernel_heap_allocate_nosleep(&heap, sizeof(*group), (void **)&group);
    group->heap = &heap;
    group->account = &account;
    group->references = 1;
    group->serial = ++last_group;
    group->fid = true;
    group->tail = &group->first;
    group->overflow.mask = FAN_Q_OVERFLOW;
    account.fanotify_groups++;
    kernel_raw_lock_init(&group->queue_lock, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&group->ready);
    return group;
}

static void notified(struct kernel_wait_node *node, uint32_t reason, uint64_t event)
{
    (void)node;
    assert(!cpu.raw_locks && reason == KERNEL_WAIT_WOKEN && !event);
    wakes++;
}

static void matching_and_merge(void)
{
    struct kernel_vfs_instance instance = {0};
    struct kernel_vfs_mount mount = {.private_data = &instance}, bind = {.private_data = &instance};
    struct kernel_vfs_node first = {.instance = &instance, .inode = 7},
                           second = {.instance = &instance, .inode = 8},
                           directory = {.instance = &instance, .inode = 9};
    struct kernel_vfs_file a = {.private_data = &first, .mount = &mount, .mode = KERNEL_VFS_S_IFREG};
    struct kernel_vfs_file b = {.private_data = &second, .mount = &mount, .mode = KERNEL_VFS_S_IFREG};
    struct fan_group *group = group_create(), *other = group_create();
    struct fan_key inode = {.instance = &instance, .object = 7, .serial = group->serial};
    struct fan_key parent = {.instance = &instance, .object = 9, .serial = group->serial};
    struct fan_key view = {.instance = &instance, .object = (uintptr_t)&mount,
        .type = FAN_MARK_MOUNT, .serial = group->serial};
    struct fan_key fs = {.instance = &instance, .type = FAN_MARK_FILESYSTEM, .serial = other->serial};
    assert(!update_mark(group, inode, FAN_MARK_ADD, VFS_NOTIFY_OPEN, 42));
    assert(!update_mark(group, parent, FAN_MARK_ADD, FAN_EVENT_ON_CHILD | VFS_NOTIFY_CLOSE_NOWRITE, 42));
    assert(!update_mark(group, view, FAN_MARK_ADD, VFS_NOTIFY_OPEN | VFS_NOTIFY_MODIFY, 42));
    assert(!update_mark(other, fs, FAN_MARK_ADD, VFS_NOTIFY_OPEN, 42));
    assert(instance.fanotify_ops && instance.fanotify_marks == 4);

    struct kernel_wait_node ready = {0};
    kernel_wait_node_init_callback(&ready, notified, NULL);
    kernel_wait_queue_add(&group->ready, &ready);
    notify(&a, NULL, &directory, NULL, VFS_NOTIFY_OPEN);
    notify(&b, NULL, &directory, NULL, VFS_NOTIFY_OPEN);
    notify(&a, NULL, &directory, NULL, VFS_NOTIFY_CLOSE_NOWRITE);
    assert(group->queued == 2 && wakes == 1);
    assert(group->first->handle.inode == 7 &&
           group->first->mask == (VFS_NOTIFY_OPEN | VFS_NOTIFY_CLOSE_NOWRITE));
    assert(group->first->next->handle.inode == 8 && other->queued == 2);

    assert(!update_mark(group, inode, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK, VFS_NOTIFY_OPEN, 42));
    a.mount = &bind;
    notify(&a, NULL, &directory, NULL, VFS_NOTIFY_OPEN);
    assert(group->queued == 2 && other->queued == 2);
    /* Modification clears the legacy ignore flag even when not selected. */
    notify(&a, NULL, &directory, NULL, VFS_NOTIFY_MODIFY);
    assert(group->queued == 2);
    notify(&a, NULL, &directory, NULL, VFS_NOTIFY_OPEN);
    assert(group->queued == 2 && group->first->mask == (VFS_NOTIFY_OPEN | VFS_NOTIFY_CLOSE_NOWRITE));

    retire_mount(&mount);
    retire_inode(&first, false);
    retire_instance(&instance);
    assert(!instance.fanotify_ops && !instance.fanotify_marks && !account.fanotify_marks);
    assert(!kernel_wait_node_remove_sync(&ready));
    release_group(group);
    release_group(other);
    assert(!allocations && !sender_a.references && !account.fanotify_groups);
}

static void mark_conversion(void)
{
    struct kernel_vfs_instance instance = {0};
    struct kernel_vfs_node first = {.instance = &instance, .inode = 7, .references = 1};
    struct kernel_vfs_node replacement = {.instance = &instance, .inode = 7, .references = 1};
    struct fan_group *group = group_create();
    struct fan_key key = inode_key(&first);
    key.serial = group->serial;
    assert(!update_mark(group, key, FAN_MARK_ADD | FAN_MARK_EVICTABLE, VFS_NOTIFY_OPEN, 42));
    assert(first.references == 1 && !first.fanotify_pins);
    retire_inode(&replacement, true);
    assert(instance.fanotify_marks == 1 && (instance.fanotify_interest & VFS_NOTIFY_OPEN));
    retire_inode(&first, true);
    assert(!instance.fanotify_marks && !instance.fanotify_interest);
    assert(!update_mark(group, key, FAN_MARK_ADD | FAN_MARK_EVICTABLE, VFS_NOTIFY_OPEN, 42));
    assert(!update_mark(group, key, FAN_MARK_ADD, VFS_NOTIFY_OPEN, 42));
    assert(first.references == 2 && first.fanotify_pins == 1);
    assert(update_mark(group, key, FAN_MARK_ADD | FAN_MARK_EVICTABLE, VFS_NOTIFY_OPEN, 42)
           == -KERNEL_EEXIST);
    assert(!update_mark(group, key, FAN_MARK_ADD | FAN_MARK_IGNORE | FAN_MARK_IGNORED_SURV_MODIFY,
                       VFS_NOTIFY_OPEN, 42));
    assert(update_mark(group, key, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK,
                       VFS_NOTIFY_OPEN, 42) == -KERNEL_EEXIST);
    release_group(group);
    assert(!instance.fanotify_marks && !allocations && !account.fanotify_groups);
    assert(first.references == 1 && !first.fanotify_pins);
}

static void rename_records(void)
{
    struct kernel_vfs_instance instance = {0};
    struct kernel_vfs_node left = {.instance = &instance, .inode = 7, .references = 1};
    struct kernel_vfs_node right = {.instance = &instance, .inode = 8, .references = 1};
    struct kernel_vfs_node child = {.instance = &instance, .inode = 9, .references = 1};
    struct kernel_vfs_path from = {.file.private_data = &left}, to = {.file.private_data = &right};
    struct kernel_vfs_file file = {.private_data = &child};
    struct fan_group *group = group_create();
    group->report = FAN_FID_FLAGS;
    struct fan_key key = inode_key(&left);
    key.serial = group->serial;
    assert(!update_mark(group, key, FAN_MARK_ADD,
        VFS_NOTIFY_CREATE | VFS_NOTIFY_MOVED_FROM | FAN_RENAME, 42));
    notify_entry(&from, "a", &file, NULL, NULL, NULL, VFS_NOTIFY_CREATE);
    notify_entry(&from, "a", &file, NULL, &to, "b", 0);
    assert(group->queued == 2 && group->first->mask == (VFS_NOTIFY_CREATE | VFS_NOTIFY_MOVED_FROM));
    struct fan_event *event = group->first->next;
    assert(event->mask == FAN_RENAME && event->info_bytes);
    struct fan_fid_info *info = (void *)(event + 1);
    assert(info->type == FAN_INFO_OLD_DFID_NAME && !strcmp((const char *)(info + 1), "a"));
    info = (void *)((char *)info + info->len);
    struct kernel_vfs_handle handle;
    memcpy(&handle, info->handle, sizeof(handle));
    assert(info->type == FAN_INFO_FID && handle.inode == 9);
    assert((char *)info + info->len == (char *)(event + 1) + event->info_bytes);
    release_group(group);
    assert(!allocations && !account.fanotify_groups);
}

static void overflow_and_sender_identity(void)
{
    struct fan_group *group = group_create();
    struct fan_event a = {.sender = &sender_a, .handle_type = 1, .fsid = 42,
                          .mask = VFS_NOTIFY_OPEN};
    for (unsigned i = 1; i <= FAN_MAX_EVENTS; i++) {
        a.handle.inode = i;
        queue_notification(group, &a);
    }
    assert(group->queued == FAN_MAX_EVENTS && sender_a.references == FAN_MAX_EVENTS);
    const struct fan_event *last = group->merge[event_bucket(&a)];
    assert(last && last->handle.inode == FAN_MAX_EVENTS);
    a.mask = VFS_NOTIFY_CLOSE_NOWRITE;
    queue_notification(group, &a);
    assert(last->mask == (VFS_NOTIFY_OPEN | VFS_NOTIFY_CLOSE_NOWRITE));
    a.sender = &sender_b;
    queue_notification(group, &a);
    queue_notification(group, &a);
    assert(group->overflow_queued && !sender_b.references);
    assert(group->tail == &group->overflow.next && !group->overflow.next);
    release_group(group);
    assert(!allocations && !sender_a.references && !sender_b.references && !account.fanotify_groups);
}

static void parent_ignore_and_unlimited_queue(void)
{
    struct kernel_vfs_instance instance = {0};
    struct kernel_vfs_mount mount = {.private_data = &instance};
    struct kernel_vfs_node directory = {.instance = &instance, .inode = 9, .references = 1};
    struct kernel_vfs_node child = {.instance = &instance, .inode = 10};
    struct kernel_vfs_file file = {.private_data = &child, .mount = &mount, .mode = KERNEL_VFS_S_IFREG};
    struct fan_group *group = group_create();
    struct fan_key parent = inode_key(&directory);
    parent.serial = group->serial;
    assert(!update_mark(group, parent, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK, VFS_NOTIFY_CLOSE_NOWRITE, 42));
    notify(&file, NULL, &directory, NULL, VFS_NOTIFY_MODIFY);
    assert(first_mark(parent)->ignored == VFS_NOTIFY_CLOSE_NOWRITE);
    assert(!update_mark(group, parent, FAN_MARK_ADD | FAN_MARK_IGNORE | FAN_MARK_IGNORED_SURV_MODIFY,
        FAN_EVENT_ON_CHILD | VFS_NOTIFY_CLOSE_NOWRITE, 42));
    assert(instance.fanotify_parent_interest & VFS_NOTIFY_CLOSE_NOWRITE);
    release_group(group);

    group = group_create();
    group->init_flags = FAN_UNLIMITED_QUEUE;
    struct fan_event event = {.handle_type = 1, .fsid = 42, .mask = VFS_NOTIFY_OPEN};
    for (unsigned i = 1; i <= FAN_MAX_EVENTS + 1; i++) {
        event.handle.inode = i;
        queue_notification(group, &event);
    }
    assert(group->queued == FAN_MAX_EVENTS + 1 && !group->overflow_queued);
    release_group(group);
    assert(!allocations && !account.fanotify_groups && !account.fanotify_marks);
}

static void read_legacy(void)
{
    struct fan_group *group = group_create();
    group->fid = false;
    group->event_flags = LINUX_O_CLOEXEC;
    struct kernel_vfs_path path = {.dentry = &watched};
    struct fan_event event = {.sender = &sender_a, .path = &path, .mask = VFS_NOTIFY_OPEN | FAN_ONDIR};
    struct kernel_files_record file_table = {0};
    kernel_raw_lock_init(&file_table.metadata_lock, KERNEL_RAW_RANK_TASK_FILES);
    struct kernel_files files = {.record = &file_table, .heap = &heap};
    struct kernel_fanotify_read read = {.group = group, .files = &files};
    struct fan_metadata output;
    struct kernel_uaccess_iovec iov = {(uint64_t)(uintptr_t)&output, sizeof(output)};
    struct kernel_uaccess_iov_cursor cursor = {&iov, 1, 0, 0};
    queue_notification(group, &event);
    assert(read_records(&read, NULL, &cursor, sizeof(output) - 1, LINUX_O_NONBLOCK) ==
           -KERNEL_EINVAL && group->queued == 1);
    fault_address = iov.base;
    assert(read_records(&read, NULL, &cursor, sizeof(output), LINUX_O_NONBLOCK) ==
           -KERNEL_EFAULT && !group->queued && !reserved && discarded == 1);
    fault_address = 0;
    fd_error = -KERNEL_EMFILE;
    queue_notification(group, &event);
    assert(read_records(&read, NULL, &cursor, sizeof(output), LINUX_O_NONBLOCK) ==
           -KERNEL_EMFILE && !group->queued && !reserved && installed == 0);
    fd_error = 0;
    queue_notification(group, &event);
    assert(read_records(&read, NULL, &cursor, sizeof(output), LINUX_O_NONBLOCK) ==
           sizeof(output) && installed == 1 && output.fd == 9 && output.pid == 17 &&
           output.mask == VFS_NOTIFY_OPEN);
    /* A task stopped between preparing a delivery and copying it owns all
     * of these independent resources. Cleanup must not publish the descriptor. */
    watch_file.device_instance = group;
    read.owner = &watch_file;
    read.opened = &legacy_file;
    read.reservation = (struct kernel_files_fd_reservation){&files, 9};
    reserved = 1;
    assert(!kernel_heap_allocate_nosleep(&heap, sizeof(event), (void **)&read.event));
    assert(!kernel_heap_allocate_nosleep(&heap, sizeof(iov), &read.vectors));
    *read.event = event;
    assert(!kernel_vfs_fanotify_acquire_path(&path, &read.event->path));
    kernel_task_identity_retain(event.sender);
    kernel_fanotify_read_abort(&read);
    assert(discarded == 2 && installed == 1);
    assert(!reserved && !path_references && !allocations && !sender_a.references);
}

static void filesystem_error_records(void)
{
    struct kernel_vfs_instance instance = {0};
    struct fan_group *group = group_create();
    struct fan_key key = {.instance = &instance, .type = FAN_MARK_FILESYSTEM,
                          .serial = group->serial};
    struct kernel_vfs_handle handle = {.inode = 191, .generation = 13};
    assert(!update_mark(group, key, FAN_MARK_ADD, FAN_FS_ERROR, 42));
    fail_heap_allocation = true;
    notify_filesystem_error(&instance, KERNEL_EUCLEAN, &handle, KERNEL_VFS_HANDLE_EXT4);
    notify_filesystem_error(&instance, KERNEL_ESHUTDOWN, NULL, 0);
    fail_heap_allocation = false;
    assert(group->queued == 1 && group->first->mask == FAN_FS_ERROR);
    struct fan_fid_info *fid = (void *)(group->first + 1);
    struct fan_error_info *error = (void *)((char *)fid + fid->len);
    assert(fid->type == FAN_INFO_FID && fid->fsid[0] == 42 &&
           fid->handle_type == KERNEL_VFS_HANDLE_EXT4 && fid->handle_bytes == sizeof(handle));
    assert(!memcmp(fid->handle, &handle, sizeof(handle)));
    assert(error->type == FAN_INFO_ERROR && error->len == sizeof(*error) &&
           error->error == KERNEL_EUCLEAN && error->count == 2);
    unsigned char record[128];
    struct kernel_uaccess_iovec iov = {(uint64_t)(uintptr_t)record, sizeof(record)};
    struct kernel_uaccess_iov_cursor cursor = {&iov, 1, 0, 0};
    struct kernel_fanotify_read read = {.group = group};
    const size_t bytes = sizeof(struct fan_metadata) + fid->len + sizeof(*error);
    assert(read_records(&read, NULL, &cursor, sizeof(record), LINUX_O_NONBLOCK) == (int)bytes);
    struct fan_metadata metadata;
    memcpy(&metadata, record, sizeof(metadata));
    assert(metadata.fd == FAN_NOFD && metadata.mask == FAN_FS_ERROR &&
           metadata.event_len == bytes && group->error_reserve_free == UINT32_MAX);
    release_group(group);
    assert(!allocations && !sender_a.references && !account.fanotify_groups);

    group = group_create();
    key.serial = group->serial;
    assert(!update_mark(group, key, FAN_MARK_ADD, FAN_FS_ERROR, 42));
    notify_filesystem_error(&instance, KERNEL_ESHUTDOWN, NULL, 0);
    fid = (void *)(group->first + 1);
    error = (void *)((char *)fid + fid->len);
    assert(fid->handle_type == 255 && !fid->handle_bytes &&
           fid->len == offsetof(struct fan_fid_info, handle));
    assert(error->error == KERNEL_ESHUTDOWN && error->count == 1);
    release_group(group);
    assert(!allocations && !sender_a.references && !account.fanotify_groups);
}

static void report_sender_identity(void)
{
    struct fan_group *group = group_create();
    struct fan_senders senders = {0};
    assert(fan_sender(&senders, group) == &sender_a);
    group->init_flags = FAN_REPORT_TID;
    assert(fan_sender(&senders, group) == &sender_b);
    assert(fan_sender(&senders, group) == &sender_b);
    assert(sender_a.references == 1 && sender_b.references == 1);
    fan_senders_release(&senders);
    assert(!sender_a.references && !sender_b.references);
    release_group(group);
}

static void read_pidfd(void)
{
    struct fan_group *group = group_create();
    group->init_flags = FAN_REPORT_PIDFD | FAN_REPORT_FID;
    struct fan_event event = {
        .sender = &sender_a, .handle_type = 1, .fsid = 42, .mask = VFS_NOTIFY_OPEN,
    };
    struct kernel_files_record file_table = {0};
    kernel_raw_lock_init(&file_table.metadata_lock, KERNEL_RAW_RANK_TASK_FILES);
    struct kernel_files files = {.record = &file_table, .heap = &heap};
    struct kernel_fanotify_read read = {.group = group, .files = &files};
    struct {
        struct fan_metadata metadata;
        struct fan_fid_info fid;
        struct fan_pidfd_info pidfd;
    } output;
    const size_t size = sizeof(output.metadata) + sizeof(output.fid) + sizeof(output.pidfd);
    struct kernel_uaccess_iovec iov = {(uint64_t)(uintptr_t)&output, size};
    struct kernel_uaccess_iov_cursor cursor = {&iov, 1, 0, 0};
    queue_notification(group, &event);
    assert(read_records(&read, NULL, &cursor, size - 1, LINUX_O_NONBLOCK) ==
           -KERNEL_EINVAL && group->queued == 1 && !pidfd_reserved);

    fault_address = (uint64_t)(uintptr_t)&output.pidfd;
    assert(read_records(&read, NULL, &cursor, size, LINUX_O_NONBLOCK) == -KERNEL_EFAULT);
    assert(!group->queued && !pidfd_reserved && !pidfd_installed && pidfd_discarded == 1);

    fault_address = 0;
    pidfd_error = -KERNEL_ESRCH;
    queue_notification(group, &event);
    cursor = (struct kernel_uaccess_iov_cursor){&iov, 1, 0, 0};
    assert(read_records(&read, NULL, &cursor, size, LINUX_O_NONBLOCK) == (int64_t)size);
    assert(output.metadata.pid == 17 && output.pidfd.pidfd == FAN_NOPIDFD && !pidfd_installed);

    pidfd_error = -KERNEL_EMFILE;
    queue_notification(group, &event);
    cursor = (struct kernel_uaccess_iov_cursor){&iov, 1, 0, 0};
    assert(read_records(&read, NULL, &cursor, size, LINUX_O_NONBLOCK) == (int64_t)size);
    assert(output.pidfd.pidfd == FAN_EPIDFD && !pidfd_reserved && !pidfd_installed);

    pidfd_error = 0;
    queue_notification(group, &event);
    cursor = (struct kernel_uaccess_iov_cursor){&iov, 1, 0, 0};
    assert(read_records(&read, NULL, &cursor, size, LINUX_O_NONBLOCK) == (int64_t)size);
    assert(output.metadata.event_len == size && output.metadata.fd == FAN_NOFD &&
           output.fid.type == FAN_INFO_FID && output.pidfd.type == FAN_INFO_PIDFD &&
           output.pidfd.len == sizeof(output.pidfd) && output.pidfd.pidfd == 10);
    assert(!pidfd_reserved && pidfd_installed == 1);
    release_group(group);
    assert(!allocations && !sender_a.references);
}

int main(void)
{
    kernel_wait_domain_init();
    matching_and_merge();
    mark_conversion();
    rename_records();
    overflow_and_sender_identity();
    parent_ignore_and_unlimited_queue();
    read_legacy();
    report_sender_identity();
    read_pidfd();
    filesystem_error_records();
    puts("PASS fanotify: mark scope, merge, overflow, copy faults and descriptor ownership");
    return 0;
}
