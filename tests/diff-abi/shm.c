#include "abi.h"

#include <stdint.h>

#define ABI_IPC_CREAT 01000
#define ABI_IPC_EXCL 02000
#define ABI_IPC_RMID 0
#define ABI_IPC_SET 1
#define ABI_IPC_STAT 2
#define ABI_IPC_INFO 3
#define ABI_IPC_PRIVATE 0

#define ABI_SHM_RDONLY 010000
#define ABI_SHM_RND 020000
#define ABI_SHM_REMAP 040000

struct abi_ipc64_perm {
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

struct abi_shmid64_ds {
    struct abi_ipc64_perm shm_perm;
    uint64_t shm_segsz;
    int64_t shm_atime;
    int64_t shm_dtime;
    int64_t shm_ctime;
    int32_t shm_cpid;
    int32_t shm_lpid;
    uint64_t shm_nattch;
    uint64_t reserved4;
    uint64_t reserved5;
};

_Static_assert(sizeof(struct abi_ipc64_perm) == 48, "ipc64_perm layout");
_Static_assert(sizeof(struct abi_shmid64_ds) == 112, "shmid64_ds layout");

static long sys_shmget(long key, long size, long shmflg)
{
    return SC3(194, key, size, shmflg);
}

static long sys_shmctl(long shmid, long cmd, void *buf)
{
    return SC3(195, shmid, cmd, buf);
}

static long sys_shmat(long shmid, const void *shmaddr, long shmflg)
{
    return SC3(196, shmid, shmaddr, shmflg);
}

static long sys_shmdt(const void *shmaddr)
{
    return SC1(197, shmaddr);
}

void abi_shm_cases(void)
{
    /* 1. shmget invalid arguments */
    long ret = sys_shmget(ABI_IPC_PRIVATE, 0, 0666 | ABI_IPC_CREAT);
    abi_record("shm.get-size-zero", ret, -1, -1, 0, 0, 0);

    ret = sys_shmget(0x192837, 4096, 0);
    abi_record("shm.get-enoent", ret, -1, -1, 0, 0, 0);

    /* 2. shmget IPC_PRIVATE creation */
    long shmid_priv = sys_shmget(ABI_IPC_PRIVATE, 4096, 0600 | ABI_IPC_CREAT);
    abi_record("shm.get-private-creat", shmid_priv < 0 ? shmid_priv : 0, -1, -1, 0, 0, 0);
    abi_require(shmid_priv >= 0);

    /* 3. shmget named key creation and conflict */
    long named_key = 0x4321;
    long shmid_named = sys_shmget(named_key, 4096, 0600 | ABI_IPC_CREAT);
    abi_record("shm.get-named-creat", shmid_named < 0 ? shmid_named : 0, -1, -1, 0, 0, 0);
    abi_require(shmid_named >= 0);

    ret = sys_shmget(named_key, 4096, 0600 | ABI_IPC_CREAT | ABI_IPC_EXCL);
    abi_record("shm.get-named-excl-conflict", ret, -1, -1, 0, 0, 0);

    long shmid_lookup = sys_shmget(named_key, 4096, 0);
    abi_record("shm.get-named-existing",
               shmid_lookup == shmid_named ? 0 : -1, -1, -1, 0, 0, 0);

    const long lookup_flags[] = {0, ABI_IPC_CREAT, ABI_IPC_EXCL,
                                ABI_IPC_CREAT | ABI_IPC_EXCL};
    const long sizes[] = {0, 4096, 8192};
    const char *existing_names[3][4] = {
        {"shm.lookup-zero", "shm.lookup-zero-create", "shm.lookup-zero-excl", "shm.lookup-zero-create-excl"},
        {"shm.lookup-size", "shm.lookup-size-create", "shm.lookup-size-excl", "shm.lookup-size-create-excl"},
        {"shm.lookup-large", "shm.lookup-large-create", "shm.lookup-large-excl", "shm.lookup-large-create-excl"},
    };
    for (unsigned size_index = 0; size_index < 3; size_index++) {
        for (unsigned flag_index = 0; flag_index < 4; flag_index++) {
            ret = sys_shmget(named_key, sizes[size_index], lookup_flags[flag_index] | 0600);
            abi_record(existing_names[size_index][flag_index],
                       ret < 0 ? ret : ret == shmid_named ? 0 : -1, -1, -1, 0, 0, 0);
        }
    }
    const char *missing_names[2][4] = {
        {"shm.missing-zero", "shm.missing-zero-create", "shm.missing-zero-excl", "shm.missing-zero-create-excl"},
        {"shm.missing-size", "shm.missing-size-create", "shm.missing-size-excl", "shm.missing-size-create-excl"},
    };
    for (unsigned size_index = 0; size_index < 2; size_index++) {
        for (unsigned flag_index = 0; flag_index < 4; flag_index++) {
            long key = named_key + 1 + size_index * 4 + flag_index;
            ret = sys_shmget(key, sizes[size_index], lookup_flags[flag_index] | 0600);
            abi_record(missing_names[size_index][flag_index], ret < 0 ? ret : 0, -1, -1, 0, 0, 0);
            if (ret >= 0) abi_require(sys_shmctl(ret, ABI_IPC_RMID, 0) == 0);
        }
    }

    /* 4. shmat errors */
    ret = sys_shmat(-1, 0, 0);
    abi_record("shm.at-invalid-id", ret, -1, -1, 0, 0, 0);

    ret = sys_shmat(shmid_named, (const void *)0x10001, 0);
    abi_record("shm.at-unaligned-addr", ret, -1, -1, 0, 0, 0);

    /* 5. shmat valid attach */
    long addr = sys_shmat(shmid_priv, 0, 0);
    abi_record("shm.at-private", addr < 0 ? addr : 0, -1, -1, 0, 0, 0);
    abi_require(addr > 0);

    /* Write pattern and read back */
    char *buf = (char *)addr;
    for (int i = 0; i < 64; i++) {
        buf[i] = (char)(0x41 + (i % 26));
    }
    int match = 1;
    for (int i = 0; i < 64; i++) {
        if (buf[i] != (char)(0x41 + (i % 26))) {
            match = 0;
            break;
        }
    }
    abi_record("shm.write-read", match ? 0 : -1, -1, -1, 0, 0, 0);

    /* 6. shmdt invalid address */
    ret = sys_shmdt((const void *)0x12345000);
    abi_record("shm.dt-invalid-addr", ret, -1, -1, 0, 0, 0);

    /* 7. shmctl errors */
    struct abi_shmid64_ds ds;
    for (usize i = 0; i < sizeof(ds); i++) ((char *)&ds)[i] = 0;
    ret = sys_shmctl(-1, ABI_IPC_STAT, &ds);
    abi_record("shm.ctl-invalid-id", ret, -1, -1, 0, 0, 0);

    ret = sys_shmctl(shmid_priv, 9999, 0);
    abi_record("shm.ctl-invalid-cmd", ret, -1, -1, 0, 0, 0);

    /* 8. shmctl IPC_STAT */
    ret = sys_shmctl(shmid_priv, ABI_IPC_STAT, &ds);
    abi_record("shm.ctl-stat", ret, (long)ds.shm_segsz, (long)ds.shm_nattch, 0, 0, 0);

    /* 9. shmctl IPC_RMID */
    ret = sys_shmctl(shmid_priv, ABI_IPC_RMID, 0);
    abi_record("shm.ctl-rmid-private", ret, -1, -1, 0, 0, 0);

    /* 10. shmdt valid detach */
    ret = sys_shmdt((const void *)addr);
    abi_record("shm.dt-private", ret, -1, -1, 0, 0, 0);

    /* 11. shmat after segment destroyed */
    ret = sys_shmat(shmid_priv, 0, 0);
    abi_record("shm.at-after-destroy", ret, -1, -1, 0, 0, 0);

    /* 12. clean up named segment */
    ret = sys_shmctl(shmid_named, ABI_IPC_RMID, 0);
    abi_record("shm.ctl-rmid-named", ret, -1, -1, 0, 0, 0);

    ret = sys_shmget(named_key, 4096, 0);
    abi_record("shm.get-named-after-rmid", ret, -1, -1, 0, 0, 0);
}
