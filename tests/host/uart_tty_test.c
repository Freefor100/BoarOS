#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define BOAROS_ARCH_RISCV_CONTEXT_H
static uintptr_t riscv_interrupt_save(void) { return 0; }
static void riscv_interrupt_restore(uintptr_t saved) { (void)saved; }
#include <arch/riscv/uart_tty.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <kernel/scheduler.h>
#include <kernel/errno.h>
static uint8_t regs[8], input[4096], errors[4096], output[8192];
static unsigned input_r,input_w,output_w,lsr_reads;
static void (*irq_handler)(void *), (*worker_entry)(void *);
static void *irq_owner,*worker_owner;
static unsigned live_allocations,wakes,rx_count,ready_count,shutdown_count;
static int fail_heap,fail_tty,fail_irq,fail_worker,temt=1,thre=1,drop_thre;
static uint64_t last_deadline;
static int tty_pending, joining;
static uint64_t now=1;
static unsigned char tty_output[4096];
static unsigned tty_read,tty_write;
static const struct kernel_tty_transport *test_transport;
static void *transport_owner;
static jmp_buf blocked;
static uint8_t hardware_read(unsigned offset) {
    if (offset==2) return input_r<input_w && (regs[1]&1) ? ((errors[input_r] && (regs[1]&4)) ? 6 : 4) : ((regs[1]&2)&&thre ? 2 : 1);
    if (offset==5) { lsr_reads++; return (thre?32:0)|(temt?64:0)|(input_r<input_w?1|errors[input_r]:0); }
    if (offset==0 && !(regs[3]&128)) return input[input_r++];
    return regs[offset];
}
static void hardware_write(unsigned offset,uint8_t value) {
    if (offset==0 && !(regs[3]&128)) { output[output_w++]=value; temt=0;if(drop_thre)thre=0; }
    else regs[offset]=value;
}
#define UART_READ(port,offset) ((void)(port),hardware_read(offset))
#define UART_WRITE(port,offset,value) ((void)(port),hardware_write(offset,value))
#include "../../arch/riscv/uart_tty.c"
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out) {
    (void)h; if(fail_heap) return KERNEL_HEAP_STATUS_EMPTY;
    *out=calloc(n,size); live_allocations++; return KERNEL_HEAP_STATUS_OK;
}
enum kernel_heap_status kernel_heap_release(struct kernel_heap *h,void *p) { (void)h; free(p);live_allocations--;return KERNEL_HEAP_STATUS_OK; }
void kernel_wait_queue_init(struct kernel_wait_queue *q) { *q=(struct kernel_wait_queue){.initialized=1}; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q;wakes++;return 0; }
enum kernel_scheduler_status kernel_thread_create_joinable(void (*fn)(void *),void *arg,struct kernel_thread_join *join) {
    if(fail_worker) return KERNEL_SCHEDULER_STATUS_NO_MEMORY;
    worker_entry=fn;worker_owner=arg;join->task=(void*)1;return 0;
}
void kernel_thread_join(struct kernel_thread_join *join) { joining=1;worker_entry(worker_owner);join->task=0;joining=0; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,uint64_t d,int i,enum kernel_wait_wake_reason *r) {
    (void)q;(void)i;(void)r;last_deadline=d;if(joining){now=d;if(tty_read==tty_write&&output_w)temt=1;return 0;}longjmp(blocked,1);
}
enum kernel_scheduler_status kernel_scheduler_yield_current(void) { return 0; }
uint64_t riscv_time_read(void) { return now; }
int riscv_plic_register(uint32_t source,void (*fn)(void*),void *owner) {
    assert(source==11);if(fail_irq)return 0;irq_handler=fn;irq_owner=owner;return 1;
}
void riscv_plic_unregister(uint32_t source,void *owner) { assert(source==11&&owner==irq_owner);irq_handler=0;irq_owner=0; }
int kernel_tty_create(struct kernel_heap *heap,const struct kernel_tty_transport *t,void *o,struct kernel_tty **out) {
    (void)heap;if(fail_tty)return -KERNEL_ENOMEM;test_transport=t;transport_owner=o;*out=(void*)2;
    struct kernel_tty_termios settings={.cflag=0x10b2};return t->configure(o,&settings);
}
int kernel_tty_destroy(struct kernel_tty **owner) { *owner=0;return 0; }
void kernel_tty_publish_serial(struct kernel_tty *tty) { (void)tty; }
void kernel_tty_shutdown(struct kernel_tty *tty) { (void)tty;shutdown_count++; }
void kernel_tty_receive(struct kernel_tty *tty,const struct kernel_tty_rx *input_rx,size_t count) {
    (void)tty;for(size_t i=0;i<count;i++){assert(input_rx[i].character==(uint8_t)rx_count);assert(input_rx[i].status==errors[rx_count]);rx_count++;}
}
size_t kernel_tty_service_output(struct kernel_tty *tty,size_t budget) {
    (void)tty;size_t n=tty_write-tty_read;if(n>budget)n=budget;
    n=test_transport->transmit(transport_owner,tty_output+tty_read,n);tty_read+=(unsigned)n;return n;
}
int kernel_tty_output_pending(struct kernel_tty *tty) { (void)tty;return tty_pending||tty_read!=tty_write; }
void kernel_tty_transport_ready(struct kernel_tty *tty) { (void)tty;ready_count++; }
static void service(void) { if(setjmp(blocked)==0)worker_entry(worker_owner); }
static void enqueue(unsigned count) { for(unsigned i=0;i<count;i++){input[input_w]=(uint8_t)input_w;errors[input_w]=(input_w%13==0)?0x1e:0;input_w++;} }
int main(void) {
    struct kernel_heap heap={0};struct riscv_uart_tty *port=0;
    struct dtb_uart_info info={{0x10000000,4096},3686400,11,0,1};
    struct dtb_memory_range mappings[2], early={0x10000000,4096};
    assert(riscv_uart_tty_mapping_ranges(early,&info,mappings)==1);
    assert(mappings[0].base==early.base&&mappings[0].size==4096);
    struct dtb_uart_info other=info;other.registers.base+=256;
    assert(riscv_uart_tty_mapping_ranges(early,&other,mappings)==1&&mappings[0].size==8192);
    other.registers.base=0x20000000;
    assert(riscv_uart_tty_mapping_ranges(early,&other,mappings)==2&&mappings[1].base==0x20000000);
    other.registers.size=0;assert(riscv_uart_tty_mapping_ranges(early,&other,mappings)==1);
    other.registers=(struct dtb_memory_range){UINT64_MAX-1,4096};
    assert(riscv_uart_tty_mapping_ranges(early,&other,mappings)==0);
    for(unsigned f=0;f<4;f++) {
        fail_heap=f==0;fail_tty=f==1;fail_irq=f==2;fail_worker=f==3;
        assert(riscv_uart_tty_start(&port,&heap,&info,regs,10000)<0);
        assert(!port&&!live_allocations&&!irq_handler&&regs[1]==0);
    }
    fail_heap=fail_tty=fail_irq=fail_worker=0;
    assert(riscv_uart_tty_start(&port,&heap,&info,regs,10000)==0);
    assert(regs[3]==3&&regs[0]==2&&(regs[1]&5)==5&&regs[4]==3);
    drop_thre=1;assert(test_transport->transmit(transport_owner,(const unsigned char*)"abc",3)==1);
    assert(output_w==1&&output[0]=='a');drop_thre=0;
    tty_output[tty_write++]='T';service();assert(regs[1]&2);
    thre=1;unsigned old_wakes=wakes;irq_handler(irq_owner);assert(!(regs[1]&2)&&wakes>old_wakes);
    service();assert(tty_read==tty_write&&last_deadline>now);
    temt=1;unsigned old_ready=ready_count;service();assert(ready_count>old_ready);
    output_w=0;tty_read=tty_write=0;
    enqueue(1100);
    unsigned before=lsr_reads;irq_handler(irq_owner);assert(input_r==64&&lsr_reads-before<=65);
    for(unsigned i=1;i<16;i++)irq_handler(irq_owner);
    assert(input_r==1024&&!(regs[1]&1));
    service();assert(rx_count==1024&&(regs[1]&1));
    irq_handler(irq_owner);irq_handler(irq_owner);service();assert(rx_count==1100);
    struct riscv_uart_statistics stats;riscv_uart_tty_statistics(port,&stats);
    assert(stats.received==1100&&stats.overruns>0);
    enqueue(1100);for(unsigned i=0;i<18;i++)irq_handler(irq_owner);service();
    irq_handler(irq_owner);irq_handler(irq_owner);service();assert(rx_count==2200);
    thre=0;for(unsigned i=0;i<1100;i++)assert(riscv_uart_tty_console('K'));
    service();riscv_uart_tty_statistics(port,&stats);assert(stats.console_dropped==76&&output_w==0);
    for(unsigned i=0;i<512;i++)tty_output[tty_write++]='U';
    thre=1;service();assert(output_w==1536&&tty_read==tty_write);
    unsigned kernels=0,users=0;for(unsigned i=0;i<output_w;i++){kernels+=output[i]=='K';users+=output[i]=='U';}
    assert(kernels==1024&&users==512);
    assert(!test_transport->drained(transport_owner));temt=1;assert(test_transport->drained(transport_owner));
    struct kernel_tty_termios settings={.cflag=0xd00f10b2};assert(!test_transport->configure(transport_owner,&settings));
    assert(!(settings.cflag&0xd00f0000));
    settings.cflag=0x4|0x40|0x100|0x200|0x800;assert(!test_transport->configure(transport_owner,&settings));assert(regs[3]==0x0c);
    test_transport->last_close(transport_owner,0x400);assert(regs[4]==0);
    settings.cflag=0x10b2;assert(!test_transport->configure(transport_owner,&settings));assert(regs[4]==3);
    tty_output[tty_write++]='S';thre=0;
    struct riscv_uart_statistics final={.transmitted=99999};
    assert(riscv_uart_tty_stop_report(&port,&final)==-KERNEL_EIO);assert(final.transmitted==99999);assert(port&&live_allocations&&irq_handler&&tty_read!=tty_write);
    thre=1;assert(!riscv_uart_tty_stop_report(&port,&final));
    assert(final.transmitted==1539&&final.received==2200&&final.console_dropped==76);
    assert(!riscv_uart_tty_stop_report(&port,&final));assert(final.transmitted==1539);assert(output[output_w-1]=='S');assert(!port&&!live_allocations&&!irq_handler&&regs[1]==0);
    assert(!riscv_uart_tty_console('x'));puts("UART IRQ/worker transport tests passed");
}
