#include "../../fs/char_device_internal.h"
#include <kernel/random.h>
#include <kernel/files.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/uaccess.h>
void kernel_console_putc(char c) { (void)c; }
uint32_t kernel_console_poll(uint32_t requested, struct kernel_wait_queue **q)
{ (void)requested;(void)q; return 0; }
int kernel_console_read_buffer(uint32_t flags, void *b, size_t s, size_t *r)
{ (void)flags;(void)b;(void)s;*r=0;return 0; }
struct kernel_task *kernel_task_current(void) { return 0; }
static int restarted;
void kernel_signal_note_syscall_restart(struct kernel_task *task)
{ (void)task; restarted++; }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,
    uint64_t address, const void *source, size_t size, size_t *copied)
{
    (void)mm; *copied=0;
    if (!address) return KERNEL_UACCESS_STATUS_FAULT;
    memcpy((void *)(uintptr_t)address, source, size); *copied=size;
    return KERNEL_UACCESS_STATUS_OK;
}
int main(void)
{
    const struct kernel_char_device *random=kernel_char_device_lookup(0x108);
    const struct kernel_char_device *urandom=kernel_char_device_lookup(0x109);
    unsigned char data[32]={0}; size_t count=99;
    assert(random && urandom && !kernel_random_ready());
    int bits=-1;
    assert(random->ioctl(0,0x80045200U,(uintptr_t)&bits)==0 && bits==0);
    assert(random->ioctl(0,0x80045200U,0)==-KERNEL_EFAULT);
    assert(random->ioctl(0,0xdeadU,0)==-KERNEL_EINVAL);
    assert(random->ioctl(0,0x40045201U,0)==-KERNEL_ENOTSUP);
    assert(random->ioctl(0,0x5207U,0)==-KERNEL_ENOTSUP);
    assert(random->poll(0,0)==(KERNEL_POLLOUT|KERNEL_POLLWRNORM));
    assert(random->read(KERNEL_FILES_O_NONBLOCK,data,32,&count)==-KERNEL_EAGAIN);
    assert(!count && !restarted);
    /* A blocking read enters the wait fixture, which delivers a signal. */
    assert(random->read(0,data,32,&count)==-KERNEL_ERESTARTSYS);
    assert(!count && restarted==1);
    assert(urandom->read(KERNEL_FILES_O_NONBLOCK,data,32,&count)==0 && count==32);
    assert(random->write(data,32,&count)==0 && !kernel_random_ready());
    kernel_random_mix(data,31,1);
    assert(urandom->ioctl(0,0x80045200U,(uintptr_t)&bits)==0 && bits==248);
    kernel_random_mix(data,1,1);
    assert(urandom->ioctl(0,0x80045200U,(uintptr_t)&bits)==0 && bits==256);
    assert(random->read(KERNEL_FILES_O_NONBLOCK,data,32,&count)==0 && count==32);
    assert(random->poll(0,0)==(KERNEL_POLLIN|KERNEL_POLLRDNORM));
    puts("random device pre-ready wait, poll, write-credit tests passed");
}
