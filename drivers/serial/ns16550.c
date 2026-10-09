#include <kernel/ns16550.h>
#include <arch/context.h>
#include <arch/timer.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/errno.h>

#define RX_SIZE 1024U
#define CONSOLE_SIZE 1024U
#define RX_INTERRUPTS 5U
struct ns16550_port {
    struct kernel_heap *heap;
    struct ns16550_irq_ops irq;
    struct kernel_tty *tty;
    struct kernel_thread_join worker;
    struct kernel_wait_queue work;
    volatile unsigned char *mapping;
    struct dtb_uart_info info;
    struct ns16550_statistics statistics;
    struct kernel_tty_rx rx[RX_SIZE];
    unsigned char console[CONSOLE_SIZE];
    uint32_t rx_read, rx_write, console_read, console_write;
    uint64_t frequency;
    uint8_t ier, input_enabled, registered, stopping, tx_active, turn;
    uint8_t published, setup_rollback;
    uint64_t stop_deadline;
    int stop_error;
};
static struct ns16550_port *console_port;
#ifndef UART_READ
static uint8_t uart_read(struct ns16550_port *p, unsigned offset)
{
    volatile void *address = p->mapping + ((size_t)offset << p->info.shift);
    return p->info.width == 4 ? (uint8_t)*(volatile uint32_t *)address
                              : *(volatile uint8_t *)address;
}
#define UART_READ(p,offset) uart_read(p,offset)
#endif
#ifndef UART_WRITE
static void uart_write(struct ns16550_port *p, unsigned offset, uint8_t value)
{
    volatile void *address = p->mapping + ((size_t)offset << p->info.shift);
    if (p->info.width == 4) *(volatile uint32_t *)address = value;
    else *(volatile uint8_t *)address = value;
}
#define UART_WRITE(p,offset,value) uart_write(p,offset,value)
#endif
static void set_ier(struct ns16550_port *p, uint8_t value)
{
    p->ier = value; UART_WRITE(p, 1, value);
}
static void wake(struct ns16550_port *p)
{
    if (kernel_wait_queue_wake_all(&p->work) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
}
static void interrupt(void *owner)
{
    struct ns16550_port *p = owner;
    p->statistics.interrupts++;
    /* 每次 claim 的硬件读取有界；IRQ只发布记录和通知worker。 */
    for (unsigned handled = 0; handled < 64; handled++) {
        uint8_t iir = UART_READ(p, 2);
        if (iir & 1) break;
        if ((iir & 14) == 2) {
            set_ier(p, p->ier & ~2U); wake(p); break;
        }
        if (p->rx_write - p->rx_read == RX_SIZE || p->stopping) {
            set_ier(p, p->ier & ~RX_INTERRUPTS); break;
        }
        uint8_t status = UART_READ(p, 5);
        if (!(status & 1)) {
            if (status & 2) p->statistics.overruns++;
            break;
        }
        struct kernel_tty_rx record = {UART_READ(p, 0), status & 0x1e};
        p->rx[p->rx_write++ % RX_SIZE] = record;
        p->statistics.received++;
        if (status & 2) p->statistics.overruns++;
        if (p->rx_write - p->rx_read == RX_SIZE)
            set_ier(p, p->ier & ~RX_INTERRUPTS);
    }
    wake(p);
}
static size_t transmit(void *owner, const unsigned char *bytes, size_t size)
{
    struct ns16550_port *p = owner;
    uintptr_t irq = arch_interrupt_save();
    /* 准备中的port尚无可见客户；回调不能替外部提前接受TX。 */
    if (!p->published || p->setup_rollback) __builtin_trap();
    size_t accepted = 0;
    while (accepted < size && (UART_READ(p, 5) & 32)) {
        UART_WRITE(p, 0, bytes[accepted++]);
        p->statistics.transmitted++; p->tx_active = 1;
    }
    arch_interrupt_restore(irq);
    return accepted;
}
static int drained(void *owner)
{
    struct ns16550_port *p = owner;
    uintptr_t irq = arch_interrupt_save();
    int ready;
    if (p->setup_rollback) {
        /* 未发布且无接受的TX；旧raw shift字节不借用这些PIO软件对象。 */
        if (p->published || p->registered || p->worker.task ||
            p->statistics.transmitted || p->console_read != p->console_write ||
            kernel_tty_output_pending(p->tty)) __builtin_trap();
        ready = 1;
    } else ready = p->console_read == p->console_write && (UART_READ(p, 5) & 64);
    arch_interrupt_restore(irq); return ready;
}
static void kick(void *owner)
{
    struct ns16550_port *p = owner;
    uintptr_t irq = arch_interrupt_save(); wake(p); arch_interrupt_restore(irq);
}
/* Linux asm-generic baud编码；输入/输出数字由TTY core解码。 */
static const uint32_t baud_rates[] = {
    0,50,75,110,134,150,200,300,600,1200,1800,2400,4800,9600,19200,38400,
    57600,115200,230400,460800,500000,576000,921600,1000000,1152000,1500000,
    2000000,2500000,3000000,3500000,4000000
};
static uint32_t baud_bits(unsigned index)
{ return index < 16 ? index : 0x1000U | (index - 15U); }
static int configure_line(struct ns16550_port *p, uint32_t flags, uint32_t rate)
{
    uint64_t divisor = rate ? ((uint64_t)p->info.clock + 8ULL * rate) / (16ULL * rate) : 0;
    /* 先完成全部可失败校验，错误不能留下DLL/DLH或软件IER的部分配置。 */
    if (rate && (!divisor || divisor > 65535)) return -KERNEL_EINVAL;
    uintptr_t irq = arch_interrupt_save();
    uint8_t lcr = (flags >> 4) & 3;
    if (flags & 0x40) lcr |= 4;
    if (flags & 0x100) { lcr |= 8; if (!(flags & 0x200)) lcr |= 16; }
    if (rate) {
        UART_WRITE(p, 3, lcr | 128);
        UART_WRITE(p, 0, (uint8_t)divisor); UART_WRITE(p, 1, (uint8_t)(divisor >> 8));
    }
    UART_WRITE(p, 3, lcr);
    UART_WRITE(p, 4, rate ? 3 : 0);
    p->input_enabled = rate && (flags & 0x80);
    set_ier(p, (p->ier & 2) | (p->input_enabled && !p->stopping && p->rx_write-p->rx_read < RX_SIZE ? RX_INTERRUPTS : 0));
    arch_interrupt_restore(irq); return 0;
}
static int configure(void *owner, struct kernel_tty_termios *settings)
{
    uint32_t code = settings->cflag & 0x100fU, rate;
    if (code < 16) rate = baud_rates[code];
    else if (code >= 0x1001 && code <= 0x1003) rate = baud_rates[15 + (code & 15)];
    else { code = 0x1002; rate = 115200; }
    /* 保持旧36字节配置的普通速率fallback与未实现hardware bits归一契约。 */
    uint32_t flags = (settings->cflag & ~(0x80000000U | 0x40000000U | 0x100f0000U | 0x100fU)) | code;
    int result = configure_line(owner, flags, rate);
    if (!result) settings->cflag = flags;
    return result;
}
static void encode_line_speed(struct kernel_tty_termios2 *settings)
{
    uint32_t rate = settings->ospeed;
    uint32_t flags = settings->basic.cflag;
    int explicit_input = (flags & 0x100f0000U) != 0;
    uint32_t output_tolerance = (flags & 0x100fU) == 0x1000U ? 0 : rate / 50U;
    uint32_t input_tolerance = (flags & 0x100f0000U) == 0x10000000U ||
        (!explicit_input && !output_tolerance) ? 0 : rate / 50U;
    unsigned output = UINT32_MAX, input = UINT32_MAX;
    for (unsigned i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++) {
        uint32_t distance = baud_rates[i] > rate ? baud_rates[i] - rate : rate - baud_rates[i];
        if (distance <= output_tolerance) output = i;
        if (distance <= input_tolerance) input = i;
    }
    flags &= ~(0x100fU | 0x100f0000U | 0x80000000U | 0x40000000U);
    flags |= output == UINT32_MAX ? 0x1000U : baud_bits(output);
    if (explicit_input)
        flags |= (input == UINT32_MAX ? 0x1000U : baud_bits(input)) << 16;
    settings->basic.cflag = flags;
    settings->ispeed = rate; /* ns16550没有独立RX/TX时钟，按输出线路归一。 */
}
static int configure2(void *owner, struct kernel_tty_termios2 *settings)
{
    struct ns16550_port *port = owner;
    /* 可量化的名义速度仍需落在UART单时钟范围；不能用divisor=1冒充更高速线路。 */
    uint64_t maximum = ((uint64_t)port->info.clock + port->info.clock / 100U) / 16U;
    if (settings->ospeed > maximum) return -KERNEL_EINVAL;
    struct kernel_tty_termios2 actual = *settings;
    encode_line_speed(&actual);
    int result = configure_line(owner, actual.basic.cflag, actual.ospeed);
    if (!result) *settings = actual;
    return result;
}
static void last_close(void *owner, uint32_t cflag)
{
    struct ns16550_port *p = owner;
    uintptr_t irq = arch_interrupt_save();
    if (cflag & 0x400) UART_WRITE(p, 4, 0);
    arch_interrupt_restore(irq);
}
static const struct kernel_tty_transport transport = {.transmit=transmit, .drained=drained, .configure=configure, .configure2=configure2, .kick=kick, .last_close=last_close};
static size_t console_service(struct ns16550_port *p, size_t budget)
{
    size_t done = 0;
    while (done < budget && p->console_read != p->console_write) {
        size_t count = p->console_write - p->console_read;
        size_t contiguous = CONSOLE_SIZE - p->console_read % CONSOLE_SIZE;
        if (count > contiguous) count = contiguous;
        if (count > budget-done) count = budget-done;
        size_t accepted = transmit(p, p->console + p->console_read % CONSOLE_SIZE, count);
        p->console_read += (uint32_t)accepted; done += accepted;
        if (accepted != count) break;
    }
    return done;
}
static void worker(void *owner)
{
    struct ns16550_port *p = owner;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        if (p->setup_rollback) { arch_interrupt_restore(irq); return; }
        if (!p->published) __builtin_trap();
        struct kernel_tty_rx batch[256]; size_t received = 0;
        while (received < 256 && p->rx_read != p->rx_write)
            batch[received++] = p->rx[p->rx_read++ % RX_SIZE];
        if (p->input_enabled && !p->stopping && p->rx_write-p->rx_read < RX_SIZE)
            set_ier(p, p->ier | RX_INTERRUPTS);
        arch_interrupt_restore(irq);
        if (received) kernel_tty_receive(p->tty, batch, received);
        irq = arch_interrupt_save();
        size_t written;
        /* 每轮最多256字节；轮换首个来源避免kernel/user持续输出互相饥饿。 */
        if (p->turn++ & 1) {
            written = kernel_tty_service_output(p->tty, 128);
            written += console_service(p, 256-written);
        } else {
            written = console_service(p, 128);
            written += kernel_tty_service_output(p->tty, 256-written);
        }
        int pending = p->console_read != p->console_write || kernel_tty_output_pending(p->tty);
        int ready = (UART_READ(p, 5) & 32) != 0;
        if (p->tx_active && (UART_READ(p, 5) & 64)) {
            p->tx_active = 0; kernel_tty_transport_ready(p->tty);
        }
        if (p->stopping) {
            if (!pending && !p->tx_active) { arch_interrupt_restore(irq); return; }
            if (arch_time_read() >= p->stop_deadline) {
                p->stop_error = -KERNEL_EIO; arch_interrupt_restore(irq); return;
            }
        }
        if (p->rx_read != p->rx_write || (pending && ready && written)) {
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            arch_interrupt_restore(irq); continue;
        }
        /* THRE IRQ只负责下一批credit；TEMT没有IRQ，以10ms期限通知drain。 */
        set_ier(p, (p->ier & ~2U) | (pending && !ready ? 2 : 0));
        uint64_t deadline = (p->tx_active || p->stopping) ? arch_time_read() + ((uint64_t)p->frequency+99)/100 : 0;
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&p->work, deadline, 0, &reason,
                (p->rx_read == p->rx_write && !((p->console_read != p->console_write || kernel_tty_output_pending(p->tty)) && (UART_READ(p, 5) & 32) && written))) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        arch_interrupt_restore(irq);
    }
}
int ns16550_start(struct ns16550_port **owner, struct kernel_heap *heap,
    const struct dtb_uart_info *info, volatile void *mapping, uint64_t frequency, const struct ns16550_irq_ops *irq_ops)
{
    if (!owner || *owner || !heap || !info || !mapping || !frequency || frequency>UINT64_MAX/2 ||
        !irq_ops || !irq_ops->register_irq || !irq_ops->unregister_irq || !info->source || !info->clock ||
        info->shift > 4 || (info->width != 1 && info->width != 4) ||
        info->registers.size < (7ULL << info->shift) + info->width ||
        (info->width == 4 && (info->shift < 2 || ((uintptr_t)mapping & 3))) || console_port)
        return -KERNEL_EINVAL;
    struct ns16550_port *p = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(heap, 1, sizeof(*p), (void **)&p);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    p->heap = heap; p->irq = *irq_ops; p->info = *info; p->mapping = mapping; p->frequency = frequency;
    kernel_wait_queue_init(&p->work);
    uintptr_t irq = arch_interrupt_save();
    UART_WRITE(p, 3, 3); set_ier(p, 0);
    UART_WRITE(p, 2, 7); /* FIFO enable/reset只在没有接受运行期字节时执行。 */
    int error = kernel_tty_create(heap, &transport, p, &p->tty);
    if (error) goto fail;
    /* THRE不代表TEMT；第一批没有新TX时也要观察早期硬件尾字节。 */
    p->tx_active = !(UART_READ(p, 5) & 64);
    if (!p->irq.register_irq(p->irq.context, info->source, interrupt, p)) { error = -KERNEL_EIO; goto fail; }
    p->registered = 1;
    if (kernel_thread_create_joinable(worker, p, &p->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        error = -KERNEL_ENOMEM; goto fail;
    }
    p->published = 1;
    *owner = p; console_port = p; kernel_tty_publish_serial(p->tty);
    arch_interrupt_restore(irq); return 0;
fail:
    set_ier(p, 0);
    if (p->registered) { p->irq.unregister_irq(p->irq.context, p->info.source, p); p->registered = 0; }
    if (p->published || console_port == p || p->statistics.transmitted ||
        p->console_read != p->console_write ||
        (p->tty && kernel_tty_output_pending(p->tty))) __builtin_trap();
    p->setup_rollback = 1;
    if (p->worker.task) { wake(p); kernel_thread_join(&p->worker); }
    if (p->tty && kernel_tty_destroy(&p->tty)) __builtin_trap();
    if (kernel_heap_release(heap, p) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    arch_interrupt_restore(irq); return error;
}
int ns16550_console(char character)
{
    uintptr_t irq = arch_interrupt_save(); struct ns16550_port *p = console_port;
    if (!p || p->stopping) { arch_interrupt_restore(irq); return 0; }
    if (p->console_write-p->console_read == CONSOLE_SIZE) p->statistics.console_dropped++;
    else { p->console[p->console_write++ % CONSOLE_SIZE] = (unsigned char)character; wake(p); }
    arch_interrupt_restore(irq); return 1;
}
void ns16550_statistics(struct ns16550_port *p, struct ns16550_statistics *statistics)
{
    uintptr_t irq = arch_interrupt_save(); *statistics = p->statistics; arch_interrupt_restore(irq);
}
int ns16550_stop_report(struct ns16550_port **owner,
                             struct ns16550_statistics *statistics)
{
    if (!owner || !*owner) return 0;
    struct ns16550_port *p = *owner;
    uintptr_t irq = arch_interrupt_save();
    if (!p->stopping) { p->stopping = 1; kernel_tty_shutdown(p->tty); }
    set_ier(p, p->ier & ~RX_INTERRUPTS);
    p->stop_error = 0; p->stop_deadline = arch_time_read() + p->frequency;
    if (!p->worker.task && kernel_thread_create_joinable(worker, p, &p->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        arch_interrupt_restore(irq); return -KERNEL_ENOMEM;
    }
    wake(p); kernel_thread_join(&p->worker);
    if (p->stop_error) { arch_interrupt_restore(irq); return p->stop_error; }
    set_ier(p, 0);
    if (p->registered) { p->irq.unregister_irq(p->irq.context, p->info.source, p); p->registered = 0; }
    console_port = 0;
    int error = kernel_tty_destroy(&p->tty);
    if (error) { arch_interrupt_restore(irq); return error; }
    if (statistics) *statistics = p->statistics;
    if (kernel_heap_release(p->heap, p) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    *owner = 0; arch_interrupt_restore(irq); return 0;
}

int ns16550_stop(struct ns16550_port **owner)
{
    return ns16550_stop_report(owner, 0);
}
