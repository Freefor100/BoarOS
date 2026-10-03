#include <arch/riscv/uart_tty.h>
#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/timer.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/errno.h>

#define RX_SIZE 1024U
#define CONSOLE_SIZE 1024U
#define RX_INTERRUPTS 5U
struct riscv_uart_tty {
    struct kernel_heap *heap;
    struct kernel_tty *tty;
    struct kernel_thread_join worker;
    struct kernel_wait_queue work;
    volatile unsigned char *mapping;
    struct dtb_uart_info info;
    struct riscv_uart_statistics statistics;
    struct kernel_tty_rx rx[RX_SIZE];
    unsigned char console[CONSOLE_SIZE];
    uint32_t rx_read, rx_write, console_read, console_write, frequency;
    uint8_t ier, input_enabled, registered, stopping, tx_active, turn;
    uint8_t published, setup_rollback;
    uint64_t stop_deadline;
    int stop_error;
};
static struct riscv_uart_tty *console_port;
#ifndef UART_READ
static uint8_t uart_read(struct riscv_uart_tty *p, unsigned offset)
{
    volatile void *address = p->mapping + ((size_t)offset << p->info.shift);
    return p->info.width == 4 ? (uint8_t)*(volatile uint32_t *)address
                              : *(volatile uint8_t *)address;
}
#define UART_READ(p,offset) uart_read(p,offset)
#endif
#ifndef UART_WRITE
static void uart_write(struct riscv_uart_tty *p, unsigned offset, uint8_t value)
{
    volatile void *address = p->mapping + ((size_t)offset << p->info.shift);
    if (p->info.width == 4) *(volatile uint32_t *)address = value;
    else *(volatile uint8_t *)address = value;
}
#define UART_WRITE(p,offset,value) uart_write(p,offset,value)
#endif
static void set_ier(struct riscv_uart_tty *p, uint8_t value)
{
    p->ier = value; UART_WRITE(p, 1, value);
}
static void wake(struct riscv_uart_tty *p)
{
    if (kernel_wait_queue_wake_all(&p->work) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
}
static void interrupt(void *owner)
{
    struct riscv_uart_tty *p = owner;
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
    struct riscv_uart_tty *p = owner;
    uintptr_t irq = riscv_interrupt_save();
    /* 准备中的port尚无可见客户；回调不能替外部提前接受TX。 */
    if (!p->published || p->setup_rollback) __builtin_trap();
    size_t accepted = 0;
    while (accepted < size && (UART_READ(p, 5) & 32)) {
        UART_WRITE(p, 0, bytes[accepted++]);
        p->statistics.transmitted++; p->tx_active = 1;
    }
    riscv_interrupt_restore(irq);
    return accepted;
}
static int drained(void *owner)
{
    struct riscv_uart_tty *p = owner;
    uintptr_t irq = riscv_interrupt_save();
    int ready;
    if (p->setup_rollback) {
        /* 未发布且无接受的TX；旧raw shift字节不借用这些PIO软件对象。 */
        if (p->published || p->registered || p->worker.task ||
            p->statistics.transmitted || p->console_read != p->console_write ||
            kernel_tty_output_pending(p->tty)) __builtin_trap();
        ready = 1;
    } else ready = p->console_read == p->console_write && (UART_READ(p, 5) & 64);
    riscv_interrupt_restore(irq); return ready;
}
static void kick(void *owner)
{
    struct riscv_uart_tty *p = owner;
    uintptr_t irq = riscv_interrupt_save(); wake(p); riscv_interrupt_restore(irq);
}
static int configure(void *owner, struct kernel_tty_termios *settings)
{
    struct riscv_uart_tty *p = owner;
    /* Linux CBAUD编码；输入速度归一到唯一的硬件线路速度。 */
    static const uint32_t rates[] = {0,50,75,110,134,150,200,300,600,1200,1800,2400,4800,9600,19200,38400};
    uint32_t baud_code = settings->cflag & 0x100fU, rate;
    if (baud_code < 16) rate = rates[baud_code];
    else if (baud_code == 0x1001) rate = 57600;
    else if (baud_code == 0x1002) rate = 115200;
    else if (baud_code == 0x1003) rate = 230400;
    else { baud_code = 0x1002; rate = 115200; }
    uint64_t divisor = rate ? ((uint64_t)p->info.clock + 8ULL * rate) / (16ULL * rate) : 0;
    if (rate && (!divisor || divisor > 65535)) return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    uint8_t lcr = (settings->cflag >> 4) & 3;
    if (settings->cflag & 0x40) lcr |= 4;
    if (settings->cflag & 0x100) { lcr |= 8; if (!(settings->cflag & 0x200)) lcr |= 16; }
    /* 不声明硬件没有实现的CMSPAR、CRTSCTS、BOTHER或独立输入速度。 */
    settings->cflag &= ~(0x80000000U | 0x40000000U | 0x100f0000U | 0x100fU);
    settings->cflag |= baud_code;
    if (rate) {
        UART_WRITE(p, 3, lcr | 128);
        UART_WRITE(p, 0, (uint8_t)divisor); UART_WRITE(p, 1, (uint8_t)(divisor >> 8));
    }
    UART_WRITE(p, 3, lcr);
    UART_WRITE(p, 4, rate ? 3 : 0);
    p->input_enabled = rate && (settings->cflag & 0x80);
    set_ier(p, (p->ier & 2) | (p->input_enabled && !p->stopping && p->rx_write-p->rx_read < RX_SIZE ? RX_INTERRUPTS : 0));
    riscv_interrupt_restore(irq); return 0;
}
static void last_close(void *owner, uint32_t cflag)
{
    struct riscv_uart_tty *p = owner;
    uintptr_t irq = riscv_interrupt_save();
    if (cflag & 0x400) UART_WRITE(p, 4, 0);
    riscv_interrupt_restore(irq);
}
static const struct kernel_tty_transport transport = {transmit, drained, configure, kick, last_close};
static size_t console_service(struct riscv_uart_tty *p, size_t budget)
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
    struct riscv_uart_tty *p = owner;
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        if (p->setup_rollback) { riscv_interrupt_restore(irq); return; }
        if (!p->published) __builtin_trap();
        struct kernel_tty_rx batch[256]; size_t received = 0;
        while (received < 256 && p->rx_read != p->rx_write)
            batch[received++] = p->rx[p->rx_read++ % RX_SIZE];
        if (p->input_enabled && !p->stopping && p->rx_write-p->rx_read < RX_SIZE)
            set_ier(p, p->ier | RX_INTERRUPTS);
        riscv_interrupt_restore(irq);
        if (received) kernel_tty_receive(p->tty, batch, received);
        irq = riscv_interrupt_save();
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
            if (!pending && !p->tx_active) { riscv_interrupt_restore(irq); return; }
            if (riscv_time_read() >= p->stop_deadline) {
                p->stop_error = -KERNEL_EIO; riscv_interrupt_restore(irq); return;
            }
        }
        if (p->rx_read != p->rx_write || (pending && ready && written)) {
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            riscv_interrupt_restore(irq); continue;
        }
        /* THRE IRQ只负责下一批credit；TEMT没有IRQ，以10ms期限通知drain。 */
        set_ier(p, (p->ier & ~2U) | (pending && !ready ? 2 : 0));
        uint64_t deadline = (p->tx_active || p->stopping) ? riscv_time_read() + ((uint64_t)p->frequency+99)/100 : 0;
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&p->work, deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        riscv_interrupt_restore(irq);
    }
}
int riscv_uart_tty_start(struct riscv_uart_tty **owner, struct kernel_heap *heap,
    const struct dtb_uart_info *info, volatile void *mapping, uint32_t frequency)
{
    if (!owner || *owner || !heap || !info || !mapping || !frequency || !info->source || !info->clock ||
        info->shift > 4 || (info->width != 1 && info->width != 4) ||
        info->registers.size < (7ULL << info->shift) + info->width ||
        (info->width == 4 && (info->shift < 2 || ((uintptr_t)mapping & 3))) || console_port)
        return -KERNEL_EINVAL;
    struct riscv_uart_tty *p = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(heap, 1, sizeof(*p), (void **)&p);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    p->heap = heap; p->info = *info; p->mapping = mapping; p->frequency = frequency;
    kernel_wait_queue_init(&p->work);
    uintptr_t irq = riscv_interrupt_save();
    UART_WRITE(p, 3, 3); set_ier(p, 0);
    UART_WRITE(p, 2, 7); /* FIFO enable/reset只在没有接受运行期字节时执行。 */
    int error = kernel_tty_create(heap, &transport, p, &p->tty);
    if (error) goto fail;
    /* THRE不代表TEMT；第一批没有新TX时也要观察早期硬件尾字节。 */
    p->tx_active = !(UART_READ(p, 5) & 64);
    if (!riscv_plic_register(info->source, interrupt, p)) { error = -KERNEL_EIO; goto fail; }
    p->registered = 1;
    if (kernel_thread_create_joinable(worker, p, &p->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        error = -KERNEL_ENOMEM; goto fail;
    }
    p->published = 1;
    *owner = p; console_port = p; kernel_tty_publish_serial(p->tty);
    riscv_interrupt_restore(irq); return 0;
fail:
    set_ier(p, 0);
    if (p->registered) { riscv_plic_unregister(p->info.source, p); p->registered = 0; }
    if (p->published || console_port == p || p->statistics.transmitted ||
        p->console_read != p->console_write ||
        (p->tty && kernel_tty_output_pending(p->tty))) __builtin_trap();
    p->setup_rollback = 1;
    if (p->worker.task) { wake(p); kernel_thread_join(&p->worker); }
    if (p->tty && kernel_tty_destroy(&p->tty)) __builtin_trap();
    if (kernel_heap_release(heap, p) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    riscv_interrupt_restore(irq); return error;
}
int riscv_uart_tty_console(char character)
{
    uintptr_t irq = riscv_interrupt_save(); struct riscv_uart_tty *p = console_port;
    if (!p || p->stopping) { riscv_interrupt_restore(irq); return 0; }
    if (p->console_write-p->console_read == CONSOLE_SIZE) p->statistics.console_dropped++;
    else { p->console[p->console_write++ % CONSOLE_SIZE] = (unsigned char)character; wake(p); }
    riscv_interrupt_restore(irq); return 1;
}
void riscv_uart_tty_statistics(struct riscv_uart_tty *p, struct riscv_uart_statistics *statistics)
{
    uintptr_t irq = riscv_interrupt_save(); *statistics = p->statistics; riscv_interrupt_restore(irq);
}
int riscv_uart_tty_stop_report(struct riscv_uart_tty **owner,
                             struct riscv_uart_statistics *statistics)
{
    if (!owner || !*owner) return 0;
    struct riscv_uart_tty *p = *owner;
    uintptr_t irq = riscv_interrupt_save();
    if (!p->stopping) { p->stopping = 1; kernel_tty_shutdown(p->tty); }
    set_ier(p, p->ier & ~RX_INTERRUPTS);
    p->stop_error = 0; p->stop_deadline = riscv_time_read() + p->frequency;
    if (!p->worker.task && kernel_thread_create_joinable(worker, p, &p->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        riscv_interrupt_restore(irq); return -KERNEL_ENOMEM;
    }
    wake(p); kernel_thread_join(&p->worker);
    if (p->stop_error) { riscv_interrupt_restore(irq); return p->stop_error; }
    set_ier(p, 0);
    if (p->registered) { riscv_plic_unregister(p->info.source, p); p->registered = 0; }
    console_port = 0;
    int error = kernel_tty_destroy(&p->tty);
    if (error) { riscv_interrupt_restore(irq); return error; }
    if (statistics) *statistics = p->statistics;
    if (kernel_heap_release(p->heap, p) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    *owner = 0; riscv_interrupt_restore(irq); return 0;
}

int riscv_uart_tty_stop(struct riscv_uart_tty **owner)
{
    return riscv_uart_tty_stop_report(owner, 0);
}

unsigned riscv_uart_tty_mapping_ranges(struct dtb_memory_range early,
    const struct dtb_uart_info *info, struct dtb_memory_range ranges[2])
{
    if (!info || !ranges || !early.size || early.base > UINT64_MAX-early.size)
        return 0;
    struct dtb_memory_range spans[2] = {early, info->registers};
    unsigned count = spans[1].size ? 2 : 1;
    for (unsigned i = 0; i < count; i++) {
        if (spans[i].base > UINT64_MAX-spans[i].size) return 0;
        uint64_t end = spans[i].base+spans[i].size;
        if (end > UINT64_MAX-BOAROS_PAGE_MASK) return 0;
        spans[i].base &= ~BOAROS_PAGE_MASK;
        spans[i].size = ((end+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK)-spans[i].base;
    }
    if (count == 2 && spans[0].base < spans[1].base+spans[1].size &&
        spans[1].base < spans[0].base+spans[0].size) {
        uint64_t start = spans[0].base < spans[1].base ? spans[0].base : spans[1].base;
        uint64_t a = spans[0].base+spans[0].size, b = spans[1].base+spans[1].size;
        spans[0] = (struct dtb_memory_range){start, (a > b ? a : b)-start};
        count = 1;
    }
    for (unsigned i = 0; i < count; i++) ranges[i] = spans[i];
    return count;
}
