#include "../../fs/char_device_internal.h"
#include "../../fs/open_file_internal.h"
#include <assert.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/tty_task.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct kernel_task {
    struct kernel_tty *tty;
    struct kernel_tty_request *request;
    int leader;
};
static struct kernel_task task, other;
static struct kernel_heap heap;
static struct kernel_tty *tty;
static const struct kernel_char_device *device;
static void *instance;
static unsigned live, fail_alloc, closes, kicks, signals[65];
static uint64_t now_ns, fault_address;
static unsigned char output[65536];
static size_t output_size, credit = 65536;
static struct kernel_pid identity = {.number = 7, .references = 1};
static void (*sleep_action)(void);
static unsigned sleeps, pending, restart_notes;
static jmp_buf cancelled;
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h, size_t c, size_t n,
                                                    void **p)
{
    (void)h;
    if (fail_alloc) {
        fail_alloc--;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    *p = calloc(c, n);
    if (!*p)
        return KERNEL_HEAP_STATUS_EMPTY;
    live++;
    return 0;
}
enum kernel_heap_status kernel_heap_release(struct kernel_heap *h, void *p)
{
    (void)h;
    if (p) {
        assert(live);
        live--;
        free(p);
    }
    return 0;
}
void kernel_pid_get(struct kernel_pid *id)
{
    assert(id->references);
    id->references++;
}
void kernel_pid_put(struct kernel_pid *id)
{
    assert(id->references > 1);
    id->references--;
}
struct kernel_tty *kernel_task_controlling_tty(const struct kernel_task *t)
{
    return t ? t->tty : 0;
}
int kernel_task_tty_set(struct kernel_task *t, struct kernel_tty *v)
{
    assert(!t->tty);
    t->tty = v;
    kernel_tty_get(v);
    return 0;
}
void kernel_task_tty_clear(struct kernel_task *t)
{
    if (t && t->tty) {
        struct kernel_tty *v = t->tty;
        t->tty = 0;
        kernel_tty_put(v);
    }
}
void kernel_task_tty_clear_session(struct kernel_tty *v, struct kernel_pid *sid)
{
    (void)sid;
    if (task.tty == v)
        kernel_task_tty_clear(&task);
    if (other.tty == v)
        kernel_task_tty_clear(&other);
}
struct kernel_pid *kernel_task_tty_identity(const struct kernel_task *t,
                                            enum kernel_pid_role r)
{
    (void)r;
    return t ? &identity : 0;
}
struct kernel_pid *kernel_task_tty_find_group(struct kernel_task *t, kernel_pid_t n)
{
    (void)t;
    return n == 7 ? &identity : 0;
}
int kernel_task_tty_session_leader(const struct kernel_task *t)
{
    return t && t->leader;
}
int kernel_task_tty_signal_ignored(const struct kernel_task *t, unsigned s)
{
    (void)t;
    (void)s;
    return 0;
}
int kernel_task_tty_group_orphaned(const struct kernel_task *t)
{
    (void)t;
    return 0;
}
void kernel_task_tty_signal_group(struct kernel_pid *g, unsigned s)
{
    if (g)
        signals[s]++;
}
void kernel_task_tty_signal_session(struct kernel_tty *v, struct kernel_pid *sid)
{
    kernel_task_tty_clear_session(v, sid);
}
void kernel_task_tty_request_set(struct kernel_task *t, struct kernel_tty_request *r)
{
    if (t) {
        assert(!t->request);
        t->request = r;
    }
}
void kernel_task_tty_request_clear(struct kernel_task *t, struct kernel_tty_request *r)
{
    if (t) {
        assert(t->request == r);
        t->request = 0;
    }
}
int kernel_signal_has_pending(const struct kernel_task *t)
{
    (void)t;
    return (int)pending;
}
void kernel_signal_note_syscall_restart(struct kernel_task *t)
{
    (void)t;
    restart_notes++;
}
void kernel_wait_queue_init(struct kernel_wait_queue *q)
{
    memset(q, 0, sizeof(*q));
    q->initialized = 1;
}
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{
    assert(q->initialized);
    return 0;
}
enum kernel_scheduler_status
kernel_scheduler_block_current(struct kernel_wait_queue *q, uint64_t deadline,
                               int interruptible, enum kernel_wait_wake_reason *reason)
{
    assert(q->initialized && interruptible);
    sleeps++;
    if (sleep_action) {
        void (*action)(void) = sleep_action;
        sleep_action = 0;
        action();
        *reason = KERNEL_WAIT_WOKEN;
    } else {
        assert(deadline);
        now_ns = deadline;
        *reason = KERNEL_WAIT_TIMEOUT;
    }
    return 0;
}
uint64_t kernel_time_monotonic_ns(void)
{
    return now_ns;
}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t n, uint64_t *d)
{
    if (n <= now_ns)
        return KERNEL_TIME_STATUS_DEADLINE_PASSED;
    *d = n;
    return 0;
}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m, uint64_t dest,
                                               const void *src, size_t n, size_t *copied)
{
    (void)m;
    size_t valid = n;
    if (fault_address && dest + n > fault_address)
        valid = dest >= fault_address ? 0 : (size_t)(fault_address - dest);
    memcpy((void *)(uintptr_t)dest, src, valid);
    *copied = valid;
    return valid == n ? 0 : KERNEL_UACCESS_STATUS_FAULT;
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m, void *dst, uint64_t src,
                                                 size_t n, size_t *c)
{
    (void)m;
    memcpy(dst, (void *)(uintptr_t)src, n);
    *c = n;
    return 0;
}
enum kernel_open_file_status
kernel_open_file_release(struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *f = *owner;
    assert(f->references);
    f->references--;
    if (!f->references) {
        f->device->release(f->device_instance);
        f->device_instance = 0;
    }
    *owner = 0;
    return 0;
}
void kernel_files_queue_description(struct kernel_files *f,
                                    struct kernel_open_file_description *d)
{
    (void)f;
    (void)d;
    assert(0);
}
static size_t transmit(void *o, const unsigned char *p, size_t n)
{
    (void)o;
    if (n > credit)
        n = credit;
    assert(output_size + n <= sizeof(output));
    memcpy(output + output_size, p, n);
    output_size += n;
    credit -= n;
    return n;
}
static int drained(void *o)
{
    (void)o;
    return 1;
}
static int configure(void *o, struct kernel_tty_termios *s)
{
    (void)o;
    s->cflag &= ~0xd00f0000U;
    return 0;
}
static void kick(void *o)
{
    (void)o;
    kicks++;
}
static void last_close(void *o, uint32_t flags)
{
    (void)o;
    (void)flags;
    closes++;
}
static const struct kernel_tty_transport transport = {transmit, drained, configure, kick,
                                                      last_close};
static int ioctl_call(unsigned command, void *data)
{
    return device->ioctl(instance, &task, 0, 0, 0, command, (uintptr_t)data);
}
static void termios(struct kernel_tty_termios *s)
{
    assert(ioctl_call(0x5401, s) == 0);
}
static void settings(struct kernel_tty_termios *s)
{
    assert(ioctl_call(0x5402, s) == 0);
}
static void feed(const unsigned char *s, size_t n)
{
    for (size_t j = 0; j < n; j++) {
        struct kernel_tty_rx c = {s[j], 0};
        kernel_tty_receive(tty, &c, 1);
    }
}
static int64_t read_into(unsigned char *buf, size_t n, unsigned flags)
{
    struct kernel_uaccess_iovec iov = {(uintptr_t)buf, n};
    int64_t r;
    assert(device->readv(instance, &task, 0, 0, 0, 0, &iov, 1, n, flags, &r) == 0);
    return r;
}
static void flush(void)
{
    assert(device->ioctl(instance, &task, 0, 0, 0, 0x540b, 2) == 0);
}
static void raw(unsigned min, unsigned time)
{
    struct kernel_tty_termios s;
    termios(&s);
    s.iflag = 0;
    s.oflag = 0;
    s.lflag = 0;
    s.cc[6] = (unsigned char)min;
    s.cc[5] = (unsigned char)time;
    settings(&s);
    flush();
}
static void injected(void)
{
    const unsigned char c = 'Q';
    feed(&c, 1);
}
static void change_raw_sleep(void)
{
    struct kernel_tty_termios s;
    assert(device->ioctl(instance, &other, 0, 0, 0, 0x5401, (uintptr_t)&s) == 0);
    s.lflag = 0;
    s.cc[6] = 1;
    s.cc[5] = 0;
    assert(device->ioctl(instance, &other, 0, 0, 0, 0x5402, (uintptr_t)&s) == 0);
    sleep_action = injected;
}
static void interrupt_sleep(void)
{
    pending = 1;
}
static void cancel_sleep(void)
{
    assert(task.request);
    kernel_tty_abort_request(task.request);
    longjmp(cancelled, 1);
}
int main(void)
{
    fail_alloc = 1;
    assert(kernel_tty_create(&heap, &transport, 0, &tty) == -KERNEL_ENOMEM && !tty && !live);
    assert(kernel_tty_create(&heap, &transport, 0, &tty) == 0);
    kernel_tty_publish_serial(tty);
    device = kernel_tty_device_lookup(0x440);
    assert(device);
    fail_alloc = 1;
    assert(device->open(&heap, &task, 0, &instance) == -KERNEL_ENOMEM && !instance);
    assert(device->open(&heap, &task, 0, &instance) == 0);
    struct kernel_tty_termios s;
    termios(&s);
    assert(sizeof(s) == 36 && s.iflag == 0x500 && s.oflag == 5 && s.cflag == 0x10b2 &&
           s.lflag == 0x8a3b);
    const struct kernel_char_device *current = kernel_tty_device_lookup(0x500);
    void *bad = 0;
    assert(current->open(&heap, &task, 0, &bad) == -KERNEL_ENXIO);
    s.lflag &= ~8U;
    settings(&s);
    unsigned char buf[512];
    feed((const unsigned char *)"ab\177c\n", 5);
    assert(read_into(buf, 128, 0) == 3 && !memcmp(buf, "ac\n", 3));
    for (unsigned n = 63; n <= 65; n++) {
        unsigned char line[67];
        memset(line, 'A', n);
        line[n] = '\n';
        feed(line, n + 1);
        assert(read_into(buf, 128, 0) == (int64_t)n + 1);
        for (unsigned j = 0; j < n; j++)
            assert(buf[j] == 'A');
        assert(buf[n] == '\n');
    }
    unsigned char eof[65];
    memset(eof, 'A', 64);
    eof[64] = 4;
    feed(eof, 65);
    assert(read_into(buf, 64, 0) == 64);
    feed((const unsigned char *)"X\n", 2);
    assert(read_into(buf, 128, 0) == 2 && !memcmp(buf, "X\n", 2));
    const unsigned char empty_eof = 4;
    feed(&empty_eof, 1);
    assert(read_into(buf, 128, 0) == 0);
    for (unsigned prefix = 1; prefix <= 63; prefix = prefix == 1 ? 8 : prefix == 8 ? 63 : 64) {
        unsigned char line[101];
        memset(line, 'F', 100);
        line[100] = '\n';
        feed(line, 101);
        fault_address = (uintptr_t)buf + prefix;
        assert(read_into(buf, 128, 0) == prefix);
        fault_address = 0;
        assert(read_into(buf, 128, 0) == 37);
        assert(buf[36] == '\n');
    }
    termios(&s);
    s.lflag = 2;
    s.cc[6] = 1;
    settings(&s);
    flush();
    sleeps = 0;
    sleep_action = change_raw_sleep;
    assert(read_into(buf, 128, 0) == 1 && buf[0] == 'Q' && sleeps == 2);
    raw(65, 0);
    unsigned char sixtyfour[64];
    memset(sixtyfour, 'P', sizeof(sixtyfour));
    feed(sixtyfour, sizeof(sixtyfour));
    assert(!(device->poll(instance, 0, 0, 0) & KERNEL_POLLIN));
    assert(read_into(buf, 128, 0) == 64);
    raw(1, 0);
    feed((const unsigned char *)"A", 1);
    assert(device->poll(instance, 0, 0, 0) & KERNEL_POLLIN);
    flush();
    raw(0, 1);
    assert(!(device->poll(instance, 0, 0, 0) & KERNEL_POLLIN));
    feed((const unsigned char *)"A", 1);
    assert(device->poll(instance, 0, 0, 0) & KERNEL_POLLIN);
    flush();
    raw(65, 0);
    unsigned char many[100];
    memset(many, 'M', 100);
    feed(many, 100);
    assert(read_into(buf, 128, 0) == 64);
    raw(0, 0);
    assert(read_into(buf, 128, 0) == 0);
    raw(0, 1);
    now_ns = 0;
    sleeps = 0;
    assert(read_into(buf, 128, 0) == 0 && now_ns == 100000000 && sleeps == 1);
    raw(2, 1);
    feed((const unsigned char *)"A", 1);
    now_ns = 0;
    sleeps = 0;
    assert(read_into(buf, 128, 0) == 1 && now_ns == 100000000 && sleeps == 1);
    raw(1, 0);
    sleep_action = injected;
    sleeps = 0;
    assert(read_into(buf, 128, 0) == 1 && buf[0] == 'Q' && sleeps == 1);
    assert(read_into(buf, 1, KERNEL_FILES_O_NONBLOCK) == -KERNEL_EAGAIN);
    raw(5, 0);
    feed((const unsigned char *)"A", 1);
    sleep_action = interrupt_sleep;
    assert(read_into(buf, 128, 0) == 1 && buf[0] == 'A' && !restart_notes);
    pending = 0;
    raw(1, 0);
    /* Abandon a blocked live syscall stack: vectors/pin/locks must all be consumed. */
    struct kernel_files files = {.heap = &heap};
    static struct kernel_open_file_description file;
    file = (struct kernel_open_file_description){
        .references = 2, .device = device, .device_instance = instance};
    static struct kernel_open_file_description *pin;
    pin = &file;
    static struct kernel_uaccess_iovec *vectors;
    vectors = 0;
    assert(kernel_heap_allocate_zeroed(&heap, 9, sizeof(*vectors), (void **)&vectors) == 0);
    vectors[0] = (struct kernel_uaccess_iovec){(uintptr_t)buf, 1};
    sleep_action = cancel_sleep;
    if (!setjmp(cancelled)) {
        int64_t result;
        device->readv(instance, &task, &files, &pin, (void **)&vectors, 0, vectors, 9, 1, 0,
                      &result);
        assert(0);
    }
    assert(!task.request && !pin && !vectors && file.references == 1 && live == 2);
    feed((const unsigned char *)"Z", 1);
    assert(read_into(buf, 1, 0) == 1 && buf[0] == 'Z');
    /* OPOST queue ordering, flow stop/start, priority xchar and real drain. */
    termios(&s);
    s.oflag = 5;
    settings(&s);
    size_t written;
    assert(device->write(instance, &task, 0, "a\nb", 3, &written) == 0 && written == 3);
    assert(device->ioctl(instance, &task, 0, 0, 0, 0x540a, 0) == 0);
    assert(kernel_tty_service_output(tty, 100) == 0);
    assert(device->ioctl(instance, &task, 0, 0, 0, 0x540a, 2) == 0);
    assert(kernel_tty_service_output(tty, 100) == 1 && output[0] == 19);
    assert(device->ioctl(instance, &task, 0, 0, 0, 0x540a, 1) == 0);
    assert(kernel_tty_service_output(tty, 100) == 4 && !memcmp(output + 1, "a\r\nb", 4));
    task.leader = 1;
    assert(device->ioctl(instance, &task, 0, 0, 0, 0x540e, 0) == 0 && task.tty == tty &&
           kernel_tty_foreground(tty) == 7);
    int32_t group;
    assert(ioctl_call(0x540f, &group) == 0 && group == 7);
    uint16_t winsize[4] = {24, 80, 0, 0};
    assert(ioctl_call(0x5414, winsize) == 0 && signals[28] == 1);
    memset(winsize, 0, sizeof(winsize));
    assert(ioctl_call(0x5413, winsize) == 0 && winsize[0] == 24 && winsize[1] == 80);
    assert(kernel_tty_destroy(&tty) == -KERNEL_EBUSY);
    assert(device->write(instance, &task, 0, "D", 1, &written) == 0);
    kernel_tty_shutdown(tty);
    assert(read_into(buf, 1, 0) == 0);
    assert(device->poll(instance, 0, 0, 0) & KERNEL_POLLHUP);
    assert(kernel_tty_service_output(tty, 100) == 1);
    assert(device->write(instance, &task, 0, "x", 1, &written) == -KERNEL_EIO);
    pin = &file;
    assert(kernel_open_file_release(&pin) == 0 && closes == 1);
    assert(kernel_tty_destroy(&tty) == 0 && !tty && !live && identity.references == 1);
    puts("TTY core host: real "
         "termios/rings/continuation/fault/timeouts/flow/cancellation/ownership pass");
}
