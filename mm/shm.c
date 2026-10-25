#include <kernel/credentials.h>
#include <kernel/shm.h>
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

struct kernel_shm_table {
    struct kernel_shm_segment segments[KERNEL_SHMMNI];
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    int initialized;
};

static struct kernel_shm_table shm_table;

static void shm_descriptor(const struct kernel_shm_segment *seg, struct kernel_shmid64_ds *ds)
{
    *ds = (struct kernel_shmid64_ds){
        .shm_perm = {.key = seg->key, .uid = seg->uid, .gid = seg->gid,
            .cuid = seg->cuid, .cgid = seg->cgid,
            .mode = seg->mode | (seg->marked_for_deletion ? SHM_DEST : 0U) |
                (seg->lock_account ? KERNEL_SHM_LOCKED : 0U), .seq = seg->seq},
        .shm_segsz = seg->size, .shm_atime = seg->atime, .shm_dtime = seg->dtime,
        .shm_ctime = seg->ctime, .shm_cpid = seg->cpid, .shm_lpid = seg->lpid,
        .shm_nattch = seg->nattch,
    };
}

int kernel_shm_proc_next(uint32_t *cursor, int32_t *id,
                         struct kernel_shmid64_ds *row, uint64_t *rss)
{
    uintptr_t irq = arch_interrupt_save();
    while (*cursor < KERNEL_SHMMNI) {
        const struct kernel_shm_segment *seg = &shm_table.segments[(*cursor)++];
        if (!seg->active) continue;
        *id = seg->shmid;
        shm_descriptor(seg, row);
        *rss = kernel_memory_object_resident_pages(seg->memory) * BOAROS_PAGE_SIZE;
        arch_interrupt_restore(irq);
        return 1;
    }
    arch_interrupt_restore(irq);
    return 0;
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

static int32_t current_tgid(struct kernel_task *caller)
{
    kernel_pid_t pid = 0;
    if (caller != 0 && kernel_task_tgid(caller, &pid) == KERNEL_TASK_STATUS_OK) {
        return (int32_t)pid;
    }
    struct kernel_task *curr = kernel_task_current();
    if (curr != 0 && kernel_task_tgid(curr, &pid) == KERNEL_TASK_STATUS_OK) {
        return (int32_t)pid;
    }
    return 0;
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
    shm_unlock(seg);
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
    seg->cpid = 0;
    seg->lpid = 0;
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
    uintptr_t irq = arch_interrupt_save();
    shm_table.heap = heap;
    shm_table.allocator = allocator;
    if (!shm_table.initialized) {
        for (uint32_t i = 0U; i < KERNEL_SHMMNI; i++) {
            shm_table.segments[i].shmid = -1;
            shm_table.segments[i].seq = 1U;
            shm_table.segments[i].key = KERNEL_IPC_PRIVATE;
            shm_table.segments[i].size = 0;
            shm_table.segments[i].aligned_size = 0;
            shm_table.segments[i].memory = 0;
            shm_table.segments[i].nattch = 0;
            shm_table.segments[i].active = 0;
            shm_table.segments[i].marked_for_deletion = 0;
        }
        shm_table.initialized = 1;
    }
    arch_interrupt_restore(irq);
    return KERNEL_SHM_STATUS_OK;
}

int kernel_shm_get(struct kernel_task *caller, int32_t key, uint64_t size,
                   int32_t shmflg, int32_t *out_shmid)
{
    if (out_shmid == 0) {
        return -KERNEL_EFAULT;
    }
    int32_t caller_pid = current_tgid(caller);

    uintptr_t irq = arch_interrupt_save();
    if (!shm_table.initialized || shm_table.heap == 0 || shm_table.allocator == 0) {
        arch_interrupt_restore(irq);
        return -KERNEL_ENOSPC;
    }

    if (key != KERNEL_IPC_PRIVATE) {
        for (uint32_t i = 0U; i < KERNEL_SHMMNI; i++) {
            struct kernel_shm_segment *seg = &shm_table.segments[i];
            if (seg->active && !seg->marked_for_deletion && seg->key == key) {
                if ((shmflg & (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) ==
                    (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) {
                    arch_interrupt_restore(irq);
                    return -KERNEL_EEXIST;
                }
                if (size > seg->size) {
                    arch_interrupt_restore(irq);
                    return -KERNEL_EINVAL;
                }
                unsigned access=(unsigned)shmflg&0777;
                access=(access|(access>>3)|(access>>6))&7;
                int permission=shm_permission(seg,access);
                if(permission){arch_interrupt_restore(irq);return permission;}
                *out_shmid = seg->shmid;
                arch_interrupt_restore(irq);
                return 0;
            }
        }
        if ((shmflg & KERNEL_IPC_CREAT) == 0) {
            arch_interrupt_restore(irq);
            return -KERNEL_ENOENT;
        }
    }

    /* 已有 key 的 size=0 是查找；创建下限只约束新段。 */
    if (size < KERNEL_SHMMIN || size > KERNEL_SHMMAX) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }
    uint64_t aligned_size = (size + BOAROS_PAGE_SIZE - 1U) & ~BOAROS_PAGE_MASK;

    /* Allocate new slot */
    int slot_idx = -1;
    for (uint32_t i = 0U; i < KERNEL_SHMMNI; i++) {
        if (!shm_table.segments[i].active) {
            slot_idx = (int)i;
            break;
        }
    }
    if (slot_idx < 0) {
        arch_interrupt_restore(irq);
        return -KERNEL_ENOSPC;
    }

    struct kernel_shm_segment *new_seg = &shm_table.segments[slot_idx];
    struct kernel_memory_object *mem = 0;
    enum kernel_memory_object_status mem_status =
        kernel_memory_object_create(shm_table.heap, shm_table.allocator, &mem);
    if (mem_status != KERNEL_MEMORY_OBJECT_OK) {
        arch_interrupt_restore(irq);
        return mem_status == KERNEL_MEMORY_OBJECT_NO_MEMORY ? -KERNEL_ENOMEM
                                                            : -KERNEL_ENOSPC;
    }

    int32_t shmid = slot_idx + (int32_t)new_seg->seq * (int32_t)KERNEL_SHMMNI;
    new_seg->shmid = shmid;
    new_seg->key = key;
    new_seg->size = size;
    new_seg->aligned_size = aligned_size;
    new_seg->memory = mem;
    new_seg->lock_account = 0;
    new_seg->nattch = 0;
    new_seg->attachments = 0;
    new_seg->atime = 0;
    new_seg->dtime = 0;
    new_seg->ctime = current_time_seconds();
    new_seg->cpid = caller_pid;
    new_seg->lpid = 0;
    new_seg->mode = (uint32_t)(shmflg & 0777);
    const struct kernel_credentials *cred=kernel_credentials_current();
    new_seg->uid = new_seg->cuid = cred->euid;
    new_seg->gid = new_seg->cgid = cred->egid;
    new_seg->marked_for_deletion = 0;
    new_seg->active = 1;

    *out_shmid = shmid;
    arch_interrupt_restore(irq);
    return 0;
}

int kernel_shm_at_mm(struct kernel_mm *mm, int32_t caller_pid, int32_t shmid, uint64_t shmaddr,
                     int32_t shmflg, uint64_t *out_attached_addr)
{
    if (out_attached_addr == 0 || mm == 0) {
        return -KERNEL_EFAULT;
    }
    if (shmid < 0) {
        return -KERNEL_EINVAL;
    }
    uint32_t slot_idx = (uint32_t)shmid % KERNEL_SHMMNI;

    uintptr_t irq = arch_interrupt_save();
    if (!shm_table.initialized) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }
    struct kernel_shm_segment *seg = &shm_table.segments[slot_idx];
    if (!seg->active || seg->shmid != shmid) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }
    if (seg->marked_for_deletion) {
        arch_interrupt_restore(irq);
        return -KERNEL_EIDRM;
    }

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

    seg->lpid = caller_pid;
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
    return kernel_shm_at_mm(mm, current_tgid(caller), shmid, shmaddr, shmflg, out_attached_addr);
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
        !attachment->segment->attachments) __builtin_trap();
}

enum kernel_shm_status kernel_shm_attachment_create(
    struct kernel_shm_segment *segment, uint64_t start, uint64_t end,
    struct kernel_shm_attachment **owner)
{
    struct kernel_shm_attachment *attachment = 0;
    uintptr_t irq = arch_interrupt_save();
    if (!owner || *owner || !segment || !segment->active || !segment->memory ||
        start >= end || segment->attachments == UINT32_MAX) __builtin_trap();
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        shm_table.heap, 1, sizeof(*attachment), (void **)&attachment);
    if (status != KERNEL_HEAP_STATUS_OK) {
        arch_interrupt_restore(irq);
        if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
        return KERNEL_SHM_STATUS_NO_MEMORY;
    }
    if (kernel_memory_object_acquire(segment->memory) != KERNEL_MEMORY_OBJECT_OK)
        __builtin_trap();
    *attachment = (struct kernel_shm_attachment){
        .heap = shm_table.heap, .segment = segment, .memory = segment->memory,
        .start = start, .end = end, .shmid = segment->shmid, .references = 1,
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
    if (--attachment->references == 0) {
        struct kernel_shm_segment *segment = attachment->segment;
        kernel_memory_object_release(&attachment->memory);
        segment->attachments--;
        kernel_heap_release(attachment->heap, attachment);
        if (!segment->nattch && !segment->attachments && segment->marked_for_deletion)
            kernel_shm_destroy_segment_locked(segment);
    }
    arch_interrupt_restore(irq);
}

void kernel_shm_attachment_open(struct kernel_shm_attachment *attachment)
{
    uintptr_t irq = arch_interrupt_save();
    kernel_shm_attachment_acquire(attachment);
    struct kernel_shm_segment *segment = attachment->segment;
    if (segment->nattch == UINT64_MAX) __builtin_trap();
    segment->nattch++;
    segment->atime = current_time_seconds();
    segment->lpid = current_tgid(0);
    arch_interrupt_restore(irq);
}

void kernel_shm_attachment_close(struct kernel_shm_attachment *attachment)
{
    uintptr_t irq = arch_interrupt_save();
    attachment_valid(attachment);
    struct kernel_shm_segment *segment = attachment->segment;
    if (!segment->nattch) __builtin_trap();
    segment->nattch--;
    segment->dtime = current_time_seconds();
    segment->lpid = current_tgid(0);
    kernel_shm_attachment_release(&attachment);
    arch_interrupt_restore(irq);
}

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

    uintptr_t irq = arch_interrupt_save();
    if (!shm_table.initialized) {
        arch_interrupt_restore(irq);
        return -KERNEL_EINVAL;
    }

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
        if (pure_cmd == KERNEL_IPC_INFO || pure_cmd == KERNEL_SHM_INFO) {
            /* CPU0 段表保护覆盖槽身份和 memory owner；驻留页数由对象原子读取。 */
            for (uint32_t i = 0; i < KERNEL_SHMMNI; i++) {
                const struct kernel_shm_segment *seg = &shm_table.segments[i];
                if (!seg->active) continue;
                result = i;
                if (pure_cmd == KERNEL_SHM_INFO) {
                    snapshot.usage.used_ids++;
                    snapshot.usage.shm_tot += seg->aligned_size >> BOAROS_PAGE_SHIFT;
                    snapshot.usage.shm_rss += kernel_memory_object_resident_pages(seg->memory);
                }
            }
            if (pure_cmd == KERNEL_IPC_INFO) {
                snapshot.limits = (struct kernel_shminfo64){
                    .shmmax = KERNEL_SHMMAX, .shmmin = KERNEL_SHMMIN,
                    .shmmni = KERNEL_SHMMNI, .shmseg = KERNEL_SHMMNI,
                    .shmall = KERNEL_SHMALL,
                };
                size = sizeof(snapshot.limits);
            } else {
                size = sizeof(snapshot.usage);
            }
        } else {
            struct kernel_shm_segment *seg = &shm_table.segments[(uint32_t)shmid % KERNEL_SHMMNI];
            /* SHM_STAT* 按槽枚举并返回当前完整 ID；IPC_STAT 还校验代次。 */
            if (!seg->active || (pure_cmd == KERNEL_IPC_STAT && seg->shmid != shmid)) {
                arch_interrupt_restore(irq);
                return -KERNEL_EINVAL;
            }
            if (pure_cmd != KERNEL_SHM_STAT_ANY) {
                int permission = shm_permission(seg, 4);
                if (permission) { arch_interrupt_restore(irq); return permission; }
            }
            shm_descriptor(seg, &snapshot.descriptor);
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

    uint32_t slot_idx = (uint32_t)shmid % KERNEL_SHMMNI;
    struct kernel_shm_segment *seg = &shm_table.segments[slot_idx];
    if (!seg->active || seg->shmid != shmid) {
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
            seg->key = KERNEL_IPC_PRIVATE;
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
