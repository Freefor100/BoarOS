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
static unsigned path_references, reserved, installed, discarded;
static int fd_error;
static uint64_t fault_address;
static struct kernel_vfs_dentry watched;
static struct kernel_open_file_description legacy_file, watch_file;

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
                           second = {.instance = &instance, .inode = 8};
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
    notify(&a, NULL, 9, VFS_NOTIFY_OPEN);
    notify(&b, NULL, 9, VFS_NOTIFY_OPEN);
    notify(&a, NULL, 9, VFS_NOTIFY_CLOSE_NOWRITE);
    assert(group->queued == 2 && wakes == 1);
    assert(group->first->handle.inode == 7 &&
           group->first->mask == (VFS_NOTIFY_OPEN | VFS_NOTIFY_CLOSE_NOWRITE));
    assert(group->first->next->handle.inode == 8 && other->queued == 2);

    assert(!update_mark(group, inode, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK, VFS_NOTIFY_OPEN, 42));
    a.mount = &bind;
    notify(&a, NULL, 9, VFS_NOTIFY_OPEN);
    assert(group->queued == 2 && other->queued == 2);
    /* Modification clears the legacy ignore flag even when not selected. */
    notify(&a, NULL, 9, VFS_NOTIFY_MODIFY);
    assert(group->queued == 2);
    notify(&a, NULL, 9, VFS_NOTIFY_OPEN);
    assert(group->queued == 2 && group->first->mask == (VFS_NOTIFY_OPEN | VFS_NOTIFY_CLOSE_NOWRITE));

    retire_mount(&mount);
    retire_inode(&instance, 7);
    retire_instance(&instance);
    assert(!instance.fanotify_ops && !instance.fanotify_marks && !account.fanotify_marks);
    assert(!kernel_wait_node_remove_sync(&ready));
    release_group(group);
    release_group(other);
    assert(!allocations && !sender_a.references && !account.fanotify_groups);
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

int main(void)
{
    kernel_wait_domain_init();
    matching_and_merge();
    overflow_and_sender_identity();
    read_legacy();
    puts("PASS fanotify: mark scope, merge, overflow, copy faults and descriptor ownership");
    return 0;
}
