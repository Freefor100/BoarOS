#ifndef BOAROS_KERNEL_BPF_MAP_H
#define BOAROS_KERNEL_BPF_MAP_H
#include <stddef.h>
#include <stdint.h>
struct kernel_heap;
struct physical_page_allocator;
struct kernel_wait_queue;
struct bpf_map;
struct bpf_ring;
/* Values follow the fixed Linux UAPI. */
enum bpf_storage_type { BPF_STORAGE_HASH=1, BPF_STORAGE_ARRAY=2,
    BPF_STORAGE_PERCPU_HASH=5, BPF_STORAGE_PERCPU_ARRAY=6, BPF_STORAGE_RINGBUF=27 };
struct bpf_map_spec {
    uint32_t type, key_size, value_size, max_entries, map_flags;
};
/* BTF, IDs and names belong to the syscall object's metadata, not storage. */
int bpf_map_create(struct kernel_heap *, struct physical_page_allocator *,
    const struct bpf_map_spec *, struct bpf_map **);
void bpf_map_get(struct bpf_map *);
void bpf_map_put(struct bpf_map *);
const struct bpf_map_spec *bpf_map_spec(const struct bpf_map *);
size_t bpf_map_user_value_size(const struct bpf_map *);
unsigned bpf_map_cpu_count(const struct bpf_map *);
int bpf_map_freeze(struct bpf_map *);
int bpf_map_is_frozen(const struct bpf_map *);
/* user=1 copies full per-CPU buffers; user=0 accesses the supplied CPU only.
 * All buffers are kernel-owned, already sized/validated by the caller. */
int bpf_map_lookup(struct bpf_map *, const void *key, void *value, int user, unsigned cpu);
int bpf_map_update(struct bpf_map *, const void *key, const void *value,
    uint64_t flags, int user, unsigned cpu);
int bpf_map_delete(struct bpf_map *, const void *key, int user);
int bpf_map_lookup_and_delete(struct bpf_map *,const void *key,void *value);
int bpf_map_next_key(struct bpf_map *, const void *key, void *next);
/* An invocation holds one borrow per referenced map, not per lookup. Deleted
 * hash entries cannot be reused until all active map borrows have ended.
 * The program/object must own the map across enter/exit. No allocation occurs. */
void bpf_map_borrow_begin(struct bpf_map *);
void bpf_map_borrow_end(struct bpf_map *);
void *bpf_map_lookup_borrowed(struct bpf_map *, const void *key, unsigned cpu);
/* Batch cursor is a uint32_t bucket/array position; NULL starts iteration.
 * *count is capacity on entry and completed elements on return. */
int bpf_map_lookup_batch(struct bpf_map *, const uint32_t *in, uint32_t *out,
    void *keys, void *values, uint32_t *count, int delete_elements);
int bpf_map_update_batch(struct bpf_map *, const void *keys, const void *values,
    uint32_t *count, uint64_t flags);
int bpf_map_delete_batch(struct bpf_map *, const void *keys, uint32_t *count);
/* Offset is relative to the map mmap area. Successful page_get transfers an
 * actual order-0 physical page reference to the caller (including aliases).
 * Array padding is zero; only rounded map data pages can be mapped. */
uint64_t bpf_map_mmap_size(const struct bpf_map *);
int bpf_map_mmap_check(struct bpf_map *, uint64_t offset, uint64_t length, int writable);
int bpf_map_page_get(struct bpf_map *, uint64_t offset, int writable, uint64_t *physical);
struct kernel_wait_queue *bpf_map_wait_queue(struct bpf_map *);
int bpf_map_poll_readable(struct bpf_map *);
struct bpf_ring *bpf_map_ring(struct bpf_map *);
/* Caller supplies contiguous storage for a reservation (VM invocation memory).
 * Commit copies to shared ring pages before publishing the non-busy header;
 * all mmap aliases therefore refer to exactly the same actual physical data. */
struct bpf_ring_reservation {
    struct bpf_ring *ring;
    void *data;
    uint64_t position;
    uint32_t size;
};
int bpf_ring_reserve(struct bpf_ring *, uint32_t size, uint64_t flags,
    void *storage, size_t capacity, struct bpf_ring_reservation *);
int bpf_ring_submit(struct bpf_ring_reservation *, uint64_t flags, int discard);
int bpf_ring_output(struct bpf_ring *, const void *data, uint32_t size, uint64_t flags);
uint64_t bpf_ring_query(struct bpf_ring *, uint64_t flags);
/* Storage lifecycle entry points used by maps.c. */
int bpf_ring_create(struct kernel_heap *, struct physical_page_allocator *, uint32_t, struct bpf_ring **);
void bpf_ring_destroy(struct bpf_ring *);
int bpf_ring_page_get(struct bpf_ring *, uint64_t offset, int writable, uint64_t *);
struct kernel_wait_queue *bpf_ring_wait_queue(struct bpf_ring *);
int bpf_ring_poll_readable(struct bpf_ring *);
#endif
