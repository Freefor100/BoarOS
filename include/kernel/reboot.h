#ifndef BOAROS_KERNEL_REBOOT_H
#define BOAROS_KERNEL_REBOOT_H

#include <stdint.h>

#define KERNEL_REBOOT_COMMAND_SIZE 256U
#define KERNEL_REBOOT_CMD_RESTART UINT32_C(0x01234567)
#define KERNEL_REBOOT_CMD_HALT UINT32_C(0xcdef0123)
#define KERNEL_REBOOT_CMD_POWER_OFF UINT32_C(0x4321fedc)
#define KERNEL_REBOOT_CMD_RESTART2 UINT32_C(0xa1b2c3d4)

enum kernel_reboot_action {
    KERNEL_REBOOT_NONE,
    KERNEL_REBOOT_RESTART,
    KERNEL_REBOOT_HALT,
    KERNEL_REBOOT_POWER_OFF,
};

struct kernel_task;
/* 0: initial namespace; 1: nested request consumed; negative: error. */
int kernel_task_reboot_namespace(struct kernel_task *,enum kernel_reboot_action);

/* Capability checking precedes this decoder in the syscall layer. */
int kernel_reboot_decode(uint32_t magic1, uint32_t magic2, uint32_t command,
                         enum kernel_reboot_action *action);
/* Only the production root cleanup task enables requests, after registration. */
void kernel_reboot_enable(void);
/* restart2 is a kernel-owned, NUL-terminated string of at most 255 bytes. */
int kernel_reboot_request(enum kernel_reboot_action action, const char *restart2);
/* Publication and user-task publication share the scheduler wait domain. */
int kernel_reboot_requested_locked(void);
enum kernel_reboot_action kernel_reboot_action_requested(void);
/* The first accepted request owns this immutable command until reset. */
const char *kernel_reboot_command(void);

#endif
