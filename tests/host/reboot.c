#include <kernel/reboot.h>
#include <kernel/errno.h>
#include <kernel/wait_internal.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq = 1;
static _Thread_local struct kernel_cpu cpu = {.initialized=KERNEL_CPU_INITIALIZED};
static unsigned notifications;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return 0; }
void kernel_cpu_flush_notifications(void) { assert(!cpu.outbound); }
void kernel_scheduler_notify_cleanup(void)
{
    assert(!cpu.raw_locks);
    assert(kernel_reboot_action_requested() != KERNEL_REBOOT_NONE);
    __atomic_add_fetch(&notifications, 1, __ATOMIC_RELAXED);
}
static void decode(void)
{
    const uint32_t magics[] = {672274793U, 85072278U, 369367448U, 537993216U};
    const uint32_t commands[] = {KERNEL_REBOOT_CMD_RESTART, KERNEL_REBOOT_CMD_RESTART2,
        KERNEL_REBOOT_CMD_HALT, KERNEL_REBOOT_CMD_POWER_OFF};
    const enum kernel_reboot_action actions[] = {KERNEL_REBOOT_RESTART, KERNEL_REBOOT_RESTART,
        KERNEL_REBOOT_HALT, KERNEL_REBOOT_POWER_OFF};
    for (unsigned i=0; i<4; i++) for (unsigned j=0; j<4; j++) {
        enum kernel_reboot_action action=KERNEL_REBOOT_NONE;
        assert(!kernel_reboot_decode(0xfee1dead, magics[i], commands[j], &action));
        assert(action==actions[j]);
    }
    const uint32_t unsupported[] = {0, 0x89abcdef, 0, 0x45584543, 0xd000fce2, UINT32_MAX};
    for (unsigned i=0; i<sizeof(unsupported)/sizeof(unsupported[0]); i++) {
        enum kernel_reboot_action action=KERNEL_REBOOT_NONE;
        assert(kernel_reboot_decode(0xfee1dead, magics[0], unsupported[i], &action)==-KERNEL_EINVAL);
        assert(action==KERNEL_REBOOT_NONE);
    }
    enum kernel_reboot_action action=KERNEL_REBOOT_NONE;
    assert(kernel_reboot_decode(0, magics[0], commands[0], &action)==-KERNEL_EINVAL);
    assert(kernel_reboot_decode(0xfee1dead, 0, commands[0], &action)==-KERNEL_EINVAL);
    assert(kernel_reboot_action_requested()==KERNEL_REBOOT_NONE);
    assert(kernel_reboot_request(KERNEL_REBOOT_HALT, 0)==-KERNEL_ENOSYS);
    assert(!notifications && !*kernel_reboot_command());
}
static pthread_barrier_t requests;
static void *request(void *argument)
{
    enum kernel_reboot_action action=(enum kernel_reboot_action)(uintptr_t)argument;
    pthread_barrier_wait(&requests);
    assert(!kernel_reboot_request(action, action==KERNEL_REBOOT_RESTART ? "first-reset" : "first-halt"));
    return 0;
}
static void coordinator(void)
{
    kernel_reboot_enable();
    pthread_barrier_init(&requests, 0, 3);
    pthread_t a,b;
    assert(!pthread_create(&a, 0, request, (void *)(uintptr_t)KERNEL_REBOOT_RESTART));
    assert(!pthread_create(&b, 0, request, (void *)(uintptr_t)KERNEL_REBOOT_HALT));
    pthread_barrier_wait(&requests);
    assert(!pthread_join(a, 0) && !pthread_join(b, 0));
    enum kernel_reboot_action first=kernel_reboot_action_requested();
    assert(first==KERNEL_REBOOT_RESTART || first==KERNEL_REBOOT_HALT);
    assert(!strcmp(kernel_reboot_command(), first==KERNEL_REBOOT_RESTART ? "first-reset" : "first-halt"));
    assert(!kernel_reboot_request(KERNEL_REBOOT_POWER_OFF, "overwrite"));
    assert(kernel_reboot_action_requested()==first && notifications==1);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    assert(kernel_reboot_requested_locked());
}
int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    decode();
    pid_t child=fork(); assert(child>=0);
    if (!child) { coordinator(); _exit(0); }
    int status; assert(waitpid(child,&status,0)==child && !status);
    puts("PASS reboot commands, magics, disabled root and concurrent immutable first request");
}
