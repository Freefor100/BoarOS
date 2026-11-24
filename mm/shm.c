#include <kernel/bitops.h>
#include <kernel/credentials.h>
#include <kernel/hugetlb.h>
#include <kernel/mm_backend.h>
#include <kernel/shm.h>
#include <kernel/ipc_namespace.h>
#include <kernel/heap.h>
#include <kernel/physical_page.h>
#include <kernel/memory_object.h>
#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/time.h>
#include <arch/context.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHM_DEST 01000

#define SHM_CHUNK_SLOTS 128U
#define SHM_CHUNKS (KERNEL_SHMMNI_MAX / SHM_CHUNK_SLOTS)
#define SHM_SLOT_MASK (KERNEL_SHMMNI_MAX - 1U)
#define SHM_CHUNK_WORDS (SHM_CHUNKS / 64U)

struct shm_chunk {
    uint64_t occupied[SHM_CHUNK_SLOTS / 64U];
    struct kernel_shm_segment segments[SHM_CHUNK_SLOTS];
};

struct kernel_shm_table {
    struct kernel_heap *heap;
    struct kernel_avl_node *keys;
    uint64_t total_pages;
    uint32_t used_ids;
    uint64_t occupied_chunks[SHM_CHUNK_WORDS];
    uint64_t full_chunks[SHM_CHUNK_WORDS];
    struct shm_chunk *chunks[SHM_CHUNKS];
};

static struct kernel_heap *shm_heap;
static struct physical_page_allocator *shm_allocator;
static uint64_t next_shm_file_identity = 1;

static struct kernel_shm_segment *shm_slot(const struct kernel_shm_table *table, uint32_t slot)
{
    if (!table || slot >= KERNEL_SHMMNI_MAX) return NULL;
    struct shm_chunk *chunk = table->chunks[slot / SHM_CHUNK_SLOTS];
    return chunk ? &chunk->segments[slot % SHM_CHUNK_SLOTS] : NULL;
}

/* Bitmap sizes are multiples of 64 and callers may start at the end. */
static uint32_t shm_next_bit(const uint64_t *words, uint32_t bits, uint32_t start)
{
    if (start >= bits) return bits;
    uint32_t word = start / 64U;
    uint64_t matches = words[word] & (UINT64_MAX << (start % 64U));
    while (!matches && ++word < bits / 64U) matches = words[word];
    return matches ? word * 64U + kernel_first_set_u64(matches) : bits;
}

static uint32_t shm_next_slot(const struct kernel_shm_table *table, uint32_t cursor)
{
    if (!table) return KERNEL_SHMMNI_MAX;
    uint32_t chunk = cursor / SHM_CHUNK_SLOTS;
    uint32_t offset = cursor % SHM_CHUNK_SLOTS;
    for (uint32_t next = shm_next_bit(table->occupied_chunks, SHM_CHUNKS, chunk);
         next < SHM_CHUNKS; next = shm_next_bit(table->occupied_chunks, SHM_CHUNKS, next + 1U)) {
        uint32_t local = shm_next_bit(table->chunks[next]->occupied, SHM_CHUNK_SLOTS,
            next == chunk ? offset : 0);
        if (local < SHM_CHUNK_SLOTS) return next * SHM_CHUNK_SLOTS + local;
    }
    return KERNEL_SHMMNI_MAX;
}

static uint32_t shm_free_slot(const struct kernel_shm_table *table)
{
    if (!table) return 0;
    for (uint32_t word = 0; word < SHM_CHUNK_WORDS; word++) {
        if (table->full_chunks[word] == UINT64_MAX) continue;
        uint32_t n = word * 64U + kernel_first_set_u64(~table->full_chunks[word]);
        const struct shm_chunk *chunk = table->chunks[n];
        uint32_t slot = n * SHM_CHUNK_SLOTS;
        if (!chunk) return slot;
        if (chunk->occupied[0] != UINT64_MAX)
            return slot + kernel_first_set_u64(~chunk->occupied[0]);
        return slot + 64U + kernel_first_set_u64(~chunk->occupied[1]);
    }
    return KERNEL_SHMMNI_MAX;
}

static uint32_t shm_highest_slot(const struct kernel_shm_table *table)
{
    if (!table || !table->used_ids) return 0;
    for (uint32_t i = SHM_CHUNK_WORDS; i > 0; i--) {
        uint64_t word = table->occupied_chunks[i - 1U];
        if (!word) continue;
        uint32_t n = (i - 1U) * 64U + kernel_last_set_u64(word);
        const struct shm_chunk *chunk = table->chunks[n];
        uint32_t half = chunk->occupied[1] ? 1U : 0U;
        return n * SHM_CHUNK_SLOTS + half * 64U + kernel_last_set_u64(chunk->occupied[half]);
    }
    __builtin_trap();
}

static void shm_slot_set_occupied(struct kernel_shm_table *table, uint32_t slot, int occupied)
{
    uint32_t n = slot / SHM_CHUNK_SLOTS;
    struct shm_chunk *chunk = table->chunks[n];
    uint64_t chunk_bit = UINT64_C(1) << (n % 64U);
    uint32_t word = n / 64U;
    uint64_t slot_bit = UINT64_C(1) << (slot % 64U);
    uint64_t *slots = &chunk->occupied[(slot % SHM_CHUNK_SLOTS) / 64U];
    if (occupied) {
        *slots |= slot_bit;
        table->occupied_chunks[word] |= chunk_bit;
        if ((chunk->occupied[0] & chunk->occupied[1]) == UINT64_MAX)
            table->full_chunks[word] |= chunk_bit;
        table->used_ids++;
    } else {
        *slots &= ~slot_bit;
        table->full_chunks[word] &= ~chunk_bit;
        if (!(chunk->occupied[0] | chunk->occupied[1]))
            table->occupied_chunks[word] &= ~chunk_bit;
        table->used_ids--;
    }
}

static struct kernel_shm_segment *shm_find_key(struct kernel_shm_table *table, int32_t key)
{
    if (!table || key == KERNEL_IPC_PRIVATE) return NULL;
    struct kernel_avl_node *node = table->keys;
    uint64_t search = (uint32_t)key;
    while (node && node->key != search) node = search < node->key ? node->left : node->right;
    return node ? (struct kernel_shm_segment *)((char *)node -
        offsetof(struct kernel_shm_segment, key_index)) : NULL;
}

static void shm_remove_key(struct kernel_shm_segment *seg)
{
    if (seg->key == KERNEL_IPC_PRIVATE) return;
    seg->table->keys = kernel_avl_remove_node(seg->table->keys, &seg->key_index);
    seg->key = KERNEL_IPC_PRIVATE;
}

static void shm_descriptor(const struct kernel_shm_segment *seg, struct kernel_shmid64_ds *ds,struct kernel_pid_registry *view)
{
    *ds = (struct kernel_shmid64_ds){
        .shm_perm = {.key = seg->key, .uid = seg->uid, .gid = seg->gid,
            .cuid = seg->cuid, .cgid = seg->cgid,
            .mode = seg->mode | (seg->marked_for_deletion ? SHM_DEST : 0U) |
                (seg->lock_account ? KERNEL_SHM_LOCKED : 0U), .seq = seg->seq},
        .shm_segsz = seg->size, .shm_atime = seg->atime, .shm_dtime = seg->dtime,
        .shm_ctime = seg->ctime, .shm_cpid = view?kernel_pid_number_in(seg->cpid,view):kernel_task_identity_user_number(seg->cpid),
        .shm_lpid = view?kernel_pid_number_in(seg->lpid,view):kernel_task_identity_user_number(seg->lpid),
        .shm_nattch = seg->nattch,
    };
}

static uint64_t shm_resident_pages(const struct kernel_shm_segment *seg)
{
    return seg->hugetlb ? kernel_hugetlb_map_resident_pages(seg->hugetlb) :
        kernel_memory_object_resident_pages(seg->memory);
}

int kernel_shm_proc_next(struct kernel_namespace *ns,struct kernel_pid_registry *view, uint32_t *cursor, int32_t *id,
                         struct kernel_shmid64_ds *row, uint64_t *rss)
{
    struct kernel_ipc_namespace *ipc = kernel_ipc_namespace_state(ns);
    uintptr_t irq = arch_interrupt_save();
    const struct kernel_shm_table *table = __atomic_load_n(&ipc->shm, __ATOMIC_ACQUIRE);
    uint32_t slot = shm_next_slot(table, *cursor);
    if (slot == KERNEL_SHMMNI_MAX) {
        arch_interrupt_restore(irq);
        return 0;
    }
    const struct kernel_shm_segment *seg = shm_slot(table, slot);
    *cursor = slot + 1U;
    *id = seg->shmid;
    shm_descriptor(seg, row, view);
    *rss = shm_resident_pages(seg) * BOAROS_PAGE_SIZE;
    arch_interrupt_restore(irq);
    return 1;
}

static int shm_permission(const struct kernel_shm_segment *seg,unsigned requested)
{
    return kernel_ipc_permission(kernel_credentials_current(),seg->mode,
        seg->uid,seg->gid,seg->cuid,seg->cgid,requested);
}
static int shm_owner(const struct kernel_shm_segment *seg)
{
    return kernel_ipc_owner(kernel_credentials_current(),seg->uid,seg->cuid);
}

static int64_t current_time_seconds(void)
{
    return (int64_t)(kernel_time_realtime_ns() / 1000000000ULL);
}

static void shm_unlock(struct kernel_shm_segment *seg)
{
    if(!seg->lock_account)return;
    /* chown 和锁定者改 UID 都不转移已经接纳的额度。 */
    struct kernel_uid_account *account=seg->lock_account;
    seg->lock_account=0;
    kernel_uid_account_shm_unlock(account,seg->aligned_size>>BOAROS_PAGE_SHIFT);
}

static void kernel_shm_destroy_segment_locked(struct kernel_shm_segment *seg)
{
    if (!seg->active) {
        return;
    }
    seg->table->total_pages -= (seg->size + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    shm_remove_key(seg);
    shm_slot_set_occupied(seg->table, (uint32_t)seg->shmid & SHM_SLOT_MASK, 0);
    shm_unlock(seg);
    if (seg->hugetlb) kernel_hugetlb_map_release(&seg->hugetlb);
    if (seg->memory != 0) {
        kernel_memory_object_release(&seg->memory);
        seg->memory = 0;
    }
    seg->active = 0;
    seg->marked_for_deletion = 0;
    seg->key = KERNEL_IPC_PRIVATE;
    seg->size = 0;
    seg->aligned_size = 0;
    seg->nattch = 0;
    seg->attachments = 0;
    kernel_task_identity_replace_tgid(&seg->cpid,NULL);
    kernel_task_identity_replace_tgid(&seg->lpid,NULL);
    seg->seq++;
    if (seg->seq == 0U) {
        seg->seq = 1U;
    }
    seg->shmid = -1;
}

enum kernel_shm_status kernel_shm_init(
    struct kernel_heap *heap, struct physical_page_allocator *allocator)
{
    if (heap == 0 || allocator == 0) {
        return KERNEL_SHM_STATUS_INVALID_ARGUMENT;
    }
    if(shm_heap && (shm_heap!=heap || shm_allocator!=allocator))return KERNEL_SHM_STATUS_INVALID_ARGUMENT;
    if(kernel_ipc_namespace_boot(heap))return KERNEL_SHM_STATUS_INVALID_ARGUMENT;
    shm_heap=heap;shm_allocator=allocator;
    return KERNEL_SHM_STATUS_OK;
}

struct shm_creation {
    struct shm_chunk *chunk;
    struct kernel_shm_table *table;
    struct kernel_memory_object *memory;
    struct kernel_hugetlb_map *hugetlb;
};

static void shm_creation_discard(struct shm_creation *creation)
{
    if (creation->memory) kernel_memory_object_release(&creation->memory);
    if (creation->hugetlb) kernel_hugetlb_map_release(&creation->hugetlb);
    if (creation->chunk) kernel_heap_release(shm_heap, creation->chunk);
    if (creation->table) kernel_heap_release(shm_heap, creation->table);
}

static int shm_find_existing(struct kernel_shm_segment *seg, uint64_t size,
    int32_t flags, int32_t *id)
{
    if ((flags & (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) ==
            (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) return -KERNEL_EEXIST;
    if (size > seg->size) return -KERNEL_EINVAL;
    unsigned access = (unsigned)flags & 0777U;
    int result = shm_permission(seg, (access | (access >> 3) | (access >> 6)) & 7U);
    if (!result) *id = seg->shmid;
    return result;
}

static int shm_check_limits(const struct kernel_ipc_namespace *ns,
    const struct kernel_shm_table *table, uint64_t size, uint64_t pages, int32_t flags)
{
    if (size < KERNEL_SHMMIN || size > kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_MAX))
        return -KERNEL_EINVAL;
    uint64_t used = table ? table->total_pages : 0;
    uint64_t maximum = kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_ALL);
    if (size > UINT64_MAX - BOAROS_PAGE_MASK || used > maximum || pages > maximum - used)
        return -KERNEL_ENOSPC;
    if (!(flags & KERNEL_SHM_HUGETLB) && size > INT64_MAX) return -KERNEL_EINVAL;
    return (table ? table->used_ids : 0) >= kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_MNI) ?
        -KERNEL_ENOSPC : 0;
}

static int shm_prepare_backing(struct shm_creation *creation, uint64_t size, int32_t flags,
    uint64_t *aligned)
{
    if (flags & KERNEL_SHM_HUGETLB) {
        unsigned shift = ((uint32_t)flags >> KERNEL_SHM_HUGE_SHIFT) & KERNEL_SHM_HUGE_MASK;
        if (shift && shift != BOAROS_PAGE_SHIFT + ARCH_MMU_HUGE_ORDER) return -KERNEL_EINVAL;
        const struct kernel_credentials *cred = kernel_credentials_current();
        if (!kernel_credentials_capable(cred, KERNEL_CAP_IPC_LOCK) &&
            !kernel_credentials_in_group(cred, 0)) return -KERNEL_EPERM;
        if (size > UINT64_MAX - (ARCH_MMU_HUGE_SIZE - 1U)) return -KERNEL_ENOSPC;
        *aligned = (size + ARCH_MMU_HUGE_SIZE - 1U) & ~(ARCH_MMU_HUGE_SIZE - 1U);
        struct kernel_hstate *h = NULL;
        enum kernel_hugetlb_status status = kernel_hstate_get_or_create(shm_heap,
            shm_allocator, ARCH_MMU_HUGE_ORDER, &h);
        if (!status) status = kernel_hugetlb_map_create(h, *aligned / ARCH_MMU_HUGE_SIZE,
            KERNEL_HUGETLB_SHARED, !!(flags & KERNEL_SHM_NORESERVE), &creation->hugetlb);
        if (h) kernel_hstate_put(h);
        return status ? -KERNEL_ENOMEM : 0;
    }
    enum kernel_memory_object_status status =
        kernel_memory_object_create(shm_heap, shm_allocator, &creation->memory);
    if (status) return status == KERNEL_MEMORY_OBJECT_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_ENOSPC;
    kernel_memory_object_set_mapping_size(creation->memory, *aligned);
    return 0;
}

int kernel_shm_get(struct kernel_task *caller, int32_t key, uint64_t size,
    int32_t shmflg, int32_t *out_shmid)
{
    if (!out_shmid) return -KERNEL_EFAULT;
    if (!shm_heap || !shm_allocator) return -KERNEL_ENOSPC;
    struct kernel_namespace *base = kernel_task_ipc_namespace();
    struct kernel_ipc_namespace *ns = kernel_ipc_namespace_state(base);
    uint64_t aligned = (size + BOAROS_PAGE_MASK) & ~(uint64_t)BOAROS_PAGE_MASK;
    uint64_t pages = aligned >> BOAROS_PAGE_SHIFT;
    struct shm_creation creation __attribute__((cleanup(shm_creation_discard))) = {0};

    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        struct kernel_shm_table *table = ns->shm;
        struct kernel_shm_segment *seg = shm_find_key(table, key);
        if (seg) {
            int result = shm_find_existing(seg, size, shmflg, out_shmid);
            arch_interrupt_restore(irq);
            return result;
        }
        if (key != KERNEL_IPC_PRIVATE && !(shmflg & KERNEL_IPC_CREAT)) {
            arch_interrupt_restore(irq);
            return -KERNEL_ENOENT;
        }
        int result = shm_check_limits(ns, table, size, pages, shmflg);
        if (result) {
            arch_interrupt_restore(irq);
            return result;
        }
        uint32_t slot = shm_free_slot(table);
        uint32_t n = slot / SHM_CHUNK_SLOTS;
        int need_table = !table && !creation.table;
        int need_chunk = (!table || !table->chunks[n]) && !creation.chunk;
        if (need_table || need_chunk || (!creation.memory && !creation.hugetlb)) {
            /* Allocation may run reclaim. Treat the inspected slot and limits
             * as hints until the complete segment can be published. */
            arch_interrupt_restore(irq);
            if (need_table && kernel_heap_allocate_zeroed(shm_heap, 1,
                    sizeof(*creation.table), (void **)&creation.table)) return -KERNEL_ENOMEM;
            if (need_chunk && kernel_heap_allocate_zeroed(shm_heap, 1,
                    sizeof(*creation.chunk), (void **)&creation.chunk)) return -KERNEL_ENOMEM;
            if (!creation.memory && !creation.hugetlb) {
                result = shm_prepare_backing(&creation, size, shmflg, &aligned);
                if (result) return result;
            }
            continue;
        }

        int publish_table = !table;
        if (publish_table) {
            table = creation.table;
            creation.table = NULL;
            table->heap = shm_heap;
        }
        if (!table->chunks[n]) {
            table->chunks[n] = creation.chunk;
            creation.chunk = NULL;
        }
        seg = shm_slot(table, slot);
        const struct kernel_credentials *cred = kernel_credentials_current();
        uint16_t sequence = seg->seq ? seg->seq : 1U;
        *seg = (struct kernel_shm_segment){
            .namespace = base, .table = table, .seq = sequence,
            .shmid = (int32_t)((uint32_t)sequence * KERNEL_SHMMNI_MAX + slot), .key = key,
            .size = size, .aligned_size = aligned, .memory = creation.memory,
            .hugetlb = creation.hugetlb, .ctime = current_time_seconds(),
            .mode = (uint32_t)shmflg & 0777U,
            .uid = cred->euid, .cuid = cred->euid, .gid = cred->egid, .cgid = cred->egid,
            .active = 1,
        };
        creation.memory = NULL;
        creation.hugetlb = NULL;
        kernel_task_identity_replace_tgid(&seg->cpid, caller ? caller : kernel_task_current());
        if (key != KERNEL_IPC_PRIVATE) {
            seg->key_index = (struct kernel_avl_node){.key = (uint32_t)key, .height = 1};
            table->keys = kernel_avl_insert_node(table->keys, &seg->key_index);
        }
        table->total_pages += pages;
        shm_slot_set_occupied(table, slot, 1);
        if (publish_table) __atomic_store_n(&ns->shm, table, __ATOMIC_RELEASE);
        *out_shmid = seg->shmid;
        arch_interrupt_restore(irq);
        return 0;
    }
}

int kernel_shm_at_mm(struct kernel_mm *mm, struct kernel_task *caller, int32_t shmid, uint64_t shmaddr,
                     int32_t shmflg, uint64_t *out_attached_addr)
{
    if (out_attached_addr == 0 || mm == 0) {
        return -KERNEL_EFAULT;
    }
    if (shmid < 0) {
        return -KERNEL_EINVAL;
    }
    struct kernel_ipc_namespace *ns = kernel_ipc_namespace_state(kernel_task_ipc_namespace());
    uintptr_t irq = arch_interrupt_save();
    struct kernel_shm_segment *seg = shm_slot(ns->shm, (uint32_t)shmid & SHM_SLOT_MASK);
    if (!seg || !seg->active || seg->shmid != shmid) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }
    /* Linux permits another shmat by ID while an RMID segment remains alive. */
    unsigned requested=(shmflg&KERNEL_SHM_RDONLY)?4U:6U;
    if(shmflg&KERNEL_SHM_EXEC)requested|=1;
    int permission=shm_permission(seg,requested);
    if(permission){arch_interrupt_restore(irq);return permission;}
    uint32_t permissions = KERNEL_MM_READ;
    if ((shmflg & KERNEL_SHM_RDONLY) == 0) {
        permissions |= KERNEL_MM_WRITE;
    }
    if ((shmflg & KERNEL_SHM_EXEC) != 0) {
        permissions |= KERNEL_MM_EXECUTE;
    }

    uint64_t attached_addr = 0;
    enum kernel_mm_status status = kernel_mm_shmat(
        mm, seg, shmaddr, permissions, (uint32_t)shmflg, &attached_addr);

    if (status != KERNEL_MM_STATUS_OK) {
        arch_interrupt_restore(irq);
        if (status == KERNEL_MM_STATUS_CONFLICT) {
            return -KERNEL_EINVAL;
        }
        if (status == KERNEL_MM_STATUS_LOCK_LIMIT) return -KERNEL_EAGAIN;
        if (status == KERNEL_MM_STATUS_LOCK_PERMISSION) return -KERNEL_EPERM;
        if (status == KERNEL_MM_STATUS_NO_MEMORY) {
            return -KERNEL_ENOMEM;
        }
        return -KERNEL_EINVAL;
    }

    kernel_task_identity_replace_tgid(&seg->lpid,caller?caller:kernel_task_current());
    seg->atime = current_time_seconds();
    *out_attached_addr = attached_addr;

    arch_interrupt_restore(irq);
    return 0;
}

int kernel_shm_at(struct kernel_task *caller, int32_t shmid, uint64_t shmaddr,
                  int32_t shmflg, uint64_t *out_attached_addr)
{
    struct kernel_mm *mm = 0;
    if (caller == 0 || kernel_task_mm_borrow_mutable(caller, &mm) !=
                           KERNEL_TASK_STATUS_OK || mm == 0) {
        return -KERNEL_EINVAL;
    }
    return kernel_shm_at_mm(mm, caller, shmid, shmaddr, shmflg, out_attached_addr);
}

int kernel_shm_dt_mm(struct kernel_mm *mm, uint64_t shmaddr)
{
    if (mm == 0 || (shmaddr & BOAROS_PAGE_MASK) != 0U) {
        return -KERNEL_EINVAL;
    }
    enum kernel_mm_status status = kernel_mm_shmdt(mm, shmaddr);
    if (status != KERNEL_MM_STATUS_OK) {
        return -KERNEL_EINVAL;
    }
    return 0;
}

int kernel_shm_dt(struct kernel_task *caller, uint64_t shmaddr)
{
    struct kernel_mm *mm = 0;
    if (caller == 0 || kernel_task_mm_borrow_mutable(caller, &mm) !=
                           KERNEL_TASK_STATUS_OK || mm == 0) {
        return -KERNEL_EINVAL;
    }
    return kernel_shm_dt_mm(mm, shmaddr);
}

static void attachment_valid(const struct kernel_shm_attachment *attachment)
{
    if (!attachment || !attachment->heap || !attachment->references ||
        !attachment->segment || !attachment->segment->active ||
        attachment->segment->shmid != attachment->shmid ||
        attachment->segment->memory != attachment->memory ||
        attachment->segment->hugetlb != attachment->hugetlb ||
        !attachment->segment->attachments) __builtin_trap();
}

enum kernel_shm_status kernel_shm_attachment_create(
    struct kernel_shm_segment *segment, uint64_t start, uint64_t end,
    const struct kernel_shm_attachment *source,
    struct kernel_shm_attachment **owner)
{
    struct kernel_shm_attachment *attachment = 0;
    uintptr_t irq = arch_interrupt_save();
    if (!owner || *owner || !segment || !segment->active || (!segment->memory && !segment->hugetlb) ||
        start >= end || segment->attachments == UINT32_MAX ||
        (source && (source->segment != segment || !source->file_identity)) ||
        (!source && !next_shm_file_identity)) __builtin_trap();
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        segment->table->heap, 1, sizeof(*attachment), (void **)&attachment);
    if (status != KERNEL_HEAP_STATUS_OK) {
        arch_interrupt_restore(irq);
        if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
        return KERNEL_SHM_STATUS_NO_MEMORY;
    }
    if (segment->hugetlb) kernel_hugetlb_map_acquire(segment->hugetlb);
    if (segment->memory && kernel_memory_object_acquire(segment->memory) != KERNEL_MEMORY_OBJECT_OK)
        __builtin_trap();
    *attachment = (struct kernel_shm_attachment){
        .heap = segment->table->heap, .namespace=kernel_namespace_get(segment->namespace), .segment = segment, .memory = segment->memory, .hugetlb = segment->hugetlb,
        .start = start, .end = end, .shmid = segment->shmid, .references = 1,
        .file_identity = source ? source->file_identity : next_shm_file_identity++,
    };
    /* 预备 owner 也保住槽身份，但不伪增用户可见 nattch。 */
    segment->attachments++;
    *owner = attachment;
    arch_interrupt_restore(irq);
    return KERNEL_SHM_STATUS_OK;
}

void kernel_shm_attachment_acquire(struct kernel_shm_attachment *attachment)
{
    uintptr_t irq = arch_interrupt_save();
    attachment_valid(attachment);
    if (attachment->references == UINT32_MAX) __builtin_trap();
    attachment->references++;
    arch_interrupt_restore(irq);
}

void kernel_shm_attachment_release(struct kernel_shm_attachment **owner)
{
    if (!owner || !*owner) return;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_shm_attachment *attachment = *owner;
    attachment_valid(attachment);
    *owner = 0;
    struct kernel_namespace *namespace=0;
    if (--attachment->references == 0) {
        namespace=attachment->namespace;
        struct kernel_shm_segment *segment = attachment->segment;
        if (attachment->memory) kernel_memory_object_release(&attachment->memory);
        if (attachment->hugetlb) kernel_hugetlb_map_release(&attachment->hugetlb);
        segment->attachments--;
        kernel_heap_release(attachment->heap, attachment);
        if (!segment->nattch && !segment->attachments && segment->marked_for_deletion)
            kernel_shm_destroy_segment_locked(segment);
    }
    arch_interrupt_restore(irq);
    if(namespace)kernel_namespace_put(namespace);
}

void kernel_shm_attachment_open(struct kernel_shm_attachment *attachment)
{
    uintptr_t irq = arch_interrupt_save();
    kernel_shm_attachment_acquire(attachment);
    struct kernel_shm_segment *segment = attachment->segment;
    if (segment->nattch == UINT64_MAX) __builtin_trap();
    segment->nattch++;
    segment->atime = current_time_seconds();
    kernel_task_identity_replace_tgid(&segment->lpid,kernel_task_current());
    arch_interrupt_restore(irq);
}

void kernel_shm_attachment_close_for_task(struct kernel_shm_attachment *attachment,const struct kernel_task *task)
{
    uintptr_t irq = arch_interrupt_save();
    attachment_valid(attachment);
    struct kernel_shm_segment *segment = attachment->segment;
    if (!segment->nattch) __builtin_trap();
    segment->nattch--;
    segment->dtime = current_time_seconds();
    kernel_task_identity_replace_tgid(&segment->lpid,task);
    kernel_shm_attachment_release(&attachment);
    arch_interrupt_restore(irq);
}

void kernel_shm_attachment_close(struct kernel_shm_attachment *attachment)
{ kernel_shm_attachment_close_for_task(attachment,kernel_task_current()); }

int kernel_shm_ctl_mm(struct kernel_mm *mm, int32_t shmid, int32_t cmd,
                      uint64_t user_buf, int64_t *out_result)
{
    if (out_result == 0) {
        return -KERNEL_EFAULT;
    }
    if(cmd<0 || shmid<0)return -KERNEL_EINVAL;
    int pure_cmd = cmd & ~KERNEL_IPC_64;
    struct kernel_shmid64_ds replacement;
    if(pure_cmd==KERNEL_IPC_SET) {
        if(mm) {
            size_t copied=0;
            if(kernel_copy_from_user(mm,&replacement,user_buf,sizeof(replacement),&copied)!=KERNEL_UACCESS_STATUS_OK ||
                copied!=sizeof(replacement))return -KERNEL_EFAULT;
        } else {
            if(!user_buf)return -KERNEL_EFAULT;
            replacement=*(const struct kernel_shmid64_ds *)(uintptr_t)user_buf;
        }
    }

    struct kernel_ipc_namespace *ns = kernel_ipc_namespace_state(kernel_task_ipc_namespace());
    uintptr_t irq = arch_interrupt_save();
    const struct kernel_shm_table *table = ns->shm;

    if (pure_cmd == KERNEL_IPC_INFO || pure_cmd == KERNEL_SHM_INFO ||
        pure_cmd == KERNEL_IPC_STAT || pure_cmd == KERNEL_SHM_STAT ||
        pure_cmd == KERNEL_SHM_STAT_ANY) {
        union {
            struct kernel_shminfo64 limits;
            struct kernel_shm_info usage;
            struct kernel_shmid64_ds descriptor;
        } snapshot = {0};
        size_t size;
        int64_t result = 0;
        if (pure_cmd == KERNEL_IPC_INFO) {
            snapshot.limits = (struct kernel_shminfo64){
                .shmmax = kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_MAX), .shmmin = KERNEL_SHMMIN,
                .shmmni = kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_MNI),
                .shmall = kernel_ipc_shm_limit(ns, KERNEL_SHM_LIMIT_ALL),
            };
            snapshot.limits.shmseg = snapshot.limits.shmmni;
            result = shm_highest_slot(table);
            size = sizeof(snapshot.limits);
        } else if (pure_cmd == KERNEL_SHM_INFO) {
            if (table) {
                snapshot.usage.used_ids = (int32_t)table->used_ids;
                snapshot.usage.shm_tot = table->total_pages;
                for (uint32_t slot = shm_next_slot(table, 0); slot < KERNEL_SHMMNI_MAX;
                     slot = shm_next_slot(table, slot + 1U))
                    snapshot.usage.shm_rss += shm_resident_pages(shm_slot(table, slot));
                result = shm_highest_slot(table);
            }
            size = sizeof(snapshot.usage);
        } else {
            uint32_t slot = (uint32_t)shmid & SHM_SLOT_MASK;
            struct kernel_shm_segment *seg = shm_slot(table, slot);
            if (!seg || !seg->active || (pure_cmd == KERNEL_IPC_STAT && seg->shmid != shmid)) {
                arch_interrupt_restore(irq);
                return -KERNEL_EINVAL;
            }
            if (pure_cmd != KERNEL_SHM_STAT_ANY) {
                int permission = shm_permission(seg, 4);
                if (permission) { arch_interrupt_restore(irq); return permission; }
            }
            shm_descriptor(seg, &snapshot.descriptor,NULL);
            size = sizeof(snapshot.descriptor);
            if (pure_cmd != KERNEL_IPC_STAT) result = seg->shmid;
        }
        arch_interrupt_restore(irq);

        if (mm != 0) {
            size_t copied = 0;
            if (kernel_copy_to_user(mm, user_buf, &snapshot, size, &copied) !=
                    KERNEL_UACCESS_STATUS_OK || copied != size) return -KERNEL_EFAULT;
        } else {
            if (!user_buf) return -KERNEL_EFAULT;
            memcpy((void *)(uintptr_t)user_buf, &snapshot, size);
        }
        *out_result = result;
        return 0;
    }

    struct kernel_shm_segment *seg = shm_slot(table, (uint32_t)shmid & SHM_SLOT_MASK);
    if (!seg || !seg->active || seg->shmid != shmid) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }

    switch (pure_cmd) {
    case KERNEL_SHM_LOCK:
    case KERNEL_SHM_UNLOCK: {
        const struct kernel_credentials *cred=kernel_credentials_current();
        /* Linux 在重复操作前仍检查 owner 和 LOCK 的零软限额。 */
        if(!kernel_credentials_capable(cred,KERNEL_CAP_IPC_LOCK) &&
            (cred->euid!=seg->uid && cred->euid!=seg->cuid)) {
            arch_interrupt_restore(irq);return -KERNEL_EPERM;
        }
        if (pure_cmd == KERNEL_SHM_LOCK && !kernel_task_current_memlock_limit()) {
            arch_interrupt_restore(irq);return -KERNEL_EPERM;
        }
        /* Huge pages cannot swap; Linux does not set SHM_LOCKED or charge UID. */
        if (seg->hugetlb) {
            *out_result=0;arch_interrupt_restore(irq);return 0;
        }
        if(pure_cmd==KERNEL_SHM_LOCK) {
            uint64_t limit=kernel_task_current_memlock_limit();
            if(!limit){arch_interrupt_restore(irq);return -KERNEL_EPERM;}
            if(!seg->lock_account) {
                int status=kernel_uid_account_shm_lock(cred->account,
                    seg->aligned_size>>BOAROS_PAGE_SHIFT,limit>>BOAROS_PAGE_SHIFT);
                if(status){arch_interrupt_restore(irq);return status;}
                /* memory_object 已持有实际驻留页；LOCK 不触页，也不逐页加 pin。 */
                seg->lock_account=cred->account;
            }
        } else shm_unlock(seg);
        *out_result=0;
        arch_interrupt_restore(irq);return 0;
    }
    case KERNEL_IPC_RMID: {
        if(!shm_owner(seg)){arch_interrupt_restore(irq);return -KERNEL_EPERM;}
        if (seg->nattch == 0U && seg->attachments == 0U) {
            kernel_shm_destroy_segment_locked(seg);
        } else {
            seg->marked_for_deletion = 1;
            shm_remove_key(seg);
            seg->ctime = current_time_seconds();
        }
        *out_result = 0;
        arch_interrupt_restore(irq);
        return 0;
    }
    case KERNEL_IPC_SET: {
        if (seg->marked_for_deletion) {
            arch_interrupt_restore(irq);
            return -KERNEL_EIDRM;
        }
        const struct kernel_shmid64_ds ds=replacement;
        if(!shm_owner(seg)){arch_interrupt_restore(irq);return -KERNEL_EPERM;}
        if(ds.shm_perm.uid==UINT32_MAX || ds.shm_perm.gid==UINT32_MAX) {
            arch_interrupt_restore(irq);return -KERNEL_EINVAL;
        }
        seg->mode = (uint32_t)(ds.shm_perm.mode & 0777);
        seg->uid = ds.shm_perm.uid;
        seg->gid = ds.shm_perm.gid;
        seg->ctime = current_time_seconds();
        *out_result = 0;
        arch_interrupt_restore(irq);
        return 0;
    }
    default:
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }
}

int kernel_shm_ctl(struct kernel_task *caller, int32_t shmid, int32_t cmd,
                   uint64_t user_buf, int64_t *out_result)
{
    struct kernel_mm *mm = 0;
    if (caller != 0) {
        (void)kernel_task_mm_borrow_mutable(caller, &mm);
    }
    return kernel_shm_ctl_mm(mm, shmid, cmd, user_buf, out_result);
}

void kernel_shm_namespace_destroy(struct kernel_shm_table *table)
{
    if (!table) return;
    for (uint32_t slot = shm_next_slot(table, 0); slot < KERNEL_SHMMNI_MAX;
         slot = shm_next_slot(table, slot + 1U)) {
        struct kernel_shm_segment *seg = shm_slot(table, slot);
        if (seg->attachments || seg->nattch) __builtin_trap();
        kernel_shm_destroy_segment_locked(seg);
    }
    for (uint32_t i = 0; i < SHM_CHUNKS; i++)
        if (table->chunks[i]) kernel_heap_release(table->heap, table->chunks[i]);
    kernel_heap_release(table->heap, table);
}
