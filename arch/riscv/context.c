#include <arch/riscv/context.h>

#include <stdint.h>

void riscv_kernel_thread_trampoline(void);
void riscv_trap_return(void);

enum riscv_context_status riscv_context_init(
    struct riscv_switch_context *context,
    uintptr_t stack_top,
    void (*entry)(void *),
    void *argument,
    void *thread_pointer)
{
    struct riscv_switch_context result;

    if (context == 0 || stack_top == 0U ||
        (stack_top & (uintptr_t)0xfU) != 0U ||
        entry == 0 || thread_pointer == 0) {
        return RISCV_CONTEXT_STATUS_INVALID_ARGUMENT;
    }

    result.ra = (uintptr_t)riscv_kernel_thread_trampoline;
    result.sp = stack_top;
    result.tp = (uintptr_t)thread_pointer;
    result.s[0] = (uintptr_t)entry;
    result.s[1] = (uintptr_t)argument;
    result.s[2] = 0U;
    result.s[3] = 0U;
    result.s[4] = 0U;
    result.s[5] = 0U;
    result.s[6] = 0U;
    result.s[7] = 0U;
    result.s[8] = 0U;
    result.s[9] = 0U;
    result.s[10] = 0U;
    result.s[11] = 0U;
    *context = result;
    return RISCV_CONTEXT_STATUS_OK;
}

enum riscv_context_status riscv_context_init_user(
    struct riscv_switch_context *context,
    uintptr_t trap_frame_pointer,
    void *thread_pointer)
{
    struct riscv_switch_context result;
    uint32_t index;

    if (context == 0 || trap_frame_pointer == 0U ||
        (trap_frame_pointer & (uintptr_t)0xfU) != 0U ||
        thread_pointer == 0) {
        return RISCV_CONTEXT_STATUS_INVALID_ARGUMENT;
    }

    result.ra = (uintptr_t)riscv_trap_return;
    result.sp = trap_frame_pointer;
    result.tp = (uintptr_t)thread_pointer;
    for (index = 0U; index < 12U; index++) {
        result.s[index] = 0U;
    }
    *context = result;
    return RISCV_CONTEXT_STATUS_OK;
}
