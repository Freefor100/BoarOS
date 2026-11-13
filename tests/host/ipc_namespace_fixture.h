/* Machine/task boundary for the real namespace and IPC backend code. */
#ifndef BOAROS_HOST_IPC_NAMESPACE_FIXTURE_H
#define BOAROS_HOST_IPC_NAMESPACE_FIXTURE_H
#include <kernel/ipc_namespace.h>
#include <assert.h>
static _Thread_local struct kernel_namespace *host_ipc_namespace;
struct kernel_namespace *kernel_task_ipc_namespace(void)
{return host_ipc_namespace?host_ipc_namespace:kernel_ipc_initial();}
#ifndef IPC_FIXTURE_SEM
void kernel_sem_namespace_destroy(struct kernel_sem_table *table){assert(!table);}
#endif
#ifndef IPC_FIXTURE_MSG
void kernel_msg_namespace_destroy(struct kernel_msg_table *table){assert(!table);}
#endif
#ifndef IPC_FIXTURE_SHM
void kernel_shm_namespace_destroy(struct kernel_shm_table *table){assert(!table);}
#endif
#ifndef IPC_FIXTURE_MQ
void kernel_mqueue_namespace_destroy(struct kernel_mqueue_tree *tree){assert(!tree);}
#endif
#endif
