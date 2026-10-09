#include <kernel/log.h>
#include <kernel/errno.h>
#include <kernel/sync.h>
#include <arch/context.h>
#include <string.h>

#define RECORD_COUNT 512U
struct log_record { uint64_t start; uint16_t length; };
static char ring[KERNEL_LOG_CAPACITY], pending[KERNEL_LOG_RECORD_MAX];
static struct log_record records[RECORD_COUNT];
static uint64_t first_sequence, next_sequence, write_position, read_sequence,
                clear_sequence;
static unsigned pending_length, read_partial, console_level = 7;
static int saved_console_level = -1;
static struct kernel_mutex readers;
static struct kernel_wait_queue published;

static void initialize(void)
{
    if (!published.initialized) {
        kernel_wait_queue_init(&published);
        kernel_mutex_init(&readers, 1, (uintptr_t)&readers);
    }
}

int kernel_log_putc(unsigned level, char character)
{
    uintptr_t irq = arch_interrupt_save();
    initialize();
    if (level > 7) level = 7;
    int visible = level < console_level;
    if (!pending_length) {
        pending[0] = '<'; pending[1] = (char)('0' + level); pending[2] = '>';
        pending_length = 3;
    }
    pending[pending_length++] = character;
    if (character == '\n' || pending_length == sizeof(pending)) {
        /* Space is reclaimed by whole records, never by a borrowed pointer. */
        while (first_sequence < next_sequence &&
               (next_sequence - first_sequence == RECORD_COUNT ||
                write_position + pending_length - records[first_sequence % RECORD_COUNT].start > sizeof(ring))) {
            first_sequence++;
        }
        struct log_record *record = &records[next_sequence % RECORD_COUNT];
        record->start = write_position; record->length = pending_length;
        for (unsigned i = 0; i < pending_length; i++)
            ring[(write_position + i) % sizeof(ring)] = pending[i];
        write_position += pending_length; next_sequence++; pending_length = 0;
        kernel_wait_queue_wake_all(&published);
    }
    arch_interrupt_restore(irq);
    return visible;
}

static void clamp_reader(void)
{
    if (read_sequence < first_sequence) {
        read_sequence = first_sequence; read_partial = 0;
    }
}

int kernel_log_action(int action, int length, int privileged,
                      kernel_log_copy_fn copy, void *context, char scratch[KERNEL_LOG_RECORD_MAX])
{
    if (!privileged && action != 3 && action != 10) return -KERNEL_EPERM;
    if (action < 0 || action > 10) return -KERNEL_EINVAL;
    if (action >= 2 && action <= 4 && (length < 0 || !copy || !scratch))
        return -KERNEL_EINVAL;
    if (action >= 2 && action <= 4 && !length) return 0;
    if (action == 8 && (length < 1 || length > 8)) return -KERNEL_EINVAL;
    uintptr_t irq = arch_interrupt_save();
    initialize();
    if (action == 0 || action == 1) { arch_interrupt_restore(irq); return 0; }
    if (action >= 5) {
        int result = 0;
        if (action == 5) clear_sequence = next_sequence;
        else if (action == 6) {
            if (saved_console_level < 0) saved_console_level = console_level;
            console_level = 1;
        } else if (action == 7) {
            if (saved_console_level >= 0) console_level = saved_console_level;
            saved_console_level = -1;
        } else if (action == 8) { console_level = length; saved_console_level = -1; }
        else if (action == 9) {
            clamp_reader();
            for (uint64_t seq = read_sequence; seq < next_sequence; seq++)
                result += records[seq % RECORD_COUNT].length;
            result -= read_partial;
        } else result = sizeof(ring);
        arch_interrupt_restore(irq); return result;
    }
    uint64_t sequence = clear_sequence > first_sequence ? clear_sequence : first_sequence;
    uint64_t end = next_sequence;
    if (action != 2) {
        unsigned bytes = 0;
        for (uint64_t seq = sequence; seq < end; seq++) bytes += records[seq % RECORD_COUNT].length;
        while (sequence < end && bytes > (unsigned)length)
            bytes -= records[sequence++ % RECORD_COUNT].length;
    }
    arch_interrupt_restore(irq);
    struct kernel_lock_guard guard = {0};
    if (action == 2) kernel_mutex_lock(&readers, &guard);
    int total = 0;
    while (total < length) {
        irq = arch_interrupt_save();
        unsigned partial = 0;
        if (action == 2) { clamp_reader(); sequence = read_sequence; partial = read_partial; end = next_sequence; }
        if (sequence < first_sequence) sequence = first_sequence;
        if (sequence >= end) {
            if (action != 2 || total) { arch_interrupt_restore(irq); break; }
            enum kernel_wait_wake_reason reason;
            kernel_lock_release(&guard);
            enum kernel_scheduler_status status = KERNEL_WAIT_RECHECK(&published, 0, 1, &reason,
                (read_sequence >= next_sequence));
            arch_interrupt_restore(irq);
            if (status != KERNEL_SCHEDULER_STATUS_OK || reason == KERNEL_WAIT_SIGNALLED)
                return status == KERNEL_SCHEDULER_STATUS_OK ? -KERNEL_EINTR : -KERNEL_EIO;
            kernel_mutex_lock(&readers, &guard); continue;
        }
        struct log_record record = records[sequence % RECORD_COUNT];
        unsigned n = record.length - partial;
        if (n > (unsigned)(length - total)) {
            if (action != 2) { arch_interrupt_restore(irq); break; }
            n = length - total;
        }
        for (unsigned i = 0; i < n; i++) scratch[i] = ring[(record.start + partial + i) % sizeof(ring)];
        if (action == 2) {
            /* Linux consumes this chunk before usercopy, including a fault. */
            read_partial = partial + n;
            if (read_partial == record.length) { read_sequence++; read_partial = 0; }
        }
        arch_interrupt_restore(irq);
        if (copy(context, total, scratch, n)) { total = action == 2 && total ? total : -KERNEL_EFAULT; break; }
        total += n; sequence++;
    }
    if (guard.lock) kernel_lock_release(&guard);
    if (action == 4) {
        irq = arch_interrupt_save();
        if (sequence > clear_sequence) clear_sequence = sequence;
        arch_interrupt_restore(irq);
    }
    return total;
}
