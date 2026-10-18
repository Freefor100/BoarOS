#ifndef BOAROS_KERNEL_LOOP_H
#define BOAROS_KERNEL_LOOP_H
#include <stdint.h>
#include <stddef.h>
struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_char_device;
#define KERNEL_LOOP_DEVICE_NUMBER(index) \
    (UINT64_C(0x700) | ((uint64_t)(index)&255U) | (((uint64_t)(index)&~UINT64_C(255))<<12))
#define KERNEL_LOOP_READ_ONLY 1U
#define KERNEL_LOOP_AUTOCLEAR 4U
/* Native Linux RV64/LA64 UAPI layouts; no compat32 ABI. */
struct kernel_loop_info64 {
    uint64_t device, inode, rdevice, offset, sizelimit;
    uint32_t number, encrypt_type, encrypt_key_size, flags;
    unsigned char file_name[64], crypt_name[64], encrypt_key[32];
    uint64_t init[2];
};
struct kernel_loop_info {
    int32_t number;
    uint32_t device;
    uint64_t inode;
    uint32_t rdevice;
    int32_t offset, encrypt_type, encrypt_key_size, flags;
    unsigned char name[64], encrypt_key[32];
    uint64_t init[2];
    unsigned char reserved[4];
};
struct kernel_loop_config {
    uint32_t fd, block_size;
    struct kernel_loop_info64 info;
    uint64_t reserved[8];
};
_Static_assert(sizeof(struct kernel_loop_info)==160,"Linux loop_info native layout");
_Static_assert(offsetof(struct kernel_loop_info,device)==4 && offsetof(struct kernel_loop_info,inode)==8 &&
    offsetof(struct kernel_loop_info,rdevice)==16 && offsetof(struct kernel_loop_info,offset)==20 &&
    offsetof(struct kernel_loop_info,name)==36 && offsetof(struct kernel_loop_info,init)==136,
    "RV64/LA64 asm-generic __kernel_old_dev_t is unsigned int");
_Static_assert(sizeof(struct kernel_loop_info64)==232,"Linux loop_info64 layout");
_Static_assert(sizeof(struct kernel_loop_config)==304,"Linux loop_config layout");
int kernel_loop_initialize(struct kernel_heap *heap);
/* Shutdown only: detach unclaimed loops, retaining failed backing owners.
 * Positive means actual progress; zero means no removable binding. */
int kernel_loop_release_unused(void);
int kernel_loop_shutdown(void);
int kernel_loop_shutdown_mounts(struct kernel_vfs_mount *root);
extern const struct kernel_char_device kernel_loop_control_device;
#endif
