#ifndef BOAROS_KERNEL_UACCESS_H
#define BOAROS_KERNEL_UACCESS_H

#include <stddef.h>
#include <stdint.h>

struct kernel_mm;

/* Single-hart diagnostic count of user-page resolution attempts. */
uint64_t kernel_uaccess_page_resolutions(void);

enum kernel_uaccess_status {
    KERNEL_UACCESS_STATUS_OK = 0,
    KERNEL_UACCESS_STATUS_INVALID_ARGUMENT,
    KERNEL_UACCESS_STATUS_FAULT,
    KERNEL_UACCESS_STATUS_TOO_LONG,
    KERNEL_UACCESS_STATUS_STATE,
};

enum kernel_uaccess_status kernel_user_range_check(
    uint64_t user_address,
    size_t size);

/* 调用者持有MM owner；当前解析到页内复制依赖单CPU调用纪律，未取得覆盖复制的
 * 额外物理页pin。fault可睡眠，恢复后重查PTE/权限；失败报告精确已复制前缀。
 * SMP前必须在同一MM保护下lookup并取得pin，睡眠后重查VMA/PTE/COW；pin只保证
 * 存活，权限许可点及撤权与在途复制关系须另定义。unmap/truncate/降权与活动CPU
 * TLB完成确认后回收必须连起来实现，不能只给resolve返回的指针加引用。 */
enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied);

enum kernel_uaccess_status kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied);

/* 当前单CPU IRQ纪律下比较并更新已解析write/COW的对齐futex字。
 * 这不是跨核原子性或lookup+pin协议；SMP桶锁/页生命周期另行实现。 */
enum kernel_uaccess_status kernel_user_cmpxchg_u32(
    struct kernel_mm *mm, uint64_t user_address,
    uint32_t expected, uint32_t desired, uint32_t *observed);

/* On success, string_length excludes the terminating NUL byte. */
enum kernel_uaccess_status kernel_copy_string_from_user(
    struct kernel_mm *mm,
    char *kernel_destination,
    uint64_t user_source,
    size_t capacity,
    size_t *string_length);

#endif
