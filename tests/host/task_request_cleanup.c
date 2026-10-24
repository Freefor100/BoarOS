/* The request backends are isolated here; their own tests exercise real I/O.
 * This checks the production exit caller consumes each stack handle before
 * releasing its scratch registration, including repeated cleanup retries. */
#include "../../kernel/sched/process.c"
#include <assert.h>
#include <stdio.h>

static struct kernel_task owner;
static struct kernel_task_io_buffer scratch;
static unsigned expected, seen;

static void consumed(unsigned bit)
{
    assert((expected & bit) && !(seen & bit));
    if (expected & 16U) assert(owner.io_buffer == &scratch);
    seen |= bit;
}
void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *request)
{ assert(request == owner.epoll_wait_request); consumed(1); owner.epoll_wait_request = 0; }
void kernel_socket_abort_write(struct kernel_socket_write_request *request)
{ assert(request == owner.socket_write_request); consumed(2); owner.socket_write_request = 0; }
void kernel_socket_abort_read(struct kernel_socket_read_request *request)
{ assert(request == owner.socket_read_request); consumed(4); owner.socket_read_request = 0; }
void kernel_tty_abort_request(struct kernel_tty_request *request)
{ assert(request == owner.tty_request); consumed(8); owner.tty_request = 0; }
void kernel_sem_abort(struct kernel_task *task)
{ assert(task == &owner); if (task->sem_request) { consumed(32); task->sem_request = 0; } }
void kernel_socket_call_abort(struct kernel_socket_call *call)
{ assert(call == owner.socket_call); consumed(64); owner.socket_call = 0; }
void kernel_msg_abort(struct kernel_task *task)
{ assert(task == &owner); if (task->msg_request) { consumed(128); task->msg_request = 0; } }
enum physical_page_status physical_page_release(struct physical_page_allocator *allocator,
                                                uint64_t address)
{ (void)allocator; (void)address; __builtin_trap(); }

int main(void)
{
    for (expected = 0; expected < 256; expected++) {
        owner = (struct kernel_task){0};
        scratch = (struct kernel_task_io_buffer){0};
        seen = 0;
        if (expected & 1) owner.epoll_wait_request = (void *)(uintptr_t)1;
        if (expected & 2) owner.socket_write_request = (void *)(uintptr_t)2;
        if (expected & 4) owner.socket_read_request = (void *)(uintptr_t)3;
        if (expected & 8) owner.tty_request = (void *)(uintptr_t)4;
        if (expected & 32) owner.sem_request = (void *)(uintptr_t)7;
        if (expected & 64) owner.socket_call = (void *)(uintptr_t)8;
        if (expected & 128) owner.msg_request = (void *)(uintptr_t)9;
        if (expected & 16) {
            owner.io_scratch_data = (void *)(uintptr_t)5;
            owner.io_scratch_physical_address = 4096;
            scratch = (struct kernel_task_io_buffer){.task = &owner,
                .allocator = (void *)(uintptr_t)6, .retained_scratch = 1,
                .physical_address = 4096, .data = owner.io_scratch_data};
            owner.io_buffer = &scratch;
        }
        cleanup_task_io_requests(&owner);
        assert(seen == (expected & ~16U));
        assert(!owner.epoll_wait_request && !owner.socket_write_request &&
               !owner.socket_read_request && !owner.tty_request && !owner.io_buffer &&
               !owner.sem_request && !owner.socket_call && !owner.msg_request);
        assert(!scratch.allocator);
        cleanup_task_io_requests(&owner);
        assert(seen == (expected & ~16U));
    }
    puts("PASS task request cleanup: 256 handle sets, scratch lifetime, retry");
    return 0;
}
