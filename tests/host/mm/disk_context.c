/* 宿主只提供线程执行、时钟与未开放消费者；缓存/VFS/后端均链接生产代码。 */
#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
void mm_test_attach(unsigned);
struct disk_host_thread { pthread_t thread; void (*entry)(void *); void *argument; };
static void *run_thread(void *p)
{
    struct disk_host_thread *thread = p;
    mm_test_attach(0);
    thread->entry(thread->argument);
    return 0;
}
enum kernel_scheduler_status kernel_thread_create_joinable(
    void (*entry)(void *), void *arg, struct kernel_thread_join *join)
{
    struct disk_host_thread *t = calloc(1, sizeof(*t));
    if (!t) return KERNEL_SCHEDULER_STATUS_NO_MEMORY;
    t->entry = entry; t->argument = arg;
    if (pthread_create(&t->thread, 0, run_thread, t)) { free(t); return KERNEL_SCHEDULER_STATUS_NO_MEMORY; }
    join->task = (struct kernel_task *)t;
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_thread_join(struct kernel_thread_join *join)
{
    struct disk_host_thread *t = (void *)join->task;
    if (!t || pthread_join(t->thread, 0)) abort();
    free(t); join->task = 0;
}
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
#if BOAROS_MAX_CPUS > 1
void kernel_assert_service_context(void)
{ if (kernel_cpu_current()->logical_id) abort(); }
/* 宿主park直接向pthread发布dispatch；没有客户机IPI设备。 */
void kernel_cpu_flush_notifications(void) {}
#endif
void kernel_thread_yield(void) { sched_yield(); }
enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{ if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
  sched_yield(); return KERNEL_SCHEDULER_STATUS_OK; }
int kernel_time_is_initialized(void) { return 1; }
uint64_t kernel_time_monotonic_ns(void)
{
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000U + t.tv_nsec;
}
uint64_t kernel_time_realtime_ns(void)
{
    struct timespec t; if (clock_gettime(CLOCK_REALTIME, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000U + t.tv_nsec;
}
void kernel_console_putc(char c) { fputc(c, stderr); }
uint64_t kernel_task_current_stack_limit(void) { return 0; }
static void excluded(void) { fputs("unopened consumer reached\n", stderr); abort(); }
#define EXCLUDED(name) void name(void) { excluded(); }
EXCLUDED(kernel_shm_attachment_close)
EXCLUDED(kernel_shm_attachment_open)
EXCLUDED(kernel_shm_attachment_acquire)
EXCLUDED(kernel_shm_attachment_release)
EXCLUDED(kernel_elf64_source_release)
EXCLUDED(kernel_elf64_source_acquire)
EXCLUDED(kernel_elf64_source_page)
#if BOAROS_MAX_CPUS == 1
EXCLUDED(kernel_task_mm_borrow)
#endif
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#if BOAROS_MAX_CPUS > 1
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{ (void)targets; (void)size; mm_test_fence(address,instructions); }
#endif
EXCLUDED(kernel_proc_task_block_reads)
EXCLUDED(kernel_proc_task_note_fault)
EXCLUDED(kernel_epoll_notify_file_release)
EXCLUDED(kernel_epoll_destroy)
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_pipe_release_endpoint)
EXCLUDED(kernel_pipe_destroy)
