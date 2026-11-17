#ifndef BOAROS_KERNEL_SECCOMP_H
#define BOAROS_KERNEL_SECCOMP_H
#include <stdint.h>
#include <kernel/bpf_vm.h>
struct kernel_task;
struct kernel_syscall_request;
struct kernel_syscall_result;
struct kernel_seccomp_filter;
struct kernel_seccomp_data {
    int32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer, args[6];
};
int kernel_seccomp_validate(const struct kernel_bpf_classic_insn *, uint32_t,
    const struct kernel_bpf_vm_env *);
/* Execution accepts only immutable instructions that passed validation. */
uint32_t kernel_seccomp_run(const struct kernel_bpf_classic_insn *, uint32_t,
    const struct kernel_seccomp_data *);
int64_t kernel_seccomp_set(struct kernel_task *, uint32_t, uint32_t, uint64_t);
int64_t kernel_seccomp_syscall(struct kernel_task *, uint32_t, uint32_t, uint64_t);
int kernel_seccomp_check(struct kernel_task *, const struct kernel_syscall_request *,
    struct kernel_syscall_result *);
/* Installation serializes with group publication and exec credential capture.
 * The prepared node stays caller-owned on failure; publication consumes it. */
void kernel_seccomp_abort_wait_locked(struct kernel_task *);
int64_t kernel_seccomp_install(struct kernel_task *, struct kernel_seccomp_filter *, uint32_t);
int64_t kernel_seccomp_attach_locked(struct kernel_task *, struct kernel_seccomp_filter *, uint32_t);
/* Child is unpublished; parent leaf is stabilized by the scheduler domain. */
void kernel_seccomp_clone(struct kernel_task *, const struct kernel_task *);
void kernel_seccomp_release(struct kernel_task *);
/* Unlike ordinary fatal signals, KILL_THREAD must not terminate siblings. */
void kernel_seccomp_terminate(unsigned signal, int group) __attribute__((noreturn));
#endif
