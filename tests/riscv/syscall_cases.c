#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/task.h>

#include <stdint.h>

static enum kernel_task_status brk_borrow_status =
    KERNEL_TASK_STATUS_OK;
static enum kernel_mm_status brk_mm_status = KERNEL_MM_STATUS_OK;
static uint64_t brk_mm_result;
static uint64_t brk_mm_requested;

enum kernel_task_status __wrap_kernel_task_mm_borrow_mutable(
    struct kernel_task *task,
    struct kernel_mm **mm)
{
    if (brk_borrow_status != KERNEL_TASK_STATUS_OK) {
        return brk_borrow_status;
    }
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    *mm = (struct kernel_mm *)(uintptr_t)2U;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_mm_status __wrap_kernel_mm_brk(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U || result == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    brk_mm_requested = requested;
    if (brk_mm_status == KERNEL_MM_STATUS_OK) {
        *result = brk_mm_result;
    }
    return brk_mm_status;
}

static unsigned long result_changed(
    const struct kernel_syscall_result *result,
    enum kernel_syscall_action action,
    int64_t value)
{
    return result->action != action || result->value != value;
}

static unsigned long run_invalid_argument_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;
    struct kernel_syscall_request request = {0};
    struct kernel_syscall_result result = {
        .action = (enum kernel_syscall_action)0x55,
        .value = INT64_C(0x1122334455667788),
    };
    unsigned long failures = 0U;

    if (kernel_syscall_dispatch(caller, 0, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       (enum kernel_syscall_action)0x55,
                       INT64_C(0x1122334455667788))) {
        failures++;
    }
    if (kernel_syscall_dispatch(caller, &request, 0) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        kernel_syscall_dispatch(0, &request, &result) !=
        KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT) {
        failures++;
    }

    return failures;
}

static unsigned long run_exit_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;
    struct kernel_syscall_request request = {
        .number = 93U,
        .arguments = {
            UINT64_C(0x11223344556677ab),
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX,
        },
    };
    struct kernel_syscall_result result = {
        .action = KERNEL_SYSCALL_ACTION_RETURN,
        .value = -1,
    };

    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_EXIT, 0xab)) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_unknown_cases(void)
{
    static const uint64_t numbers[] = {0U, 94U, 174U, UINT64_MAX};
    struct kernel_syscall_request request = {0};
    struct kernel_syscall_result result;
    unsigned long failures = 0U;
    unsigned long index;
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;

    for (index = 0U; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
        request.number = numbers[index];
        result.action = KERNEL_SYSCALL_ACTION_EXIT;
        result.value = 1;
        if (kernel_syscall_dispatch(caller, &request, &result) !=
                KERNEL_SYSCALL_STATUS_OK ||
            result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -38)) {
            failures++;
        }
    }

    return failures;
}

static unsigned long run_process_decode_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;
    struct kernel_syscall_request request = {
        .number = 220U,
        .arguments = {17U, 0U, 0U, 0U, 0U, 0U},
    };
    struct kernel_syscall_result result;
    unsigned long failures = 0U;

    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_CLONE, 0)) {
        failures++;
    }
    request.arguments[0] = 0x111U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -95)) {
        failures++;
    }
    request.arguments[0] = 18U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -22)) {
        failures++;
    }
    request.arguments[0] = UINT64_C(1) << 40U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -22)) {
        failures++;
    }
    request.arguments[0] = 17U;
    request.arguments[1] = 0x1000U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -95)) {
        failures++;
    }
    request.number = 260U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_WAIT4, 0)) {
        failures++;
    }
    return failures;
}

static unsigned long run_brk_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;
    struct kernel_syscall_request request = {
        .number = 214U,
        .arguments = {UINT64_C(0x12345678), 0U, 0U, 0U, 0U, 0U},
    };
    struct kernel_syscall_result result = {
        .action = KERNEL_SYSCALL_ACTION_EXIT,
        .value = INT64_C(0x11223344),
    };

    brk_borrow_status = KERNEL_TASK_STATUS_OK;
    brk_mm_status = KERNEL_MM_STATUS_OK;
    brk_mm_result = UINT64_C(0x12345000);
    brk_mm_requested = 0U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_RETURN,
                       INT64_C(0x12345000)) ||
        brk_mm_requested != request.arguments[0]) {
        return 1U;
    }

    brk_borrow_status = KERNEL_TASK_STATUS_STATE;
    result.action = KERNEL_SYSCALL_ACTION_EXIT;
    result.value = INT64_C(0x11223344);
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_EXIT,
                       INT64_C(0x11223344))) {
        brk_borrow_status = KERNEL_TASK_STATUS_OK;
        return 2U;
    }
    brk_borrow_status = KERNEL_TASK_STATUS_OK;
    return 0U;
}

unsigned long run_syscall_cases(void)
{
    unsigned long failures = run_invalid_argument_cases();

    failures += run_exit_cases();
    failures += run_unknown_cases();
    failures += run_process_decode_cases();
    failures += run_brk_cases();
    return failures;
}
