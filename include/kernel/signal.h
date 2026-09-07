#ifndef BOAROS_KERNEL_SIGNAL_H
#define BOAROS_KERNEL_SIGNAL_H

#include <kernel/pid.h>

#include <stdint.h>

#define KERNEL_SIGNAL_COUNT 64U
#define KERNEL_SIGNAL_DFL UINT64_C(0)
#define KERNEL_SIGNAL_IGN UINT64_C(1)
#define KERNEL_SIGNAL_UNBLOCKABLE_MASK \
    ((UINT64_C(1) << 8U) | (UINT64_C(1) << 18U))

#define LINUX_SIG_BLOCK 0U
#define LINUX_SIG_UNBLOCK 1U
#define LINUX_SIG_SETMASK 2U

#define LINUX_SA_NOCLDSTOP UINT64_C(0x00000001)
#define LINUX_SA_NOCLDWAIT UINT64_C(0x00000002)
#define LINUX_SA_SIGINFO UINT64_C(0x00000004)
#define LINUX_SA_ONSTACK UINT64_C(0x08000000)
#define LINUX_SA_RESTART UINT64_C(0x10000000)
#define LINUX_SA_NODEFER UINT64_C(0x40000000)
#define LINUX_SA_RESETHAND UINT64_C(0x80000000)

/* Linux uapi struct sigaction prefix used by riscv64 rt_sigaction.  The
 * kernel consumes the first 64-bit sigset word; the remaining user sigset
 * storage is reserved and must be zero when passed from musl. */
struct kernel_linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

enum kernel_signal_status {
    KERNEL_SIGNAL_STATUS_OK = 0,
    KERNEL_SIGNAL_STATUS_NOT_INITIALIZED,
    KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT,
    KERNEL_SIGNAL_STATUS_INVALID_STATE,
    KERNEL_SIGNAL_STATUS_NO_MEMORY,
};

struct kernel_task;
struct riscv_trap_frame;

/* Applies the default action or enters a user handler for one pending
 * signal at a user-return tail of the trap dispatch.  Runs until the
 * task stops, exits, or has nothing deliverable left. */
void kernel_signal_deliver_pending(struct riscv_trap_frame *frame);

/* rt_sigreturn: rebuilds the interrupted trap frame from the signal
 * frame at the user stack pointer.  A frame that cannot be read kills
 * the task with SIGSEGV. */
void kernel_signal_restore_current(struct riscv_trap_frame *frame);

/* Records that the current user syscall must be retried after signal
 * handling; the generic form keeps the original trap arguments. */
void kernel_signal_note_syscall_restart(struct kernel_task *task);

/* Records a relative nanosleep restart using its absolute deadline. */
void kernel_signal_note_nanosleep_restart(struct kernel_task *task,
                                          uint64_t deadline,
                                          uint64_t remaining_address);

/* Called immediately before returning to user mode, including from a
 * freshly scheduled task whose context starts at the trap-return stub. */
void kernel_signal_prepare_user_return(struct riscv_trap_frame *frame);

/* Marks the signal pending on the target (first sender wins) and drops
 * ignored signals.  Stopped targets are resumed for SIGCONT and
 * SIGKILL; waking blocked targets is the interruptible-sleep path. */
enum kernel_signal_status kernel_signal_send(struct kernel_task *target,
                                             uint32_t sig,
                                             kernel_pid_t sender_tid);

enum kernel_signal_status kernel_signal_get_action(
    struct kernel_task *task,
    uint32_t sig,
    struct kernel_linux_sigaction *action);

/* Installs one disposition; KILL/STOP bits are stripped from the mask
 * and SIGKILL/SIGSTOP cannot be given a handler. */
enum kernel_signal_status kernel_signal_set_action(
    struct kernel_task *task,
    uint32_t sig,
    const struct kernel_linux_sigaction *action);

/* Fork inheritance: dispositions and blocked mask are copied, pending
 * signals are cleared, and an allocated disposition page is duplicated. */
enum kernel_signal_status kernel_signal_fork(
    struct kernel_task *child,
    const struct kernel_task *parent);

enum kernel_signal_status kernel_signal_get_blocked(
    const struct kernel_task *task,
    uint64_t *blocked);

/* how is LINUX_SIG_BLOCK/UNBLOCK/SETMASK; KILL/STOP bits never land in
 * the resulting mask. */
enum kernel_signal_status kernel_signal_update_blocked(
    struct kernel_task *task,
    uint32_t how,
    const uint64_t *new_mask,
    uint64_t *old_mask);

enum kernel_signal_status kernel_signal_get_pending(
    const struct kernel_task *task,
    uint64_t *pending);

/* execve: every installed handler resets to DFL, IGN stays, and
 * pending signals that are now ignored are flushed. */
void kernel_signal_reset_on_exec(struct kernel_task *task);

/* Nonzero when the parent has a real SIGCHLD handler (SA_NOCLDWAIT not
 * set), i.e. a child state change should send SIGCHLD. */
int kernel_signal_wants_sigchld(const struct kernel_task *parent);

/* Child state transitions wake wait4 and optionally raise SIGCHLD. */
void kernel_signal_notify_child_exit(struct kernel_task *child);
void kernel_signal_notify_child_stop(struct kernel_task *child,
                                     int continued);

/* Resolves kill(2) targets: pid > 0 selects the process, 0 the caller's
 * process group, < -1 the group -pid, and -1 every live process except
 * the caller and the init task.  Returns the number of targets found. */
uint32_t kernel_signal_resolve_targets(struct kernel_task *caller,
                                       int64_t pid);

/* Sends to every target of the same resolution; returns how many were
 * delivered or marked pending. */
uint32_t kernel_signal_send_targets(struct kernel_task *caller,
                                    int64_t pid,
                                    uint32_t sig,
                                    kernel_pid_t sender_tid);

/* Sends to one thread id, optionally checking its thread-group id. */
uint32_t kernel_signal_send_thread(struct kernel_task *caller,
                                   kernel_pid_t tgid,
                                   kernel_pid_t tid,
                                   uint32_t sig,
                                   kernel_pid_t sender_tid);

struct kernel_task *kernel_signal_find_by_tid(kernel_pid_t tid);

#endif
