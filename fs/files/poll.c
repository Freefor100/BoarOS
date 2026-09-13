#include "private.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KERNEL_POLL_STACK_CAPACITY 4U

struct kernel_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

_Static_assert(sizeof(struct kernel_pollfd) == 8U,
               "Linux pollfd ABI size must remain 8 bytes");

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

_Static_assert(sizeof(struct kernel_timespec) == 16U,
               "Linux timespec ABI size must remain 16 bytes");

struct kernel_sigset_argpack {
    uint64_t sigmask_ptr;
    uint64_t sigsetsize;
};

_Static_assert(sizeof(struct kernel_sigset_argpack) == 16U,
               "Linux sigset_argpack ABI size must remain 16 bytes");

static int poll_compute_deadline(
    struct kernel_mm *mm,
    uint64_t user_timeout,
    uint64_t *out_deadline,
    int *out_has_timeout,
    int *out_immediate)
{
    struct kernel_timespec ts;
    size_t copied = 0U;
    enum kernel_uaccess_status status;

    if (user_timeout == 0U) {
        *out_has_timeout = 0;
        *out_immediate = 0;
        *out_deadline = 0U;
        return 0;
    }
    *out_has_timeout = 1;
    status = kernel_copy_from_user(mm, &ts, user_timeout, sizeof(ts), &copied);
    if (status == KERNEL_UACCESS_STATUS_FAULT) {
        return -KERNEL_EFAULT;
    }
    if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(ts)) {
        return -KERNEL_EINVAL;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        (uint64_t)ts.tv_nsec >= 1000000000ULL) {
        return -KERNEL_EINVAL;
    }
    if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
        *out_immediate = 1;
        *out_deadline = 0U;
        return 0;
    }
    {
        unsigned __int128 requested =
            (unsigned __int128)(uint64_t)ts.tv_sec * 1000000000ULL +
            (uint64_t)ts.tv_nsec;
        uint64_t duration_ns =
            requested > (unsigned __int128)INT64_MAX ? (uint64_t)INT64_MAX
                                                     : (uint64_t)requested;
        uint64_t now_ns = kernel_time_monotonic_ns();
        uint64_t target_monotonic_ns =
            duration_ns > (uint64_t)INT64_MAX - now_ns ? (uint64_t)INT64_MAX
                                                       : now_ns + duration_ns;
        enum kernel_time_status time_status =
            kernel_time_deadline_from_monotonic(target_monotonic_ns,
                                                out_deadline);
        if (time_status == KERNEL_TIME_STATUS_DEADLINE_PASSED) {
            *out_immediate = 1;
            *out_deadline = 0U;
            return 0;
        }
        if (time_status != KERNEL_TIME_STATUS_OK) {
            return -KERNEL_EINVAL;
        }
    }
    *out_immediate = 0;
    return 0;
}

static int poll_setup_sigmask(
    struct kernel_mm *mm,
    struct kernel_task *task,
    uint64_t user_sigmask,
    size_t sigsetsize,
    uint64_t *saved_mask,
    int *mask_modified)
{
    uint64_t new_mask;
    size_t copied = 0U;
    enum kernel_uaccess_status status;

    if (user_sigmask == 0U) {
        *mask_modified = 0;
        return 0;
    }
    if (sigsetsize != sizeof(uint64_t)) {
        return -KERNEL_EINVAL;
    }
    status = kernel_copy_from_user(mm,
                                   &new_mask,
                                   user_sigmask,
                                   sizeof(new_mask),
                                   &copied);
    if (status == KERNEL_UACCESS_STATUS_FAULT) {
        return -KERNEL_EFAULT;
    }
    if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(new_mask)) {
        return -KERNEL_EINVAL;
    }
    if (kernel_signal_set_temporary_mask(task, new_mask, saved_mask) !=
        KERNEL_SIGNAL_STATUS_OK) {
        return -KERNEL_EINVAL;
    }
    *mask_modified = 1;
    return 0;
}

static void poll_restore_sigmask(
    struct kernel_task *task,
    uint64_t saved_mask,
    int mask_modified,
    int interrupted)
{
    if (mask_modified != 0) {
        kernel_signal_restore_temporary_mask(task, saved_mask, interrupted);
    }
}

static int core_poll_run(
    struct kernel_files *files,
    struct kernel_task *task,
    struct kernel_pollfd *pfds,
    uint32_t nfds,
    uint64_t deadline,
    int immediate,
    int mask_modified,
    uint64_t saved_mask)
{
    struct kernel_open_file_description *stack_pinned[KERNEL_POLL_STACK_CAPACITY];
    struct kernel_wait_node stack_nodes[KERNEL_POLL_STACK_CAPACITY];
    uint8_t stack_registered[KERNEL_POLL_STACK_CAPACITY];

    struct kernel_open_file_description **pinned = stack_pinned;
    struct kernel_wait_node *nodes = stack_nodes;
    uint8_t *registered = stack_registered;
    void *heap_block = 0;
    int ready_count = 0;
    int error = 0;

    if (nfds > KERNEL_POLL_STACK_CAPACITY) {
        size_t alloc_size =
            nfds * (sizeof(*pinned) + sizeof(*nodes) + sizeof(*registered));
        enum kernel_heap_status h_status =
            kernel_heap_allocate(files->heap, alloc_size, &heap_block);
        if (h_status != KERNEL_HEAP_STATUS_OK) {
            poll_restore_sigmask(task, saved_mask, mask_modified, 0);
            return -KERNEL_ENOMEM;
        }
        pinned = (struct kernel_open_file_description **)heap_block;
        nodes = (struct kernel_wait_node *)((uintptr_t)pinned +
                                            nfds * sizeof(*pinned));
        registered = (uint8_t *)((uintptr_t)nodes + nfds * sizeof(*nodes));
    }

    for (uint32_t i = 0; i < nfds; i++) {
        pinned[i] = 0;
        registered[i] = 0U;
        kernel_wait_node_init(&nodes[i], task);
    }

    /* First pass: pin descriptors and check for initial readiness */
    for (uint32_t i = 0; i < nfds; i++) {
        pfds[i].revents = 0;
        if (pfds[i].fd < 0) {
            continue;
        }
        int64_t pin_result = 0;
        enum kernel_files_status pin_status =
            kernel_files_pin(files, pfds[i].fd, &pinned[i], &pin_result);
        if (pin_status != KERNEL_FILES_STATUS_OK || pin_result != 0 ||
            pinned[i] == 0) {
            pinned[i] = 0;
            pfds[i].revents = (int16_t)KERNEL_POLLNVAL;
            ready_count++;
            continue;
        }
        struct kernel_wait_queue *wq = 0;
        uint32_t active = kernel_open_file_poll(
            pinned[i], (uint32_t)(uint16_t)pfds[i].events, &wq);
        uint32_t rev =
            active & ((uint32_t)(uint16_t)pfds[i].events | KERNEL_POLLERR |
                      KERNEL_POLLHUP);
        if (rev != 0U) {
            pfds[i].revents = (int16_t)rev;
            ready_count++;
        }
    }

    /* If events already ready, or immediate zero timeout, skip sleeping */
    if (ready_count == 0 && !immediate) {
        uintptr_t saved_intr = riscv_interrupt_save();

        /* Register wait nodes on OFD wait queues */
        for (uint32_t i = 0; i < nfds; i++) {
            if (pinned[i] != 0 && pfds[i].revents == 0) {
                struct kernel_wait_queue *wq = 0;
                (void)kernel_open_file_poll(
                    pinned[i], (uint32_t)(uint16_t)pfds[i].events, &wq);
                if (wq != 0) {
                    kernel_wait_queue_add(wq, &nodes[i]);
                    registered[i] = 1U;
                }
            }
        }

        /* Re-check readiness under disabled interrupts to avoid race */
        for (uint32_t i = 0; i < nfds; i++) {
            if (pinned[i] != 0 && pfds[i].revents == 0) {
                struct kernel_wait_queue *wq = 0;
                uint32_t active = kernel_open_file_poll(
                    pinned[i], (uint32_t)(uint16_t)pfds[i].events, &wq);
                uint32_t rev =
                    active & ((uint32_t)(uint16_t)pfds[i].events |
                              KERNEL_POLLERR | KERNEL_POLLHUP);
                if (rev != 0U) {
                    pfds[i].revents = (int16_t)rev;
                    ready_count++;
                }
            }
        }

        enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
        if (ready_count == 0) {
            /* Block current task until wake, deadline, or signal */
            (void)kernel_scheduler_block_current(0, deadline, 1, &wake_reason);
        }

        /* Unregister all wait nodes */
        for (uint32_t i = 0; i < nfds; i++) {
            if (registered[i] != 0U) {
                kernel_wait_queue_remove(&nodes[i]);
                registered[i] = 0U;
            }
        }

        riscv_interrupt_restore(saved_intr);

        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            error = -KERNEL_EINTR;
        }
    }

    int interrupted = (error == -KERNEL_EINTR);
    poll_restore_sigmask(task, saved_mask, mask_modified, interrupted);

    if (error == 0) {
        /* Compute final revents and count */
        ready_count = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            if (pfds[i].fd < 0) {
                pfds[i].revents = 0;
                continue;
            }
            if (pinned[i] == 0) {
                pfds[i].revents = (int16_t)KERNEL_POLLNVAL;
                ready_count++;
                continue;
            }
            struct kernel_wait_queue *wq = 0;
            uint32_t active = kernel_open_file_poll(
                pinned[i], (uint32_t)(uint16_t)pfds[i].events, &wq);
            uint32_t rev =
                active & ((uint32_t)(uint16_t)pfds[i].events | KERNEL_POLLERR |
                          KERNEL_POLLHUP);
            pfds[i].revents = (int16_t)rev;
            if (rev != 0U) {
                ready_count++;
            }
        }
    }

    /* Release all pinned OFDs */
    for (uint32_t i = 0; i < nfds; i++) {
        if (pinned[i] != 0) {
            (void)kernel_open_file_release(&pinned[i]);
        }
    }

    if (heap_block != 0) {
        (void)kernel_heap_release(files->heap, heap_block);
    }

    return error != 0 ? error : ready_count;
}

enum kernel_files_status kernel_files_ppoll(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    uint64_t user_fds,
    uint64_t nfds,
    uint64_t user_timeout,
    uint64_t user_sigmask,
    size_t sigsetsize,
    int64_t *linux_result)
{
    struct kernel_pollfd stack_pfds[KERNEL_POLL_STACK_CAPACITY];
    struct kernel_pollfd *pfds = stack_pfds;
    void *heap_pfds = 0;
    uint64_t deadline = 0U;
    int has_timeout = 0;
    int immediate = 0;
    uint64_t saved_mask = 0U;
    int mask_modified = 0;
    int err;

    if (files == 0 || mm == 0 || task == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (nfds > (uint64_t)KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }

    err = poll_compute_deadline(mm,
                                user_timeout,
                                &deadline,
                                &has_timeout,
                                &immediate);
    if (err != 0) {
        *linux_result = err;
        return KERNEL_FILES_STATUS_OK;
    }

    err = poll_setup_sigmask(mm,
                             task,
                             user_sigmask,
                             sigsetsize,
                             &saved_mask,
                             &mask_modified);
    if (err != 0) {
        *linux_result = err;
        return KERNEL_FILES_STATUS_OK;
    }

    if (nfds > 0U) {
        if (nfds > KERNEL_POLL_STACK_CAPACITY) {
            enum kernel_heap_status hs = kernel_heap_allocate(
                files->heap, nfds * sizeof(*pfds), &heap_pfds);
            if (hs != KERNEL_HEAP_STATUS_OK) {
                poll_restore_sigmask(task, saved_mask, mask_modified, 0);
                *linux_result = -KERNEL_ENOMEM;
                return KERNEL_FILES_STATUS_OK;
            }
            pfds = (struct kernel_pollfd *)heap_pfds;
        }
        size_t copied = 0U;
        enum kernel_uaccess_status ustatus =
            kernel_copy_from_user(mm,
                                  pfds,
                                  user_fds,
                                  nfds * sizeof(*pfds),
                                  &copied);
        if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
            if (heap_pfds != 0) {
                (void)kernel_heap_release(files->heap, heap_pfds);
            }
            poll_restore_sigmask(task, saved_mask, mask_modified, 0);
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (ustatus != KERNEL_UACCESS_STATUS_OK ||
            copied != nfds * sizeof(*pfds)) {
            if (heap_pfds != 0) {
                (void)kernel_heap_release(files->heap, heap_pfds);
            }
            poll_restore_sigmask(task, saved_mask, mask_modified, 0);
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
    }

    int ret = core_poll_run(files,
                            task,
                            pfds,
                            (uint32_t)nfds,
                            deadline,
                            immediate,
                            mask_modified,
                            saved_mask);

    if (ret >= 0 && nfds > 0U) {
        size_t copied = 0U;
        enum kernel_uaccess_status ustatus =
            kernel_copy_to_user(mm,
                                user_fds,
                                pfds,
                                nfds * sizeof(*pfds),
                                &copied);
        if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
            ret = -KERNEL_EFAULT;
        }
    }

    if (heap_pfds != 0) {
        (void)kernel_heap_release(files->heap, heap_pfds);
    }

    *linux_result = ret;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_pselect6(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    int64_t nfds,
    uint64_t user_readfds,
    uint64_t user_writefds,
    uint64_t user_exceptfds,
    uint64_t user_timeout,
    uint64_t user_sigdata,
    int64_t *linux_result)
{
    uint64_t stack_fds[6] = {0};
    uint64_t *in_rfds = &stack_fds[0];
    uint64_t *in_wfds = &stack_fds[1];
    uint64_t *in_efds = &stack_fds[2];
    uint64_t *out_rfds = &stack_fds[3];
    uint64_t *out_wfds = &stack_fds[4];
    uint64_t *out_efds = &stack_fds[5];
    void *heap_bitsets = 0;
    struct kernel_pollfd stack_pfds[KERNEL_POLL_STACK_CAPACITY];
    struct kernel_pollfd *pfds = stack_pfds;
    void *heap_pfds = 0;
    uint64_t deadline = 0U;
    int has_timeout = 0;
    int immediate = 0;
    uint64_t saved_mask = 0U;
    int mask_modified = 0;
    int sigmask_restored = 0;
    uint64_t user_sigmask = 0U;
    size_t sigsetsize = 0U;
    int err;

    if (files == 0 || mm == 0 || task == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (nfds < 0 || nfds > (int64_t)KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }

    /* Unpack sigset_argpack if provided */
    if (user_sigdata != 0U) {
        struct kernel_sigset_argpack argpack;
        size_t copied = 0U;
        enum kernel_uaccess_status ustatus =
            kernel_copy_from_user(mm,
                                  &argpack,
                                  user_sigdata,
                                  sizeof(argpack),
                                  &copied);
        if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (ustatus != KERNEL_UACCESS_STATUS_OK || copied != sizeof(argpack)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        user_sigmask = argpack.sigmask_ptr;
        sigsetsize = (size_t)argpack.sigsetsize;
    }

    err = poll_compute_deadline(mm,
                                user_timeout,
                                &deadline,
                                &has_timeout,
                                &immediate);
    if (err != 0) {
        *linux_result = err;
        return KERNEL_FILES_STATUS_OK;
    }

    err = poll_setup_sigmask(mm,
                             task,
                             user_sigmask,
                             sigsetsize,
                             &saved_mask,
                             &mask_modified);
    if (err != 0) {
        *linux_result = err;
        return KERNEL_FILES_STATUS_OK;
    }

    size_t words = ((size_t)nfds + 63U) / 64U;
    size_t bytes = words * sizeof(uint64_t);

    if (words > 1U) {
        enum kernel_heap_status hs = kernel_heap_allocate(
            files->heap, 6U * words * sizeof(uint64_t), &heap_bitsets);
        if (hs != KERNEL_HEAP_STATUS_OK) {
            poll_restore_sigmask(task, saved_mask, mask_modified, 0);
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        memset(heap_bitsets, 0, 6U * words * sizeof(uint64_t));
        in_rfds = (uint64_t *)heap_bitsets;
        in_wfds = in_rfds + words;
        in_efds = in_wfds + words;
        out_rfds = in_efds + words;
        out_wfds = out_rfds + words;
        out_efds = out_wfds + words;
    }

    int ret = 0;
    if (bytes > 0U) {
        size_t copied = 0U;
        if (user_readfds != 0U) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_from_user(mm,
                                      in_rfds,
                                      user_readfds,
                                      bytes,
                                      &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
                goto cleanup;
            }
        }
        if (user_writefds != 0U) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_from_user(mm,
                                      in_wfds,
                                      user_writefds,
                                      bytes,
                                      &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
                goto cleanup;
            }
        }
        if (user_exceptfds != 0U) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_from_user(mm,
                                      in_efds,
                                      user_exceptfds,
                                      bytes,
                                      &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
                goto cleanup;
            }
        }
    }

    /* Count active bits and check validity */
    uint32_t count = 0U;
    for (int32_t fd = 0; fd < nfds; fd++) {
        uint32_t widx = (uint32_t)fd / 64U;
        uint64_t bit = UINT64_C(1) << ((uint32_t)fd % 64U);
        int r = (in_rfds[widx] & bit) != 0U;
        int w = (in_wfds[widx] & bit) != 0U;
        int e = (in_efds[widx] & bit) != 0U;
        if (r || w || e) {
            /* Linux select: all queried fds must be open in files! */
            if (kernel_files_lookup_description(files, fd) == 0) {
                ret = -KERNEL_EBADF;
                goto cleanup;
            }
            count++;
        }
    }

    if (count > KERNEL_POLL_STACK_CAPACITY) {
        enum kernel_heap_status hs = kernel_heap_allocate(
            files->heap, count * sizeof(*pfds), &heap_pfds);
        if (hs != KERNEL_HEAP_STATUS_OK) {
            ret = -KERNEL_ENOMEM;
            goto cleanup;
        }
        pfds = (struct kernel_pollfd *)heap_pfds;
    }

    uint32_t idx = 0U;
    for (int32_t fd = 0; fd < nfds; fd++) {
        uint32_t widx = (uint32_t)fd / 64U;
        uint64_t bit = UINT64_C(1) << ((uint32_t)fd % 64U);
        int r = (in_rfds[widx] & bit) != 0U;
        int w = (in_wfds[widx] & bit) != 0U;
        int e = (in_efds[widx] & bit) != 0U;
        if (r || w || e) {
            pfds[idx].fd = fd;
            pfds[idx].events =
                (int16_t)((r ? (KERNEL_POLLIN | KERNEL_POLLRDNORM) : 0) |
                          (w ? (KERNEL_POLLOUT | KERNEL_POLLWRNORM) : 0) |
                          (e ? KERNEL_POLLPRI : 0));
            pfds[idx].revents = 0;
            idx++;
        }
    }

    ret = core_poll_run(files,
                        task,
                        pfds,
                        count,
                        deadline,
                        immediate,
                        mask_modified,
                        saved_mask);
    sigmask_restored = 1;

    if (ret >= 0) {
        int total_bits = 0;
        for (uint32_t i = 0; i < count; i++) {
            int32_t fd = pfds[i].fd;
            uint32_t widx = (uint32_t)fd / 64U;
            uint64_t bit = UINT64_C(1) << ((uint32_t)fd % 64U);
            int r = (in_rfds[widx] & bit) != 0U;
            int w = (in_wfds[widx] & bit) != 0U;
            int e = (in_efds[widx] & bit) != 0U;
            if (r &&
                (pfds[i].revents &
                 (KERNEL_POLLIN | KERNEL_POLLRDNORM | KERNEL_POLLHUP |
                  KERNEL_POLLERR)) != 0) {
                out_rfds[widx] |= bit;
                total_bits++;
            }
            if (w &&
                (pfds[i].revents & (KERNEL_POLLOUT | KERNEL_POLLWRNORM)) != 0) {
                out_wfds[widx] |= bit;
                total_bits++;
            }
            if (e && (pfds[i].revents & KERNEL_POLLPRI) != 0) {
                out_efds[widx] |= bit;
                total_bits++;
            }
        }
        size_t copied = 0U;
        if (user_readfds != 0U) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_to_user(mm,
                                    user_readfds,
                                    out_rfds,
                                    bytes,
                                    &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
            }
        }
        if (user_writefds != 0U && ret >= 0) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_to_user(mm,
                                    user_writefds,
                                    out_wfds,
                                    bytes,
                                    &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
            }
        }
        if (user_exceptfds != 0U && ret >= 0) {
            enum kernel_uaccess_status ustatus =
                kernel_copy_to_user(mm,
                                    user_exceptfds,
                                    out_efds,
                                    bytes,
                                    &copied);
            if (ustatus == KERNEL_UACCESS_STATUS_FAULT) {
                ret = -KERNEL_EFAULT;
            }
        }
        if (ret >= 0) {
            ret = total_bits;
        }
    }

cleanup:
    if (!sigmask_restored) {
        poll_restore_sigmask(task, saved_mask, mask_modified, 0);
    }
    if (heap_pfds != 0) {
        (void)kernel_heap_release(files->heap, heap_pfds);
    }
    if (heap_bitsets != 0) {
        (void)kernel_heap_release(files->heap, heap_bitsets);
    }

    *linux_result = ret;
    return KERNEL_FILES_STATUS_OK;
}
