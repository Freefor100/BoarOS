#ifndef BOAROS_KERNEL_CPU_CONFIG_H
#define BOAROS_KERNEL_CPU_CONFIG_H
#ifndef BOAROS_MAX_CPUS
#define BOAROS_MAX_CPUS 1
#endif
#ifndef BOAROS_LOCK_DEBUG
#define BOAROS_LOCK_DEBUG 0
#endif
#if BOAROS_LOCK_DEBUG != 0 && BOAROS_LOCK_DEBUG != 1
#error unsupported lock debug setting
#endif
#ifndef BOAROS_USER_SMP
#define BOAROS_USER_SMP 0
#endif
#if BOAROS_USER_SMP && BOAROS_MAX_CPUS != 2
#error remote user execution requires the two-CPU build
#endif
#if BOAROS_MAX_CPUS != 1 && BOAROS_MAX_CPUS != 2
#error unsupported CPU capacity
#endif
#if BOAROS_MAX_CPUS > 1 && BOAROS_COST_DIAGNOSTICS
#error SMP COST observation has not been implemented
#endif
#endif
