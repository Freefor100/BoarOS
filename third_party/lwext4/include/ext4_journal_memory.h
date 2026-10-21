#ifndef EXT4_JOURNAL_MEMORY_H_
#define EXT4_JOURNAL_MEMORY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct jbd_journal;
struct jbd_memory;
struct jbd_image;
struct ext4_buf;
struct jbd_memory_stats {
    size_t used, peak, limit, cached, reserved;
    uint64_t budget_failures;
};

/* The journal owns one anchor; every allocated header keeps the same owner
 * alive until its storage, and any attached image payload, is really freed. */
int jbd_memory_start(struct jbd_journal *, uint32_t block_size);
void jbd_memory_stop(struct jbd_journal *);
void jbd_memory_get(struct jbd_memory *);
void jbd_memory_put(struct jbd_memory *);
/* Retain this owner to observe outstanding loans after the journal stops. */
void jbd_memory_snapshot(struct jbd_memory *, struct jbd_memory_stats *);
size_t jbd_memory_used(const struct jbd_journal *);
size_t jbd_memory_peak(const struct jbd_journal *);
size_t jbd_memory_limit(const struct jbd_journal *);
size_t jbd_memory_cached(const struct jbd_journal *);
uint64_t jbd_memory_budget_failures(const struct jbd_journal *);
void jbd_memory_set_limit(struct jbd_journal *, size_t);
void jbd_memory_set_grouped(struct jbd_journal *, bool);
void jbd_memory_set_cache_disabled(struct jbd_journal *, bool);
unsigned jbd_memory_cached_images(const struct jbd_journal *);

/* Charge reserves capacity atomically, including concurrent preparation.
 * Used bytes remain charged through cache/reader/DMA ownership and disposal. */
bool jbd_memory_charge(struct jbd_journal *, size_t);
void jbd_memory_uncharge(struct jbd_journal *, size_t);
void jbd_memory_owner_uncharge(struct jbd_memory *, size_t);
bool jbd_memory_pin_home(struct jbd_journal *, struct ext4_buf *);
void jbd_memory_unpin_home(struct jbd_journal *, struct ext4_buf *);
size_t jbd_allocation_charge(size_t);
void *jbd_alloc(struct jbd_journal *, size_t);
void jbd_free(void *);
bool jbd_budget_room(struct jbd_journal *, size_t);
void jbd_trim_cache(struct jbd_journal *, size_t target);
size_t jbd_image_charge(const struct jbd_journal *);
struct jbd_image *jbd_image_alloc(struct jbd_journal *);
void jbd_image_free(struct jbd_image *);

#endif
