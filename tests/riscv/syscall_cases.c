#include <kernel/syscall.h>
#include <kernel/files.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/task.h>

#include <stdint.h>

static enum kernel_task_status brk_borrow_status =
    KERNEL_TASK_STATUS_OK;
static enum kernel_mm_status brk_mm_status = KERNEL_MM_STATUS_OK;
static uint64_t brk_mm_result;
static uint64_t brk_mm_requested;
static enum kernel_mm_status mmap_mm_status = KERNEL_MM_STATUS_OK;
static enum kernel_mm_status munmap_mm_status = KERNEL_MM_STATUS_OK;
static enum kernel_mm_status mprotect_mm_status = KERNEL_MM_STATUS_OK;
static uint64_t mmap_mm_result;
static uint64_t mmap_mm_hint;
static uint64_t mmap_mm_length;
static uint32_t mmap_mm_permissions;
static uint32_t mmap_mm_flags;
static uint64_t range_mm_address;
static uint64_t range_mm_length;
static uint32_t range_mm_permissions;
static enum kernel_task_status files_borrow_status =
    KERNEL_TASK_STATUS_OK;
static enum kernel_files_status pin_status = KERNEL_FILES_STATUS_OK;
static int64_t pin_linux_result;
static enum kernel_mm_status file_mmap_status = KERNEL_MM_STATUS_OK;
static int64_t file_mmap_fd;
static uint64_t file_mmap_offset;
static uint32_t file_release_calls;

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

enum kernel_mm_status __wrap_kernel_mm_mmap_anonymous(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U || address == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    mmap_mm_hint = hint;
    mmap_mm_length = length;
    mmap_mm_permissions = permissions;
    mmap_mm_flags = flags;
    if (mmap_mm_status == KERNEL_MM_STATUS_OK) {
        *address = mmap_mm_result;
    }
    return mmap_mm_status;
}

enum kernel_task_status __wrap_kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files)
{
    if (files_borrow_status != KERNEL_TASK_STATUS_OK) {
        return files_borrow_status;
    }
    if (task == 0 || files == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    *files = (struct kernel_files *)(uintptr_t)3U;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_files_status __wrap_kernel_files_pin(
    struct kernel_files *files,
    int64_t fd,
    struct kernel_open_file_description **owner,
    int64_t *linux_result)
{
    if (files != (struct kernel_files *)(uintptr_t)3U || owner == 0 ||
        *owner != 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    file_mmap_fd = fd;
    *linux_result = pin_linux_result;
    if (pin_status == KERNEL_FILES_STATUS_OK && pin_linux_result == 0) {
        *owner = (struct kernel_open_file_description *)(uintptr_t)4U;
    }
    return pin_status;
}

enum kernel_mm_status __wrap_kernel_mm_mmap_file_private(
    struct kernel_mm *mm,
    struct kernel_open_file_description **file,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U || file == 0 ||
        *file != (struct kernel_open_file_description *)(uintptr_t)4U ||
        address == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    mmap_mm_hint = hint;
    mmap_mm_length = length;
    mmap_mm_permissions = permissions;
    mmap_mm_flags = flags;
    file_mmap_offset = file_offset;
    if (file_mmap_status == KERNEL_MM_STATUS_OK) {
        *file = 0;
        *address = mmap_mm_result;
    }
    return file_mmap_status;
}

enum kernel_open_file_status __wrap_kernel_open_file_release(
    struct kernel_open_file_description **owner)
{
    if (owner == 0 ||
        *owner != (struct kernel_open_file_description *)(uintptr_t)4U) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    file_release_calls++;
    *owner = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_mm_status __wrap_kernel_mm_munmap(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    range_mm_address = address;
    range_mm_length = length;
    return munmap_mm_status;
}

enum kernel_mm_status __wrap_kernel_mm_mprotect(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    range_mm_address = address;
    range_mm_length = length;
    range_mm_permissions = permissions;
    return mprotect_mm_status;
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

static unsigned long run_memory_mapping_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;
    struct kernel_syscall_request request = {
        .number = 222U,
        .arguments = {UINT64_C(0x12000), UINT64_C(0x2345),
                      3U, UINT64_C(0x22), UINT64_MAX, 0U},
    };
    struct kernel_syscall_result result;
    unsigned long failures = 0U;

    brk_borrow_status = KERNEL_TASK_STATUS_OK;
    mmap_mm_status = KERNEL_MM_STATUS_OK;
    mmap_mm_result = UINT64_C(0x3f000);
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN,
                       INT64_C(0x3f000)) ||
        mmap_mm_hint != UINT64_C(0x12000) ||
        mmap_mm_length != UINT64_C(0x2345) ||
        mmap_mm_permissions != (KERNEL_MM_READ | KERNEL_MM_WRITE) ||
        mmap_mm_flags != 0U) {
        failures++;
    }
    request.arguments[3] = UINT64_C(0x100022);
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        mmap_mm_flags != KERNEL_MM_MAP_FIXED_NOREPLACE) {
        failures++;
    }
    mmap_mm_status = KERNEL_MM_STATUS_CONFLICT;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -17)) {
        failures++;
    }
    request.arguments[3] = UINT64_C(0x21);
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -95)) {
        failures++;
    }
    request.arguments[3] = UINT64_C(0x80000022);
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -22)) {
        failures++;
    }

    request.arguments[0] = UINT64_C(0x45000);
    request.arguments[1] = UINT64_C(0x3456);
    request.arguments[2] = 5U;
    request.arguments[3] = 2U;
    request.arguments[4] = 7U;
    request.arguments[5] = UINT64_C(0x2000);
    files_borrow_status = KERNEL_TASK_STATUS_OK;
    pin_status = KERNEL_FILES_STATUS_OK;
    pin_linux_result = 0;
    file_mmap_status = KERNEL_MM_STATUS_OK;
    mmap_mm_result = UINT64_C(0x88000);
    file_release_calls = 0U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_RETURN,
                       INT64_C(0x88000)) ||
        file_mmap_fd != 7 ||
        file_mmap_offset != UINT64_C(0x2000) ||
        mmap_mm_hint != UINT64_C(0x45000) ||
        mmap_mm_length != UINT64_C(0x3456) ||
        mmap_mm_permissions != (KERNEL_MM_READ | KERNEL_MM_EXECUTE) ||
        mmap_mm_flags != 0U || file_release_calls != 0U) {
        failures++;
    }
    file_mmap_status = KERNEL_MM_STATUS_NO_MEMORY;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -12) ||
        file_release_calls != 1U) {
        failures++;
    }
    pin_linux_result = -9;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -9) ||
        file_release_calls != 1U) {
        failures++;
    }
    pin_linux_result = 0;
    files_borrow_status = KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -9)) {
        failures++;
    }
    files_borrow_status = KERNEL_TASK_STATUS_OK;
    request.arguments[5] = 1U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -22)) {
        failures++;
    }
    request.arguments[5] = 0U;
    request.arguments[3] = 1U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -95)) {
        failures++;
    }

    request.number = 215U;
    request.arguments[0] = UINT64_C(0x23000);
    request.arguments[1] = UINT64_C(0x3456);
    munmap_mm_status = KERNEL_MM_STATUS_OK;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        range_mm_address != UINT64_C(0x23000) ||
        range_mm_length != UINT64_C(0x3456)) {
        failures++;
    }

    request.number = 226U;
    request.arguments[0] = UINT64_C(0x34000);
    request.arguments[1] = UINT64_C(0x4567);
    request.arguments[2] = 2U;
    mprotect_mm_status = KERNEL_MM_STATUS_NOT_MAPPED;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -12) ||
        range_mm_address != UINT64_C(0x34000) ||
        range_mm_length != UINT64_C(0x4567) ||
        range_mm_permissions != KERNEL_MM_WRITE) {
        failures++;
    }

    result.action = KERNEL_SYSCALL_ACTION_EXIT;
    result.value = INT64_C(0x55667788);
    mprotect_mm_status = KERNEL_MM_STATUS_STATE;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_EXIT,
                       INT64_C(0x55667788))) {
        failures++;
    }
    request.number = 222U;
    request.arguments[2] = 3U;
    request.arguments[3] = UINT64_C(0x22);
    request.arguments[5] = 0U;
    mmap_mm_status = KERNEL_MM_STATUS_STATE;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_EXIT,
                       INT64_C(0x55667788))) {
        failures++;
    }
    request.number = 215U;
    munmap_mm_status = KERNEL_MM_STATUS_STATE;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT ||
        result_changed(&result,
                       KERNEL_SYSCALL_ACTION_EXIT,
                       INT64_C(0x55667788))) {
        failures++;
    }
    return failures;
}

unsigned long run_syscall_cases(void)
{
    unsigned long failures = run_invalid_argument_cases();

    failures += run_exit_cases();
    failures += run_unknown_cases();
    failures += run_process_decode_cases();
    failures += run_brk_cases();
    failures += run_memory_mapping_cases();
    return failures;
}
