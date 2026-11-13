#include <kernel/reboot.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <kernel/wait_internal.h>

static int enabled;
static enum kernel_reboot_action requested;
static char command[KERNEL_REBOOT_COMMAND_SIZE];

int kernel_reboot_decode(uint32_t magic1, uint32_t magic2, uint32_t value,
                         enum kernel_reboot_action *action)
{
    if (magic1 != UINT32_C(0xfee1dead) ||
        (magic2 != 672274793U && magic2 != 85072278U &&
         magic2 != 369367448U && magic2 != 537993216U)) return -KERNEL_EINVAL;
    switch (value) {
    case KERNEL_REBOOT_CMD_RESTART:
    case KERNEL_REBOOT_CMD_RESTART2:
        *action=KERNEL_REBOOT_RESTART;
        return 0;
    case KERNEL_REBOOT_CMD_HALT:
        *action=KERNEL_REBOOT_HALT;
        return 0;
    case KERNEL_REBOOT_CMD_POWER_OFF:
        *action=KERNEL_REBOOT_POWER_OFF;
        return 0;
    default:
        return -KERNEL_EINVAL;
    }
}

void kernel_reboot_enable(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (enabled) __builtin_trap();
    enabled=1;
}

int kernel_reboot_request(enum kernel_reboot_action action, const char *restart2)
{
    if (action<KERNEL_REBOOT_RESTART || action>KERNEL_REBOOT_POWER_OFF) __builtin_trap();
    int publish=0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!enabled) return -KERNEL_ENOSYS;
        if (requested==KERNEL_REBOOT_NONE) {
            unsigned i=0;
            if (restart2) {
                for (; i<KERNEL_REBOOT_COMMAND_SIZE-1 && restart2[i]; i++) command[i]=restart2[i];
            }
            command[i]='\0';
            requested=action;
            publish=1;
        }
    }
    /* The persistent request also participates in the cleanup wait predicate;
     * a notification before wait registration cannot lose the transition. */
    if (publish) kernel_scheduler_notify_cleanup();
    return 0;
}

int kernel_reboot_requested_locked(void)
{
    if (!kernel_raw_lock_owned(&kernel_wait_domain)) __builtin_trap();
    return requested!=KERNEL_REBOOT_NONE;
}

enum kernel_reboot_action kernel_reboot_action_requested(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return requested;
}

const char *kernel_reboot_command(void) { return command; }
