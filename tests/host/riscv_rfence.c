#include <arch/riscv/smp.h>
#include <arch/riscv/sbi.h>
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct kernel_cpu model[8];
static struct kernel_cpu *cpus[8];
static unsigned cpu_count;
static _Thread_local struct kernel_cpu *current;
static _Thread_local jmp_buf failure;
static const char *failed_stage;
static long failed_error;
static uint64_t failed_mask, failed_base;
static unsigned flushes, instruction_syncs;
static uint64_t flushed_address;
struct request { uint64_t mask, base, address, size; int instructions; };
static struct request requests[16];
static unsigned request_count, fail_request;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned hold, entered, acknowledged, observed, returned;

void *sync_test_cpu(void) { return current; }
struct kernel_cpu *riscv_smp_cpu(unsigned logical)
{ return logical < cpu_count ? cpus[logical] : NULL; }
void mm_test_fence(uint64_t address, int instructions)
{
    assert(!current->raw_locks);
    if (instructions) instruction_syncs++;
    else { flushes++; flushed_address = address; }
}
void riscv_smp_fail(const char *stage, long error)
{ failed_stage = stage; failed_error = error; longjmp(failure, 1); }
void riscv_smp_rfence_fail(const char *stage, long error, uint64_t mask, uint64_t base)
{
    failed_mask = mask; failed_base = base;
    riscv_smp_fail(stage, error);
}
static long request(uint64_t mask, uint64_t base, uint64_t address,
    uint64_t size, int instructions)
{
    assert(mask && base != UINT64_MAX);
    pthread_mutex_lock(&lock);
    assert(request_count < sizeof(requests) / sizeof(requests[0]));
    requests[request_count++] = (struct request){mask, base, address, size, instructions};
    unsigned targets = 0;
    for (unsigned i = 0; i < cpu_count; i++) {
        uint64_t hart = cpus[i]->hardware_id;
        if (hart >= base && hart - base < 64 && (mask & (UINT64_C(1) << (hart - base)))) {
            assert(cpus[i] != current && kernel_cpu_online(cpus[i]));
            targets |= 1U << i;
        }
    }
    assert(__builtin_popcountll(mask) == __builtin_popcount(targets));
    if (hold) {
        entered = targets;
        pthread_cond_broadcast(&changed);
        while ((acknowledged & targets) != targets) {
            observed = acknowledged & targets;
            pthread_cond_broadcast(&changed);
            pthread_cond_wait(&changed, &lock);
        }
    }
    long result = request_count == fail_request ? -4 : 0;
    pthread_mutex_unlock(&lock);
    return result;
}
long sbi_remote_sfence(uint64_t mask, uint64_t base, uint64_t address, uint64_t size)
{ return request(mask, base, address, size, 0); }
long sbi_remote_fence_i(uint64_t mask, uint64_t base)
{ return request(mask, base, 0, 0, 1); }

static void setup(const uint64_t *harts, unsigned count)
{
    memset(model, 0, sizeof(model));
    memset(requests, 0, sizeof(requests));
    cpu_count = count;
    for (unsigned i = 0; i < count; i++) {
        cpus[i] = &model[i];
        model[i].initialized = KERNEL_CPU_INITIALIZED;
        model[i].hardware_id = harts[i];
        model[i].logical_id = i;
        model[i].state = KERNEL_CPU_ONLINE;
        model[i].preempt_depth = 1;
    }
    current = cpus[0];
    request_count = fail_request = flushes = instruction_syncs = 0;
    hold = entered = acknowledged = observed = returned = 0;
    failed_stage = NULL; failed_mask = failed_base = 0;
}
static void same_window(void)
{
    const uint64_t harts[] = {0, 1, 2, 3, 4, 5, 6, 7};
    setup(harts, 8);
    riscv_smp_sync_user(0xff, 0x4000, 4096, 0);
    assert(request_count == 1 && "same-window targets must share one SBI confirmation");
    assert(requests[0].mask == 0xfe && requests[0].base == 0);
    assert(requests[0].address == 0x4000 && requests[0].size == 4096);
    assert(flushes == 1 && flushed_address == 0x4000 && !instruction_syncs);

    setup(harts, 8);
    riscv_smp_sync_user(0xff, 0, UINT64_MAX, 1);
    assert(request_count == 2 && requests[0].mask == 0xfe && requests[1].mask == 0xfe);
    assert(!requests[0].instructions && requests[1].instructions);
    assert(flushes == 1 && flushed_address == UINT64_MAX && instruction_syncs == 1);

    setup(harts, 8);
    riscv_smp_sync_instructions(0xfd);
    assert(request_count == 1 && requests[0].mask == 0xfc && requests[0].instructions);
    assert(!flushes && instruction_syncs == 1);

    setup(harts, 8);
    model[3].state = KERNEL_CPU_STOPPED;
    riscv_smp_sync_kernel(0x8000, 8192);
    assert(request_count == 1 && requests[0].mask == 0xf6 && !requests[0].instructions);
    assert(flushes == 1 && !instruction_syncs);
}
static void sparse_windows(void)
{
    const uint64_t harts[] = {19, 7, 63, 64, UINT64_C(0x100000002),
        UINT64_C(0x100000007), UINT64_MAX, 3};
    setup(harts, 8);
    riscv_smp_sync_user(0xff, 0x9000, 4096, 0);
    assert(request_count == 4);
    assert(requests[0].base == 0 && requests[0].mask == UINT64_C(0x8000000000000088));
    assert(requests[1].base == 64 && requests[1].mask == 1);
    assert(requests[2].base == UINT64_C(0x100000000) && requests[2].mask == 0x84);
    assert(requests[3].base == UINT64_C(0xffffffffffffffc0) &&
        requests[3].mask == UINT64_C(0x8000000000000000));
    setup(harts, 8);
    riscv_smp_sync_user(1, 0x9000, 4096, 1);
    assert(!request_count && flushes == 1 && instruction_syncs == 1);
}
static void *synchronize(void *unused)
{
    (void)unused; current = cpus[0];
    riscv_smp_sync_user(7, 0x4000, 4096, 0);
    pthread_mutex_lock(&lock);
    returned = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}
static void every_target_confirmation(void)
{
    const uint64_t harts[] = {0, 1, 3};
    setup(harts, 3); hold = 1;
    pthread_t publisher;
    assert(!pthread_create(&publisher, NULL, synchronize, NULL));
    pthread_mutex_lock(&lock);
    while (!entered) pthread_cond_wait(&changed, &lock);
    assert(entered == 6 && !returned);
    acknowledged = 2;
    pthread_cond_broadcast(&changed);
    while (observed != 2) pthread_cond_wait(&changed, &lock);
    assert(!returned);
    acknowledged = 6;
    pthread_cond_broadcast(&changed);
    while (!returned) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    assert(!pthread_join(publisher, NULL));
    assert(request_count == 1 && requests[0].mask == 0xa);
}
static void errors(void)
{
    const uint64_t harts[] = {0, 1, 3, 129};
    setup(harts, 4); fail_request = 2;
    if (!setjmp(failure)) {
        riscv_smp_sync_user(15, 0x4000, 4096, 0);
        assert(!"failed target window returned success");
    }
    assert(failed_stage && !strcmp(failed_stage, "MM-RFENCE") && failed_error == -4);
    assert(failed_mask == 2 && failed_base == 128 && request_count == 2);
    setup(harts, 4); fail_request = 2;
    if (!setjmp(failure)) {
        riscv_smp_sync_user(7, 0x4000, 4096, 1);
        assert(!"failed instruction confirmation returned success");
    }
    assert(failed_stage && !strcmp(failed_stage, "MM-FENCE.I") && failed_error == -4);
    assert(failed_mask == 0xa && !failed_base && request_count == 2);
    setup(harts, 4); fail_request = 1;
    if (!setjmp(failure)) {
        riscv_smp_sync_instructions(7);
        assert(!"failed instruction-only confirmation returned success");
    }
    assert(failed_stage && !strcmp(failed_stage, "ICACHE-FENCE.I") && failed_error == -4);
    assert(failed_mask == 0xa && !failed_base && request_count == 1 && !flushes);
    setup(harts, 4); model[2].state = KERNEL_CPU_STOPPED;
    if (!setjmp(failure)) {
        riscv_smp_sync_user(7, 0x4000, 4096, 0);
        assert(!"inactive target returned success");
    }
    assert(failed_stage && !strcmp(failed_stage, "MM-inactive-target") && !request_count);
}
int main(void)
{
    alarm(10);
    same_window(); sparse_windows(); every_target_confirmation(); errors();
    alarm(0);
    puts("RV RFENCE grouped hart windows, sparse identities, all-target confirmation and errors passed");
}
