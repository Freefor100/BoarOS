#define main serial_contract_main
#include "core_host.c"
#undef main
#include "../../fs/uaccess_iov_internal.h"

static int external_refs;
static void refs(void *owner, int delta) { (void)owner; external_refs += delta; }
int main(void)
{
    serial_contract_main();
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
    puts("TTY endpoints: dynamic identity, owner and nonblocking RX contracts pass");
    return 0;
}
