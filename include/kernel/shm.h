#ifndef BOAROS_KERNEL_SHM_H
#define BOAROS_KERNEL_SHM_H

#include <stdint.h>
#include <stddef.h>

/* Linux SysV IPC Flags & Commands */
#define KERNEL_IPC_CREAT  00001000
#define KERNEL_IPC_EXCL   00002000
#define KERNEL_IPC_NOWAIT 00004000
#define KERNEL_IPC_RMID   0
#define KERNEL_IPC_SET    1
#define KERNEL_IPC_STAT   2
#define KERNEL_IPC_INFO   3
#define KERNEL_IPC_64     0x0100
#define KERNEL_IPC_PRIVATE ((int32_t)0)

#define KERNEL_SHM_RDONLY 010000
#define KERNEL_SHM_RND    020000
#define KERNEL_SHM_REMAP  040000
#define KERNEL_SHM_EXEC   0100000

#define KERNEL_SHM_LOCK   11
#define KERNEL_SHM_UNLOCK 12
#define KERNEL_SHM_STAT   13
#define KERNEL_SHM_INFO   14

#define KERNEL_SHMMIN 1ULL
#define KERNEL_SHMMNI 128U
#define KERNEL_SHMMAX (1024ULL * 1024ULL * 1024ULL) /* 1 GiB */
#define KERNEL_SHMALL (KERNEL_SHMMAX >> 12)

struct kernel_ipc64_perm {
    int32_t key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    uint16_t seq;
    uint16_t __pad2;
    uint64_t __unused1;
    uint64_t __unused2;
};

struct kernel_shmid64_ds {
    struct kernel_ipc64_perm shm_perm;
    uint64_t shm_segsz;
    int64_t shm_atime;
    int64_t shm_dtime;
    int64_t shm_ctime;
    int32_t shm_cpid;
    int32_t shm_lpid;
    uint64_t shm_nattch;
    uint64_t __unused4;
    uint64_t __unused5;
};

struct kernel_shminfo64 {
    uint64_t shmmax;
    uint64_t shmmin;
    uint64_t shmmni;
    uint64_t shmseg;
    uint64_t shmall;
    uint64_t __unused1;
    uint64_t __unused2;
    uint64_t __unused3;
    uint64_t __unused4;
};

_Static_assert(sizeof(struct kernel_ipc64_perm) == 48, "ipc64_perm layout");
_Static_assert(sizeof(struct kernel_shmid64_ds) == 112, "shmid64_ds layout");

struct kernel_memory_object;

struct kernel_shm_segment {
    int32_t shmid;
    uint16_t seq;
    int32_t key;
    uint64_t size;
    uint64_t aligned_size;
    struct kernel_memory_object *memory;
    uint64_t nattch;
    uint32_t attachments;
    int64_t atime;
    int64_t dtime;
    int64_t ctime;
    int32_t cpid;
    int32_t lpid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint8_t marked_for_deletion;
    uint8_t active;
};

/* Stable logical attachment shared by its VMA fragments and fork copies.
 * Temporary owners do not contribute to the user-visible fragment count. */
struct kernel_shm_attachment {
    struct kernel_heap *heap;
    struct kernel_shm_segment *segment;
    struct kernel_memory_object *memory;
    uint64_t start, end;
    int32_t shmid;
    uint32_t references;
};

struct kernel_heap;
struct physical_page_allocator;
struct kernel_task;
struct kernel_mm;

enum kernel_shm_status {
    KERNEL_SHM_STATUS_OK = 0,
    KERNEL_SHM_STATUS_INVALID_ARGUMENT,
    KERNEL_SHM_STATUS_NO_MEMORY,
    KERNEL_SHM_STATUS_NO_SPACE,
    KERNEL_SHM_STATUS_NOT_FOUND,
    KERNEL_SHM_STATUS_EXIST,
    KERNEL_SHM_STATUS_REMOVED,
    KERNEL_SHM_STATUS_STATE,
};

enum kernel_shm_status kernel_shm_init(
    struct kernel_heap *heap, struct physical_page_allocator *allocator);

int kernel_shm_get(struct kernel_task *caller, int32_t key, uint64_t size,
                   int32_t shmflg, int32_t *out_shmid);

int kernel_shm_at(struct kernel_task *caller, int32_t shmid, uint64_t shmaddr,
                  int32_t shmflg, uint64_t *out_attached_addr);
int kernel_shm_at_mm(struct kernel_mm *mm, int32_t caller_pid, int32_t shmid, uint64_t shmaddr,
                     int32_t shmflg, uint64_t *out_attached_addr);

int kernel_shm_dt(struct kernel_task *caller, uint64_t shmaddr);
int kernel_shm_dt_mm(struct kernel_mm *mm, uint64_t shmaddr);

int kernel_shm_ctl(struct kernel_task *caller, int32_t shmid, int32_t cmd,
                   uint64_t user_buf, int64_t *out_result);
int kernel_shm_ctl_mm(struct kernel_mm *mm, int32_t shmid, int32_t cmd,
                      uint64_t user_buf, int64_t *out_result);

enum kernel_shm_status kernel_shm_attachment_create(
    struct kernel_shm_segment *segment, uint64_t start, uint64_t end,
    struct kernel_shm_attachment **owner);
void kernel_shm_attachment_acquire(struct kernel_shm_attachment *attachment);
/* Consumes the owner; accepts an empty slot for scoped cleanup. */
void kernel_shm_attachment_release(struct kernel_shm_attachment **owner);
/* Only committed VMA descriptors contribute to nattch. These cannot sleep. */
void kernel_shm_attachment_open(struct kernel_shm_attachment *attachment);
void kernel_shm_attachment_close(struct kernel_shm_attachment *attachment);

#endif /* BOAROS_KERNEL_SHM_H */
