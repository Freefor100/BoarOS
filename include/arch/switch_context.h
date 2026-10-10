#ifndef BOAROS_ARCH_SWITCH_CONTEXT_H
#define BOAROS_ARCH_SWITCH_CONTEXT_H
#include <stdint.h>

/* CPU的退出丢弃上下文不依赖任务或调度器定义；布局仍由架构汇编契约校验。 */
#if defined(BOAROS_ARCH_LOONGARCH)
struct arch_switch_context {
    uint64_t ra, sp, tp, fp, s[9];
} __attribute__((aligned(16)));
#else
struct riscv_switch_context {
    uint64_t ra, sp, tp, s[12];
} __attribute__((aligned(16)));
#define arch_switch_context riscv_switch_context
#endif
#endif
