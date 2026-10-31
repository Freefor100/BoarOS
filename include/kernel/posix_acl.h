#ifndef BOAROS_KERNEL_POSIX_ACL_H
#define BOAROS_KERNEL_POSIX_ACL_H
#include <stddef.h>
#include <stdint.h>
struct kernel_credentials;
struct kernel_acl_entry { uint16_t tag, perm; uint32_t id; };
#define KERNEL_ACL_ACCESS "system.posix_acl_access"
#define KERNEL_ACL_DEFAULT "system.posix_acl_default"
int kernel_acl_validate(const void *,size_t);
int kernel_acl_normalize(void *,size_t);
int kernel_acl_permission(const void *,size_t,uint32_t,uint32_t,const struct kernel_credentials *,unsigned);
int kernel_acl_dac_permission(const void *,size_t,uint32_t,uint32_t,uint32_t,
                              const struct kernel_credentials *,unsigned);
int kernel_acl_create(void *,size_t,uint32_t *);
int kernel_acl_chmod(void *,size_t,uint32_t);
int kernel_acl_mode(const void *,size_t,uint32_t *);
int kernel_acl_to_ext4(const void *,size_t,void *,size_t,size_t *);
int kernel_acl_from_ext4(const void *,size_t,void *,size_t,size_t *);
#endif
