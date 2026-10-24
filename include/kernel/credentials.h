#ifndef BOAROS_KERNEL_CREDENTIALS_H
#define BOAROS_KERNEL_CREDENTIALS_H

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_task;
struct kernel_exec_security;

#define KERNEL_CAP_LAST_CAP 40U
#define KERNEL_CAP_VALID_MASK ((UINT64_C(1) << 41) - 1)
#define KERNEL_GROUPS_MAX 65536U
enum kernel_capability {
    KERNEL_CAP_CHOWN=0, KERNEL_CAP_DAC_OVERRIDE=1,
    KERNEL_CAP_DAC_READ_SEARCH=2, KERNEL_CAP_FOWNER=3, KERNEL_CAP_FSETID=4,
    KERNEL_CAP_KILL=5, KERNEL_CAP_SETGID=6, KERNEL_CAP_SETUID=7,
    KERNEL_CAP_SETPCAP=8, KERNEL_CAP_LINUX_IMMUTABLE=9,
    KERNEL_CAP_NET_BIND_SERVICE=10, KERNEL_CAP_NET_ADMIN=12,
    KERNEL_CAP_NET_RAW=13, KERNEL_CAP_IPC_LOCK=14, KERNEL_CAP_IPC_OWNER=15,
    KERNEL_CAP_SYS_MODULE=16, KERNEL_CAP_SYS_RAWIO=17,
    KERNEL_CAP_SYS_CHROOT=18, KERNEL_CAP_SYS_PTRACE=19,
    KERNEL_CAP_SYS_ADMIN=21, KERNEL_CAP_SYS_NICE=23,
    KERNEL_CAP_SYS_RESOURCE=24, KERNEL_CAP_SYS_TIME=25,
    KERNEL_CAP_SYS_TTY_CONFIG=26, KERNEL_CAP_MKNOD=27,
    KERNEL_CAP_SETFCAP=31, KERNEL_CAP_MAC_OVERRIDE=32,
    KERNEL_CAP_SYSLOG=34,
};
#define KERNEL_SECBIT_NOROOT UINT32_C(1)
#define KERNEL_SECBIT_NO_SETUID_FIXUP UINT32_C(4)
#define KERNEL_SECBIT_KEEP_CAPS UINT32_C(16)
#define KERNEL_SECBIT_KEEP_CAPS_LOCKED UINT32_C(32)
#define KERNEL_SECBIT_NO_CAP_AMBIENT_RAISE UINT32_C(64)

/* references 与 pending 均由 kernel_wait_domain 保护。待决记录单独持有账户，
 * 因而线程改 UID 后仍向接纳该记录的账户归还额度。 */
struct kernel_uid_account {
    struct kernel_uid_account *next;
    struct kernel_heap *heap;
    uint64_t references, pending;
    uint64_t unix_inflight; /* AF_UNIX queued descriptors, protected by rights mutex. */
    uint32_t uid;
};
struct kernel_credential_groups {
    struct kernel_heap *heap;
    uint32_t references, count;
    uint32_t ids[];
};
/* 发布后除 references 外均不可修改；候选由单个调用者独占。 */
struct kernel_credentials {
    struct kernel_heap *heap;
    uint32_t references;
    uint32_t uid,euid,suid,fsuid,gid,egid,sgid,fsgid;
    struct kernel_credential_groups *groups;
    uint64_t effective,permitted,inheritable,bounding,ambient;
    uint32_t securebits;
    struct kernel_uid_account *account;
};

const struct kernel_credentials *kernel_credentials_initial(void);
/* 当前执行线程或已有同步 I/O 请求借用的原调用者；返回调用期借用。 */
const struct kernel_credentials *kernel_credentials_current(void);
/* 调用者持有 replacement，恢复返回的 previous 后归还该借用。 */
const struct kernel_credentials *kernel_credentials_override(const struct kernel_credentials *replacement);
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *);
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *);
void kernel_credentials_put(const struct kernel_credentials *);
/* 返回独立引用；调用者持有 task 的实际存储借用。 */
const struct kernel_credentials *kernel_credentials_snapshot(const struct kernel_task *);
struct kernel_credentials *kernel_credentials_clone(struct kernel_heap *,const struct kernel_credentials *);
/* 消费 candidate 引用，在现有调度保护下发布；旧引用在锁外归还。 */
void kernel_credentials_publish(struct kernel_task *,struct kernel_credentials *candidate);
/* 在发布前为新的真实 UID 取得账户。失败不改变候选。 */
int kernel_credentials_set_account(struct kernel_credentials *,uint32_t uid);
int kernel_credentials_capable(const struct kernel_credentials *,unsigned capability);
int kernel_credentials_in_group(const struct kernel_credentials *,uint32_t gid);

struct kernel_uid_account *kernel_uid_account_get(struct kernel_heap *,uint32_t uid);
struct kernel_uid_account *kernel_uid_account_get_locked(struct kernel_uid_account *);
/* 返回最后引用对应的已摘链账户；在锁外调用 release。 */
struct kernel_uid_account *kernel_uid_account_put_locked(struct kernel_uid_account *);
void kernel_uid_account_release(struct kernel_uid_account *);
void kernel_uid_account_put(struct kernel_uid_account *);

struct kernel_credential_groups *kernel_credential_groups_allocate(struct kernel_heap *,uint32_t count);
void kernel_credential_groups_put(struct kernel_credential_groups *);
void kernel_credential_groups_sort(struct kernel_credential_groups *);

/* 与用户映像共享关系一致，但独立于页表寿命；僵尸任务仍可查询属主。
 * create 在锁外分配，发布/共享由调度域保护，最后任务引用退出后释放。 */
struct kernel_exec_security *kernel_exec_security_create(struct kernel_heap *,unsigned dumpable);
struct kernel_exec_security *kernel_exec_security_get(struct kernel_exec_security *);
void kernel_exec_security_put(struct kernel_exec_security *);
unsigned kernel_exec_security_dumpable(const struct kernel_exec_security *);
void kernel_exec_security_set_dumpable(struct kernel_exec_security *,unsigned);

/* 凭据发布的执行安全副作用由进程层提供，在 kernel_wait_domain 内调用。 */
void kernel_credentials_changed_locked(struct kernel_task *,const struct kernel_credentials *old,
    const struct kernel_credentials *replacement);
int kernel_credentials_dumpable_get(struct kernel_task *);
void kernel_credentials_dumpable_set(struct kernel_task *,unsigned value);

int kernel_credentials_may_access_locked(const struct kernel_task *,const struct kernel_task *,int fscreds);

#endif
