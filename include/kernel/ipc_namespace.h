#ifndef BOAROS_KERNEL_IPC_NAMESPACE_H
#define BOAROS_KERNEL_IPC_NAMESPACE_H
#include <kernel/namespace.h>
#include <kernel/raw_lock.h>
struct kernel_sem_table;
struct kernel_msg_table;
struct kernel_shm_table;
struct kernel_mqueue_tree;
/* Namespace owns discovery, never a task or file. Objects that can outlive
 * discovery retain their original table/tree instead of consulting current. */
struct kernel_ipc_namespace {
    struct kernel_namespace base;
    struct kernel_raw_lock lock;
    struct kernel_sem_table *sem;
    struct kernel_msg_table *msg;
    struct kernel_shm_table *shm;
    struct kernel_mqueue_tree *mq;
};
struct kernel_namespace *kernel_ipc_initial(void);
int kernel_ipc_namespace_create(struct kernel_heap *,struct kernel_namespace **);
struct kernel_namespace *kernel_task_ipc_namespace(void);
struct kernel_ipc_namespace *kernel_ipc_namespace_state(struct kernel_namespace *);
int kernel_ipc_namespace_boot(struct kernel_heap *);
/* After task/file/mount teardown; retain the permanent initial identity. */
int kernel_ipc_initial_shutdown(void);
void kernel_sem_namespace_destroy(struct kernel_sem_table *);
void kernel_msg_namespace_destroy(struct kernel_msg_table *);
void kernel_shm_namespace_destroy(struct kernel_shm_table *);
void kernel_mqueue_namespace_destroy(struct kernel_mqueue_tree *);
#endif
