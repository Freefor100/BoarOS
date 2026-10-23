#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/vfs.h>
#include "exec_internal.h"

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

/* 与固定 Linux commoncap 的顺序一致：先解码文件能力，再作 root
 * 兼容、unsafe/NNP 降权及 ambient 合并。bounding 只限制文件 P 分支。 */
int kernel_exec_credentials_prepare(struct kernel_credentials *next,
    const struct kernel_credentials *old, const struct kernel_vfs_exec_metadata *file,
    unsigned unsafe_fs, unsigned no_new_privs, unsigned *secureexec, unsigned *dumpable)
{
    uint64_t file_permitted=0, file_inheritable=0;
    int effective=0, has_filecap=0;
    next->permitted=0;
    if (!file->nosuid && !no_new_privs) {
        if (file->mode & 04000U) next->euid=file->uid;
        if ((file->mode & 02010U)==02010U) next->egid=file->gid;
    }
    if (!file->nosuid && file->filecap_present) {
        if (file->filecap_size<4) return -KERNEL_EINVAL;
        uint32_t magic=little32(file->filecap), revision=magic>>24;
        size_t expected=revision==1 ? 12 : revision==2 ? 20 : revision==3 ? 24 : 0;
        if (!expected || expected!=file->filecap_size) return -KERNEL_EINVAL;
        /* rev3 的 rootid 不属于 initial namespace 的 root 时无效。 */
        if (revision!=3 || !little32(file->filecap+20)) {
            has_filecap=1; effective=!!(magic&1U);
            file_permitted=little32(file->filecap+4);
            file_inheritable=little32(file->filecap+8);
            if (revision!=1) {
                file_permitted|=(uint64_t)little32(file->filecap+12)<<32;
                file_inheritable|=(uint64_t)little32(file->filecap+16)<<32;
            }
            file_permitted&=KERNEL_CAP_VALID_MASK;
            file_inheritable&=KERNEL_CAP_VALID_MASK;
            next->permitted=(next->bounding&file_permitted)|(next->inheritable&file_inheritable);
            if (effective && (file_permitted&~next->permitted)) return -KERNEL_EPERM;
        }
    }
    if (!(old->securebits & KERNEL_SECBIT_NOROOT) &&
        !(has_filecap && next->uid && !next->euid)) {
        if (!next->uid || !next->euid) next->permitted=old->bounding|old->inheritable;
        if (!next->euid) effective=1;
    }
    int id_changed=next->euid!=old->euid || !kernel_credentials_in_group(old,next->egid);
    if ((id_changed || (next->permitted&~old->permitted)) &&
        (unsafe_fs || no_new_privs)) {
        if (!kernel_credentials_capable(old,KERNEL_CAP_SETUID) || no_new_privs) {
            next->euid=next->uid; next->egid=next->gid;
        }
        next->permitted&=old->permitted;
    }
    next->suid=next->fsuid=next->euid;
    next->sgid=next->fsgid=next->egid;
    if (has_filecap || id_changed) next->ambient=0;
    next->permitted|=next->ambient;
    next->effective=effective ? next->permitted : next->ambient;
    next->securebits&=~KERNEL_SECBIT_KEEP_CAPS;
    *secureexec=id_changed || next->euid!=old->uid || next->egid!=old->gid ||
        (next->uid && (effective || (next->permitted&~next->ambient)));
    /* 不可读文件单独禁止 dump；capability 增益还触发 commit_creds 的规则。 */
    *dumpable=file->readable && old->euid==old->uid && old->egid==old->gid &&
        next->euid==old->euid && next->egid==old->egid &&
        next->fsuid==old->fsuid && next->fsgid==old->fsgid &&
        !(next->permitted&~old->permitted);
    return 0;
}
