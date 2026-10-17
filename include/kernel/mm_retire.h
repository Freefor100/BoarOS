#ifndef BOAROS_KERNEL_MM_RETIRE_H
#define BOAROS_KERNEL_MM_RETIRE_H
#include <stdint.h>
#define KERNEL_MM_RETIRE_CAPACITY 16U
/* PTE摘除把实际owner交给固定批次；确认翻译失效前不能归还。
 * 最坏一个基页同时退休data及两级空表，caller至少预留三个槽位。
 * 无动态descriptor，允许不同PTE引用同一物理页；每条归还一个实际引用。 */
/* 有界range walk仅保留VA、扫描下标和占用标记；跨同步不借用PTE指针。
 * 叶表完整扫描一次，范围外的叶只判断占用；中间表每个范围段结束后检查一次。 */
struct kernel_mm_detach_cursor {
    uint64_t address, middle_address;
    unsigned scan, occupied, phase;
};
struct kernel_mm_retirement {
    uint64_t pages[KERNEL_MM_RETIRE_CAPACITY];
    uint32_t count, nonleaf;
};
static inline void kernel_mm_retirement_add(struct kernel_mm_retirement *batch,
    uint64_t page, int table)
{
    if (!batch || batch->count >= KERNEL_MM_RETIRE_CAPACITY) __builtin_trap();
    batch->pages[batch->count++] = page;
    if (table) batch->nonleaf = 1;
}
#endif
