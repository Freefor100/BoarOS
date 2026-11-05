#ifndef BOAROS_KERNEL_MM_RETIRE_H
#define BOAROS_KERNEL_MM_RETIRE_H
#include <stdint.h>
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
