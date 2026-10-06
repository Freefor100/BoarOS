#include <arch/riscv/virtio_mmio_block.h>
#include <arch/riscv/plic.h>
#include <arch/bus.h>
#include <stddef.h>
static unsigned offset(enum virtio_block_register reg)
{
    static const uint16_t offsets[]={0x70,0x14,0x10,0x24,0x20,0x30,0x34,0x38,0x44,0x28,0x3c,0x40,0x50,0x80,0x84,0x90,0x94,0xa0,0xa4,0xfc,0x100,0x104};
    if ((unsigned)reg>=sizeof(offsets)/sizeof(offsets[0])) __builtin_trap();
    return offsets[reg];
}
static uint32_t mmio_read(void *context,enum virtio_block_register reg)
{ return *(volatile uint32_t *)((unsigned char *)context+offset(reg)); }
static void mmio_write(void *context,enum virtio_block_register reg,uint32_t value)
{ *(volatile uint32_t *)((unsigned char *)context+offset(reg))=value; }
static uint32_t mmio_ack(void *context)
{
    uint32_t pending=*(volatile uint32_t *)((unsigned char *)context+0x60);
    if (pending) *(volatile uint32_t *)((unsigned char *)context+0x64)=pending;
    return pending;
}
static int register_irq(void *context,uint32_t source,void (*handle)(void *),void *owner)
{ (void)context;return riscv_plic_register(source,handle,owner); }
static void unregister_irq(void *context,uint32_t source,void *owner)
{ (void)context;riscv_plic_unregister(source,owner); }
static struct virtio_block_transport_ops mmio_ops;
volatile void *riscv_virtio_mmio_block_base(const struct virtio_block_device *device)
{ return device && device->transport.ops==&mmio_ops ? device->transport.context : NULL; }
enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_init(
    struct virtio_block_device *device,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,virtio_dma_address_fn dma,uint32_t frequency)
{
    if (!device || device->state || !allocator || !dma || !frequency || !mmio || size<0x108) return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    volatile uint32_t *registers=mmio;
    if (registers[0]!=0x74726976) return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    if (registers[2]!=2) return RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    /* 启动期可仍在物理别名执行；从实际 PC 形成回调，不使用链接 VA 常量表。 */
    volatile struct virtio_block_transport_ops *ops=&mmio_ops;
    ops->read=mmio_read;ops->write=mmio_write;ops->ack_interrupt=mmio_ack;
    ops->register_irq=register_irq;ops->unregister_irq=unregister_irq;
    struct virtio_block_transport transport={(void *)mmio,&mmio_ops,registers[1]};
    return virtio_block_init(device,&transport,allocator,dma,frequency);
}
int riscv_virtio_mmio_block_enable_irq(struct virtio_block_device *device,uint32_t source)
{ return virtio_block_enable_irq(device,source); }
enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_destroy(struct virtio_block_device *device)
{ return virtio_block_destroy(device); }
void riscv_virtio_mmio_block_get_statistics(const struct virtio_block_device *device,struct virtio_block_statistics *statistics)
{ virtio_block_get_statistics(device,statistics); }
