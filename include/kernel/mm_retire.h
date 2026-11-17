#ifndef BOAROS_KERNEL_MM_RETIRE_H
#define BOAROS_KERNEL_MM_RETIRE_H
#include <stdint.h>
#include <kernel/physical_page.h>
#define KERNEL_MM_RETIRE_CAPACITY 16U
/* PTE摘除把实际owner交给固定批次；确认翻译失效前不能归还。
 * 最坏一个基页同时退休data及两级空表，caller至少预留三个槽位。
 * 无动态descriptor，允许不同PTE引用同一物理页；每条归还一个实际引用。 */
/* 有界range walk只保留VA与阶段，跨同步不借用PTE指针或前缀空判定。
 * 表页释放前在同一次写资格下复查当前整表，其他范围可在批次间发布新叶。 */
struct kernel_mm_detach_cursor {
    uint64_t address, middle_address;
    unsigned phase;
};
struct kernel_mm_retirement {
    uint64_t pages[KERNEL_MM_RETIRE_CAPACITY];
    uint32_t count, nonleaf, compound;
    uint8_t orders[KERNEL_MM_RETIRE_CAPACITY];
};
/* The detached PTE transfers its existing owner, never just a physical hint.
 * Keep the whole allocation alive until the caller completes remote TLB sync.
 * Page tables are always order zero; a large data leaf occupies one slot. */
static inline void kernel_mm_retirement_add_order(struct kernel_mm_retirement *batch,
    uint64_t page, uint32_t order, int table)
{
    if (!batch || batch->count >= KERNEL_MM_RETIRE_CAPACITY ||
        order > PHYSICAL_PAGE_MAX_ORDER || (table && order)) __builtin_trap();
    batch->orders[batch->count] = (uint8_t)order;
    batch->pages[batch->count++] = page;
    if (order) batch->compound = 1;
    if (table) batch->nonleaf = 1;
}
static inline void kernel_mm_retirement_add(struct kernel_mm_retirement *batch,
    uint64_t page, int table)
{ kernel_mm_retirement_add_order(batch, page, 0, table); }
/* Must only be called after the required local/remote invalidation. */
static inline void kernel_mm_retirement_release(struct physical_page_allocator *allocator,
    struct kernel_mm_retirement *batch)
{
    if (batch->compound)
        physical_page_release_order_batch(allocator, batch->pages, batch->orders, batch->count);
    else
        physical_page_release_batch(allocator, batch->pages, batch->count);
    *batch = (struct kernel_mm_retirement){0};
}
#endif
