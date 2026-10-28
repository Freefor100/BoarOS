/* 宿主只提供线程执行、时钟与未开放消费者；缓存/VFS/后端均链接生产代码。 */
#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include "../../../mm/uaccess_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#ifdef FILE_IO_CASES
#include "../../../kernel/sched/private.h"
#endif
void mm_test_attach(unsigned);
struct disk_host_thread { pthread_t thread; void (*entry)(void *); void *argument; };
static void *run_thread(void *p)
{
    struct disk_host_thread *thread = p;
    mm_test_attach(0);
#ifdef FILE_IO_CASES
    void mm_test_activate_task(void);
    mm_test_activate_task();
#endif
    thread->entry(thread->argument);
#ifdef FILE_IO_CASES
    void mm_test_finish(void);
    mm_test_finish();
#endif
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
#ifndef FILE_IO_CASES
int kernel_scheduler_can_sleep(void) { return 1; }
#endif
int riscv_plic_in_interrupt(void) { return 0; }
#if BOAROS_MAX_CPUS > 1
void kernel_assert_service_context(void)
{
    if (kernel_cpu_current()->logical_id) abort();
#ifdef FILE_IO_CASES
    if (kernel_cpu_current()->current && kernel_cpu_current()->current->controlled) abort();
#endif
}
/* 宿主park直接向pthread发布dispatch；没有客户机IPI设备。 */
void kernel_cpu_flush_notifications(void) {}
#endif
void kernel_thread_yield(void) { sched_yield(); }
enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{ if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
  sched_yield(); return KERNEL_SCHEDULER_STATUS_OK; }
uint32_t kernel_time_is_initialized(void) { return 1; }
uint64_t kernel_time_monotonic_ns(void)
{
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000U + t.tv_nsec;
}
#if defined(FILES_PIPE_CASES) && !defined(FILE_IO_CASES)
__attribute__((weak)) enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *deadline)
{ *deadline=ns;return KERNEL_TIME_STATUS_OK; }
#endif
/* The host deadline model uses nanoseconds as its architecture tick unit. */
uint64_t riscv_time_read(void) { return kernel_time_monotonic_ns(); }
uint64_t kernel_time_realtime_ns(void)
{
    struct timespec t; if (clock_gettime(CLOCK_REALTIME, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000U + t.tv_nsec;
}
void kernel_console_putc(char c) { fputc(c, stderr); }
#ifndef FILE_IO_CASES
__attribute__((weak)) uint64_t kernel_task_current_stack_limit(void) { return UINT64_MAX; }
#include <kernel/rlimit.h>
__attribute__((weak)) void kernel_task_current_memory_limits(struct kernel_memory_limits *out)
{ *out = (struct kernel_memory_limits){UINT64_MAX, UINT64_MAX, UINT64_MAX}; }
#endif
static void excluded(void) { fputs("unopened consumer reached\n", stderr); abort(); }
#define EXCLUDED(name) void name(void) { excluded(); }
EXCLUDED(kernel_event_fd_close)
EXCLUDED(kernel_pidfd_close)
EXCLUDED(kernel_pidfd_inode)
#if !defined(FILE_IO_CASES) && !defined(CACHE_MM_CASES)
/* 此缓存fixture仅接纳内核来源；MM/uaccess组合由CACHE_MM_CASES链接真实实现。 */
enum kernel_uaccess_status kernel_uaccess_pin_user_page(struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{ (void)mm; (void)address; (void)access; (void)pin; excluded(); return KERNEL_UACCESS_STATUS_STATE; }
enum kernel_mm_status kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{ (void)mm; (void)address; (void)access; (void)pin; excluded(); return KERNEL_MM_STATUS_STATE; }
void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{ (void)pin; excluded(); }
#endif
EXCLUDED(kernel_shm_attachment_close)
EXCLUDED(kernel_shm_attachment_open)
EXCLUDED(kernel_shm_attachment_acquire)
struct kernel_shm_attachment;
void kernel_shm_attachment_release(struct kernel_shm_attachment **owner)
{ if (owner && *owner) excluded(); }
EXCLUDED(kernel_elf64_source_release)
EXCLUDED(kernel_elf64_source_acquire)
EXCLUDED(kernel_elf64_source_page)
EXCLUDED(kernel_elf64_source_file)
EXCLUDED(kernel_elf64_source_run_count)
EXCLUDED(kernel_elf64_source_run_at)
#if BOAROS_MAX_CPUS == 1
EXCLUDED(kernel_task_mm_borrow)
#endif
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#if BOAROS_MAX_CPUS > 1
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{ (void)targets; (void)size; mm_test_fence(address,instructions); }
#endif
#ifndef FILE_IO_CASES
EXCLUDED(kernel_epoll_notify_file_release)
EXCLUDED(kernel_epoll_destroy)
#endif
#ifndef FILE_IO_CASES
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_socket_is_unix)
EXCLUDED(kernel_socket_rights_changed)
#endif
#ifndef FILES_PIPE_CASES
EXCLUDED(kernel_pipe_release_endpoint)
EXCLUDED(kernel_pipe_destroy)
#endif
