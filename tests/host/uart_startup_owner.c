/* Real TTY create/destroy with an ns16550 register and scheduler boundary model.
 * The hardware-only early shift byte borrows no port/core software storage. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main basic_main
#define kernel_scheduler_block_current fixture_core_block
#define kernel_wait_queue_wake_all fixture_core_wake
#define kernel_heap_allocate_zeroed fixture_core_allocate
#include "../tty/core_host.c"
#undef kernel_heap_allocate_zeroed
#undef kernel_wait_queue_wake_all
#undef kernel_scheduler_block_current
#undef main
#pragma GCC diagnostic pop
#include <arch/riscv/uart_tty.h>
unsigned fixture_irq_enabled=1;
static uint8_t registers[8], dll, dlh;
static unsigned hardware_empty, tx_writes, fail_registration, fail_worker, registered;
static unsigned worker_creations, event_wakes, drain_wakes;
static struct kernel_wait_queue *drain_waiters;
static uint64_t ticks=1, last_deadline;
static void (*worker_entry)(void *);
static void *worker_owner;
static unsigned joining;
static jmp_buf worker_park;
static size_t port_bytes, core_bytes;
uint8_t fixture_uart_read(unsigned offset) {
    if(offset==2)return 1;
    if(offset==5)return 32|(hardware_empty?64:0);
    if((registers[3]&128)&&offset==0)return dll;
    if((registers[3]&128)&&offset==1)return dlh;
    return registers[offset];
}
void fixture_uart_write(unsigned offset,uint8_t value) {
    if(offset==0) {
        if(registers[3]&128)dll=value;
        else {tx_writes++;hardware_empty=0;}
    } else if(offset==1&&(registers[3]&128))dlh=value;
    else registers[offset]=value;
}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,
                                                    size_t size,void **p) {
    if(!port_bytes)port_bytes=n*size;else if(!core_bytes)core_bytes=n*size;
    return fixture_core_allocate(h,n,size,p);
}
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q) {
    assert(!fixture_irq_enabled);event_wakes++;
    if(q==drain_waiters)drain_wakes++;
    return fixture_core_wake(q);
}
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,
    uint64_t deadline,int interruptible,enum kernel_wait_wake_reason *reason) {
    assert(!fixture_irq_enabled);
    if(interruptible)return fixture_core_block(q,deadline,interruptible,reason);
    assert(q&&q->initialized);last_deadline=deadline;
    if(joining){assert(deadline);ticks=deadline;*reason=KERNEL_WAIT_TIMEOUT;return 0;}
    longjmp(worker_park,1);
}
int riscv_plic_register(uint32_t source,void (*entry)(void *),void *owner) {
    assert(source==11&&entry&&owner&&!registered&&!fixture_irq_enabled);
    assert(!kernel_tty_device_lookup(0x440)&&!riscv_uart_tty_console('X'));
    assert(!tx_writes);
    if(fail_registration)return 0;
    registered=1;return 1;
}
void riscv_plic_unregister(uint32_t source,void *owner) {
    assert(source==11&&owner&&registered&&!fixture_irq_enabled);registered=0;
}
enum kernel_scheduler_status kernel_thread_create_joinable(void (*entry)(void *),
    void *owner,struct kernel_thread_join *join) {
    assert(entry&&owner&&!join->task&&!fixture_irq_enabled);
    if(fail_worker) {
        assert(!kernel_tty_device_lookup(0x440)&&!riscv_uart_tty_console('X')&&!tx_writes);
        return KERNEL_SCHEDULER_STATUS_NO_MEMORY;
    }
    worker_entry=entry;worker_owner=owner;join->task=(void*)1;worker_creations++;return 0;
}
void kernel_thread_join(struct kernel_thread_join *join) {
    assert(join->task);unsigned previous=fixture_irq_enabled;
    joining=1;fixture_irq_enabled=1;worker_entry(worker_owner);
    fixture_irq_enabled=previous;joining=0;join->task=0;
}
enum kernel_scheduler_status kernel_scheduler_yield_current(void) {return 0;}
uint64_t riscv_time_read(void) {return ticks;}
static void service_worker(void) {
    unsigned previous=fixture_irq_enabled;fixture_irq_enabled=1;
    if(!setjmp(worker_park))worker_entry(worker_owner);
    fixture_irq_enabled=previous;
}
static void shift_completed(void) {hardware_empty=1;service_worker();}
int main(int argc,char **argv) {
    assert(argc==2);struct riscv_uart_tty *port=0;
    struct dtb_uart_info info={{0x10000000,4096},3686400,11,0,1};
    hardware_empty=0; /* THRE ready, TEMT busy from the early raw shift byte. */
    fail_registration=!strcmp(argv[1],"irq-failure");fail_worker=!strcmp(argv[1],"worker-failure");
    int expected=fail_registration?-KERNEL_EIO:fail_worker?-KERNEL_ENOMEM:0;
    int ret=riscv_uart_tty_start(&port,&heap,&info,registers,10000);
    assert(ret==expected&&!tx_writes&&fixture_irq_enabled);
    if(expected) {
        assert(!port&&!live&&!registered&&!registers[1]);
        assert(!kernel_tty_device_lookup(0x440)); /* Software owner was never published. */
    } else if(!strcmp(argv[1],"initial-busy")) {
        assert(port&&live==2&&registered);service_worker();
        assert(last_deadline>ticks&&last_deadline-ticks<=100);
        device=kernel_tty_device_lookup(0x440);assert(device);
        assert(!device->open(&heap,&task,0400,0,&instance));
        (void)device->poll(instance,0,0,&drain_waiters);assert(drain_waiters);
        unsigned before=drain_wakes;sleep_action=shift_completed;
        assert(!device->ioctl(instance,&task,0,0,0,0x5409,1));
        assert(drain_wakes>before&&hardware_empty&&last_deadline==0&&!tx_writes);
        device->release(instance);instance=0;assert(!riscv_uart_tty_stop(&port));
        assert(!port&&!live&&!registered);
    } else {
        assert(!strcmp(argv[1],"published-stop"));
        assert(port&&live==2&&registered);
        assert(riscv_uart_tty_stop(&port)==-KERNEL_EIO);
        assert(port&&live==2&&registered&&!hardware_empty&&!tx_writes);
        unsigned created=worker_creations;hardware_empty=1;
        assert(!riscv_uart_tty_stop(&port)&&worker_creations==created+1);
        assert(!port&&!live&&!registered);
    }
    printf("UART startup real-core %s pass; port=%zu core=%zu bytes\n",argv[1],port_bytes,core_bytes);
    return 0;
}
