#define main tty_reference_test
#include "../tty/core_host.c"
#undef main
#include "../../fs/pty_internal.h"
#include "../../fs/vfs_objects.h"
#include <kernel/devpts.h>

/* 模型只替代VFS身份与调度；传输和行规程执行生产代码。 */
struct kernel_devpts_entry {
    struct kernel_vfs_mount *mount;
    void *binding;
    unsigned number, references, reserved;
};
struct model_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_devpts_entry *entries[64];
    struct kernel_vfs_path root;
    void *pty;
    void (*worker)(void *);
    void *worker_argument;
};
static struct model_mount model;
static jmp_buf worker_sleep;
static unsigned worker_yields, worker_blocks;
static int publish_error;

int kernel_devpts_is_mount(const struct kernel_vfs_mount *m)
{ return m && m->private_data == &((struct model_mount *)m)->instance; }
void *kernel_devpts_mount_private(const struct kernel_vfs_mount *m)
{ return ((struct model_mount *)m)->pty; }
void kernel_devpts_mount_set_private(struct kernel_vfs_mount *m, void *p)
{ ((struct model_mount *)m)->pty = p; }
int kernel_devpts_mount_root(struct kernel_vfs_mount *m, struct kernel_vfs_path **p)
{ *p = &((struct model_mount *)m)->root; (*p)->references++; return 0; }
int kernel_devpts_publish(struct kernel_vfs_mount *m, void *p,
    struct kernel_devpts_entry **owner)
{
    struct model_mount *v = (void *)m;
    if (publish_error) return publish_error;
    unsigned n = 0;
    while (n < 32 && v->entries[n]) n++;
    if (n == 32) return -KERNEL_ENOSPC;
    assert(kernel_heap_allocate_zeroed(&heap, 1, sizeof(**owner), (void **)owner) == 0);
    **owner = (struct kernel_devpts_entry){m, p, n, 1, 1};
    v->entries[n] = *owner;
    return 0;
}
int kernel_devpts_entry_acquire(struct kernel_devpts_entry *e)
{ assert(e->references); e->references++; return 0; }
void kernel_devpts_entry_release(struct kernel_devpts_entry **e)
{
    assert((*e)->references);
    if (!--(*e)->references) assert(kernel_heap_release(&heap, *e) == 0);
    *e = 0;
}
void kernel_devpts_unpublish(struct kernel_devpts_entry *e) { e->binding = 0; }
void kernel_devpts_retire(struct kernel_devpts_entry *e)
{
    struct model_mount *m = (void *)e->mount;
    kernel_devpts_unpublish(e);
    assert(e->reserved && m->entries[e->number] == e);
    m->entries[e->number] = 0;
    e->reserved = 0;
}
uint32_t kernel_devpts_entry_number(const struct kernel_devpts_entry *e) { return e->number; }
void *kernel_devpts_entry_binding(const struct kernel_devpts_entry *e) { return e->binding; }
struct kernel_vfs_mount *kernel_devpts_file_mount(const struct kernel_vfs_file *f)
{ return f->mount; }
struct kernel_devpts_entry *kernel_devpts_file_entry(const struct kernel_vfs_file *f)
{ return f->private_data; }
int kernel_devpts_file_is_ptmx(const struct kernel_vfs_file *f) { return !f->private_data; }
int kernel_vfs_path_acquire(struct kernel_vfs_path *p) { p->references++; return 0; }
int kernel_vfs_path_lookup(struct kernel_vfs_path *p, const char *name, size_t n,
    struct kernel_vfs_path **out)
{
    unsigned number = 0;
    for (size_t i = 0; i < n; i++) number = number * 10 + (unsigned)(name[i] - '0');
    struct model_mount *m = (void *)p->file.mount;
    assert(number < 32 && m->entries[number]);
    assert(kernel_heap_allocate_zeroed(&heap, 1, sizeof(**out), (void **)out) == 0);
    (*out)->references = 1;
    (*out)->file.mount = p->file.mount;
    (*out)->file.private_data = m->entries[number];
    assert(kernel_devpts_entry_acquire(m->entries[number]) == 0);
    return 0;
}
int kernel_vfs_path_release(struct kernel_vfs_path **p)
{
    assert((*p)->references);
    if (!--(*p)->references) {
        struct kernel_devpts_entry *e = (*p)->file.private_data;
        kernel_devpts_entry_release(&e);
        assert(kernel_heap_release(&heap, *p) == 0);
    }
    *p = 0;
    return 0;
}
enum kernel_scheduler_status kernel_thread_create_joinable(void (*fn)(void *),
    void *arg, struct kernel_thread_join *join)
{
    struct model_mount *m = &model;
    assert(!m->worker);
    m->worker = fn;
    m->worker_argument = arg;
    join->task = (void *)m;
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_thread_join(struct kernel_thread_join *j)
{
    struct model_mount *m = (void *)j->task;
    m->worker(m->worker_argument);
    m->worker = 0;
    j->task = 0;
}
enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{ worker_yields++; return KERNEL_SCHEDULER_STATUS_OK; }
static void sleep_worker(void) { worker_blocks++; longjmp(worker_sleep, 1); }
static void run_worker(void)
{
    sleep_action = sleep_worker;
    if (!setjmp(worker_sleep)) model.worker(model.worker_argument);
    sleep_action = 0;
}
/* Root提供真实fd安装；此模型只检查传入的是已钉住的绑定路径。 */
int kernel_files_open_pty_peer(struct kernel_files *f, struct kernel_task *t,
    struct kernel_vfs_path *p, uint32_t flags)
{ (void)f; (void)t; (void)flags; assert(p && p->references); return 17; }
static void initialize(void)
{
    memset(&model, 0, sizeof(model));
    model.mount.private_data = &model.instance;
    model.instance.heap = &heap;
    model.root.references = 1;
    model.root.file.mount = &model.mount;
    assert(kernel_pty_mount_start(&model.mount) == 0);
}
static int pty_ioctl(const struct kernel_char_device *d, void *i, uint64_t cmd, void *p)
{ return d->ioctl(i, &task, 0, 0, 0, cmd, (uintptr_t)p); }
static void raw_pty(const struct kernel_char_device *d, void *i)
{
    struct kernel_tty_termios s;
    assert(pty_ioctl(d, i, 0x5401, &s) == 0);
    s.iflag = s.oflag = s.lflag = 0;
    s.cc[6] = 1; s.cc[5] = 0;
    assert(pty_ioctl(d, i, 0x5402, &s) == 0);
}
static void transport_contract(void)
{
    void *master = 0, *slave = 0;
    const struct kernel_char_device *md = 0, *sd = 0;
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &master, &md) == 0);
    unsigned number = UINT32_MAX;
    assert(pty_ioctl(md, master, 0x80045430U, &number) == 0 && number == 0);
    struct kernel_vfs_file file = {.mount = &model.mount, .private_data = model.entries[number]};
    assert(kernel_pty_open(&file, &heap, &task, 2, &slave, &sd) == -KERNEL_EIO);
    int locked = 0;
    assert(pty_ioctl(md, master, 0x40045431U, &locked) == 0);
    assert(kernel_pty_open(&file, &heap, &task, 2, &slave, &sd) == 0);
    raw_pty(sd, slave);
    size_t n;
    assert(md->write(master, &task, 04000, "input", 5, &n) == 0 && n == 5);
    run_worker();
    char bytes[16];
    assert(sd->read(slave, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 5 && !memcmp(bytes, "input", 5));
    assert(sd->write(slave, &task, 04000, "tail", 4, &n) == 0 && n == 4);
    sd->release(slave); slave = 0;
    /* 接受的末尾必须在HUP/EIO前交付，读/poll也能做有界推进。 */
    assert(md->poll(master, 0, KERNEL_POLLIN, 0) & KERNEL_POLLIN);
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 4 && !memcmp(bytes, "tail", 4));
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == -KERNEL_EIO && n == 0);
    assert(kernel_pty_open(&file, &heap, &task, 2, &slave, &sd) == 0);
    assert(sd->write(slave, &task, 04000, "again", 5, &n) == 0 && n == 5);
    run_worker();
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 5 && !memcmp(bytes, "again", 5));
    md->release(master); master = 0;
    assert(!kernel_devpts_entry_binding(file.private_data));
    assert(kernel_pty_mount_stop(&model.mount) == -KERNEL_EBUSY);
    run_worker();
    assert(sd->read(slave, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 0);
    sd->release(slave);
    run_worker();
    assert(!model.entries[number]);
    assert(kernel_pty_mount_stop(&model.mount) == 0 && !live);
}
static void pressure_and_fairness(void)
{
    void *masters[2] = {0}, *slaves[2] = {0};
    const struct kernel_char_device *md[2] = {0}, *sd[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &masters[i], &md[i]) == 0);
        int unlocked = 0;
        assert(pty_ioctl(md[i], masters[i], 0x40045431U, &unlocked) == 0);
        struct kernel_vfs_file f = {.mount = &model.mount, .private_data = model.entries[i]};
        assert(kernel_pty_open(&f, &heap, &task, 2, &slaves[i], &sd[i]) == 0);
        raw_pty(sd[i], slaves[i]);
    }
    unsigned char payload[16384];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)(i * 37U + i / 255U);
    size_t written = 0;
    while (written < sizeof(payload)) {
        size_t n = 0;
        int r = md[0]->write(masters[0], &task, 04000, payload + written, sizeof(payload) - written, &n);
        if (r == -KERNEL_EAGAIN) break;
        assert(!r && n);
        written += n;
        run_worker();
    }
    assert(written > 4096 && written < sizeof(payload));
    /* 一个raw接收端已满时，另一对必须能在同一worker继续前进。 */
    size_t n = 0;
    assert(md[1]->write(masters[1], &task, 04000, "fair", 4, &n) == 0 && n == 4);
    run_worker();
    char small[8];
    assert(sd[1]->read(slaves[1], &task, 04000, small, sizeof(small), &n) == 0 && n == 4 && !memcmp(small, "fair", 4));
    unsigned char received[16384];
    size_t read = 0;
    while (read < written) {
        n = 0;
        int r = sd[0]->read(slaves[0], &task, 04000, received + read, sizeof(received) - read, &n);
        assert(!r && n);
        read += n;
        run_worker();
    }
    assert(read == written && !memcmp(received, payload, read));
    for (unsigned i = 0; i < 2; i++) { md[i]->release(masters[i]); sd[i]->release(slaves[i]); }
    run_worker();
    assert(kernel_pty_mount_stop(&model.mount) == 0 && !live);
}
static void rollback_and_stable_binding(void)
{
    void *master = 0, *slave = 0;
    const struct kernel_char_device *md = 0, *sd = 0;
    publish_error = -KERNEL_ENOMEM;
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &master, &md) == -KERNEL_ENOMEM && !master);
    publish_error = 0;
    run_worker();
    assert(model.root.references == 1 && live == 1);
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &master, &md) == 0);
    struct kernel_devpts_entry *old = model.entries[0];
    assert(kernel_devpts_entry_acquire(old) == 0);
    struct kernel_vfs_file old_file = {.mount = &model.mount, .private_data = old};
    int unlocked = 0;
    assert(pty_ioctl(md, master, 0x40045431U, &unlocked) == 0);
    task.leader = 1;
    assert(kernel_pty_open(&old_file, &heap, &task, 2, &slave, &sd) == 0 && task.tty);
    md->release(master); master = 0;
    run_worker();
    assert(!task.tty && identity.references == 1);
    sd->release(slave); slave = 0;
    task.leader = 0;
    run_worker();
    assert(!old->binding && !old->reserved && !model.entries[0]);
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &master, &md) == 0);
    assert(model.entries[0] != old);
    assert(kernel_pty_open(&old_file, &heap, &task, 2, &slave, &sd) == -KERNEL_EIO && !slave);
    kernel_devpts_entry_release(&old);
    md->release(master);
    run_worker();
    assert(kernel_pty_mount_stop(&model.mount) == 0 && !live);
}
static void packet_contract(void)
{
    void *master = 0, *slave = 0;
    const struct kernel_char_device *md = 0, *sd = 0;
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &master, &md) == 0);
    int value = 0;
    assert(pty_ioctl(md, master, 0x40045431U, &value) == 0);
    struct kernel_vfs_file f = {.mount = &model.mount, .private_data = model.entries[0]};
    assert(kernel_pty_open(&f, &heap, &task, 2, &slave, &sd) == 0);
    raw_pty(sd, slave);
    value = 1;
    assert(pty_ioctl(md, master, 0x5420, &value) == 0);
    size_t n;
    unsigned char bytes[16];
    assert(sd->write(slave, &task, 04000, "pkt", 3, &n) == 0 && n == 3);
    assert(md->read(master, &task, 04000, bytes, 1, &n) == 0 && n == 1 && bytes[0] == 0);
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 4 && bytes[0] == 0 && !memcmp(bytes + 1, "pkt", 3));
    assert(sd->ioctl(slave, &task, 0, 0, 0, 0x540a, 0) == 0);
    assert(sd->ioctl(slave, &task, 0, 0, 0, 0x540a, 1) == 0);
    assert(md->poll(master, 0, KERNEL_POLLPRI, 0) & KERNEL_POLLPRI);
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 1 && bytes[0] == 8);
    struct kernel_tty_termios s;
    assert(pty_ioctl(sd, slave, 0x5401, &s) == 0);
    s.iflag |= 1024U;
    assert(pty_ioctl(sd, slave, 0x5402, &s) == 0);
    s.iflag &= ~1024U;
    assert(pty_ioctl(sd, slave, 0x5402, &s) == 0);
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 1 && bytes[0] == 16);
    assert(sd->ioctl(slave, &task, 0, 0, 0, 0x540b, 2) == 0);
    assert(md->read(master, &task, 04000, bytes, sizeof(bytes), &n) == 0 && n == 1 && bytes[0] == 3);
    md->release(master); sd->release(slave);
    run_worker();
    assert(kernel_pty_mount_stop(&model.mount) == 0 && !live);
}
static void capacity_and_cleanup(void)
{
    void *masters[32] = {0};
    const struct kernel_char_device *devices[32] = {0};
    fail_alloc = 1;
    void *failed = 0;
    const struct kernel_char_device *unused = 0;
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &failed, &unused) == -KERNEL_ENOMEM && !failed);
    for (unsigned i = 0; i < 32; i++) {
        assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &masters[i], &devices[i]) == 0);
        unsigned number = UINT32_MAX;
        assert(pty_ioctl(devices[i], masters[i], 0x80045430U, &number) == 0 && number == i);
    }
    assert(kernel_pty_open_ptmx(&model.mount, &heap, &task, 2, &failed, &unused) == -KERNEL_ENOSPC && !failed);
    run_worker();
    for (unsigned i = 0; i < 32; i++) devices[i]->release(masters[i]);
    run_worker();
    for (unsigned i = 0; i < 32; i++) assert(!model.entries[i]);
    assert(kernel_pty_mount_stop(&model.mount) == 0 && !live);
}
int main(void)
{
    initialize();
    transport_contract();
    initialize();
    pressure_and_fairness();
    initialize();
    rollback_and_stable_binding();
    initialize();
    capacity_and_cleanup();
    initialize();
    packet_contract();
    assert(worker_blocks && worker_yields);
    puts("pty transport host: PASS");
    return 0;
}
