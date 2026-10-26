#ifndef BOAROS_KERNEL_BLOCK_H
#define BOAROS_KERNEL_BLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <common/block_io.h>
#include <kernel/sync.h>

enum kernel_block_status {
    KERNEL_BLOCK_STATUS_OK = 0,
    KERNEL_BLOCK_STATUS_INVALID,
    KERNEL_BLOCK_STATUS_OUT_OF_RANGE,
    KERNEL_BLOCK_STATUS_IO,
    KERNEL_BLOCK_STATUS_TIMEOUT,
    KERNEL_BLOCK_STATUS_UNSUPPORTED,
    KERNEL_BLOCK_STATUS_NO_MEMORY,
    KERNEL_BLOCK_STATUS_STATE,
    KERNEL_BLOCK_STATUS_NOT_SUBMITTED,
    KERNEL_BLOCK_STATUS_READ_ONLY,
};

struct kernel_block_device;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_mm;

enum kernel_block_cache_mode {
    KERNEL_BLOCK_CACHE_UNKNOWN = 0,
    KERNEL_BLOCK_CACHE_WRITETHROUGH,
    KERNEL_BLOCK_CACHE_WRITEBACK,
};

typedef enum kernel_block_status (*kernel_block_flush_fn)(void *context);

typedef enum kernel_block_status (*kernel_block_read_fn)(
    void *context,
    uint64_t offset,
    void *buffer,
    size_t size);

typedef enum kernel_block_status (*kernel_block_write_fn)(
    void *context,
    uint64_t offset,
    const void *buffer,
    size_t size);

#define KERNEL_BLOCK_BATCH_MAX 32U

struct kernel_block_span {
    uint64_t offset;
    const void *buffer;
    size_t size;
};

typedef enum kernel_block_status (*kernel_block_read_batch_fn)(
    void *context, struct block_read_span *spans, size_t count,
    const struct block_read_completion *completion);

typedef enum kernel_block_status (*kernel_block_write_batch_fn)(
    void *context, const struct kernel_block_span *spans, size_t count);

/* Restartable immutable producer for one journal I/O phase. context is stable
 * across independent cursor traversals. The owner has
 * already established disjoint disk ranges. Each span is sector aligned. */
typedef int (*kernel_block_phase_next_fn)(const void *context, const void **cursor,
    struct kernel_block_span *span);
typedef enum kernel_block_status (*kernel_block_write_phase_fn)(void *context,
    kernel_block_phase_next_fn next, const void *next_context, const void *first);

struct kernel_block_device {
    /* Immutable while registered; each child registration retains its parent. */
    struct kernel_block_device *parent;
    uint64_t start_bytes;
    void *context;
    kernel_block_read_fn read;
    kernel_block_write_fn write;
    kernel_block_write_batch_fn write_batch;
    kernel_block_read_batch_fn read_batch;
    kernel_block_write_phase_fn write_phase;
    uint64_t capacity_bytes;
    uint32_t logical_block_size;
    kernel_block_flush_fn flush;
    enum kernel_block_cache_mode cache_mode;
    /* Registration owns the object lifetime; references cover OFDs, mounts and
     * in-flight requests. Callbacks never run with registry protection held. */
    int (*open)(struct kernel_block_device *, uint32_t flags);
    void (*close)(struct kernel_block_device *, uint32_t flags);
    void (*owners_changed)(struct kernel_block_device *);
    int64_t (*ioctl)(struct kernel_block_device *, struct kernel_files *,
        struct kernel_open_file_description **, struct kernel_mm *, uint64_t, uint64_t);
    struct kernel_rwlock phase_barrier;
    uint32_t io_initialized, references, open_count, notifications;
    uint32_t read_only, soft_block_size;

    struct kernel_block_device *registry_next;
    const void *claim_owner;
    uint64_t device_number;
    uint32_t registered;
    struct kernel_rwlock transfer_lock;
    struct kernel_block_device *pressure_parent;
    uint8_t virtual_backing;
};

/* Linux new_encode_dev(252, disk_index * 16); primary partitions add 1..4. */
#define KERNEL_BLOCK_DEVICE_NUMBER(index) \
    (UINT64_C(0xfc00) | (((uint64_t)(index) * 16U) & 0xffU) | \
     ((((uint64_t)(index) * 16U) & ~UINT64_C(0xff)) << 12))

/* Return zero or negative errno. get pins registry membership; put consumes
 * that owner. A raw device pointer requires an existing lifetime owner.
 * Unregister is refused while any open, mount or request retains a reference. */
int kernel_block_register(struct kernel_block_device *device, uint64_t number);
int kernel_block_unregister(struct kernel_block_device *device);
/* Boot discovery. Caller owns four zeroed, stable child objects until teardown.
 * No signature means a whole-disk filesystem; invalid tables publish nothing. */
int kernel_block_scan_mbr(struct kernel_block_device *device,
    struct kernel_block_device children[4]);
/* Boot/test inspection only; callers that may sleep must use get. */
struct kernel_block_device *kernel_block_lookup(uint64_t number);
struct kernel_block_device *kernel_block_get(uint64_t number);
void kernel_block_get_device(struct kernel_block_device *device);
void kernel_block_put(struct kernel_block_device *device);
/* Virtual bindings retain their actual parent independently of I/O locking.
 * Configuration ownership excludes rebinding; publication uses the registry
 * protection. A NULL parent on a virtual device denotes memory backing. */
void kernel_block_set_pressure_parent(struct kernel_block_device *,struct kernel_block_device *);
/* Return a retained physical pressure target, or NULL for memory backing. */
struct kernel_block_device *kernel_block_pressure_device(struct kernel_block_device *);

int kernel_block_open(struct kernel_block_device *, const void *owner, uint32_t flags);
void kernel_block_close(struct kernel_block_device *, const void *owner, uint32_t flags);
int kernel_block_claimed(struct kernel_block_device *);
void kernel_block_usage(struct kernel_block_device *, uint32_t *references, uint32_t *opens);
/* Whole-device capacity only; published partition extents are immutable. */
void kernel_block_set_capacity(struct kernel_block_device *, uint64_t bytes);
int kernel_block_set_read_only(struct kernel_block_device *, int read_only);
int kernel_block_read_only(const struct kernel_block_device *);
/* Task I/O and flush may execute on any CPU. Device references and the physical
 * parent transfer lock cover completion. Flush/configuration take the phase
 * barrier first, then exclusive transfer ownership to include both long phases
 * and ordinary operations.
 * Reads share the transfer lock; writes retain exclusive transfer ownership.
 * Byte I/O retains it across each sector read/modify/write. scratch must
 * hold one logical sector and remains borrowed until the request returns.
 * completed always reports the prefix confirmed by successful callbacks,
 * including before a later error. A failing callback supplies no finer progress;
 * its unreported disk or buffer effects remain unspecified. */
enum kernel_block_status kernel_block_read_bytes(struct kernel_block_device *,
    uint64_t, void *, size_t, void *scratch, size_t scratch_size, size_t *completed);
enum kernel_block_status kernel_block_write_bytes(struct kernel_block_device *,
    uint64_t, const void *, size_t, void *scratch, size_t scratch_size, size_t *completed);
int kernel_block_claim(struct kernel_block_device *device, const void *owner);
void kernel_block_release_claim(struct kernel_block_device *device,
                                const void *owner);

/* Complete all prior writes at the device's persistence boundary. A missing
 * callback is sufficient only for a declared write-through device. */
enum kernel_block_status kernel_block_flush(struct kernel_block_device *device);

enum kernel_block_status kernel_block_read_at(
    struct kernel_block_device *device,
    uint64_t offset,
    void *buffer,
    size_t size);

enum kernel_block_status kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size);

/* Synchronous, non-transactional batch of at most 32 disjoint byte ranges.
 * Preflight every span before I/O. Empty spans permit a null buffer but their
 * offset must be in capacity; count zero needs no spans. A missing callback
 * falls back in input order and stops on the first failed write. All buffers
 * remain borrowed until every published DMA completes or reset stops it. */
enum kernel_block_status kernel_block_write_batch(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count);

/* Internal journal phase: preflight the entire restartable producer before
 * publishing I/O. Payloads and producer nodes remain stable until return,
 * including on errors. Transfer ownership is returned between drained windows
 * of at most 32 spans/128 KiB; flush/configuration wait for the entire call.
 * Unlike write_batch, disjointness is established by the
 * journal's block ownership/indexes; this interface validates alignment,
 * capacity and RAM bounds. A missing callback writes in producer order. */
enum kernel_block_status kernel_block_write_phase(struct kernel_block_device *device,
    kernel_block_phase_next_fn next, const void *next_context, const void *first);

/* Preflight all spans before I/O. Disk ranges may overlap, but mutable outputs
 * must be disjoint from each other and these descriptors. Every span reports
 * its completed prefix; only OK guarantees its complete size. No read_batch
 * callback falls back to all scalar reads in input order, preserving each
 * result. The return is the first failed span status. Published DMA remains
 * owned until every request finishes or device reset has been acknowledged. */
enum kernel_block_status kernel_block_read_batch(struct kernel_block_device *device,
    struct block_read_span *spans, size_t count, const struct block_read_completion *completion);

#endif
