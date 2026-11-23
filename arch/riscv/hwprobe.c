#include <arch/riscv/hwprobe.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/smp.h>
#include <kernel/cpu.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

static struct riscv_hwprobe_cpu_info capabilities[BOAROS_MAX_CPUS];
static uint32_t timebase;

const struct riscv_hwprobe_cpu_info *riscv_hwprobe_get_cpu_info(unsigned cpu)
{
    return &capabilities[cpu];
}

void riscv_hwprobe_initialize(unsigned cpu, uint32_t features, uint32_t common, uint32_t frequency)
{
    struct riscv_hwprobe_cpu_info *values = &capabilities[cpu];
    if (!cpu)
        timebase = frequency;
    values->ids_valid = sbi_hardware_ids(values->ids) == 0;
    if (features & DTB_CPU_ISA_FD) values->extensions |= RISCV_HWPROBE_FD;
    if (features & DTB_CPU_ISA_C) values->extensions |= RISCV_HWPROBE_C;
    if (common & DTB_CPU_ISA_V) values->extensions |= RISCV_HWPROBE_V;
    if (features & DTB_CPU_ZBA) values->extensions |= RISCV_HWPROBE_ZBA;
    if (features & DTB_CPU_ZBB) values->extensions |= RISCV_HWPROBE_ZBB;
    if (features & DTB_CPU_ZBS) values->extensions |= RISCV_HWPROBE_ZBS;
    if (features & DTB_CPU_ZICBOZ_64) {
        /* Expose CBO.ZERO only after enabling and observing user permission. */
        uintptr_t envcfg;
        asm volatile("csrs 0x10a, %1; csrr %0, 0x10a" : "=r"(envcfg) : "r"(128UL) : "memory");
        if (envcfg & 128U) values->extensions |= RISCV_HWPROBE_ZICBOZ;
    }
}

static unsigned online_mask(void)
{
#if BOAROS_MAX_CPUS > 1
    unsigned mask = 0;
    for (unsigned cpu = 0; cpu < kernel_cpu_get_boot_count(); cpu++) {
        struct kernel_cpu *target = arch_smp_cpu(cpu);
        if (target && kernel_cpu_online(target))
            mask |= 1U << cpu;
    }
    return mask;
#else
    return 1;
#endif
}

struct hwprobe_pair { int64_t key; uint64_t value; };

static struct hwprobe_pair probe_pair(int64_t key, unsigned mask)
{
    struct hwprobe_pair pair = {.key = key};
    unsigned cpu = kernel_cpu_first_in_mask(mask);
    if (key >= 0 && key <= 2) {
        pair.value = capabilities[cpu].ids[key];
        while (mask) {
            cpu = kernel_cpu_first_in_mask(mask);
            mask &= mask - 1U;
            if (!capabilities[cpu].ids_valid || capabilities[cpu].ids[key] != pair.value) {
                pair.value = UINT64_MAX;
                break;
            }
        }
    } else if (key == 3) {
        pair.value = 1; /* The kernel requires the IMA base. */
    } else if (key == 4 || key == 6) {
        uint64_t bits = UINT64_MAX;
        while (mask) {
            cpu = kernel_cpu_first_in_mask(mask);
            mask &= mask - 1U;
            bits &= capabilities[cpu].extensions;
        }
        pair.value = key == 4 ? bits : ((bits & RISCV_HWPROBE_ZICBOZ) ? 64U : 0U);
    } else if (key == 7) {
        pair.value = RISCV_SV39_USER_LIMIT - 1;
    } else if (key == 8) {
        pair.value = timebase;
    } else if (key != 5 && key != 9 && key != 10) {
        pair.key = -1;
    }
    return pair;
}

static int probe_copy(struct kernel_mm *mm, uint64_t address, void *value,
    size_t length, int write)
{
    size_t copied;
    enum kernel_uaccess_status status = write ? kernel_copy_to_user(mm,address,value,length,&copied) :
        kernel_copy_from_user(mm,value,address,length,&copied);
    return status == KERNEL_UACCESS_STATUS_OK && copied == length ? 0 : -KERNEL_EFAULT;
}

static int hwprobe(struct kernel_mm *mm, const uint64_t args[6])
{
    unsigned online = online_mask();
    uint64_t pairs = args[0], count = args[1], size = args[2], cpus = args[3];
    unsigned flags = (unsigned)args[4];
    int which = flags == 1U;
    if (flags > 1U || (which && (!size || !cpus)))
        return -KERNEL_EINVAL;
    uint64_t mask = online;
    if (size || cpus) {
        if (size > sizeof(mask)) size = sizeof(mask);
        mask = 0;
        if (probe_copy(mm,cpus,&mask,(size_t)size,0)) return -KERNEL_EFAULT;
        if (!mask && which) mask = online;
        mask &= online;
    }
    if (!mask && !which)
        return -KERNEL_EINVAL;
    if (count && (count > (UINT64_MAX - pairs) / sizeof(struct hwprobe_pair)))
        return -KERNEL_EFAULT;
    for (uint64_t i = 0; i < count; i++, pairs += sizeof(struct hwprobe_pair)) {
        struct hwprobe_pair requested;
        size_t length = which ? sizeof(requested) : sizeof(requested.key);
        if (probe_copy(mm,pairs,&requested,length,0)) return -KERNEL_EFAULT;
        if (!which) {
            struct hwprobe_pair value = probe_pair(requested.key,(unsigned)mask);
            if (probe_copy(mm,pairs,&value.key,sizeof(value.key),1) ||
                probe_copy(mm,pairs+sizeof(value.key),&value.value,sizeof(value.value),1))
                return -KERNEL_EFAULT;
        } else {
            struct hwprobe_pair example = probe_pair(requested.key,1U);
            if (example.key == -1) {
                mask = 0;
                if (probe_copy(mm,pairs,&example,sizeof(example),1)) return -KERNEL_EFAULT;
                continue;
            }
            unsigned candidates = (unsigned)mask;
            while (candidates) {
                unsigned cpu = kernel_cpu_first_in_mask(candidates);
                candidates &= candidates - 1U;
                struct hwprobe_pair value = probe_pair(requested.key,1U << cpu);
                if (requested.key == 4 ? (value.value & requested.value) != requested.value :
                    value.value != requested.value) mask &= ~(1ULL << cpu);
            }
        }
    }
    return which ? probe_copy(mm,cpus,&mask,(size_t)size,1) : 0;
}

enum kernel_syscall_status riscv_syscall_hwprobe(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *result)
{
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller,&mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    result->action = KERNEL_SYSCALL_ACTION_RETURN;
    result->value = hwprobe(mm, request->arguments);
    return KERNEL_SYSCALL_STATUS_OK;
}
