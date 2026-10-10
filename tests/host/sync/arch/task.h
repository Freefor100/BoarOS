#ifndef BOAROS_SYNC_TEST_ARCH_TASK_H
#define BOAROS_SYNC_TEST_ARCH_TASK_H
#include_next <arch/task.h>
/* 宿主仅模拟栈槽的页大小；真实RV/LA窗口与翻译由客户机门禁验证。 */
#undef ARCH_KERNEL_STACK_SLOT_SIZE
#define ARCH_KERNEL_STACK_SLOT_SIZE (3U * BOAROS_PAGE_SIZE)
#endif
