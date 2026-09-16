#include "exec_internal.h"

#include <kernel/errno.h>
#include <kernel/elf64_source.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_EXEC_STRING_LIMIT UINT64_C(0x20000)
#define KERNEL_EXEC_VECTOR_LIMIT \
    (KERNEL_EXEC_STRING_LIMIT / sizeof(uint64_t))
#define KERNEL_EXEC_INITIAL_STRING_CAPACITY 256U
#define KERNEL_EXEC_INITIAL_VECTOR_CAPACITY 8U

enum exec_capture_status {
    EXEC_CAPTURE_OK = 0,
    EXEC_CAPTURE_LINUX_ERROR,
    EXEC_CAPTURE_STATE,
};

static int mm_owner_absent(const struct kernel_mm *mm)
{
    return mm->state == KERNEL_MM_EMPTY ||
           mm->state == KERNEL_MM_MOVED ||
           mm->state == KERNEL_MM_RELEASED;
}

static int image_cleanup_complete(const struct kernel_exec_image *image)
{
    return mm_owner_absent(&image->mm) && image->arch_private == 0;
}

static int transaction_resources_empty(
    const struct kernel_exec_transaction *transaction)
{
    return transaction->original_path == 0 &&
           transaction->resolved_path == 0 &&
           transaction->string_bytes == 0 &&
           transaction->arguments == 0 &&
           transaction->environment == 0 &&
           transaction->executable_file == 0 &&
           transaction->interpreter_file == 0 &&
           transaction->executable_source == 0 &&
           transaction->interpreter_source == 0 &&
           image_cleanup_complete(&transaction->image) &&
           mm_owner_absent(&transaction->retired_mm);
}

static int release_allocation(struct kernel_exec_transaction *transaction,
                              void **pointer)
{
    if (*pointer == 0) {
        return 1;
    }
    (void)kernel_heap_release(transaction->heap, *pointer);
    *pointer = 0;
    return 1;
}

enum kernel_exec_status kernel_exec_transaction_cleanup(
    struct kernel_exec_transaction *transaction)
{
    int failed = 0;

    if (transaction == 0 || transaction->heap == 0 ||
        transaction->state == KERNEL_EXEC_TRANSACTION_PREPARED) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    if (kernel_exec_image_cleanup(transaction->heap,
                                  &transaction->image) !=
        KERNEL_EXEC_IMAGE_STATUS_OK) {
        failed = 1;
    }
    if (transaction->retired_mm.state == KERNEL_MM_LIVE ||
        transaction->retired_mm.state == KERNEL_MM_CLEANUP) {
        if (kernel_mm_release(&transaction->retired_mm) !=
            KERNEL_MM_STATUS_OK) {
            failed = 1;
        }
    }
    if (transaction->interpreter_source != 0) {
        if (kernel_elf64_source_release(&transaction->interpreter_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
            failed = 1;
        }
    }
    if (transaction->executable_source != 0) {
        if (kernel_elf64_source_release(&transaction->executable_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
            failed = 1;
        }
    }
    if (transaction->interpreter_file != 0) {
        if (kernel_open_file_release(&transaction->interpreter_file) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            failed = 1;
        }
    }
    if (transaction->executable_file != 0) {
        if (kernel_open_file_release(&transaction->executable_file) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            failed = 1;
        }
    }
    if (!release_allocation(transaction,
                            (void **)&transaction->environment)) {
        failed = 1;
    }
    if (!release_allocation(transaction,
                            (void **)&transaction->arguments)) {
        failed = 1;
    }
    if (!release_allocation(transaction,
                            (void **)&transaction->string_bytes)) {
        failed = 1;
    }
    if (!release_allocation(transaction,
                            (void **)&transaction->resolved_path)) {
        failed = 1;
    }
    if (!release_allocation(transaction,
                            (void **)&transaction->original_path)) {
        failed = 1;
    }
    if (!failed && transaction_resources_empty(transaction)) {
        transaction->state = KERNEL_EXEC_TRANSACTION_EMPTY_CLEANUP;
        return KERNEL_EXEC_STATUS_OK;
    }
    return KERNEL_EXEC_STATUS_CLEANUP_REQUIRED;
}

static enum kernel_exec_status drain_attached_transaction(
    struct kernel_task *task)
{
    struct kernel_exec_transaction *transaction;
    enum kernel_exec_status status =
        kernel_task_exec_borrow(task, &transaction);

    if (status != KERNEL_EXEC_STATUS_OK) {
        return status;
    }
    if (transaction == 0) {
        return KERNEL_EXEC_STATUS_OK;
    }
    if (transaction->state == KERNEL_EXEC_TRANSACTION_PREPARED) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    status = kernel_exec_transaction_cleanup(transaction);
    if (status != KERNEL_EXEC_STATUS_OK) {
        return status;
    }
    return kernel_task_exec_finish(task, transaction);
}

static enum exec_capture_status grow_allocation(
    struct kernel_exec_transaction *transaction,
    void *old_pointer,
    size_t size,
    void **new_pointer,
    int64_t *linux_result)
{
    enum kernel_heap_status status = kernel_heap_resize(
        transaction->heap,
        old_pointer,
        size,
        new_pointer);

    if (status == KERNEL_HEAP_STATUS_OK) {
        return EXEC_CAPTURE_OK;
    }
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return EXEC_CAPTURE_LINUX_ERROR;
    }
    return EXEC_CAPTURE_STATE;
}

static enum exec_capture_status reserve_string_space(
    struct kernel_exec_transaction *transaction,
    int64_t *linux_result)
{
    size_t capacity;
    char *resized = transaction->string_bytes;
    enum exec_capture_status status;

    if (transaction->string_size < transaction->string_capacity) {
        return EXEC_CAPTURE_OK;
    }
    if (transaction->string_capacity == KERNEL_EXEC_STRING_LIMIT) {
        *linux_result = -KERNEL_E2BIG;
        return EXEC_CAPTURE_LINUX_ERROR;
    }
    capacity = transaction->string_capacity == 0U
                   ? KERNEL_EXEC_INITIAL_STRING_CAPACITY
                   : transaction->string_capacity * 2U;
    if (capacity > KERNEL_EXEC_STRING_LIMIT) {
        capacity = KERNEL_EXEC_STRING_LIMIT;
    }
    status = grow_allocation(transaction,
                             transaction->string_bytes,
                             capacity,
                             (void **)&resized,
                             linux_result);
    if (status == EXEC_CAPTURE_OK) {
        transaction->string_bytes = resized;
        transaction->string_capacity = capacity;
    }
    return status;
}

static enum exec_capture_status reserve_vector_slot(
    struct kernel_exec_transaction *transaction,
    struct kernel_exec_string **strings,
    size_t count,
    size_t *capacity,
    int64_t *linux_result)
{
    struct kernel_exec_string *resized = *strings;
    size_t new_capacity;
    enum exec_capture_status status;

    if (count < *capacity) {
        return EXEC_CAPTURE_OK;
    }
    if (count >= KERNEL_EXEC_VECTOR_LIMIT) {
        *linux_result = -KERNEL_E2BIG;
        return EXEC_CAPTURE_LINUX_ERROR;
    }
    new_capacity = *capacity == 0U
                       ? KERNEL_EXEC_INITIAL_VECTOR_CAPACITY
                       : *capacity * 2U;
    if (new_capacity > KERNEL_EXEC_VECTOR_LIMIT) {
        new_capacity = KERNEL_EXEC_VECTOR_LIMIT;
    }
    status = grow_allocation(transaction,
                             *strings,
                             new_capacity * sizeof(*resized),
                             (void **)&resized,
                             linux_result);
    if (status == EXEC_CAPTURE_OK) {
        *strings = resized;
        *capacity = new_capacity;
    }
    return status;
}

static enum exec_capture_status append_user_string(
    struct kernel_exec_transaction *transaction,
    struct kernel_mm *mm,
    uint64_t user_string,
    struct kernel_exec_string *string,
    int64_t *linux_result)
{
    size_t start = transaction->string_size;

    for (;;) {
        size_t length;
        size_t available;
        uint64_t source_offset =
            (uint64_t)(transaction->string_size - start);
        enum kernel_uaccess_status access_status;
        enum exec_capture_status capture_status =
            reserve_string_space(transaction, linux_result);

        if (capture_status != EXEC_CAPTURE_OK) {
            return capture_status;
        }
        available = transaction->string_capacity -
                    transaction->string_size;
        if (source_offset > UINT64_MAX - user_string) {
            *linux_result = -KERNEL_EFAULT;
            return EXEC_CAPTURE_LINUX_ERROR;
        }
        access_status = kernel_copy_string_from_user(
            mm,
            transaction->string_bytes + transaction->string_size,
            user_string + source_offset,
            available,
            &length);
        if (access_status == KERNEL_UACCESS_STATUS_OK) {
            transaction->string_size += length + 1U;
            string->bytes = (const char *)(uintptr_t)(start + 1U);
            string->length = transaction->string_size - start - 1U;
            return EXEC_CAPTURE_OK;
        }
        if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) {
            transaction->string_size += length;
            if (transaction->string_size >= KERNEL_EXEC_STRING_LIMIT) {
                *linux_result = -KERNEL_E2BIG;
                return EXEC_CAPTURE_LINUX_ERROR;
            }
            continue;
        }
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return EXEC_CAPTURE_LINUX_ERROR;
        }
        return EXEC_CAPTURE_STATE;
    }
}

static enum exec_capture_status capture_vector(
    struct kernel_exec_transaction *transaction,
    struct kernel_mm *mm,
    uint64_t user_vector,
    int arguments,
    int64_t *linux_result)
{
    struct kernel_exec_string **strings = arguments
        ? &transaction->arguments
        : &transaction->environment;
    size_t *count = arguments ? &transaction->argument_count
                              : &transaction->environment_count;
    size_t *capacity = arguments ? &transaction->argument_capacity
                                 : &transaction->environment_capacity;
    size_t index;

    if (user_vector == 0U) {
        enum exec_capture_status capture_status;

        if (!arguments) {
            return EXEC_CAPTURE_OK;
        }
        capture_status = reserve_vector_slot(transaction,
                                             strings,
                                             *count,
                                             capacity,
                                             linux_result);
        if (capture_status != EXEC_CAPTURE_OK) {
            return capture_status;
        }
        (*strings)[0].bytes = "";
        (*strings)[0].length = 0U;
        *count = 1U;
        return EXEC_CAPTURE_OK;
    }
    for (index = 0U; index < KERNEL_EXEC_VECTOR_LIMIT; index++) {
        uint64_t user_string;
        size_t copied = 0U;
        enum kernel_uaccess_status access_status;
        enum exec_capture_status capture_status;

        if (user_vector > UINT64_MAX - index * sizeof(uint64_t)) {
            *linux_result = -KERNEL_EFAULT;
            return EXEC_CAPTURE_LINUX_ERROR;
        }
        access_status = kernel_copy_from_user(
            mm,
            &user_string,
            user_vector + index * sizeof(uint64_t),
            sizeof(user_string),
            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return EXEC_CAPTURE_LINUX_ERROR;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(user_string)) {
            return EXEC_CAPTURE_STATE;
        }
        if (user_string == 0U) {
            if (arguments && index == 0U) {
                return capture_vector(transaction,
                                      mm,
                                      0U,
                                      1,
                                      linux_result);
            }
            return EXEC_CAPTURE_OK;
        }
        capture_status = reserve_vector_slot(transaction,
                                             strings,
                                             *count,
                                             capacity,
                                             linux_result);
        if (capture_status != EXEC_CAPTURE_OK) {
            return capture_status;
        }
        capture_status = append_user_string(transaction,
                                            mm,
                                            user_string,
                                            &(*strings)[*count],
                                            linux_result);
        if (capture_status != EXEC_CAPTURE_OK) {
            return capture_status;
        }
        (*count)++;
    }
    *linux_result = -KERNEL_E2BIG;
    return EXEC_CAPTURE_LINUX_ERROR;
}

static void resolve_staged_strings(struct kernel_exec_string *strings,
                                   size_t count,
                                   const char *bytes)
{
    size_t index;

    for (index = 0U; index < count; index++) {
        uintptr_t encoded = (uintptr_t)strings[index].bytes;

        if (encoded != 0U && encoded <= KERNEL_EXEC_STRING_LIMIT) {
            strings[index].bytes = bytes + encoded - 1U;
        }
    }
}

static enum kernel_exec_status finish_prepare_failure(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction,
    int64_t failure,
    int64_t *linux_result)
{
    enum kernel_exec_status cleanup_status =
        kernel_exec_transaction_cleanup(transaction);

    *linux_result = failure;
    if (cleanup_status == KERNEL_EXEC_STATUS_OK) {
        (void)kernel_task_exec_finish(task, transaction);
    }
    return KERNEL_EXEC_STATUS_OK;
}

static enum kernel_exec_status finish_prepare_state(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction)
{
    enum kernel_exec_status cleanup_status =
        kernel_exec_transaction_cleanup(transaction);

    if (cleanup_status == KERNEL_EXEC_STATUS_OK) {
        (void)kernel_task_exec_finish(task, transaction);
    }
    return KERNEL_EXEC_STATUS_STATE;
}

enum kernel_exec_status kernel_execve_prepare(
    struct kernel_task *task,
    uint64_t user_filename,
    uint64_t user_argv,
    uint64_t user_envp,
    int64_t *linux_result)
{
    struct kernel_mm *mm;
    struct kernel_files *files;
    const struct kernel_fs_context *fs;
    struct kernel_exec_transaction *transaction;
    struct kernel_vfs_mount *mount;
    const char *interpreter_path;
    size_t interpreter_length;
    struct kernel_exec_image_request image_request;
    size_t filename_length;
    int path_result;
    enum kernel_exec_image_status image_status;
    enum kernel_heap_status heap_status;
    enum kernel_uaccess_status access_status;
    enum exec_capture_status capture_status;
    enum kernel_exec_status exec_status;

    if (task == 0 || linux_result == 0) {
        return KERNEL_EXEC_STATUS_INVALID_ARGUMENT;
    }
    exec_status = drain_attached_transaction(task);
    if (exec_status != KERNEL_EXEC_STATUS_OK) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    if (kernel_task_mm_borrow_mutable(task, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_task_files_borrow(task, &files) != KERNEL_TASK_STATUS_OK ||
        kernel_task_fs_context_borrow(task, &fs) != KERNEL_TASK_STATUS_OK) {
        *linux_result = -KERNEL_ENODEV;
        return KERNEL_EXEC_STATUS_OK;
    }
    heap_status = kernel_heap_allocate_zeroed(files->heap,
                                              1U,
                                              sizeof(*transaction),
                                              (void **)&transaction);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_EXEC_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    transaction->heap = files->heap;
    transaction->state = KERNEL_EXEC_TRANSACTION_PREPARING;
    if (kernel_task_exec_attach(task, transaction) !=
        KERNEL_EXEC_STATUS_OK) {
        (void)kernel_heap_release(files->heap, transaction);
        return KERNEL_EXEC_STATUS_STATE;
    }
    heap_status = kernel_heap_allocate(transaction->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&transaction->original_path);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? finish_prepare_failure(task,
                                            transaction,
                                            -KERNEL_ENOMEM,
                                            linux_result)
                   : finish_prepare_state(task, transaction);
    }
    access_status = kernel_copy_string_from_user(
        mm,
        transaction->original_path,
        user_filename,
        KERNEL_FS_PATH_MAX,
        &filename_length);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        return finish_prepare_failure(task,
                                      transaction,
                                      -KERNEL_EFAULT,
                                      linux_result);
    }
    if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) {
        return finish_prepare_failure(task,
                                      transaction,
                                      -KERNEL_ENAMETOOLONG,
                                      linux_result);
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK) {
        return finish_prepare_state(task, transaction);
    }
    heap_status = kernel_heap_allocate(transaction->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&transaction->resolved_path);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? finish_prepare_failure(task,
                                            transaction,
                                            -KERNEL_ENOMEM,
                                            linux_result)
                   : finish_prepare_state(task, transaction);
    }
    if (kernel_fs_context_resolve_kernel_path(
            fs,
            KERNEL_FS_AT_FDCWD,
            transaction->original_path,
            filename_length,
            transaction->resolved_path,
            KERNEL_FS_PATH_MAX,
            &mount,
            &path_result) != KERNEL_FS_CONTEXT_STATUS_OK) {
        return finish_prepare_state(task, transaction);
    }
    if (path_result != 0) {
        return finish_prepare_failure(task,
                                      transaction,
                                      path_result,
                                      linux_result);
    }
    if (kernel_open_file_create_executable(transaction->heap,
                                           mount,
                                           transaction->resolved_path,
                                           &transaction->executable_file,
                                           &path_result) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
        return finish_prepare_state(task, transaction);
    }
    if (path_result != 0) {
        return finish_prepare_failure(task,
                                      transaction,
                                      path_result,
                                      linux_result);
    }
    {
        enum kernel_elf64_source_status source_status =
            kernel_elf64_source_create(transaction->heap,
                                       &transaction->executable_file,
                                       BOAROS_PAGE_SIZE,
                                       KERNEL_ELF64_MACHINE_RISCV,
                                       &transaction->executable_source);

        if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
            if (source_status == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY) {
                return finish_prepare_failure(task,
                                              transaction,
                                              -KERNEL_ENOMEM,
                                              linux_result);
            }
            if (source_status == KERNEL_ELF64_SOURCE_STATUS_IO) {
                return finish_prepare_failure(task,
                                              transaction,
                                              -KERNEL_EIO,
                                              linux_result);
            }
            return finish_prepare_failure(task,
                                          transaction,
                                          -KERNEL_ENOEXEC,
                                          linux_result);
        }
    }
    interpreter_path = kernel_elf64_source_interpreter(
        transaction->executable_source,
        &interpreter_length);
    if (interpreter_path != 0) {
        if (kernel_fs_context_resolve_kernel_path(
                fs,
                KERNEL_FS_AT_FDCWD,
                interpreter_path,
                interpreter_length,
                transaction->resolved_path,
                KERNEL_FS_PATH_MAX,
                &mount,
                &path_result) != KERNEL_FS_CONTEXT_STATUS_OK) {
            return finish_prepare_state(task, transaction);
        }
        if (path_result != 0) {
            return finish_prepare_failure(task,
                                          transaction,
                                          path_result,
                                          linux_result);
        }
        if (kernel_open_file_create_executable(transaction->heap,
                                               mount,
                                               transaction->resolved_path,
                                               &transaction->interpreter_file,
                                               &path_result) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
            return finish_prepare_state(task, transaction);
        }
        if (path_result != 0) {
            return finish_prepare_failure(task,
                                          transaction,
                                          path_result,
                                          linux_result);
        }
        {
            enum kernel_elf64_source_status source_status =
                kernel_elf64_source_create(transaction->heap,
                                           &transaction->interpreter_file,
                                           BOAROS_PAGE_SIZE,
                                           KERNEL_ELF64_MACHINE_RISCV,
                                           &transaction->interpreter_source);

            if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
                int64_t interpreter_error = -KERNEL_ELIBBAD;

                if (source_status == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY) {
                    interpreter_error = -KERNEL_ENOMEM;
                } else if (source_status == KERNEL_ELF64_SOURCE_STATUS_IO) {
                    interpreter_error = -KERNEL_EIO;
                }
                return finish_prepare_failure(task,
                                              transaction,
                                              interpreter_error,
                                              linux_result);
            }
            if (kernel_elf64_source_interpreter(transaction->interpreter_source,
                                                0) != 0) {
                return finish_prepare_failure(task,
                                              transaction,
                                              -KERNEL_ELIBBAD,
                                              linux_result);
            }
        }
    }
    capture_status = capture_vector(transaction,
                                    mm,
                                    user_argv,
                                    1,
                                    linux_result);
    if (capture_status == EXEC_CAPTURE_LINUX_ERROR) {
        return finish_prepare_failure(task,
                                      transaction,
                                      *linux_result,
                                      linux_result);
    }
    if (capture_status != EXEC_CAPTURE_OK) {
        return finish_prepare_state(task, transaction);
    }
    capture_status = capture_vector(transaction,
                                    mm,
                                    user_envp,
                                    0,
                                    linux_result);
    if (capture_status == EXEC_CAPTURE_LINUX_ERROR) {
        return finish_prepare_failure(task,
                                      transaction,
                                      *linux_result,
                                      linux_result);
    }
    if (capture_status != EXEC_CAPTURE_OK) {
        return finish_prepare_state(task, transaction);
    }
    resolve_staged_strings(transaction->arguments,
                           transaction->argument_count,
                           transaction->string_bytes);
    resolve_staged_strings(transaction->environment,
                           transaction->environment_count,
                           transaction->string_bytes);
    image_request.executable_source = transaction->executable_source;
    image_request.interpreter_source = transaction->interpreter_source;
    {
        struct kernel_rlimit64 stack_limit;

        if (kernel_task_get_rlimit(task, KERNEL_RLIMIT_STACK,
                                   &stack_limit) != KERNEL_TASK_STATUS_OK)
            return finish_prepare_state(task, transaction);
        image_request.stack_limit = stack_limit.current;
        image_request.stack_limit_valid = 1U;
    }
    image_request.executable.bytes = transaction->original_path;
    image_request.executable.length = filename_length;
    image_request.arguments = transaction->arguments;
    image_request.argument_count = transaction->argument_count;
    image_request.environment = transaction->environment;
    image_request.environment_count = transaction->environment_count;
    image_status = kernel_exec_image_prepare(&image_request,
                                             transaction->heap,
                                             &transaction->image,
                                             linux_result);
    if (image_status == KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR) {
        return finish_prepare_failure(task,
                                      transaction,
                                      *linux_result,
                                      linux_result);
    }
    if (image_status != KERNEL_EXEC_IMAGE_STATUS_OK) {
        (void)finish_prepare_state(task, transaction);
        return image_status == KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED
                   ? KERNEL_EXEC_STATUS_CLEANUP_REQUIRED
                   : KERNEL_EXEC_STATUS_STATE;
    }
    transaction->state = KERNEL_EXEC_TRANSACTION_PREPARED;
    *linux_result = 0;
    return KERNEL_EXEC_STATUS_OK;
}
