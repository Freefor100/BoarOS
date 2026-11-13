#define main serial_contract_main
#include "core.c"
#undef main
#include "../../../fs/uaccess_iov_internal.h"

static int external_refs;
static void refs(void *owner, int delta) { (void)owner; external_refs += delta; }
static unsigned async_events[7], invalidations;
static void async_event(struct kernel_wait_node *node, uint32_t reason, uint64_t event)
{
    (void)node;
    assert(reason == KERNEL_WAIT_WOKEN);
    if (event == KERNEL_ASYNC_INVALIDATE) invalidations++;
    else { assert(event > 0 && event < 7); async_events[event]++; }
}
static void hangup_contract(void)
{
    credentials.effective = 0;
    assert(kernel_tty_vhangup(&task) == -KERNEL_EPERM);
    credentials.effective = UINT64_C(1) << KERNEL_CAP_SYS_ADMIN;
    assert(kernel_tty_vhangup(&task) == -KERNEL_EPERM);
    credentials.effective = UINT64_C(1) << KERNEL_CAP_SYS_TTY_CONFIG;
    assert(!kernel_tty_vhangup(&task)); /* Privileged caller need not have a ctty. */
    assert(!kernel_tty_create(&heap, &transport, 0, &tty));
    kernel_tty_publish_serial(tty);
    device = kernel_tty_device_lookup(0x440);
    task.leader = 0;
    assert(!device->open(&heap, &task, 0400, 0, &instance));
    void *console = 0;
    const struct kernel_char_device *console_device = kernel_tty_device_lookup(0x501);
    assert(!console_device->open(&heap, &task, 0, 0, &console));
    struct kernel_wait_queue *source = device->async_source(instance), *poll_source = 0;
    device->poll(instance, 0, 0, &poll_source);
    assert(source && source != poll_source);
    struct kernel_wait_node observer = {.callback = async_event};
    source->head = source->tail = &observer;
    int type = -1;
    struct kernel_pid *owner = device->async_owner(instance, &task, &type);
    assert(owner == &identity && type == 1 && identity.references == 2 && last_identity_role == KERNEL_PID_TID);
    kernel_task_identity_put(owner);
    struct kernel_tty_termios settings_value;
    termios(&settings_value);
    settings_value.lflag &= ~8U;
    settings(&settings_value);
    feed((const unsigned char *)"a", 1);
    assert(!async_events[KERNEL_ASYNC_POLL_IN]);
    feed((const unsigned char *)"\n", 1);
    assert(async_events[KERNEL_ASYNC_POLL_IN] == 1);
    unsigned char bytes[8];
    assert(read_into(bytes, sizeof(bytes), 0) == 2);
    flush();
    assert(async_events[KERNEL_ASYNC_POLL_IN] == 1);
    feed((const unsigned char *)"\4", 1);
    assert(async_events[KERNEL_ASYNC_POLL_IN] == 2 && !read_into(bytes, sizeof(bytes), 0));
    raw(65, 0);
    feed((const unsigned char *)"Q", 1);
    assert(async_events[KERNEL_ASYNC_POLL_IN] == 3);
    assert(!(device->poll(instance, 0, 0, 0) & KERNEL_POLLIN));
    termios(&settings_value);
    settings_value.lflag = 1; /* ISIG flush must not publish an input event. */
    settings(&settings_value);
    feed((const unsigned char *)"\3", 1);
    assert(async_events[KERNEL_ASYNC_POLL_IN] == 3);
    kernel_tty_transport_ready(tty);
    kernel_tty_transport_ready(tty);
    assert(!async_events[KERNEL_ASYNC_POLL_OUT]);
    flush();
    unsigned char output_block[256]; memset(output_block, 'W', sizeof(output_block));
    size_t n;
    credit = 65536;
    assert(!device->write(instance, &task, 0, output_block, sizeof(output_block), &n) && n == sizeof(output_block));
    assert(!device->ioctl(instance, &task, 0, 0, 0, 0x540a, 0));
    assert(!device->ioctl(instance, &task, 0, 0, 0, 0x540a, 1));
    assert(!async_events[KERNEL_ASYNC_POLL_OUT]);
    assert(kernel_tty_service_output(tty, 1) == 1 && async_events[KERNEL_ASYNC_POLL_OUT] == 1);
    assert(kernel_tty_service_output(tty, 1024) == 255 && async_events[KERNEL_ASYNC_POLL_OUT] == 1);
    task.leader = 1;
    assert(!device->ioctl(instance, &task, 0, 0, 0, 0x540e, 0));
    assert(!kernel_task_tty_set(&other, tty));
    owner = device->async_owner(instance, &task, &type);
    assert(owner == &identity && type == 2);
    kernel_task_identity_put(owner);
    unsigned foreground_hups = signals[1], old_session_signals = session_signals;
    assert(!kernel_tty_vhangup(&task));
    assert(!task.tty && !other.tty && kernel_tty_foreground(tty) == -1);
    assert(signals[1] == foreground_hups && session_signals == old_session_signals + 1);
    assert(invalidations == 1 && !device->async_source(instance));
    assert(console_device->async_source(console) == source);
    source->head = source->tail = 0;
    assert(!read_into(bytes, sizeof(bytes), 0));
    assert(device->write(instance, &task, 0, "X", 1, &n) == -KERNEL_EIO && !n);
    assert(ioctl_call(0x5401, &settings_value) == -KERNEL_EIO);
    assert(ioctl_call(0x5410, (void *)1) == -KERNEL_ENOTTY);
    uint32_t mask = device->poll(instance, 0, 0, 0);
    assert(mask == (KERNEL_POLLIN | KERNEL_POLLRDNORM | KERNEL_POLLOUT | KERNEL_POLLWRNORM | KERNEL_POLLERR | KERNEL_POLLHUP));
    assert(!console_device->ioctl(console, &task, 0, 0, 0, 0x5401, (uintptr_t)&settings_value));
    void *reopened = 0;
    assert(!device->open(&heap, &task, 0400, 0, &reopened));
    assert(device->async_source(reopened) == source);
    assert(!(device->poll(reopened, 0, 0, 0) & (KERNEL_POLLERR | KERNEL_POLLHUP)));
    feed((const unsigned char *)"R", 1);
    assert(!read_into(bytes, sizeof(bytes), 0));
    assert(!device->read(reopened, &task, KERNEL_FILES_O_NONBLOCK, bytes, 1, &n) && n == 1 && bytes[0] == 'R');
    device->release(instance);
    device->release(reopened);
    console_device->release(console);
    kernel_tty_discard(tty);
    assert(!kernel_tty_destroy(&tty) && !live && identity.references == 1);
    task.leader = 0;
    credentials.effective = KERNEL_CAP_VALID_MASK;
}
static struct kernel_tty *mode_peer;
static struct kernel_tty *peer_tty(void *owner) { (void)owner; return mode_peer; }
static void master_hungup_ioctl(void)
{
    assert(!kernel_tty_create(&heap, &transport, 0, &mode_peer));
    struct kernel_tty_transport master_transport = transport;
    master_transport.peer = peer_tty;
    assert(!kernel_tty_create(&heap, &master_transport, 0, &tty));
    kernel_tty_configure_identity(tty, 0x502, 1);
    device = kernel_tty_device_template();
    assert(!kernel_tty_open(tty, &heap, &task, 0400, 0, &instance));
    struct kernel_tty_termios settings_value;
    assert(!ioctl_call(0x5401, &settings_value));
    kernel_tty_disassociate(tty, 1);
    assert(ioctl_call(0x5401, &settings_value) == -KERNEL_EIO);
    struct kernel_tty_termios2 modern;
    assert(ioctl_call(0x802c542a, &modern) == -KERNEL_EIO);
    device->release(instance);
    assert(!kernel_tty_destroy(&tty));
    assert(!kernel_tty_destroy(&mode_peer) && !live);
}
int main(void)
{
    serial_contract_main();
    hangup_contract();
    master_hungup_ioctl();
    struct kernel_tty_transport transport = {.transmit = transmit, .drained = drained,
        .configure = configure, .kick = kick, .external_ref = refs};
    assert(!kernel_tty_create(&heap, &transport, 0, &tty));
    kernel_tty_configure_identity(tty, 0x8807, 1);
    assert(kernel_tty_rdev(tty) == 0x8807 && kernel_tty_base_only(tty));
    task.leader = 0;
    assert(!kernel_tty_open(tty, &heap, &task, 0400, 0, &instance));
    device = kernel_tty_device_template();
    assert(external_refs == 1 && kernel_tty_open_count(tty) == 1);
    struct kernel_tty_rx rx = {'A', 0};
    for (unsigned i = 0; i < 4095; i++)
        assert(kernel_tty_receive_nonblocking(tty, &rx, 1) == 1);
    assert(kernel_tty_receive_nonblocking(tty, &rx, 1) == 0);
    unsigned char bytes[64];
    assert(read_into(bytes, 64, KERNEL_FILES_O_NONBLOCK) == 64);
    assert(kernel_tty_receive_nonblocking(tty, &rx, 1) == 1);
    /* 已接受TX耗尽后取消整个用户写请求；staging、writer和OFD pin一起归还。 */
    credit = 0;
    unsigned char full[4096]; memset(full, 'T', sizeof(full));
    size_t written;
    assert(!device->write(instance, &task, 0, full, sizeof(full), &written) && written == sizeof(full));
    struct kernel_files files = {.heap = &heap};
    static struct kernel_open_file_description file;
    file = (struct kernel_open_file_description){.references = 2, .device = device, .device_instance = instance};
    static struct kernel_open_file_description *pin;
    pin = &file;
    struct kernel_uaccess_iov_snapshot snapshot={.count=9,.total=63};
    assert(!kernel_heap_allocate(&heap,9*sizeof(struct kernel_uaccess_iovec),&snapshot.allocation));
    struct kernel_uaccess_iovec *vectors=snapshot.allocation;
    for (unsigned i=0;i<9;i++) vectors[i]=(struct kernel_uaccess_iovec){(uintptr_t)full+i*7,7};
    snapshot.view=vectors;
    assert(snapshot.allocation && snapshot.view==snapshot.allocation && live==3);
    sleep_action = cancel_sleep;
    if (!setjmp(cancelled)) {
        int64_t result;
        device->writev(instance, &task, &files, &pin, &snapshot.allocation, 0, snapshot.view, snapshot.count, snapshot.total, 0, &result);
        assert(0);
    }
    assert(!task.request && !pin && !snapshot.allocation && file.references == 1 && live == 2);
    kernel_uaccess_iov_cleanup(&snapshot,&heap);
    kernel_tty_discard(tty);
    /* 正常恢复把同一个堆owner归还调用者，随后由snapshot cleanup释放。 */
    assert(!kernel_heap_allocate(&heap,9*sizeof(*vectors),&snapshot.allocation));
    vectors=snapshot.allocation;
    for (unsigned i=0;i<9;i++) vectors[i]=(struct kernel_uaccess_iovec){(uintptr_t)full+i*7,7};
    snapshot.view=vectors;pin=&file;file.references++;
    int64_t result;
    assert(!device->writev(instance,&task,&files,&pin,&snapshot.allocation,0,
        snapshot.view,snapshot.count,snapshot.total,0,&result) && result==63);
    assert(pin==&file && snapshot.allocation==vectors && snapshot.view==vectors && live==3);
    kernel_uaccess_iov_cleanup(&snapshot,&heap);
    assert(!kernel_open_file_release(&pin) && file.references==1 && live==2);
    kernel_tty_discard(tty);
    device->release(instance);
    assert(!external_refs && kernel_tty_base_only(tty));
    kernel_tty_discard(tty);
    assert(!kernel_tty_destroy(&tty) && !live);
    puts("TTY endpoints: async events, hangup generations, dynamic identity and owner contracts pass");
    return 0;
}
