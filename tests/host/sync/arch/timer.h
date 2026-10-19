#ifndef BOAROS_SYNC_TEST_ARCH_TIMER_H
#define BOAROS_SYNC_TEST_ARCH_TIMER_H
#include_next <arch/timer.h>
/* 宿主counter由测试的原子变量/握手提供，没有CSR I/O排序。 */
#undef arch_time_read_ordered
#define arch_time_read_ordered arch_time_read
#endif
