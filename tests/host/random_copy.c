/* The production syscall owns no pool lock while copying user data. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BOAROS_ARCH_CONTEXT_H
#define ARCH_INTERRUPT_ENABLE_MASK 2U
static uintptr_t irq;
static void *arch_current_cpu_get(void) { return NULL; }
static uintptr_t arch_interrupt_save(void) { uintptr_t old=irq;irq=0;return old; }
static void arch_interrupt_restore(uintptr_t value) { irq=value; }
#ifndef RANDOM_SOURCE
#define RANDOM_SOURCE "../../kernel/syscall/random.c"
#endif
#include RANDOM_SOURCE
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"random copy line %d: %s\n",__LINE__,#x);exit(1); } } while (0)
static unsigned fills, copies, signal_after, fail_fill, fail_copy;
static size_t output_offset;
static struct kernel_mm memory;
static unsigned char scratch[BOAROS_PAGE_SIZE];
static unsigned scratch_live, scratch_requests, scratch_releases, scratch_oom;
static size_t generated_extent;
enum kernel_task_status kernel_task_io_buffer_acquire(struct kernel_task_io_buffer *buffer,struct physical_page_allocator *allocator)
{
    CHECK(!scratch_live && allocator==memory.allocator);scratch_requests++;
    if(scratch_oom)return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
    memset(scratch,0x7b,sizeof(scratch));generated_extent=0;
    scratch_live=1;*buffer=(struct kernel_task_io_buffer){.allocator=allocator,.data=scratch};
    return KERNEL_TASK_STATUS_OK;
}
void kernel_task_io_buffer_release(struct kernel_task_io_buffer *buffer)
{
    CHECK(scratch_live && buffer->data==scratch);
    for(size_t i=0;i<generated_extent;i++)CHECK(!scratch[i]);
    scratch_live=0;scratch_releases++;*buffer=(struct kernel_task_io_buffer){0};
}
void kernel_assert_can_block(void) { }
int kernel_random_wait_ready(int nonblock) { (void)nonblock;return 0; }
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *task,struct kernel_mm **out)
{ (void)task;*out=&memory;return KERNEL_TASK_STATUS_OK; }
enum kernel_uaccess_status kernel_user_range_check(uint64_t address,size_t size)
{ return address && size<=3*BOAROS_PAGE_SIZE?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT; }
int kernel_signal_has_pending(const struct kernel_task *task)
{ (void)task;return signal_after && copies>=signal_after; }
void kernel_signal_note_syscall_restart(struct kernel_task *task) { (void)task;abort(); }
enum kernel_random_status kernel_random_fill(void *buffer,size_t size)
{ CHECK(irq==2);fills++;if(buffer==scratch && size>generated_extent)generated_extent=size;memset(buffer,0xa5,size);return fills==fail_fill?KERNEL_RANDOM_STATUS_STATE:KERNEL_RANDOM_STATUS_OK; }
void kernel_random_erase(void *buffer,size_t size) { memset(buffer,0,size); }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t address,const void *from,size_t size,size_t *copied)
{ CHECK(mm==&memory && irq==2);CHECK(!scratch_live || (address&BOAROS_PAGE_MASK)+size<=BOAROS_PAGE_SIZE);copies++;*copied=copies==fail_copy?7:size;memcpy((void *)(uintptr_t)address,from,*copied);return copies==fail_copy?KERNEL_UACCESS_STATUS_FAULT:KERNEL_UACCESS_STATUS_OK; }
static void run(uintptr_t initial,size_t bytes,unsigned signal,unsigned fill_error,unsigned copy_error,int64_t expected)
{
    _Alignas(BOAROS_PAGE_SIZE) unsigned char output[3*BOAROS_PAGE_SIZE]={0};
    unsigned char *data=output+output_offset;
    struct kernel_syscall_request request={.arguments={(uintptr_t)data,bytes,0}};
    struct kernel_syscall_result result;
    irq=initial;fills=copies=0;scratch_requests=scratch_releases=0;signal_after=signal;fail_fill=fill_error;fail_copy=copy_error;
    CHECK(syscall_handle_getrandom(NULL,&request,&result)==KERNEL_SYSCALL_STATUS_OK);
    CHECK(result.value==expected && irq==initial);
    if(expected>0)for(int64_t i=0;i<expected;i++)CHECK(data[i]==0xa5);
    if(expected>=0 && (size_t)expected<sizeof(output))CHECK(data[expected]==0);
    if(expected==(int64_t)bytes) CHECK(copies==(bytes<=256?1:(bytes+output_offset+BOAROS_PAGE_MASK)/BOAROS_PAGE_SIZE));
    CHECK(!scratch_live && scratch_requests==(bytes>256) && scratch_releases==(bytes>256 && !scratch_oom));
}
int main(void)
{
    memory.allocator=(struct physical_page_allocator *)&memory;
    size_t bytes=2*BOAROS_PAGE_SIZE+17;
    for(unsigned initial=0;initial<=2;initial+=2) {
        run(initial,bytes,0,0,0,bytes);run(initial,bytes,1,0,0,BOAROS_PAGE_SIZE);
        run(initial,bytes,0,1,0,-KERNEL_EIO);run(initial,bytes,0,2,0,BOAROS_PAGE_SIZE);
        run(initial,bytes,0,0,1,7);run(initial,bytes,0,0,2,BOAROS_PAGE_SIZE+7);
        run(initial,256,0,0,0,256);run(initial,257,0,0,0,257);
        output_offset=BOAROS_PAGE_SIZE-2;run(initial,BOAROS_PAGE_SIZE+5,0,0,0,BOAROS_PAGE_SIZE+5);output_offset=0;
        scratch_oom=1;run(initial,bytes,0,0,0,-KERNEL_ENOMEM);scratch_oom=0;
    }
    puts("getrandom IRQ: preparation/copy enabled, signal/error prefix and entry-state restore passed");
}
