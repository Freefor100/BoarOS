#ifndef BOAROS_KERNEL_IPC_H
#define BOAROS_KERNEL_IPC_H
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <stdint.h>

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

/* IPC uses effective IDs and creator/owner identities, independently of
 * filesystem fsuid checks. Callers supply one protected permission snapshot. */
static inline int kernel_ipc_permission(const struct kernel_credentials *cred,
    unsigned mode, uint32_t uid, uint32_t gid, uint32_t cuid, uint32_t cgid,
    unsigned requested)
{
    if (cred->euid==uid || cred->euid==cuid) mode>>=6;
    else if (kernel_credentials_in_group(cred,gid) || kernel_credentials_in_group(cred,cgid)) mode>>=3;
    return (requested&~mode&7U) && !kernel_credentials_capable(cred,KERNEL_CAP_IPC_OWNER)?-KERNEL_EACCES:0;
}
static inline int kernel_ipc_owner(const struct kernel_credentials *cred,
    uint32_t uid, uint32_t cuid)
{
    return cred->euid==uid || cred->euid==cuid ||
        kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN);
}
_Static_assert(sizeof(struct kernel_ipc64_perm)==48,"ipc64_perm layout");
#endif
