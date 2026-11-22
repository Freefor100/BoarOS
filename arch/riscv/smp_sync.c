#include <arch/riscv/smp.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/riscv/sbi.h>
#include <arch/riscv/mmu_context.h>
#include <kernel/page.h>

static uint32_t remote_targets(uint32_t targets, int kernel, const char *stage)
{
    struct kernel_cpu *local = kernel_cpu_current();
    if (!kernel) {
        targets &= ~(1U << local->logical_id);
        if (!targets) return 0;
    }
    uint32_t remote = 0;
    for (unsigned i = 0; i < BOAROS_MAX_CPUS; i++) {
        if (!kernel && !(targets & (1U << i))) continue;
        struct kernel_cpu *cpu = riscv_smp_cpu(i);
        if (!cpu) break;
        if (cpu == local) continue;
        if (!kernel_cpu_online(cpu)) {
            if (kernel) continue;
            riscv_smp_fail(stage, -1);
        }
        remote |= 1U << i;
    }
    return remote;
}

static void sync_remote(uint32_t targets, uint64_t address, uint64_t size,
    int translations, int instructions, const char *tlb_stage, const char *instruction_stage)
{
    struct kernel_cpu *local = kernel_cpu_current();
    while (targets) {
        unsigned first = 0;
        while (!(targets & (1U << first))) first++;
        /* Aligned windows also avoid SBI's special base=-1 broadcast value,
         * even when a real hart ID occupies the top unsigned-long bit. */
        uint64_t base = riscv_smp_cpu(first)->hardware_id & ~UINT64_C(63);
        uint64_t mask = 0;
        for (unsigned logical = first; logical < BOAROS_MAX_CPUS; logical++) {
            if (!(targets & (1U << logical))) continue;
            uint64_t hart = riscv_smp_cpu(logical)->hardware_id;
            if ((hart & ~UINT64_C(63)) == base) {
                mask |= UINT64_C(1) << (hart - base);
                targets &= ~(1U << logical);
            }
        }
        if (translations) {
            long error = sbi_remote_sfence(mask, base, address, size);
            if (error) riscv_smp_rfence_fail(tlb_stage, error, mask, base);
            local->rfence_requests++;
        }
        if (instructions) {
            long error = sbi_remote_fence_i(mask, base);
            if (error) riscv_smp_rfence_fail(instruction_stage, error, mask, base);
            local->rfence_requests++;
        }
    }
}

void riscv_smp_sync_kernel(uint64_t address, uint64_t size)
{
    struct kernel_cpu *local = kernel_cpu_current();
    if (local->raw_locks) __builtin_trap();
    riscv_mmu_flush_kernel_range(address, size);
    sync_remote(remote_targets(0, 1, 0), address, size, 1, 0, "RFENCE", 0);
}

void riscv_smp_sync_instructions(uint32_t targets)
{
    struct kernel_cpu *local = kernel_cpu_current();
    if (local->raw_locks || !local->preempt_depth ||
        (targets & ~((1U << BOAROS_MAX_CPUS)-1U))) __builtin_trap();
    uint32_t remote = remote_targets(targets, 0, "ICACHE-inactive-target");
    riscv_mmu_instruction_publish_barrier();
    riscv_mmu_sync_instructions();
    sync_remote(remote, 0, 0, 0, 1, 0, "ICACHE-FENCE.I");
}

void riscv_smp_sync_user(uint32_t targets, uint64_t address, uint64_t size,
    int instructions)
{
    struct kernel_cpu *local = kernel_cpu_current();
    if (local->raw_locks || (targets & ~((1U << BOAROS_MAX_CPUS) - 1U))) __builtin_trap();
    uint32_t remote = remote_targets(targets, 0, "MM-inactive-target");
    if (size == BOAROS_PAGE_SIZE) riscv_mmu_flush_address(address);
    else riscv_mmu_flush_all();
    if (instructions) {
        riscv_mmu_instruction_publish_barrier();
        riscv_mmu_sync_instructions();
    }
    sync_remote(remote, address, size, 1, instructions, "MM-RFENCE", "MM-FENCE.I");
}
#endif
