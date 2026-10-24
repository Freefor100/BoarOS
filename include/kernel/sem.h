#ifndef BOAROS_KERNEL_SEM_H
#define BOAROS_KERNEL_SEM_H

#include <kernel/ipc.h>
#include <stdint.h>

#define KERNEL_SEM_UNDO 0x1000
#define KERNEL_SEM_GETPID 11
#define KERNEL_SEM_GETVAL 12
#define KERNEL_SEM_GETALL 13
#define KERNEL_SEM_GETNCNT 14
#define KERNEL_SEM_GETZCNT 15
#define KERNEL_SEM_SETVAL 16
#define KERNEL_SEM_SETALL 17
#define KERNEL_SEM_STAT 18
#define KERNEL_SEM_INFO 19
#define KERNEL_SEM_STAT_ANY 20
#define KERNEL_SEMMNI 32000U
#define KERNEL_SEMMSL 32000U
#define KERNEL_SEMMNS 1024000000U
#define KERNEL_SEMOPM 500U
#define KERNEL_SEMVMX 32767
#define KERNEL_SEMAEM 32767

struct kernel_sembuf { uint16_t sem_num; int16_t sem_op, sem_flg; };
struct kernel_semid64_ds {
    struct kernel_ipc64_perm sem_perm;
    int64_t sem_otime, sem_ctime;
    uint64_t sem_nsems, __unused3, __unused4;
};
struct kernel_seminfo {
    int32_t semmap, semmni, semmns, semmnu, semmsl;
    int32_t semopm, semume, semusz, semvmx, semaem;
};
_Static_assert(sizeof(struct kernel_sembuf) == 6, "sembuf layout");
_Static_assert(sizeof(struct kernel_semid64_ds) == 88, "semid64_ds layout");
_Static_assert(sizeof(struct kernel_seminfo) == 40, "seminfo layout");

struct kernel_heap;
struct kernel_task;
struct kernel_mm;
struct kernel_sem_undo;
struct kernel_sem_request;
int kernel_sem_init(struct kernel_heap *);
int64_t kernel_sem_get(struct kernel_task *, int32_t key, int32_t nsems, int32_t flags);
int64_t kernel_sem_ctl(struct kernel_task *, struct kernel_mm *, int32_t id,
    int32_t num, int32_t cmd, uint64_t arg);
int64_t kernel_sem_op(struct kernel_task *, struct kernel_mm *, int32_t id,
    uint64_t ops, uint64_t count, uint64_t timeout_address);
/* Before child publication; an empty group must already be shared. */
int kernel_sem_clone(struct kernel_task *parent, struct kernel_task *child, int shared);
/* The caller owns a stopped task, or is that task. Scheduler wait disposal
 * separately drains notification borrows before reclaiming the task storage. */
void kernel_sem_abort(struct kernel_task *);
void kernel_sem_exit(struct kernel_task *);
/* Slot cursor starts at zero; the copied row holds no set/undo/PID owners. */
int kernel_sem_proc_next(uint32_t *cursor, int32_t *id, struct kernel_semid64_ds *row);
#endif
