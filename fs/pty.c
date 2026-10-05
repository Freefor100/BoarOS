#include "pty_internal.h"
#include "char_device_internal.h"
#include "files/private.h"
#include "vfs_objects.h"
#include <arch/riscv/context.h>
#include <kernel/devpts.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/tty.h>
#include <string.h>

#ifndef BOAROS_PTY_MAX
#define BOAROS_PTY_MAX 64U
#endif
#define PTY_GLOBAL_LIMIT BOAROS_PTY_MAX
_Static_assert(PTY_GLOBAL_LIMIT > 0, "positive PTY global budget");
#define PTY_FIFO_SIZE 4096U
#define PTY_FIFO_MASK (PTY_FIFO_SIZE - 1U)
#define PTY_BATCH 256U
#define PTY_MASTER 0U
#define PTY_SLAVE 1U

struct pty_pair;
struct pty_fifo {
    uint64_t head, tail;
    unsigned char bytes[PTY_FIFO_SIZE];
};
struct pty_end {
    struct pty_pair *pair;
    struct kernel_tty *tty;
    struct pty_fifo outgoing;
    unsigned side;
    uint8_t configured;
};
struct pty_mount {
    struct kernel_vfs_mount *mount;
    struct kernel_heap *heap;
    struct pty_pair *pairs, *cursor;
    struct kernel_wait_queue work;
    struct kernel_thread_join worker;
    uint8_t requested, stopping, servicing;
};
struct pty_pair {
    struct pty_mount *mount;
    struct pty_pair *next;
    struct kernel_devpts_entry *entry;
    struct kernel_vfs_path *root, *slave_path;
    struct pty_end ends[2];
    uint32_t owners;
    uint8_t creating, master_closed, slave_seen, slave_locked, hangup_pending, busy;
};
static unsigned global_pairs;

static void request_work(struct pty_mount *mount)
{
    uintptr_t irq = riscv_interrupt_save();
    /* 本轮自身的kick不能把无进展FIFO变成持续忙轮询。 */
    if (!mount->servicing) mount->requested = 1;
    (void)kernel_wait_queue_wake_all(&mount->work);
    riscv_interrupt_restore(irq);
}
static void pair_get(struct pty_pair *pair)
{
    if (pair->owners == UINT32_MAX) __builtin_trap();
    pair->owners++;
}
static void pair_put(struct pty_pair *pair)
{
    if (!pair->owners) __builtin_trap();
    pair->owners--;
    request_work(pair->mount);
}
static size_t transmit(void *owner, const unsigned char *bytes, size_t size)
{
    struct pty_end *end = owner;
    struct pty_pair *pair = end->pair;
    if (pair->master_closed) return 0;
    struct pty_fifo *fifo = &end->outgoing;
    size_t room = PTY_FIFO_SIZE - (size_t)(fifo->head - fifo->tail);
    if (size > room) size = room;
    for (size_t i = 0; i < size; i++) fifo->bytes[fifo->head++ & PTY_FIFO_MASK] = bytes[i];
    if (size) request_work(pair->mount);
    return size;
}
static int drained(void *owner)
{
    struct pty_fifo *fifo = &((struct pty_end *)owner)->outgoing;
    return fifo->head == fifo->tail;
}
static int configure(void *owner, struct kernel_tty_termios *settings)
{
    struct pty_end *end = owner;
    if (!end->configured) {
        settings->cflag = 0xbfU; /* B38400|CS8|CREAD，固定Linux Unix98初值。 */
        end->configured = 1;
    }
    settings->cflag = (settings->cflag & ~(0x30U | 0x100U)) | 0x30U | 0x80U;
    return 0;
}
static void kick(void *owner) { request_work(((struct pty_end *)owner)->pair->mount); }
static void external_ref(void *owner, int delta)
{
    struct pty_pair *pair = ((struct pty_end *)owner)->pair;
    if (delta == 1) pair_get(pair);
    else if (delta == -1) pair_put(pair);
    else __builtin_trap();
}
static void opened(void *owner)
{
    struct pty_end *end = owner;
    if (end->side == PTY_SLAVE) end->pair->slave_seen = 1;
    kernel_tty_transport_ready(end->pair->ends[end->side ^ 1U].tty);
    request_work(end->pair->mount);
}
static int open_check(void *owner)
{
    struct pty_end *end = owner;
    if (end->pair->master_closed) return -KERNEL_EIO;
    if (end->side == PTY_SLAVE && (end->pair->creating || end->pair->slave_locked))
        return -KERNEL_EIO;
    return 0;
}
static void last_close(void *owner, uint32_t cflag)
{
    (void)cflag;
    struct pty_end *end = owner;
    struct pty_pair *pair = end->pair;
    if (end->side == PTY_MASTER) {
        pair->master_closed = pair->hangup_pending = 1;
        /* 立即撤销名字和弱绑定；真实行规程/ctty清理交给worker。 */
        kernel_devpts_unpublish(pair->entry);
    }
    kernel_tty_transport_ready(pair->ends[end->side ^ 1U].tty);
    request_work(pair->mount);
}
static enum kernel_tty_peer_state peer_state(void *owner)
{
    struct pty_end *end = owner;
    struct pty_pair *pair = end->pair;
    if (pair->master_closed) return KERNEL_TTY_PEER_CLOSED;
    if (end->side == PTY_MASTER && pair->slave_seen &&
        !kernel_tty_open_count(pair->ends[PTY_SLAVE].tty)) return KERNEL_TTY_PEER_ABSENT;
    return KERNEL_TTY_PEER_LIVE;
}
static struct kernel_tty *peer(void *owner)
{
    struct pty_end *end = owner;
    return end->pair->ends[end->side ^ 1U].tty;
}
static void event(void *owner, unsigned flags)
{
    struct pty_end *end = owner;
    /* 输出flush同时丢弃尚未进入对端行规程的传输字节。 */
    if (flags & 2U) end->outgoing.tail = end->outgoing.head;
    if (end->side == PTY_SLAVE)
        kernel_tty_packet_event(end->pair->ends[PTY_MASTER].tty, flags & 0x7fU);
    request_work(end->pair->mount);
}
static size_t receive_fifo(struct pty_end *end, size_t budget)
{
    struct pty_fifo *fifo = &end->outgoing;
    struct kernel_tty_rx input[PTY_BATCH];
    size_t count = (size_t)(fifo->head - fifo->tail);
    if (count > budget) count = budget;
    for (size_t i = 0; i < count; i++)
        input[i] = (struct kernel_tty_rx){fifo->bytes[(fifo->tail + i) & PTY_FIFO_MASK], 0};
    size_t accepted = kernel_tty_receive_nonblocking(peer(end), input, count);
    if (accepted > count) __builtin_trap();
    fifo->tail += accepted;
    if (accepted) kernel_tty_transport_ready(end->tty);
    return accepted;
}
static size_t progress_pair(struct pty_pair *pair)
{
    if (pair->creating || pair->master_closed || pair->busy) return 0;
    pair->busy = 1;
    size_t progress = 0;
    for (unsigned side = 0; side < 2; side++) {
        struct pty_end *end = &pair->ends[side];
        size_t accepted = receive_fifo(end, PTY_BATCH);
        progress += accepted;
        progress += kernel_tty_service_output(end->tty, PTY_BATCH);
        if (accepted < PTY_BATCH) progress += receive_fifo(end, PTY_BATCH - accepted);
    }
    pair->busy = 0;
    return progress;
}
static void progress(void *owner)
{
    struct pty_pair *pair = ((struct pty_end *)owner)->pair;
    uintptr_t irq = riscv_interrupt_save();
    uint8_t servicing = pair->mount->servicing;
    pair->mount->servicing = 1;
    size_t advanced = progress_pair(pair); /* read/poll在检查HUP前交付已接受的尾部。 */
    pair->mount->servicing = servicing;
    if (advanced) request_work(pair->mount);
    riscv_interrupt_restore(irq);
}
static int copy_int(struct kernel_mm *mm, uint64_t address, int32_t *value, int output)
{
    size_t copied = 0;
    enum kernel_uaccess_status status = output
        ? kernel_copy_to_user(mm, address, value, sizeof(*value), &copied)
        : kernel_copy_from_user(mm, value, address, sizeof(*value), &copied);
    if (status == KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(*value)) __builtin_trap();
    return 0;
}
static int ioctl(void *owner, struct kernel_task *caller, struct kernel_files *files,
    struct kernel_open_file_description **file, struct kernel_mm *mm,
    uint64_t command, uint64_t argument)
{
    (void)file;
    struct pty_end *end = owner;
    if (end->side != PTY_MASTER) return -KERNEL_ENOTTY;
    struct pty_pair *pair = end->pair;
    int32_t value;
    switch (command) {
    case UINT64_C(0x80045430): /* TIOCGPTN */
        value = (int32_t)kernel_devpts_entry_number(pair->entry);
        return copy_int(mm, argument, &value, 1);
    case UINT64_C(0x40045431): { /* TIOCSPTLCK */
        int result = copy_int(mm, argument, &value, 0);
        if (!result) {
            uintptr_t irq = riscv_interrupt_save();
            pair->slave_locked = value != 0;
            riscv_interrupt_restore(irq);
        }
        return result;
    }
    case UINT64_C(0x80045439): /* TIOCGPTLCK */
        value = pair->slave_locked;
        return copy_int(mm, argument, &value, 1);
    case UINT64_C(0x5441): { /* TIOCGPTPEER:只使用既有slave路径身份。 */
        struct kernel_vfs_path *path = pair->slave_path;
        if (!path || pair->master_closed) return -KERNEL_EIO;
        int result = kernel_vfs_path_acquire(path);
        if (result) return result;
        result = kernel_files_open_pty_peer(files, caller, path, (uint32_t)argument);
        if (kernel_vfs_path_release(&path)) __builtin_trap();
        return result;
    }
    default: return -KERNEL_ENOTTY;
    }
}
static const struct kernel_tty_transport transport = {
    .transmit = transmit, .drained = drained, .configure = configure, .kick = kick,
    .last_close = last_close, .external_ref = external_ref, .opened = opened,
    .open_check = open_check,
    .peer_state = peer_state, .progress = progress, .event = event, .peer = peer,
    .ioctl = ioctl
};
static void release_path(struct kernel_vfs_path **owner)
{
    /* 通用path引用/弱registry沿单hart发布契约操作；worker不能在SIE开启时借用它。 */
    uintptr_t irq = riscv_interrupt_save();
    if (*owner && kernel_vfs_path_release(owner)) __builtin_trap();
    riscv_interrupt_restore(irq);
}
static int retire_pair(struct pty_pair *pair)
{
    uintptr_t irq = riscv_interrupt_save();
    if (pair->creating || !pair->master_closed || pair->owners != 1 || pair->busy) {
        riscv_interrupt_restore(irq);
        return 0;
    }
    for (unsigned side = 0; side < 2; side++) {
        struct kernel_tty *tty = pair->ends[side].tty;
        if (tty && !kernel_tty_base_only(tty)) { riscv_interrupt_restore(irq); return 0; }
    }
    struct pty_mount *mount = pair->mount;
    struct pty_pair **link = &mount->pairs;
    while (*link && *link != pair) link = &(*link)->next;
    if (!*link || !global_pairs) __builtin_trap();
    *link = pair->next;
    if (mount->cursor == pair) mount->cursor = pair->next ? pair->next : mount->pairs;
    pair->creating = 1; /* 已摘工作链，剩余清理由当前worker独占。 */
    mount->servicing = 1;
    for (unsigned side = 0; side < 2; side++) {
        struct pty_end *end = &pair->ends[side];
        end->outgoing.tail = end->outgoing.head;
        if (end->tty) {
            kernel_tty_shutdown(end->tty);
            kernel_tty_discard(end->tty);
        }
    }
    /* 两端flush仍可能通知对端packet状态；全部静止后才拆base引用。 */
    for (unsigned side = 0; side < 2; side++)
        if (pair->ends[side].tty && kernel_tty_destroy(&pair->ends[side].tty)) __builtin_trap();
    if (pair->entry) kernel_devpts_retire(pair->entry);
    mount->servicing = 0;
    riscv_interrupt_restore(irq);
    release_path(&pair->slave_path);
    release_path(&pair->root);
    if (pair->entry) kernel_devpts_entry_release(&pair->entry);
    irq = riscv_interrupt_save();
    if (pair->owners != 1) __builtin_trap();
    pair->owners = 0; /* 消费本轮临时owner；不再向已摘链对象发送kick。 */
    global_pairs--;
    (void)kernel_wait_queue_wake_all(&mount->work);
    riscv_interrupt_restore(irq);
    if (kernel_heap_release(mount->heap, pair) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return 1;
}
static void worker(void *argument)
{
    struct pty_mount *mount = argument;
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        while (!mount->requested && !mount->stopping) {
            enum kernel_wait_wake_reason reason;
            if (kernel_scheduler_block_current(&mount->work, 0, 1, &reason) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        }
        if (mount->stopping) { riscv_interrupt_restore(irq); return; }
        mount->requested = 0;
        struct pty_pair *pair = mount->cursor ? mount->cursor : mount->pairs;
        unsigned count = 0;
        for (struct pty_pair *p = mount->pairs; p; p = p->next) count++;
        riscv_interrupt_restore(irq);
        size_t advanced = 0;
        while (count-- && pair) {
            irq = riscv_interrupt_save();
            struct pty_pair *next = pair->next ? pair->next : mount->pairs;
            if (next == pair) next = 0;
            pair_get(pair); /* SIE恢复和路径释放跨等待前先取得独立owner。 */
            mount->servicing = 1;
            if (pair->hangup_pending) {
                pair->hangup_pending = 0;
                if (pair->ends[PTY_SLAVE].tty) kernel_tty_shutdown(pair->ends[PTY_SLAVE].tty);
            }
            advanced += progress_pair(pair);
            mount->servicing = 0;
            riscv_interrupt_restore(irq);
            int retired = retire_pair(pair);
            advanced += retired;
            if (!retired) {
                irq = riscv_interrupt_save();
                if (!pair->owners) __builtin_trap();
                pair->owners--; /* 本轮借用归还不产生新的工作事件。 */
                riscv_interrupt_restore(irq);
            }
            pair = next;
        }
        irq = riscv_interrupt_save();
        mount->cursor = pair ? pair : mount->pairs;
        if (advanced) mount->requested = 1;
        riscv_interrupt_restore(irq);
        /* 每轮每方向至多256字节，轮后让出CPU；无进展必须等待真实新事件。 */
        irq = riscv_interrupt_save();
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        riscv_interrupt_restore(irq);
    }
}
int kernel_pty_mount_start(struct kernel_vfs_mount *mount)
{
    if (!kernel_devpts_is_mount(mount) || kernel_devpts_mount_private(mount)) return -KERNEL_EINVAL;
    struct kernel_vfs_instance *instance = mount->private_data;
    struct pty_mount *owner = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(instance->heap, 1,
        sizeof(*owner), (void **)&owner);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    owner->heap = instance->heap;
    owner->mount = mount;
    kernel_wait_queue_init(&owner->work);
    enum kernel_scheduler_status created = kernel_thread_create_joinable(worker, owner, &owner->worker);
    if (created != KERNEL_SCHEDULER_STATUS_OK) {
        if (kernel_heap_release(owner->heap, owner) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return created == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EINVAL;
    }
    kernel_devpts_mount_set_private(mount, owner);
    return 0;
}
int kernel_pty_mount_stop(struct kernel_vfs_mount *mount)
{
    if (!kernel_devpts_is_mount(mount)) return -KERNEL_EINVAL;
    struct pty_mount *owner = kernel_devpts_mount_private(mount);
    if (!owner) return 0;
    uintptr_t irq = riscv_interrupt_save();
    if (owner->pairs) { riscv_interrupt_restore(irq); return -KERNEL_EBUSY; }
    owner->stopping = 1;
    (void)kernel_wait_queue_wake_all(&owner->work);
    kernel_thread_join(&owner->worker);
    kernel_devpts_mount_set_private(mount, 0);
    riscv_interrupt_restore(irq);
    if (kernel_heap_release(owner->heap, owner) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return 0;
}
int kernel_pty_open_ptmx(struct kernel_vfs_mount *mount, struct kernel_heap *heap,
    struct kernel_task *caller, uint32_t flags, void **instance,
    const struct kernel_char_device **device)
{
    if (!kernel_devpts_is_mount(mount) || !heap || !instance || !device) return -KERNEL_EINVAL;
    struct pty_mount *owner = kernel_devpts_mount_private(mount);
    if (!owner || owner->stopping) return -KERNEL_EIO;
    uintptr_t irq = riscv_interrupt_save();
    for (;;) {
        int reclaimable = 0;
        for (struct pty_pair *p = owner->pairs; p; p = p->next)
            if (!p->creating && p->master_closed && !p->owners)
                reclaimable = 1;
        if (!reclaimable) break;
        /* 最后close已归还资格但worker尚未回收；不能把这种暂态冒充配额耗尽。 */
        if (caller && kernel_signal_has_pending(caller)) {
            kernel_signal_note_syscall_restart(caller);
            riscv_interrupt_restore(irq);
            return -KERNEL_ERESTARTSYS;
        }
        request_work(owner);
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&owner->work, 0, 1, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        if (reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(caller);
            riscv_interrupt_restore(irq);
            return -KERNEL_ERESTARTSYS;
        }
    }
    if (global_pairs == PTY_GLOBAL_LIMIT) { riscv_interrupt_restore(irq); return -KERNEL_ENOSPC; }
    global_pairs++;
    riscv_interrupt_restore(irq);
    struct pty_pair *pair = 0;
    enum kernel_heap_status allocated = kernel_heap_allocate_zeroed(owner->heap, 1,
        sizeof(*pair), (void **)&pair);
    if (allocated != KERNEL_HEAP_STATUS_OK) {
        irq = riscv_interrupt_save(); global_pairs--; riscv_interrupt_restore(irq);
        return allocated == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    pair->mount = owner;
    pair->creating = pair->slave_locked = 1;
    pair->owners = 1; /* 构造期间的临时owner；失败也交worker回收。 */
    irq = riscv_interrupt_save();
    pair->next = owner->pairs; owner->pairs = pair;
    riscv_interrupt_restore(irq);
    int result = 0;
    for (unsigned side = 0; side < 2 && !result; side++) {
        pair->ends[side].pair = pair;
        pair->ends[side].side = side;
        result = kernel_tty_create(owner->heap, &transport, &pair->ends[side], &pair->ends[side].tty);
    }
    if (!result) result = kernel_devpts_publish(mount, pair, &pair->entry);
    if (!result) result = kernel_devpts_mount_root(mount, &pair->root);
    if (!result) {
        uint32_t number = kernel_devpts_entry_number(pair->entry);
        char name[12];
        size_t n = 0;
        do { name[n++] = (char)('0' + number % 10); number /= 10; } while (number);
        for (size_t j = 0; j < n / 2; j++) {
            char c = name[j]; name[j] = name[n - j - 1]; name[n - j - 1] = c;
        }
        result = kernel_vfs_path_lookup(pair->root, name, n, &pair->slave_path);
    }
    if (!result) {
        uint32_t number = kernel_devpts_entry_number(pair->entry);
        kernel_tty_configure_identity(pair->ends[PTY_MASTER].tty, UINT64_C(0x8000) + number, 1);
        kernel_tty_configure_identity(pair->ends[PTY_SLAVE].tty, UINT64_C(0x8800) + number, 0);
        result = kernel_tty_open(pair->ends[PTY_MASTER].tty, heap, caller, flags, 0, instance);
    }
    irq = riscv_interrupt_save();
    pair->creating = 0;
    if (result) {
        pair->master_closed = pair->hangup_pending = 1;
        if (pair->entry) kernel_devpts_unpublish(pair->entry);
    } else *device = kernel_tty_device_template();
    pair_put(pair);
    riscv_interrupt_restore(irq);
    return result;
}
int kernel_pty_open(const struct kernel_vfs_file *file, struct kernel_heap *heap,
    struct kernel_task *caller, uint32_t flags, void **instance,
    const struct kernel_char_device **device)
{
    if (!file || !heap || !instance || !device) return -KERNEL_EINVAL;
    struct kernel_vfs_mount *mount = kernel_devpts_file_mount(file);
    if (!mount) return -KERNEL_ENODEV;
    if (kernel_devpts_file_is_ptmx(file))
        return kernel_pty_open_ptmx(mount, heap, caller, flags, instance, device);
    struct kernel_devpts_entry *entry = kernel_devpts_file_entry(file);
    if (!entry) return -KERNEL_ENODEV;
    uintptr_t irq = riscv_interrupt_save();
    struct pty_pair *pair = kernel_devpts_entry_binding(entry);
    if (!pair || pair->creating || pair->master_closed || pair->slave_locked) {
        riscv_interrupt_restore(irq);
        return -KERNEL_EIO;
    }
    pair_get(pair);
    riscv_interrupt_restore(irq);
    int result = kernel_tty_open(pair->ends[PTY_SLAVE].tty, heap, caller, flags, 1, instance);
    irq = riscv_interrupt_save();
    if (!result) *device = kernel_tty_device_template();
    pair_put(pair);
    riscv_interrupt_restore(irq);
    return result;
}
