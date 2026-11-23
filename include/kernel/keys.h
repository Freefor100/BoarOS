#ifndef BOAROS_KERNEL_KEYS_H
#define BOAROS_KERNEL_KEYS_H

struct kernel_credentials;
struct kernel_heap;

enum kernel_key_quota {
    KERNEL_KEY_QUOTA_MAXKEYS,
    KERNEL_KEY_QUOTA_MAXBYTES,
    KERNEL_KEY_QUOTA_ROOT_MAXKEYS,
    KERNEL_KEY_QUOTA_ROOT_MAXBYTES,
    KERNEL_KEY_QUOTA_COUNT,
};

unsigned kernel_keys_quota_get(enum kernel_key_quota quota);
int kernel_keys_quota_set(enum kernel_key_quota quota, unsigned limit);

#if defined(__riscv) || defined(__loongarch__)
void kernel_keys_credentials_get(struct kernel_credentials *);
void kernel_keys_credentials_put(const struct kernel_credentials *);
void kernel_keys_credentials_fork(struct kernel_credentials *, int thread);
void kernel_keys_credentials_exec(struct kernel_credentials *);
void kernel_keys_credentials_change_ids(const struct kernel_credentials *,
    const struct kernel_credentials *);
#else
/* Native host models compile credentials without the guest key service. */
static inline void kernel_keys_credentials_get(struct kernel_credentials *cred) { (void)cred; }
static inline void kernel_keys_credentials_put(const struct kernel_credentials *cred) { (void)cred; }
static inline void kernel_keys_credentials_fork(struct kernel_credentials *cred, int thread)
{ (void)cred; (void)thread; }
static inline void kernel_keys_credentials_exec(struct kernel_credentials *cred) { (void)cred; }
static inline void kernel_keys_credentials_change_ids(const struct kernel_credentials *old,
    const struct kernel_credentials *replacement) { (void)old; (void)replacement; }
#endif

int kernel_keys_shutdown(struct kernel_heap *);

#endif
