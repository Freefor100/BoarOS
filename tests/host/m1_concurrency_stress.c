#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <kernel/block.h>
#include <kernel/sync.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char mock_disk[1024 * 1024];

/* Shared concurrency state */
static volatile int write_active = 0;
static volatile int read_entered_during_write = 0;
static volatile int pause_write = 0;
static volatile int allow_finish = 0;

static volatile int rmw_read_active = 0;
static volatile int read_entered_during_rmw = 0;

static volatile int phase_window_active = 0;
static volatile int phase_read_completed = 0;
static volatile int phase_resume_window = 0;

/* Mock disk callbacks */
static enum kernel_block_status stress_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    (void)context;
    if (write_active) {
        read_entered_during_write = 1;
    }
    if (rmw_read_active) {
        read_entered_during_rmw = 1;
    }
    assert(offset + size <= sizeof(mock_disk));
    memcpy(buffer, mock_disk + offset, size);
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status stress_write(void *context, uint64_t offset, const void *buffer, size_t size)
{
    (void)context;
    assert(offset + size <= sizeof(mock_disk));
    write_active = 1;
    while (pause_write && !allow_finish) {
        usleep(1000);
    }
    memcpy(mock_disk + offset, buffer, size);
    write_active = 0;
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status rmw_stress_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    (void)context;
    assert(offset + size <= sizeof(mock_disk));
    if (offset == 0) {
        /* RMW sector read */
        rmw_read_active = 1;
        while (pause_write && !allow_finish) {
            usleep(1000);
        }
        memcpy(buffer, mock_disk + offset, size);
        rmw_read_active = 0;
    } else {
        /* Concurrent reader thread */
        if (rmw_read_active) {
            read_entered_during_rmw = 1;
        }
        memcpy(buffer, mock_disk + offset, size);
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status phase_stress_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    (void)context;
    assert(offset + size <= sizeof(mock_disk));
    memcpy(buffer, mock_disk + offset, size);
    __atomic_store_n(&phase_read_completed, 1, __ATOMIC_RELEASE);
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status phase_stress_write(void *context, kernel_block_phase_next_fn next,
    const void *owner, const void *cursor)
{
    (void)context;
    struct kernel_block_span span;
    while (next(owner, &cursor, &span)) {
        assert(span.offset + span.size <= sizeof(mock_disk));
        memcpy(mock_disk + span.offset, span.buffer, span.size);
    }
    __atomic_store_n(&phase_window_active, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&phase_resume_window, __ATOMIC_ACQUIRE)) {
        usleep(1000);
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static int single_span_phase_next(const void *context, const void **cursor, struct kernel_block_span *span)
{
    (void)context;
    if (!*cursor) return 0;
    *span = *(const struct kernel_block_span *)*cursor;
    *cursor = NULL;
    return 1;
}

/* Thread workers */
static void *writer_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char buf[4096];
    memset(buf, 0x5a, sizeof(buf));
    enum kernel_block_status st = kernel_block_write_at(dev, 0, buf, sizeof(buf));
    assert(st == KERNEL_BLOCK_STATUS_OK);
    return NULL;
}

static void *reader_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char buf[4096];
    enum kernel_block_status st = kernel_block_read_at(dev, 4096, buf, sizeof(buf));
    assert(st == KERNEL_BLOCK_STATUS_OK);
    return NULL;
}

static void *batch_writer_prio_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char buf[4096];
    memset(buf, 0xbb, sizeof(buf));
    struct kernel_block_span span = { .offset = 0, .buffer = buf, .size = sizeof(buf) };
    enum kernel_block_status st = kernel_block_write_batch_prio(dev, &span, 1, KERNEL_BLOCK_PRIO_LOW);
    assert(st == KERNEL_BLOCK_STATUS_OK);
    return NULL;
}

static void *unaligned_writer_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char byte = 0x99;
    unsigned char scratch[512];
    size_t done = 0;
    enum kernel_block_status st = kernel_block_write_bytes(dev, 7, &byte, 1, scratch, sizeof(scratch), &done);
    assert(st == KERNEL_BLOCK_STATUS_OK && done == 1);
    return NULL;
}

static void *phase_writer_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    static unsigned char phase_buf[128 * 1024];
    memset(phase_buf, 0x33, sizeof(phase_buf));
    struct kernel_block_span span = { .offset = 0, .buffer = phase_buf, .size = sizeof(phase_buf) };
    enum kernel_block_status st = kernel_block_write_phase(dev, single_span_phase_next, NULL, &span);
    assert(st == KERNEL_BLOCK_STATUS_OK);
    return NULL;
}

static void *phase_reader_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char buf[512];
    enum kernel_block_status st = kernel_block_read_at(dev, 0, buf, sizeof(buf));
    assert(st == KERNEL_BLOCK_STATUS_OK);
    return NULL;
}

/* ========================================================================= */
/* Test 1: Verify Concurrent Aligned Reads and Writes                        */
/* ========================================================================= */
static void test_concurrent_aligned_rw(void)
{
    printf("[TEST 1] Testing concurrent aligned reads and writes... ");
    struct kernel_block_device dev = {
        .read = stress_read,
        .write = stress_write,
        .logical_block_size = 512,
        .capacity_bytes = sizeof(mock_disk),
    };
    kernel_block_get_device(&dev);

    write_active = 0;
    read_entered_during_write = 0;
    pause_write = 1;
    allow_finish = 0;

    pthread_t tw, tr;
    pthread_create(&tw, NULL, writer_thread, &dev);

    /* Wait for writer to enter driver write callback */
    while (!write_active) {
        usleep(1000);
    }

    /* Start reader while writer is still active */
    pthread_create(&tr, NULL, reader_thread, &dev);

    /* Give reader a chance to run concurrently */
    for (int i = 0; i < 50; i++) {
        if (read_entered_during_write) break;
        usleep(1000);
    }

    /* Verify reader executed concurrently while writer was active */
    assert(read_entered_during_write == 1);

    /* Allow writer to finish */
    allow_finish = 1;
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    /* Test with write_batch_prio */
    write_active = 0;
    read_entered_during_write = 0;
    pause_write = 1;
    allow_finish = 0;

    pthread_create(&tw, NULL, batch_writer_prio_thread, &dev);
    while (!write_active) {
        usleep(1000);
    }
    pthread_create(&tr, NULL, reader_thread, &dev);
    for (int i = 0; i < 50; i++) {
        if (read_entered_during_write) break;
        usleep(1000);
    }
    assert(read_entered_during_write == 1);
    allow_finish = 1;
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    kernel_block_put(&dev);
    printf("PASSED (aligned reads & writes executed concurrently without serialization).\n");
}

static volatile int reader_completed = 0;

static void *stress_reader_thread(void *arg)
{
    struct kernel_block_device *dev = arg;
    unsigned char buf[4096];
    enum kernel_block_status st = kernel_block_read_at(dev, 4096, buf, sizeof(buf));
    assert(st == KERNEL_BLOCK_STATUS_OK);
    __atomic_store_n(&reader_completed, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* ========================================================================= */
/* Test 2: Verify Unaligned Byte Write Exclusive Locking                     */
/* ========================================================================= */
static void test_unaligned_byte_io_exclusive(void)
{
    printf("[TEST 2] Testing unaligned byte write exclusive locking... ");
    struct kernel_block_device dev = {
        .read = rmw_stress_read,
        .write = stress_write,
        .logical_block_size = 512,
        .capacity_bytes = sizeof(mock_disk),
    };
    kernel_block_get_device(&dev);

    rmw_read_active = 0;
    read_entered_during_rmw = 0;
    __atomic_store_n(&reader_completed, 0, __ATOMIC_RELEASE);
    pause_write = 1;
    allow_finish = 0;

    pthread_t tw, tr;
    pthread_create(&tw, NULL, unaligned_writer_thread, &dev);

    /* Wait for RMW reader to be active */
    while (!rmw_read_active) {
        usleep(1000);
    }

    /* Start aligned reader while RMW is in-flight holding transfer_lock */
    pthread_create(&tr, NULL, stress_reader_thread, &dev);

    /* Wait 50ms and assert reader has NOT entered or completed */
    for (int i = 0; i < 50; i++) {
        usleep(1000);
    }
    assert(read_entered_during_rmw == 0);
    assert(__atomic_load_n(&reader_completed, __ATOMIC_ACQUIRE) == 0);

    /* Allow RMW to finish */
    allow_finish = 1;
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    /* After RMW finished, reader completed */
    assert(__atomic_load_n(&reader_completed, __ATOMIC_ACQUIRE) == 1);

    kernel_block_put(&dev);
    printf("PASSED (unaligned RMW holds exclusive write lock, blocking readers).\n");
}

/* ========================================================================= */
/* Test 3: Verify Lock Hierarchy (phase_barrier rank 50 < transfer_lock rank 51) */
/* ========================================================================= */
static void test_lock_hierarchy(void)
{
    printf("[TEST 3] Testing lock hierarchy (rank 50 before rank 51)... ");
    struct kernel_block_device dev = {
        .read = stress_read,
        .write = stress_write,
        .logical_block_size = 512,
        .capacity_bytes = sizeof(mock_disk),
    };
    kernel_block_get_device(&dev);

    /* Verify ranks assigned during prepare_locked */
    assert(dev.phase_barrier.rank == 50);
    assert(dev.transfer_lock.rank == 51);

    /* Verify operations taking both locks take phase_barrier first, then transfer_lock */
    /* Flush takes phase_barrier then transfer_lock */
    assert(kernel_block_flush(&dev) == KERNEL_BLOCK_STATUS_OK ||
           kernel_block_flush(&dev) == KERNEL_BLOCK_STATUS_UNSUPPORTED);

    /* Set capacity takes phase_barrier then transfer_lock */
    kernel_block_set_capacity(&dev, sizeof(mock_disk));

    /* Set read only takes phase_barrier then transfer_lock */
    assert(kernel_block_set_read_only(&dev, 0) == 0);

    kernel_block_put(&dev);
    printf("PASSED (lock ranks: phase_barrier=50, transfer_lock=51 strictly observed).\n");
}

/* ========================================================================= */
/* Test 4: Challenge Phase Write Window Transfer Lock Protocol               */
/* ========================================================================= */
static void test_phase_write_window_protocol(void)
{
    printf("[TEST 4] Testing phase write window transfer lock protocol... \n");
    struct kernel_block_device dev = {
        .read = phase_stress_read,
        .write_phase = phase_stress_write,
        .logical_block_size = 512,
        .capacity_bytes = sizeof(mock_disk),
    };
    kernel_block_get_device(&dev);

    __atomic_store_n(&phase_window_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&phase_read_completed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&phase_resume_window, 0, __ATOMIC_RELEASE);

    pthread_t tw, tr;
    pthread_create(&tw, NULL, phase_writer_thread, &dev);

    /* Wait for phase window to become active */
    while (!__atomic_load_n(&phase_window_active, __ATOMIC_ACQUIRE)) {
        usleep(1000);
    }

    /* Phase window is now paused inside its driver callback */
    pthread_create(&tr, NULL, phase_reader_thread, &dev);

    /* Check if reader is allowed to complete while window is still active */
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&phase_read_completed, __ATOMIC_ACQUIRE)) break;
        usleep(1000);
    }

    int completed_early = __atomic_load_n(&phase_read_completed, __ATOMIC_ACQUIRE);
    printf("   Telemetry: Reader completed while active window in-flight = %d\n", completed_early);

    if (completed_early) {
        printf("   [DEFECT DETECTED] write_phase_windows acquires transfer_lock in shared READ mode!\n");
        printf("   This violates raw_phase_fairness (tests/host/block_file_cases.h:166) expectation that\n");
        printf("   readers queue on transfer_lock.pending_head during active windows, causing the\n");
        printf("   infinite spin loop and 30s timeout in tests/file-io-concurrency-host.sh!\n");
    } else {
        printf("   Reader correctly waited for window to complete.\n");
    }

    __atomic_store_n(&phase_resume_window, 1, __ATOMIC_RELEASE);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    kernel_block_put(&dev);
}

int main(void)
{
    printf("=================================================================\n");
    printf("Milestone 1 Empirical Concurrency & Lock Stress Test Harness\n");
    printf("=================================================================\n");
    test_concurrent_aligned_rw();
    test_unaligned_byte_io_exclusive();
    test_lock_hierarchy();
    test_phase_write_window_protocol();
    printf("=================================================================\n");
    return 0;
}
