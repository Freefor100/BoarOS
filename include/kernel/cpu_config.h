#ifndef BOAROS_KERNEL_CPU_CONFIG_H
#define BOAROS_KERNEL_CPU_CONFIG_H
#ifndef BOAROS_MAX_CPUS
#define BOAROS_MAX_CPUS 1
#endif
#if BOAROS_MAX_CPUS != 1 && BOAROS_MAX_CPUS != 2
#error unsupported CPU capacity
#endif
#if BOAROS_MAX_CPUS > 1 && BOAROS_COST_DIAGNOSTICS
#error SMP COST observation has not been implemented
#endif
#endif
