#include "char_device_internal.h"
#include "files/private.h"
#include "open_file_internal.h"
#include "uaccess_iov_internal.h"
#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/tty_task.h>
#include <string.h>

#define RING 4096U
#define MASK (RING - 1U)
#define ORDER (2U * RING)
#define IGNBRK 1U
#define BRKINT 2U
#define IGNPAR 4U
#define PARMRK 8U
#define INPCK 16U
#define ISTRIP 32U
#define INLCR 64U
#define IGNCR 128U
#define ICRNL 256U
#define IXON 1024U
#define IXANY 2048U
#define IXOFF 4096U
#define IUTF8 16384U
#define OPOST 1U
#define ONLCR 4U
#define OCRNL 8U
#define ONOCR 16U
#define ONLRET 32U
#define TAB3 0x1800U
#define ISIG 1U
#define ICANON 2U
#define ECHO 8U
#define ECHOE 16U
#define ECHOK 32U
#define ECHONL 64U
#define NOFLSH 128U
#define TOSTOP 256U
#define ECHOCTL 512U
#define ECHOKE 2048U
#define IEXTEN 32768U
#define EXTPROC 65536U
#define O_NOCTTY 0400U
#define VINTR 0U
#define VQUIT 1U
#define VERASE 2U
#define VKILL 3U
#define VEOF 4U
#define VTIME 5U
#define VMIN 6U
#define VSTART 8U
#define VSTOP 9U
#define VSUSP 10U
#define VEOL 11U
#define VREPRINT 12U
#define VWERASE 14U
#define VLNEXT 15U
#define VEOL2 16U

struct kernel_tty {
    struct kernel_heap *heap;
    struct kernel_tty_transport transport;
    void *transport_owner;
    struct kernel_tty_termios settings;
    struct kernel_wait_queue events;
    struct kernel_tty_request *reader, *writer, *mode_owner;
    struct kernel_pid *session, *foreground;
    uint64_t rdev, generation, head, tail, canonical_head;
    uint64_t tx_head, tx_tail, echo_head, echo_tail, order_head, order_tail;
    uint32_t references, opens, column;
    uint32_t ispeed, ospeed;
    uint16_t rows, columns, xpixel, ypixel;
    uint8_t stopped, flow_stopped, throttled, xchar, xchar_valid, quoted, shutdown;
    uint8_t master, packet, packet_status, deferred_flush, nonblocking_rx;
    unsigned char input[RING], flags[RING], width[RING];
    unsigned char tx[RING], echo[RING], order[ORDER];
};
struct tty_instance {
    struct kernel_tty *tty;
    struct kernel_heap *heap;
    uint64_t generation;
    uint8_t console;
};
struct kernel_tty_request {
    struct tty_instance *instance;
    struct kernel_task *caller;
    unsigned char *write_scratch;
    struct kernel_files *files;
    struct kernel_open_file_description *file;
    struct kernel_open_file_description **file_owner;
    void *vectors;
    void **vector_owner;
    uint64_t deadline_ns;
    unsigned min, time;
    uint8_t registered, reading, mode, continuation, write_qualified, initial_canonical, packet_started, writing;
};
_Static_assert(sizeof(struct kernel_tty_request) <= 160, "TTY request stack budget");
static struct kernel_tty *serial_tty;
static struct kernel_char_device devices[3];
static int device_read(void *, struct kernel_task *, uint32_t, void *, size_t, size_t *);
static int device_write(void *, struct kernel_task *, uint32_t, const void *, size_t,
                        size_t *);
static int device_ioctl(void *, struct kernel_task *, struct kernel_files *,
                        struct kernel_open_file_description **, struct kernel_mm *, uint64_t,
                        uint64_t);
static uint32_t device_poll(void *, uint32_t, uint32_t, struct kernel_wait_queue **);
static enum kernel_files_status
device_readv(void *, struct kernel_task *, struct kernel_files *,
             struct kernel_open_file_description **, void **, struct kernel_mm *,
             const struct kernel_uaccess_iovec *, size_t, uint64_t, uint32_t, int64_t *);
static enum kernel_files_status
device_writev(void *, struct kernel_task *, struct kernel_files *,
              struct kernel_open_file_description **, void **, struct kernel_mm *,
              const struct kernel_uaccess_iovec *, size_t, uint64_t, uint32_t, int64_t *);
static void changed(struct kernel_tty *tty)
{
    (void)kernel_wait_queue_wake_all(&tty->events);
}
void kernel_tty_get(struct kernel_tty *tty)
{
    if (!tty || !tty->references || tty->references == UINT32_MAX)
        __builtin_trap();
    tty->references++;
    if (tty->transport.external_ref) tty->transport.external_ref(tty->transport_owner, 1);
}
void kernel_tty_put(struct kernel_tty *tty)
{
    if (!tty || tty->references <= 1)
        __builtin_trap();
    tty->references--;
    if (tty->transport.external_ref) tty->transport.external_ref(tty->transport_owner, -1);
}
uint64_t kernel_tty_rdev(const struct kernel_tty *tty)
{
    return tty ? tty->rdev : 0;
}
int32_t kernel_tty_foreground(const struct kernel_tty *tty)
{
    return tty && tty->foreground ? tty->foreground->number : -1;
}
static int hungup(const struct tty_instance *instance)
{
    return instance->tty->shutdown || instance->generation != instance->tty->generation;
}
static void packet_event(struct kernel_tty *tty, unsigned flags)
{
    if (tty->transport.event) tty->transport.event(tty->transport_owner, flags);
}
static void stop_output(struct kernel_tty *tty)
{
    if (tty->flow_stopped) return;
    tty->flow_stopped = 1;
    packet_event(tty, 4);
    changed(tty);
}
static void start_output(struct kernel_tty *tty)
{
    /* TCOOFF 是独立资格；VSTART、IXANY 和信号不能越过它。 */
    if (!tty->flow_stopped || tty->stopped) return;
    tty->flow_stopped = 0;
    packet_event(tty, 8);
    changed(tty);
}
static int canonical_mode(struct kernel_tty *tty)
{ return (tty->settings.lflag & (ICANON | EXTPROC)) == ICANON; }
static enum kernel_tty_peer_state peer_state(struct kernel_tty *tty)
{ return tty->transport.peer_state ? tty->transport.peer_state(tty->transport_owner) : KERNEL_TTY_PEER_LIVE; }
static uint32_t decode_speed(uint32_t code, uint32_t arbitrary)
{
    static const uint32_t rates[] = {0,50,75,110,134,150,200,300,600,1200,1800,2400,
        4800,9600,19200,38400,57600,115200,230400,460800,500000,576000,921600,
        1000000,1152000,1500000,2000000,2500000,3000000,3500000,4000000};
    code &= 0x100fU;
    if (code == 0x1000U) return arbitrary;
    return rates[code < 16 ? code : 15U + (code & 15U)];
}
static void resolve_speeds(struct kernel_tty_termios2 *settings)
{
    settings->ospeed = decode_speed(settings->basic.cflag, settings->ospeed);
    uint32_t code = (settings->basic.cflag >> 16) & 0x100fU;
    settings->ispeed = code ? decode_speed(code, settings->ispeed) : settings->ospeed;
}
static int configure_settings(struct kernel_tty *tty, struct kernel_tty_termios2 *settings)
{
    resolve_speeds(settings);
    if (tty->transport.configure2)
        return tty->transport.configure2(tty->transport_owner, settings);
    int result = tty->transport.configure(tty->transport_owner, &settings->basic);
    if (!result) resolve_speeds(settings);
    return result;
}
static int special(const struct kernel_tty *tty, unsigned index, unsigned char c)
{
    return tty->settings.cc[index] != 0 && tty->settings.cc[index] == c;
}
static void flush_input(struct kernel_tty *tty)
{
    packet_event(tty, 1);
    tty->head = tty->tail = tty->canonical_head = 0;
    tty->quoted = 0;
    memset(tty->flags, 0, sizeof(tty->flags));
    if (tty->throttled) {
        tty->xchar = tty->settings.cc[VSTART];
        tty->xchar_valid = tty->xchar != 0;
        tty->throttled = 0;
    }
    changed(tty);
}
static void flush_output(struct kernel_tty *tty)
{
    packet_event(tty, 2);
    tty->tx_head = tty->tx_tail = tty->echo_head = tty->echo_tail = tty->order_head =
        tty->order_tail = 0;
    changed(tty);
}
static void finish_mode(struct kernel_tty_request *request)
{
    struct kernel_tty *tty = request->instance->tty;
    if (request->mode) {
        if (tty->mode_owner != request)
            __builtin_trap();
        tty->mode_owner = 0;
        request->mode = 0;
        if (tty->deferred_flush) {
            tty->deferred_flush = 0; flush_input(tty); flush_output(tty);
            tty->transport.kick(tty->transport_owner);
        }
        changed(tty);
    }
}
static void finish_request(struct kernel_tty_request *request)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_tty *tty = request->instance->tty;
    finish_mode(request);
    if (request->reading) {
        if (tty->reader != request)
            __builtin_trap();
        tty->reader = 0;
        request->reading = 0;
        changed(tty);
    }
    if (request->writing) {
        if (tty->writer != request) __builtin_trap();
        tty->writer = 0; request->writing = 0; changed(tty);
    }
    if (request->write_scratch) {
        if (kernel_heap_release(request->instance->heap, request->write_scratch) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        request->write_scratch = 0;
    }
    if (request->registered) {
        kernel_task_tty_request_clear(request->caller, request);
        request->registered = 0;
    }
    if (request->file_owner) {
        *request->file_owner = request->file;
        request->file = 0;
        request->file_owner = 0;
    }
    if (request->vector_owner) {
        *request->vector_owner = request->vectors;
        request->vectors = 0;
        request->vector_owner = 0;
    }
    arch_interrupt_restore(irq);
}
void kernel_tty_abort_request(struct kernel_tty_request *request)
{
    struct kernel_open_file_description *file = request->file;
    struct kernel_files *files = request->files;
    void *vectors = request->vectors;
    request->file_owner = 0;
    request->vector_owner = 0;
    request->file = 0;
    request->vectors = 0;
    finish_request(request);
    if (vectors && kernel_heap_release(files->heap, vectors) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    if (file) {
        enum kernel_open_file_status status = kernel_open_file_release(&file);
        if (status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED && file)
            kernel_files_queue_description(files, file);
        else if (status != KERNEL_OPEN_FILE_STATUS_OK)
            __builtin_trap();
    }
}
static int wait_event(struct kernel_tty_request *request, uint32_t flags, int may_timeout)
{
    struct kernel_tty *tty = request->instance->tty;
    if (hungup(request->instance))
        return -KERNEL_EIO;
    if (flags & KERNEL_FILES_O_NONBLOCK)
        return -KERNEL_EAGAIN;
    if (kernel_signal_has_pending(request->caller)) {
        return -KERNEL_ERESTARTSYS;
    }
    uint64_t deadline = 0;
    if (may_timeout && request->deadline_ns) {
        enum kernel_time_status status =
            kernel_time_deadline_from_monotonic(request->deadline_ns, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED)
            return 1;
        if (status != KERNEL_TIME_STATUS_OK)
            return -KERNEL_EIO;
    }
    enum kernel_wait_wake_reason reason;
    if (kernel_scheduler_block_current(&tty->events, deadline, 1, &reason) !=
        KERNEL_SCHEDULER_STATUS_OK)
        return -KERNEL_EIO;
    if (reason == KERNEL_WAIT_SIGNALLED) {
        return -KERNEL_ERESTARTSYS;
    }
    if (reason == KERNEL_WAIT_TIMEOUT)
        return 1;
    return 0;
}
static void begin_request(struct kernel_tty_request *r, struct tty_instance *instance,
                          struct kernel_task *caller)
{
    memset(r, 0, sizeof(*r));
    r->instance = instance;
    r->caller = caller;
    kernel_task_tty_request_set(caller, r);
    r->registered = 1;
}
static int lock_mode(struct kernel_tty_request *r, uint32_t flags)
{
    while (r->instance->tty->mode_owner && r->instance->tty->mode_owner != r) {
        int error = wait_event(r, flags, 0);
        if (error)
            return error;
    }
    r->instance->tty->mode_owner = r;
    r->mode = 1;
    return 0;
}
static int job_control(struct tty_instance *i, struct kernel_task *caller, unsigned sig, int console_io)
{
    struct kernel_tty *tty = i->tty;
    if ((console_io && i->console) || kernel_task_controlling_tty(caller) != tty)
        return 0;
    struct kernel_pid *group = kernel_task_tty_identity(caller, KERNEL_PID_PGID);
    if (!tty->foreground || group == tty->foreground)
        return 0;
    if (kernel_task_tty_signal_ignored(caller, sig))
        return sig == 21 ? -KERNEL_EIO : 0;
    if (kernel_task_tty_group_orphaned(caller))
        return -KERNEL_EIO;
    kernel_task_tty_signal_group(group, sig);
    return -KERNEL_ERESTARTSYS;
}
/* Encoded output is enqueued atomically. Independent echo capacity cannot block RX. */
static int queue_output(struct kernel_tty *tty, const unsigned char *bytes, size_t size,
                        int echo)
{
    uint64_t used = echo ? tty->echo_head - tty->echo_tail : tty->tx_head - tty->tx_tail;
    if (size > RING - used)
        return 0;
    for (size_t j = 0; j < size; j++) {
        if (echo)
            tty->echo[tty->echo_head++ & MASK] = bytes[j];
        else
            tty->tx[tty->tx_head++ & MASK] = bytes[j];
        tty->order[tty->order_head++ & (ORDER - 1)] = (unsigned char)echo;
    }
    return 1;
}
static size_t render(struct kernel_tty *tty, unsigned char c, unsigned char output[8],
                     uint32_t *column)
{
    uint32_t col = *column;
    size_t n = 0;
    uint32_t oflag = tty->settings.oflag;
    if (oflag & OPOST) {
        if (c == '\r') {
            if ((oflag & ONOCR) && !col)
                return 0;
            if (oflag & OCRNL) {
                output[0] = '\n';
                if (oflag & ONLRET)
                    col = 0;
                *column = col;
                return 1;
            }
        }
        if (c == '\n') {
            if (oflag & ONLCR) {
                output[n++] = '\r';
                col = 0;
            }
            if (oflag & ONLRET)
                col = 0;
        }
        if (c == '\t' && (oflag & TAB3) == TAB3) {
            unsigned spaces = 8 - (col & 7);
            while (spaces--)
                output[n++] = ' ';
            *column = (col + 8) & ~7U;
            return n;
        }
    }
    output[n++] = c;
    if (c == '\r')
        col = 0;
    else if (c == '\b') {
        if (col)
            col--;
    } else if (c == '\t')
        col = (col + 8) & ~7U;
    else if (c >= 32 && c != 127 && !((tty->settings.iflag & IUTF8) && (c & 0xc0) == 0x80))
        col++;
    *column = col;
    return n;
}
static void echo_bytes(struct kernel_tty *tty, const unsigned char *bytes, size_t count)
{
    unsigned char op[32];
    size_t size = 0;
    uint32_t col = tty->column;
    for (size_t j = 0; j < count; j++) {
        unsigned char out[8];
        size_t n = render(tty, bytes[j], out, &col);
        if (size + n > sizeof(op))
            return;
        memcpy(op + size, out, n);
        size += n;
    }
    if (queue_output(tty, op, size, 1))
        tty->column = col;
}
static unsigned echo_character(struct kernel_tty *tty, unsigned char c)
{
    unsigned char op[2];
    size_t n = 1;
    unsigned before = tty->column;
    op[0] = c;
    if (c != '\t' && (c < 32 || c == 127) && (tty->settings.lflag & ECHOCTL)) {
        op[0] = '^';
        op[1] = c == 127 ? '?' : c + 64;
        n = 2;
    }
    echo_bytes(tty, op, n);
    return tty->column >= before ? tty->column - before : 0;
}
static void erase_one(struct kernel_tty *tty, int literal)
{
    if (tty->head == tty->canonical_head)
        return;
    uint64_t head = tty->head;
    unsigned width = 0;
    do {
        head--;
        width += tty->width[head & MASK];
    } while ((tty->settings.iflag & IUTF8) && (tty->input[head & MASK] & 0xc0) == 0x80 &&
             head > tty->canonical_head);
    if ((tty->settings.iflag & IUTF8) && (tty->input[head & MASK] & 0xc0) == 0x80)
        return;
    tty->head = head;
    if (!(tty->settings.lflag & ECHO))
        return;
    if (literal) {
        (void)echo_character(tty, tty->settings.cc[VERASE]);
        return;
    }
    unsigned char op[24];
    size_t n = 0;
    while (width && n + 3 <= sizeof(op)) {
        op[n++] = '\b';
        op[n++] = ' ';
        op[n++] = '\b';
        width--;
    }
    echo_bytes(tty, op, n);
}
static void signal_input(struct kernel_tty *tty, unsigned signal)
{
    kernel_task_tty_signal_group(tty->foreground, signal);
    if (tty->settings.lflag & NOFLSH)
        return;
    if (tty->nonblocking_rx && tty->mode_owner) {
        tty->deferred_flush = 1;
        return;
    }
    /* RX worker waits on the logical mode guard; no allocation/usercopy/device lock is held.
     */
    while (tty->mode_owner) {
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&tty->events, 0, 0, &reason) !=
            KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    flush_input(tty);
    flush_output(tty);
}
static void input_byte(struct kernel_tty *tty, unsigned char c, int literal)
{
    int canonical = canonical_mode(tty);
    if (tty->settings.lflag & EXTPROC) {
        if (tty->settings.iflag & ISTRIP) c &= 127;
        literal = 2;
    }
    if (!literal && tty->quoted) {
        tty->quoted = 0;
        literal = 1;
        if (tty->settings.iflag & ISTRIP)
            c &= 127;
    }
    if (!literal) {
        if (tty->settings.iflag & ISTRIP)
            c &= 127;
        if (tty->settings.iflag & IXON) {
            if (special(tty, VSTOP, c)) {
                stop_output(tty);
                return;
            }
            if (special(tty, VSTART, c)) {
                start_output(tty);
                return;
            }
        }
    }
    if (!literal && (tty->settings.lflag & ISIG)) {
        unsigned sig = special(tty, VINTR, c)   ? 2
                       : special(tty, VQUIT, c) ? 3
                       : special(tty, VSUSP, c) ? 20
                                                : 0;
        if (sig) {
            signal_input(tty, sig);
            if (tty->settings.lflag & ECHO)
                (void)echo_character(tty, c);
            if (tty->settings.iflag & IXON) start_output(tty);
            return;
        }
    }
    if (!literal) {
        if ((tty->settings.iflag & (IXON | IXANY)) == (IXON | IXANY) && tty->flow_stopped) {
            start_output(tty);
        }
        if (c == '\r') {
            if (tty->settings.iflag & IGNCR)
                return;
            if (tty->settings.iflag & ICRNL)
                c = '\n';
        } else if (c == '\n' && (tty->settings.iflag & INLCR))
            c = '\r';
    }
    if (!literal && canonical) {
        if ((tty->settings.lflag & IEXTEN) && canonical && special(tty, VLNEXT, c)) {
            tty->quoted = 1;
            if ((tty->settings.lflag & (ECHO | ECHOCTL)) == (ECHO | ECHOCTL)) {
                const unsigned char op[] = {'^', '\b'};
                echo_bytes(tty, op, 2);
            }
            return;
        }
        if (special(tty, VERASE, c)) {
            erase_one(tty, !(tty->settings.lflag & ECHOE));
            return;
        }
        if ((tty->settings.lflag & IEXTEN) && special(tty, VWERASE, c)) {
            int alnum = 0;
            while (tty->head > tty->canonical_head) {
                unsigned char prev = tty->input[(tty->head - 1) & MASK];
                int word = (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                           (prev >= '0' && prev <= '9') || prev == '_';
                if (!word && alnum)
                    break;
                if (word)
                    alnum = 1;
                erase_one(tty, 0);
            }
            return;
        }
        if (special(tty, VKILL, c)) {
            if ((tty->settings.lflag & (ECHOKE | ECHOE | ECHOK)) == (ECHOKE | ECHOE | ECHOK))
                while (tty->head > tty->canonical_head)
                    erase_one(tty, 0);
            else {
                tty->head = tty->canonical_head;
                if (tty->settings.lflag & ECHO) {
                    (void)echo_character(tty, c);
                    if (tty->settings.lflag & ECHOK) {
                        const unsigned char nl = '\n';
                        echo_bytes(tty, &nl, 1);
                    }
                }
            }
            return;
        }
        if ((tty->settings.lflag & IEXTEN) && special(tty, VREPRINT, c)) {
            if (tty->settings.lflag & ECHO) {
                (void)echo_character(tty, c);
                const unsigned char nl = '\n';
                echo_bytes(tty, &nl, 1);
                for (uint64_t p = tty->canonical_head; p < tty->head; p++)
                    (void)echo_character(tty, tty->input[p & MASK]);
            }
            return;
        }
    }
    unsigned flag = canonical && !literal
                        ? (special(tty, VEOF, c) ? 2
                           : c == '\n' || special(tty, VEOL, c) ||
                                   ((tty->settings.lflag & IEXTEN) && special(tty, VEOL2, c))
                               ? 1
                               : 0)
                        : 0;
    uint64_t used = tty->head - tty->tail;
    if (used >= RING || (!flag && used >= RING - 1))
        return;
    if (literal != 2 && flag != 2 && c == 255 && (tty->settings.iflag & PARMRK) &&
        !(tty->settings.iflag & ISTRIP)) {
        if (used > (flag ? RING - 2 : RING - 3))
            return;
        tty->input[tty->head & MASK] = 255;
        tty->flags[tty->head & MASK] = tty->width[tty->head & MASK] = 0;
        tty->head++;
    }
    unsigned width = 0;
    if (literal != 2 && flag != 2 &&
        ((tty->settings.lflag & ECHO) ||
         (!literal && c == '\n' && (tty->settings.lflag & ECHONL)))) {
        if (!literal && c == '\n')
            echo_bytes(tty, &c, 1);
        else
            width = echo_character(tty, c);
    }
    tty->input[tty->head & MASK] = flag == 2 ? 0 : c;
    tty->flags[tty->head & MASK] = (unsigned char)flag;
    tty->width[tty->head & MASK] = (unsigned char)width;
    tty->head++;
    if (flag)
        tty->canonical_head = tty->head;
    if ((tty->settings.iflag & IXOFF) && !tty->throttled &&
        tty->head - tty->tail >= RING - 128) {
        tty->xchar = tty->settings.cc[VSTOP];
        tty->xchar_valid = tty->xchar != 0;
        tty->throttled = 1;
    }
    changed(tty);
}
static size_t receive_input(struct kernel_tty *tty, const struct kernel_tty_rx *input,
                        size_t count, int nonblocking)
{
    if (!tty || (!input && count))
        __builtin_trap();
    size_t j = 0;
    for (; j < count; j++) {
        uintptr_t irq = arch_interrupt_save();
        if (nonblocking && tty->deferred_flush) {
            if (tty->mode_owner) { arch_interrupt_restore(irq); break; }
            tty->deferred_flush = 0;
            flush_input(tty); flush_output(tty);
        }
        unsigned char c = input[j].character, status = input[j].status;
        size_t needed = (tty->settings.iflag & PARMRK) && !(tty->settings.lflag & EXTPROC) ?
            ((status & 28U) ? 3U : (c == 255 && !(tty->settings.iflag & ISTRIP) ? 2U : 1U)) : 1U;
        if (nonblocking && !canonical_mode(tty) && tty->head - tty->tail > RING - 1 - needed) {
            int control = !(tty->settings.lflag & EXTPROC) &&
                (((tty->settings.iflag & IXON) && (special(tty, VSTART, c) || special(tty, VSTOP, c))) ||
                 ((tty->settings.lflag & ISIG) && (special(tty, VINTR, c) || special(tty, VQUIT, c) || special(tty, VSUSP, c))));
            if (!control) { arch_interrupt_restore(irq); break; }
        }
        tty->nonblocking_rx = nonblocking;
        if (tty->shutdown || !(tty->settings.cflag & 0x80U)) {
            arch_interrupt_restore(irq);
            continue;
        }
        if (status & 16U) {
            if (!(tty->settings.iflag & IGNBRK)) {
                if (tty->settings.iflag & BRKINT) {
                    signal_input(tty, 2);
                } else if (tty->settings.iflag & PARMRK) {
                    if (tty->head - tty->tail >= RING - 3) {
                        arch_interrupt_restore(irq);
                        continue;
                    }
                    input_byte(tty, 255, 2);
                    input_byte(tty, 0, 2);
                    input_byte(tty, 0, 2);
                } else
                    input_byte(tty, 0, 2);
            }
        } else if ((status & 12U) && (tty->settings.iflag & INPCK)) {
            if (!(tty->settings.iflag & IGNPAR) &&
                (!(tty->settings.iflag & PARMRK) || tty->head - tty->tail < RING - 3)) {
                if (tty->settings.iflag & PARMRK) {
                    input_byte(tty, 255, 2);
                    input_byte(tty, 0, 2);
                    input_byte(tty, c, 2);
                } else
                    input_byte(tty, 0, 2);
            }
        } else if (status & 12U) {
            input_byte(tty, c, 2);
        } else {
            input_byte(tty, c, 0);
        }
        arch_interrupt_restore(irq);
    }
    tty->nonblocking_rx = 0;
    tty->transport.kick(tty->transport_owner);
    return j;
}
void kernel_tty_receive(struct kernel_tty *tty, const struct kernel_tty_rx *input, size_t count)
{ (void)receive_input(tty, input, count, 0); }
size_t kernel_tty_receive_nonblocking(struct kernel_tty *tty, const struct kernel_tty_rx *input, size_t count)
{ return receive_input(tty, input, count, 1); }
size_t kernel_tty_service_output(struct kernel_tty *tty, size_t budget)
{
    size_t total = 0;
    if (!tty)
        return 0;
    while (total < budget) {
        uintptr_t irq = arch_interrupt_save();
        unsigned char c;
        int priority = tty->xchar_valid;
        if (priority)
            c = tty->xchar;
        else {
            if ((!tty->shutdown && (tty->stopped || tty->flow_stopped)) ||
                tty->order_head == tty->order_tail) {
                arch_interrupt_restore(irq);
                break;
            }
            c = tty->order[tty->order_tail & (ORDER - 1)] ? tty->echo[tty->echo_tail & MASK]
                                                          : tty->tx[tty->tx_tail & MASK];
        }
        /* Transport accepts only hardware credit, never busy-waits or sleeps. */
        size_t sent = tty->transport.transmit(tty->transport_owner, &c, 1);
        if (sent > 1)
            __builtin_trap();
        if (sent) {
            if (priority)
                tty->xchar_valid = 0;
            else {
                if (tty->order[tty->order_tail++ & (ORDER - 1)])
                    tty->echo_tail++;
                else
                    tty->tx_tail++;
            }
            changed(tty);
            total++;
        }
        arch_interrupt_restore(irq);
        if (!sent)
            break;
    }
    return total;
}
int kernel_tty_output_pending(struct kernel_tty *tty)
{
    uintptr_t irq = arch_interrupt_save();
    int result = tty && (tty->xchar_valid || tty->order_head != tty->order_tail);
    arch_interrupt_restore(irq);
    return result;
}
void kernel_tty_transport_ready(struct kernel_tty *tty)
{
    if (tty) {
        uintptr_t irq = arch_interrupt_save();
        changed(tty);
        arch_interrupt_restore(irq);
    }
}
static void drop_session(struct kernel_tty *tty)
{
    if (tty->session) {
        kernel_pid_put(tty->session);
        tty->session = 0;
    }
    if (tty->foreground) {
        kernel_pid_put(tty->foreground);
        tty->foreground = 0;
    }
}
void kernel_tty_disassociate(struct kernel_tty *tty, int on_exit)
{
    uintptr_t irq = arch_interrupt_save();
    if (on_exit) {
        tty->generation++;
        flush_input(tty);
        kernel_task_tty_signal_session(tty, tty->session);
        kernel_task_tty_signal_group(tty->foreground, 1);
    } else {
        kernel_task_tty_signal_group(tty->foreground, 1);
        kernel_task_tty_signal_group(tty->foreground, 18);
        kernel_task_tty_clear_session(tty, tty->session);
    }
    drop_session(tty);
    changed(tty);
    arch_interrupt_restore(irq);
}
void kernel_tty_shutdown(struct kernel_tty *tty)
{
    if (!tty)
        return;
    kernel_tty_disassociate(tty, 1);
    uintptr_t irq = arch_interrupt_save();
    tty->shutdown = 1;
    tty->stopped = tty->flow_stopped = 0;
    changed(tty);
    arch_interrupt_restore(irq);
    tty->transport.kick(tty->transport_owner);
}
static int acquire_ctty(struct kernel_tty *tty, struct kernel_task *caller, int steal)
{
    if (!kernel_task_tty_session_leader(caller))
        return -KERNEL_EPERM;
    struct kernel_tty *current = kernel_task_controlling_tty(caller);
    if (current == tty)
        return 0;
    if (current)
        return -KERNEL_EPERM;
    if (tty->session) {
        if (!steal)
            return -KERNEL_EPERM;
        /* Current uid model is immutable root; steal is CAP_SYS_ADMIN-equivalent. */
        kernel_task_tty_clear_session(tty, tty->session);
        drop_session(tty);
    }
    struct kernel_pid *sid = kernel_task_tty_identity(caller, KERNEL_PID_SID),
                      *pgid = kernel_task_tty_identity(caller, KERNEL_PID_PGID);
    if (!sid || !pgid)
        return -KERNEL_EPERM;
    kernel_pid_get(sid);
    kernel_pid_get(pgid);
    tty->session = sid;
    tty->foreground = pgid;
    return kernel_task_tty_set(caller, tty);
}
static int open_instance(struct kernel_tty *tty, struct kernel_heap *heap, struct kernel_task *caller, uint32_t flags,
                         void **result, int kind)
{
    if (!tty)
        return -KERNEL_ENXIO;
    struct tty_instance *instance = 0;
    enum kernel_heap_status allocation =
        kernel_heap_allocate_zeroed(heap, 1, sizeof(*instance), (void **)&instance);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    uintptr_t irq = arch_interrupt_save();
    int open_error = tty->transport.open_check ? tty->transport.open_check(tty->transport_owner) : 0;
    if (tty->shutdown || open_error) {
        arch_interrupt_restore(irq);
        (void)kernel_heap_release(heap, instance);
        return open_error ? open_error : -KERNEL_ENXIO;
    }
    if (!tty->opens) {
        struct kernel_tty_termios2 settings = {tty->settings, tty->ispeed, tty->ospeed};
        int error = configure_settings(tty, &settings);
        if (error) {
            arch_interrupt_restore(irq);
            (void)kernel_heap_release(heap, instance);
            return error;
        }
        tty->settings = settings.basic;
        tty->ispeed = settings.ispeed; tty->ospeed = settings.ospeed;
    }
    instance->tty = tty;
    instance->heap = heap;
    instance->generation = tty->generation;
    instance->console = kind == 1;
    kernel_tty_get(tty);
    tty->opens++;
    if (tty->transport.opened) tty->transport.opened(tty->transport_owner);
    if (!kind && !(flags & O_NOCTTY) && (flags & 3) != 1 &&
        kernel_task_tty_session_leader(caller) && !kernel_task_controlling_tty(caller) &&
        !tty->session)
        (void)acquire_ctty(tty, caller, 0);
    arch_interrupt_restore(irq);
    *result = instance;
    return 0;
}
static int open_serial(struct kernel_heap *h, struct kernel_task *c, uint32_t f, const struct kernel_vfs_file *path, void **i)
{
    (void)path;
    return open_instance(serial_tty, h, c, f, i, 0);
}
static int open_console(struct kernel_heap *h, struct kernel_task *c, uint32_t f, const struct kernel_vfs_file *path, void **i)
{
    (void)path;
    return open_instance(serial_tty, h, c, f, i, 1);
}
static int open_current(struct kernel_heap *h, struct kernel_task *c, uint32_t f, const struct kernel_vfs_file *path, void **i)
{
    (void)path;
    return open_instance(kernel_task_controlling_tty(c), h, c, f, i, 2);
}
static void release_instance(void *opaque)
{
    struct tty_instance *i = opaque;
    struct kernel_tty *tty = i->tty;
    uintptr_t irq = arch_interrupt_save();
    if (!tty->opens)
        __builtin_trap();
    tty->opens--;
    if (!tty->opens && tty->transport.last_close)
        tty->transport.last_close(tty->transport_owner, tty->settings.cflag);
    kernel_tty_put(tty);
    arch_interrupt_restore(irq);
    if (kernel_heap_release(i->heap, i) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}
int kernel_tty_create(struct kernel_heap *heap, const struct kernel_tty_transport *transport,
                      void *owner, struct kernel_tty **out)
{
    if (!heap || !transport || !transport->transmit || !transport->configure ||
        !transport->kick || !transport->drained || !out)
        return -KERNEL_EINVAL;
    struct kernel_tty *tty = 0;
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(heap, 1, sizeof(*tty), (void **)&tty);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    tty->heap = heap;
    tty->transport = *transport;
    tty->transport_owner = owner;
    tty->rdev = 0x440;
    tty->references = 1;
    tty->generation = 1;
    tty->settings.iflag = ICRNL | IXON;
    tty->settings.oflag = OPOST | ONLCR;
    tty->settings.cflag = 0x10b2U;
    tty->settings.lflag = 0x8a3bU;
    const unsigned char defaults[19] = {3,  28, 127, 21, 4,  0,  1, 0, 17, 19,
                                        26, 0,  18,  15, 23, 22, 0, 0, 0};
    memcpy(tty->settings.cc, defaults, 19);
    kernel_wait_queue_init(&tty->events);
    int error = transport->configure(owner, &tty->settings);
    if (error) {
        (void)kernel_heap_release(heap, tty);
        return error;
    }
    tty->ispeed = tty->ospeed = decode_speed(tty->settings.cflag, 0);
    *out = tty;
    return 0;
}
int kernel_tty_destroy(struct kernel_tty **owner)
{
    if (!owner || !*owner)
        return -KERNEL_EINVAL;
    struct kernel_tty *tty = *owner;
    uintptr_t irq = arch_interrupt_save();
    if (tty->references != 1 || tty->reader || tty->writer || tty->mode_owner || tty->session ||
        tty->foreground || kernel_tty_output_pending(tty) ||
        !tty->transport.drained(tty->transport_owner)) {
        arch_interrupt_restore(irq);
        return -KERNEL_EBUSY;
    }
    if (serial_tty == tty)
        serial_tty = 0;
    tty->references = 0;
    arch_interrupt_restore(irq);
    if (kernel_heap_release(tty->heap, tty) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    *owner = 0;
    return 0;
}
void kernel_tty_publish_serial(struct kernel_tty *tty)
{
    uintptr_t irq = arch_interrupt_save();
    if (serial_tty && serial_tty != tty)
        __builtin_trap();
    serial_tty = tty;
    for (unsigned j = 0; j < 3; j++)
        devices[j] = (struct kernel_char_device){.rdev = j == 0   ? 0x440
                                                         : j == 1 ? 0x501
                                                                  : 0x500,
                                                 .kind = KERNEL_OPEN_FILE_KIND_CONSOLE,
                                                 .open = j == 0   ? open_serial
                                                         : j == 1 ? open_console
                                                                  : open_current,
                                                 .release = release_instance,
                                                 .read = device_read,
                                                 .write = device_write,
                                                 .ioctl = device_ioctl,
                                                 .poll = device_poll,
                                                 .readv = device_readv,
                                                 .writev = device_writev};
    arch_interrupt_restore(irq);
}
const struct kernel_char_device *kernel_tty_device_lookup(uint64_t rdev)
{
    if (!serial_tty)
        return 0;
    for (unsigned j = 0; j < 3; j++)
        if (devices[j].rdev == rdev)
            return &devices[j];
    return 0;
}

static void handoff(struct kernel_tty_request *r, struct kernel_files *files,
                    struct kernel_open_file_description **owner, void **vectors)
{
    r->files = files;
    r->file_owner = owner;
    r->vector_owner = vectors;
    if (owner) {
        r->file = *owner;
        *owner = 0;
    }
    if (vectors) {
        r->vectors = *vectors;
        *vectors = 0;
    }
}
static void unthrottle(struct kernel_tty *tty)
{
    if (tty->throttled && tty->head - tty->tail < 256) {
        tty->throttled = 0;
        tty->xchar = tty->settings.cc[VSTART];
        tty->xchar_valid = tty->xchar != 0;
        tty->transport.kick(tty->transport_owner);
    }
}
/* Consume before copying to userspace, exactly as fixed iterate_tty_read. */
static size_t take_input(struct kernel_tty *tty, unsigned char *buffer, size_t capacity,
                         int canonical, int *more)
{
    size_t n = 0;
    *more = 0;
    uint64_t limit = canonical ? tty->canonical_head : tty->head;
    if (!canonical && capacity && tty->head - tty->tail == 1 &&
        (tty->settings.lflag & (EXTPROC | ICANON)) == (EXTPROC | ICANON) &&
        special(tty, VEOF, tty->input[tty->tail & MASK])) {
        tty->tail++; unthrottle(tty); tty->transport.kick(tty->transport_owner); return 0;
    }
    while (n < capacity && tty->tail < limit) {
        unsigned pos = tty->tail & MASK, flag = canonical ? tty->flags[pos] : 0;
        unsigned char c = tty->input[pos];
        tty->flags[pos] = 0;
        tty->tail++;
        if (flag != 2)
            buffer[n++] = c;
        if (flag) {
            unthrottle(tty);
            tty->transport.kick(tty->transport_owner);
            return n;
        }
    }
    *more = tty->tail < limit;
    unthrottle(tty);
    tty->transport.kick(tty->transport_owner);
    return n;
}
static int read_stage(struct kernel_tty_request *r, uint32_t flags, unsigned char *buffer,
                      size_t capacity, size_t *staged)
{
    struct kernel_tty *tty = r->instance->tty;
    *staged = 0;
    if (tty->transport.progress) tty->transport.progress(tty->transport_owner);
    uintptr_t irq = arch_interrupt_save();
    if (hungup(r->instance)) {
        r->continuation = 0;
        arch_interrupt_restore(irq);
        return 0;
    }
    if (r->continuation) {
        if (!capacity) {
            if ((tty->settings.lflag & ICANON) && tty->tail < tty->canonical_head &&
                tty->flags[tty->tail & MASK] == 2) {
                tty->flags[tty->tail & MASK] = 0;
                tty->tail++;
                unthrottle(tty);
            }
            r->continuation = 0;
            arch_interrupt_restore(irq);
            return 0;
        }
        int more;
        *staged =
            take_input(tty, buffer, capacity, canonical_mode(tty), &more);
        r->continuation = (uint8_t)more;
        arch_interrupt_restore(irq);
        return 0;
    }
    if (hungup(r->instance)) {
        arch_interrupt_restore(irq);
        return 0;
    }
    int error = job_control(r->instance, r->caller, 21, 1);
    if (error) {
        arch_interrupt_restore(irq);
        return error;
    }
    while (tty->reader && tty->reader != r) {
        error = wait_event(r, flags, 0);
        if (error) {
            arch_interrupt_restore(irq);
            return error;
        }
    }
    tty->reader = r;
    r->reading = 1;
    error = lock_mode(r, flags);
    if (error) {
        arch_interrupt_restore(irq);
        return error;
    }
    r->initial_canonical = canonical_mode(tty);
    r->min = r->initial_canonical ? 0 : tty->settings.cc[VMIN];
    r->time = canonical_mode(tty) ? 0 : tty->settings.cc[VTIME];
    if (!r->min && r->time)
        r->deadline_ns = kernel_time_monotonic_ns() + (uint64_t)r->time * 100000000;
    for (;;) {
        int canonical = canonical_mode(tty);
        if (hungup(r->instance)) {
            error = 0;
            break;
        }
        if (tty->packet && !r->packet_started && tty->packet_status) {
            if (capacity) { buffer[0] = tty->packet_status; tty->packet_status = 0; *staged = 1; }
            r->continuation = 0;
            break;
        }
        int available = canonical ? tty->canonical_head > tty->tail : tty->head > tty->tail;
        if (available) {
            if (tty->packet && !canonical && !r->packet_started) {
                buffer[(*staged)++] = 0; r->packet_started = 1;
                if (*staged == capacity) { r->continuation = 0; break; }
            }
            int more;
            size_t n = take_input(tty, buffer + *staged, capacity - *staged, canonical, &more);
            *staged += n;
            if (!n && (tty->settings.lflag & EXTPROC)) break;
            if (canonical) {
                r->continuation = (uint8_t)more;
                break;
            }
            if (more && *staged >= r->min) {
                r->continuation = 1;
                break;
            }
            if (*staged >= r->min || *staged == capacity)
                break;
            if (r->time && n)
                r->deadline_ns = kernel_time_monotonic_ns() + (uint64_t)r->time * 100000000;
        } else if (!canonical && !r->initial_canonical && !r->min && !r->time)
            break;
        if (peer_state(tty) != KERNEL_TTY_PEER_LIVE) {
            error = peer_state(tty) == KERNEL_TTY_PEER_ABSENT ? -KERNEL_EIO : 0;
            break;
        }
        /* Waiting releases only mode protection; this invocation keeps read serialization. */
        finish_mode(r);
        error = wait_event(r, flags, 1);
        if (error) {
            if (error == 1)
                error = 0;
            break;
        }
        error = lock_mode(r, flags);
        if (error)
            break;
    }
    arch_interrupt_restore(irq);
    return *staged ? 0 : error;
}
static enum kernel_files_status
device_readv(void *opaque, struct kernel_task *caller, struct kernel_files *files,
             struct kernel_open_file_description **owner, void **vectors, struct kernel_mm *mm,
             const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
             uint32_t flags, int64_t *result)
{
    struct tty_instance *i = opaque;
    struct kernel_tty_request r;
    unsigned char scratch[64];
    uint64_t delivered = 0;
    int error = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    begin_request(&r, i, caller);
    handoff(&r, files, owner, vectors);
    if (!count) {
        *result = 0;
        finish_request(&r);
        return status;
    }
    do {
        size_t n = 0, capacity = count - delivered < sizeof(scratch)
                                     ? (size_t)(count - delivered)
                                     : sizeof(scratch);
        error = read_stage(&r, flags, scratch, capacity, &n);
        if (error || !n)
            break;
        size_t copied = 0;
        enum kernel_uaccess_status access =
            kernel_copy_to_user_iov(mm, &cursor, scratch, n, &copied);
        delivered += copied;
        if (access != KERNEL_UACCESS_STATUS_OK || copied != n) {
            error = -KERNEL_EFAULT;
            if (access != KERNEL_UACCESS_STATUS_FAULT)
                status = KERNEL_FILES_STATUS_STATE;
            /* Cleanup continuation with zero size, including the fixed EOF-skip rule. */
            if (r.continuation) {
                size_t ignored;
                (void)read_stage(&r, flags, scratch, 0, &ignored);
            }
            break;
        }
    } while (r.continuation);
    memset(scratch, 0, sizeof(scratch));
    finish_request(&r);
    *result = delivered ? (int64_t)delivered : error;
    return status;
}
static int device_read(void *opaque, struct kernel_task *caller, uint32_t flags, void *buffer,
                       size_t size, size_t *read)
{
    struct kernel_tty_request r;
    begin_request(&r, opaque, caller);
    int error = size ? read_stage(&r, flags, buffer, size, read) : 0;
    if (!size)
        *read = 0;
    finish_request(&r);
    return error;
}
static int lock_writer(struct kernel_tty_request *r, uint32_t flags)
{
    struct kernel_tty *tty = r->instance->tty;
    while (tty->writer && tty->writer != r) {
        int error = wait_event(r, flags, 0);
        if (error) return error;
    }
    tty->writer = r; r->writing = 1;
    return 0;
}
static int write_bytes(struct kernel_tty_request *r, uint32_t flags,
                       const unsigned char *input, size_t size, size_t *written)
{
    struct kernel_tty *tty = r->instance->tty;
    *written = 0;
    uintptr_t irq = arch_interrupt_save();
    int error = hungup(r->instance) || peer_state(tty) != KERNEL_TTY_PEER_LIVE ? -KERNEL_EIO : 0;
    if (!error && !r->write_qualified && (tty->settings.lflag & TOSTOP))
        error = job_control(r->instance, r->caller, 22, 1);
    r->write_qualified = 1;
    if (!error && !r->writing) error = lock_writer(r, flags);
    while (!error && *written < size) {
        if (hungup(r->instance) || peer_state(tty) != KERNEL_TTY_PEER_LIVE) {
            error = -KERNEL_EIO;
            break;
        }
        if (kernel_signal_has_pending(r->caller)) {
            error = -KERNEL_ERESTARTSYS;
            break;
        }
        error = lock_mode(r, flags);
        if (error)
            break;
        uint32_t column = tty->column;
        unsigned char op[8];
        size_t n = render(tty, input[*written], op, &column);
        if (queue_output(tty, op, n, 0)) {
            tty->column = column;
            (*written)++;
            finish_mode(r);
            tty->transport.kick(tty->transport_owner);
            continue;
        }
        finish_mode(r);
        tty->transport.kick(tty->transport_owner);
        error = wait_event(r, flags, 0);
    }
    arch_interrupt_restore(irq);
    return *written ? 0 : error;
}
static int device_write(void *opaque, struct kernel_task *caller, uint32_t flags,
                        const void *buffer, size_t size, size_t *written)
{
    struct kernel_tty_request r;
    begin_request(&r, opaque, caller);
    int error = write_bytes(&r, flags, buffer, size, written);
    finish_request(&r);
    return error;
}
static enum kernel_files_status
device_writev(void *opaque, struct kernel_task *caller, struct kernel_files *files,
              struct kernel_open_file_description **owner, void **vectors,
              struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov, size_t iov_count,
              uint64_t count, uint32_t flags, int64_t *result)
{
    struct kernel_tty_request r;
    begin_request(&r, opaque, caller);
    handoff(&r, files, owner, vectors);
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    uint64_t total = 0;
    int error = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    size_t staging = count < 2048U ? (size_t)count : 2048U;
    uintptr_t irq = arch_interrupt_save();
    error = hungup(r.instance)                           ? -KERNEL_EIO
            : (r.instance->tty->settings.lflag & TOSTOP) ? job_control(r.instance, caller, 22, 1)
                                                         : 0;
    r.write_qualified = 1;
    if (!error && count) error = lock_writer(&r, flags);
    arch_interrupt_restore(irq);
    if (!error && count) {
        enum kernel_heap_status allocated = kernel_heap_allocate(r.instance->heap, staging, (void **)&r.write_scratch);
        if (allocated != KERNEL_HEAP_STATUS_OK) error = allocated == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    if (error) {
        finish_request(&r);
        *result = error;
        return status;
    }
    while (total < count) {
        size_t n = 0, capacity = count - total < staging ? (size_t)(count - total) : staging;
        enum kernel_uaccess_status access =
            kernel_copy_from_user_iov(mm, &cursor, r.write_scratch, capacity, &n);
        if (access != KERNEL_UACCESS_STATUS_OK || n != capacity) {
            error = -KERNEL_EFAULT;
            if (access != KERNEL_UACCESS_STATUS_FAULT) status = KERNEL_FILES_STATUS_STATE;
            break;
        }
        size_t written = 0;
        error = write_bytes(&r, flags, r.write_scratch, n, &written);
        total += written;
        if (error || written < n) break;
    }
    finish_request(&r);
    *result = total ? (int64_t)total : error;
    return status;
}
static uint32_t device_poll(void *opaque, uint32_t flags, uint32_t requested,
                            struct kernel_wait_queue **queue)
{
    (void)flags;
    (void)requested;
    struct tty_instance *i = opaque;
    struct kernel_tty *tty = i->tty;
    if (tty->transport.progress) tty->transport.progress(tty->transport_owner);
    uintptr_t irq = arch_interrupt_save();
    uint32_t result = 0;
    if (queue)
        *queue = &tty->events;
    if (hungup(i))
        result = KERNEL_POLLIN | KERNEL_POLLRDNORM | KERNEL_POLLOUT | KERNEL_POLLWRNORM |
                 KERNEL_POLLHUP;
    else {
        if (canonical_mode(tty)) {
            if (tty->canonical_head > tty->tail)
                result |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        } else {
            unsigned minimum = tty->settings.cc[VTIME] ? 1 : tty->settings.cc[VMIN];
            if (!minimum)
                minimum = 1;
            if (tty->head - tty->tail >= minimum)
                result |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        }
        if (peer_state(tty) != KERNEL_TTY_PEER_LIVE) result |= KERNEL_POLLHUP;
        if (tty->packet && tty->packet_status) result |= KERNEL_POLLPRI | KERNEL_POLLIN | KERNEL_POLLRDNORM;
        if (tty->order_head - tty->order_tail < 256 && tty->tx_head - tty->tx_tail < RING)
            result |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    }
    arch_interrupt_restore(irq);
    return result;
}
static int copy_ioctl(struct kernel_mm *mm, uint64_t address, void *buffer, size_t size,
                      int output)
{
    size_t copied = 0;
    enum kernel_uaccess_status status =
        output ? kernel_copy_to_user(mm, address, buffer, size, &copied)
               : kernel_copy_from_user(mm, buffer, address, size, &copied);
    if (status == KERNEL_UACCESS_STATUS_FAULT)
        return -KERNEL_EFAULT;
    /* 用户 fault 是 ABI 结果；内核调用参数/复制不变量损坏不能伪装成 EFAULT。 */
    if (status != KERNEL_UACCESS_STATUS_OK || copied != size)
        __builtin_trap();
    return 0;
}
static int drain(struct kernel_tty_request *r)
{
    struct kernel_tty *tty = r->instance->tty;
    uintptr_t irq = arch_interrupt_save();
    int error = 0;
    while (kernel_tty_output_pending(tty) || !tty->transport.drained(tty->transport_owner)) {
        tty->transport.kick(tty->transport_owner);
        error = wait_event(r, 0, 0);
        if (error)
            break;
    }
    arch_interrupt_restore(irq);
    return error;
}
static int device_ioctl(void *opaque, struct kernel_task *caller, struct kernel_files *files,
                        struct kernel_open_file_description **owner, struct kernel_mm *mm,
                        uint64_t command, uint64_t argument)
{
    struct tty_instance *i = opaque;
    struct kernel_tty *tty = i->tty;
    struct kernel_tty *control = tty->master && tty->transport.peer ? tty->transport.peer(tty->transport_owner) : tty;
    struct tty_instance target = *i;
    /* Linux mode ioctl在master上操作slave；master自身行规程仍为独立raw状态。 */
    int extended = command == 0x802c542a || (command >= 0x402c542b && command <= 0x402c542d);
    if (((command >= 0x5401 && command <= 0x5404) || extended) && control != tty) {
        target.tty = control; target.generation = control->generation;
        i = &target; tty = control;
    }
    struct kernel_tty_request r;
    begin_request(&r, i, caller);
    handoff(&r, files, owner, 0);
    int error = 0;
    if (hungup(i)) {
        finish_request(&r);
        return command == 0x5410 ? -KERNEL_ENOTTY : -KERNEL_EIO;
    }
    switch ((uint32_t)command) {
    case 0x5401: /* TCGETS保持36字节，不能按libc结构长度复制。 */
    case 0x802c542a: {
        struct kernel_tty_termios2 settings;
        uintptr_t irq = arch_interrupt_save();
        settings = (struct kernel_tty_termios2){tty->settings, tty->ispeed, tty->ospeed};
        arch_interrupt_restore(irq);
        error = copy_ioctl(mm, argument, &settings, extended ? sizeof(settings) : sizeof(settings.basic), 1);
        break;
    }
    case 0x5402:
    case 0x5403:
    case 0x5404:
    case 0x402c542b:
    case 0x402c542c:
    case 0x402c542d: {
        struct kernel_tty_termios2 modern;
        struct kernel_tty_termios settings;
        error = job_control(i, caller, 22, 0);
        if (error)
            break;
        uintptr_t snapshot = arch_interrupt_save();
        modern = (struct kernel_tty_termios2){tty->settings, tty->ispeed, tty->ospeed};
        arch_interrupt_restore(snapshot);
        error = copy_ioctl(mm, argument, &modern, extended ? sizeof(modern) : sizeof(modern.basic), 0);
        if (error)
            break;
        settings = modern.basic;
        if (settings.line) {
            error = -KERNEL_EINVAL;
            break;
        }
        settings.iflag &= 0x7dffU;
        settings.oflag &= 0x183dU;
        settings.lflag &= tty->transport.peer ? 0x18bfbU : 0x8bfbU;
        if ((settings.oflag & TAB3) != TAB3)
            settings.oflag &= ~TAB3;
        settings.cflag &= 0xd00f1fffU;
        if (command != 0x5402 && command != 0x402c542b) {
            error = drain(&r);
            if (error)
                break;
        }
        uintptr_t irq = arch_interrupt_save();
        error = lock_mode(&r, 0);
        if (!error) {
            modern.basic = settings;
            error = configure_settings(tty, &modern);
            settings = modern.basic;
        }
        if (!error) {
            int was_canonical = canonical_mode(tty);
            int old_stop = (tty->settings.iflag & IXON) && tty->settings.cc[VSTOP] == 19 && tty->settings.cc[VSTART] == 17;
            int new_stop = (settings.iflag & IXON) && settings.cc[VSTOP] == 19 && settings.cc[VSTART] == 17;
            if (old_stop != new_stop) packet_event(tty, new_stop ? 32 : 16);
            if ((tty->settings.lflag | settings.lflag) & EXTPROC) packet_event(tty, 64);
            tty->settings = settings;
            tty->ispeed = modern.ispeed; tty->ospeed = modern.ospeed;
            if (!(settings.iflag & IXON))
                start_output(tty);
            if (command == 0x5404 || command == 0x402c542d)
                flush_input(tty);
            else if (was_canonical != canonical_mode(tty)) {
                memset(tty->flags, 0, sizeof(tty->flags));
                tty->canonical_head = tty->tail;
                if (canonical_mode(tty) && tty->head > tty->tail) {
                    tty->flags[(tty->head - 1) & MASK] =
                        tty->input[(tty->head - 1) & MASK] == 0 ? 2 : 1;
                    tty->canonical_head = tty->head;
                }
            }
            changed(tty);
        }
        finish_mode(&r);
        arch_interrupt_restore(irq);
        tty->transport.kick(tty->transport_owner);
        break;
    }
    case 0x5409: /* TCSBRK(1)=drain; break generation has no transport owner. */
        error = job_control(i, caller, 22, 0);
        if (!error)
            error = argument ? drain(&r) : -KERNEL_ENOTSUP;
        break;
    case 0x540a: { /* TCXONC */
        error = job_control(i, caller, 22, 0);
        if (error)
            break;
        uintptr_t irq = arch_interrupt_save();
        if (argument == 0) {
            tty->stopped = 1;
            stop_output(tty);
        } else if (argument == 1) {
            if (tty->stopped) {
                tty->stopped = 0;
                start_output(tty);
            }
        }
        else if (argument == 2 || argument == 3) {
            tty->xchar = tty->settings.cc[argument == 2 ? VSTOP : VSTART];
            tty->xchar_valid = tty->xchar != 0;
        } else
            error = -KERNEL_EINVAL;
        changed(tty);
        arch_interrupt_restore(irq);
        tty->transport.kick(tty->transport_owner);
        break;
    }
    case 0x540b: {
        error = job_control(i, caller, 22, 0);
        if (error)
            break;
        if (argument > 2) {
            error = -KERNEL_EINVAL;
            break;
        }
        uintptr_t irq = arch_interrupt_save();
        error = lock_mode(&r, 0);
        if (!error) {
            if (argument != 1)
                flush_input(tty);
            if (argument != 0)
                flush_output(tty);
        }
        finish_mode(&r);
        arch_interrupt_restore(irq);
        break;
    }
    case 0x540e: {
        uintptr_t irq = arch_interrupt_save();
        /* PTY master 的控制身份属于 slave，与组查询和 hangup 使用同一 owner。 */
        error = acquire_ctty(control, caller, argument == 1);
        arch_interrupt_restore(irq);
        break;
    }
    case 0x5422: {
        if (kernel_task_controlling_tty(caller) != tty) {
            error = -KERNEL_ENOTTY;
            break;
        }
        if (kernel_task_tty_session_leader(caller))
            kernel_tty_disassociate(tty, 0);
        else {
            uintptr_t irq = arch_interrupt_save();
            kernel_task_tty_clear(caller);
            arch_interrupt_restore(irq);
        }
        break;
    }
    case 0x540f:
    case 0x5429: {
        if (!tty->master && kernel_task_controlling_tty(caller) != control) {
            error = -KERNEL_ENOTTY;
            break;
        }
        uintptr_t irq = arch_interrupt_save();
        if (command == 0x5429 && !control->session) { arch_interrupt_restore(irq); error = -KERNEL_ENOTTY; break; }
        int32_t value = command == 0x540f ? (control->foreground ? control->foreground->number : 0)
                        : control->session    ? control->session->number
                                          : -1;
        arch_interrupt_restore(irq);
        error = copy_ioctl(mm, argument, &value, sizeof(value), 1);
        break;
    }
    case 0x5410: {
        struct tty_instance control_instance = *i; control_instance.tty = control;
        error = job_control(&control_instance, caller, 22, 0);
        if (error)
            break;
        if (kernel_task_controlling_tty(caller) != control ||
            control->session != kernel_task_tty_identity(caller, KERNEL_PID_SID)) {
            error = -KERNEL_ENOTTY;
            break;
        }
        int32_t number;
        error = copy_ioctl(mm, argument, &number, sizeof(number), 0);
        if (error)
            break;
        if (number < 0) {
            error = -KERNEL_EINVAL;
            break;
        }
        uintptr_t irq = arch_interrupt_save();
        struct kernel_pid *group = 0;
        error = kernel_task_tty_find_group(caller, number, &group);
        if (!error) {
            kernel_pid_get(group);
            if (control->foreground)
                kernel_pid_put(control->foreground);
            control->foreground = group;
            changed(tty);
        }
        arch_interrupt_restore(irq);
        break;
    }
    case 0x5413:
    case 0x5414: {
        uint16_t size[4];
        uintptr_t irq = arch_interrupt_save();
        size[0] = control->rows;
        size[1] = control->columns;
        size[2] = control->xpixel;
        size[3] = control->ypixel;
        arch_interrupt_restore(irq);
        if (command == 0x5413)
            error = copy_ioctl(mm, argument, size, sizeof(size), 1);
        else {
            error = copy_ioctl(mm, argument, size, sizeof(size), 0);
            if (!error) {
                irq = arch_interrupt_save();
                if (size[0] != control->rows || size[1] != control->columns ||
                    size[2] != control->xpixel || size[3] != control->ypixel) {
                    control->rows = size[0];
                    control->columns = size[1];
                    control->xpixel = size[2];
                    control->ypixel = size[3];
                    struct kernel_tty *other = control->transport.peer ? control->transport.peer(control->transport_owner) : 0;
                    if (other) {
                        other->rows = size[0]; other->columns = size[1]; other->xpixel = size[2]; other->ypixel = size[3];
                        if (other->foreground != control->foreground) kernel_task_tty_signal_group(other->foreground, 28);
                    }
                    kernel_task_tty_signal_group(control->foreground, 28);
                }
                arch_interrupt_restore(irq);
            }
        }
        break;
    }
    case 0x541b:
    case 0x5411:
    case 0x5424: {
        int32_t value = 0;
        uintptr_t irq = arch_interrupt_save();
        if (command == 0x5411)
            value = (int32_t)(tty->order_head - tty->order_tail + tty->xchar_valid);
        else if (command == 0x541b) {
            uint64_t end = canonical_mode(tty) ? tty->canonical_head : tty->head;
            for (uint64_t p = tty->tail; p < end; p++)
                if (!canonical_mode(tty) || tty->flags[p & MASK] != 2)
                    value++;
        }
        arch_interrupt_restore(irq);
        error = copy_ioctl(mm, argument, &value, sizeof(value), 1);
        break;
    }
    case 0x5423: {
        int32_t line;
        error = copy_ioctl(mm, argument, &line, sizeof(line), 0);
        if (!error && line)
            error = -KERNEL_EINVAL;
        break;
    }
    case 0x5420: { /* TIOCPKT */
        int value;
        if (!tty->master) { error = -KERNEL_ENOTTY; break; }
        error = copy_ioctl(mm, argument, &value, sizeof(value), 0);
        if (!error) {
            uintptr_t irq = arch_interrupt_save();
            if (!tty->packet && value) tty->packet_status = 0;
            tty->packet = value != 0; changed(tty);
            arch_interrupt_restore(irq);
        }
        break;
    }
    case 0x80045438: {
        if (!tty->master) { error = -KERNEL_ENOTTY; break; }
        int value = tty->packet;
        error = copy_ioctl(mm, argument, &value, sizeof(value), 1);
        break;
    }
    case 0x40045436: { /* TIOCSIG carries signal by value, not by pointer. */
        if (!tty->master || !tty->transport.peer) { error = -KERNEL_ENOTTY; break; }
        if (argument != 2 && argument != 3 && argument != 20) { error = -KERNEL_EINVAL; break; }
        struct kernel_tty *peer = tty->transport.peer(tty->transport_owner);
        kernel_task_tty_signal_group(peer->foreground, (unsigned)argument);
        break;
    }
    default:
        error = tty->transport.ioctl ? tty->transport.ioctl(tty->transport_owner, caller, files, &r.file, mm, command, argument) : -KERNEL_ENOTTY;
        break;
    }
    finish_request(&r);
    return error;
}

void kernel_tty_configure_identity(struct kernel_tty *tty, uint64_t rdev, int raw)
{
    if (tty->opens || tty->references != 1) __builtin_trap();
    tty->rdev = rdev; tty->master = raw != 0;
    if (raw) { tty->settings.iflag = 0; tty->settings.oflag = 0; tty->settings.lflag = 0; }
}
int kernel_tty_open(struct kernel_tty *tty, struct kernel_heap *heap, struct kernel_task *caller, uint32_t flags, int automatic, void **instance)
{ return open_instance(tty, heap, caller, flags, instance, automatic ? 0 : 2); }
void *kernel_tty_instance_owner(void *opaque)
{ return ((struct tty_instance *)opaque)->tty->transport_owner; }
unsigned kernel_tty_open_count(struct kernel_tty *tty) { return tty->opens; }
int kernel_tty_base_only(struct kernel_tty *tty)
{ return tty->references == 1 && !tty->opens && !tty->reader && !tty->writer && !tty->mode_owner; }
void kernel_tty_packet_event(struct kernel_tty *tty, unsigned flags)
{
    uintptr_t irq = arch_interrupt_save();
    if (tty->packet) {
        if (flags & 12U) tty->packet_status &= ~12U;
        if (flags & 48U) tty->packet_status &= ~48U;
        tty->packet_status |= (uint8_t)flags; changed(tty);
    }
    arch_interrupt_restore(irq);
}
void kernel_tty_discard(struct kernel_tty *tty)
{
    uintptr_t irq = arch_interrupt_save();
    flush_input(tty); flush_output(tty); tty->xchar_valid = 0;
    arch_interrupt_restore(irq);
}
const struct kernel_char_device *kernel_tty_device_template(void)
{
    static const struct kernel_char_device operations = {
        .kind = KERNEL_OPEN_FILE_KIND_CONSOLE, .release = release_instance,
        .read = device_read, .write = device_write, .readv = device_readv,
        .writev = device_writev, .ioctl = device_ioctl, .poll = device_poll
    };
    return &operations;
}
