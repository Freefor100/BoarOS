#ifndef BOAROS_KERNEL_MSG_H
#define BOAROS_KERNEL_MSG_H
#include <kernel/ipc.h>
#include <stdint.h>
#define KERNEL_MSG_NOERROR 010000
#define KERNEL_MSG_EXCEPT 020000
#define KERNEL_MSG_COPY 040000
#define KERNEL_MSG_STAT 11
#define KERNEL_MSG_INFO 12
#define KERNEL_MSG_STAT_ANY 13
#define KERNEL_MSGMNI 32000U
#define KERNEL_MSGMAX 8192U
#define KERNEL_MSGMNB 16384U
struct kernel_msqid64_ds {
    struct kernel_ipc64_perm msg_perm;
    int64_t msg_stime, msg_rtime, msg_ctime;
    uint64_t msg_cbytes, msg_qnum, msg_qbytes;
    int32_t msg_lspid, msg_lrpid;
    uint64_t __unused4, __unused5;
};
struct kernel_msginfo {
    int32_t msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql;
    uint16_t msgseg, __pad;
};
_Static_assert(sizeof(struct kernel_msqid64_ds) == 120, "msqid64_ds layout");
_Static_assert(sizeof(struct kernel_msginfo) == 32, "msginfo layout");
struct kernel_heap;
struct kernel_task;
struct kernel_mm;
struct kernel_msg_request;
int kernel_msg_init(struct kernel_heap *);
int64_t kernel_msg_get(struct kernel_task *, int32_t key, int32_t flags);
int64_t kernel_msg_ctl(struct kernel_task *, struct kernel_mm *, int32_t id, int32_t cmd,
                       uint64_t arg);
int64_t kernel_msg_send(struct kernel_task *, struct kernel_mm *, int32_t id, uint64_t msgp,
                        uint64_t size, int32_t flags);
int64_t kernel_msg_receive(struct kernel_task *, struct kernel_mm *, int32_t id, uint64_t msgp,
                           uint64_t size, int64_t type, int32_t flags);
/* The caller owns this stopped task or is the task. Scheduler wait disposal
 * drains notification borrows before reclaiming task storage. */
void kernel_msg_abort(struct kernel_task *);
/* Slot cursor starts at zero. Copy one live row under index/object protection;
 * the returned descriptor owns no queue, PID, message or wait references. */
int kernel_msg_proc_next(uint32_t *cursor, int32_t *id, struct kernel_msqid64_ds *row);
#endif
