#include <kernel/syscall.h>

#include <stdint.h>

static unsigned long result_changed(
    const struct kernel_syscall_result *result,
    enum kernel_syscall_action action,
    int64_t value)
{
    return result->action != action || result->value != value;
}

static unsigned long run_invalid_argument_cases(void)
{
    struct kernel_syscall_request request = {0};
    struct kernel_syscall_result result = {
        .action = (enum kernel_syscall_action)0x55,
        .value = INT64_C(0x1122334455667788),
    };
    unsigned long failures = 0U;

    if (kernel_syscall_dispatch(0, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       (enum kernel_syscall_action)0x55,
                       INT64_C(0x1122334455667788))) {
        failures++;
    }
    if (kernel_syscall_dispatch(&request, 0) !=
        KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT) {
        failures++;
    }

    return failures;
}

static unsigned long run_exit_cases(void)
{
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

    if (kernel_syscall_dispatch(&request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_EXIT, 0xab)) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_unknown_cases(void)
{
    static const uint64_t numbers[] = {0U, 94U, UINT64_MAX};
    struct kernel_syscall_request request = {0};
    struct kernel_syscall_result result;
    unsigned long failures = 0U;
    unsigned long index;

    for (index = 0U; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
        request.number = numbers[index];
        result.action = KERNEL_SYSCALL_ACTION_EXIT;
        result.value = 1;
        if (kernel_syscall_dispatch(&request, &result) !=
                KERNEL_SYSCALL_STATUS_OK ||
            result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -38)) {
            failures++;
        }
    }

    return failures;
}

unsigned long run_syscall_cases(void)
{
    unsigned long failures = run_invalid_argument_cases();

    failures += run_exit_cases();
    failures += run_unknown_cases();
    return failures;
}
