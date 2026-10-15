#ifndef BOAROS_KERNEL_MM_PREPARE_H
#define BOAROS_KERNEL_MM_PREPARE_H
#include <stdint.h>
/* 私有、已清零的两个候选页表页。成功仅消费实际发布的项并置为NO_PAGE；
 * 地址0仍可表达合法RV页；NO_PAGE不满足页对齐，不能由allocator发放。
 * caller使用LEAF_PREPARATION_INIT初始化；其余项仍归准备者。mask的bit0/1分别表示root child/leaf table缺失。 */
#define KERNEL_MM_NO_PAGE UINT64_MAX
#define KERNEL_MM_LEAF_PREPARATION_INIT { .tables = {KERNEL_MM_NO_PAGE, KERNEL_MM_NO_PAGE} }
struct kernel_mm_leaf_preparation { uint64_t tables[2]; };
#endif
