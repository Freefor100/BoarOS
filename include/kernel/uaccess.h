#ifndef BOAROS_KERNEL_UACCESS_H
#define BOAROS_KERNEL_UACCESS_H

#include <stddef.h>
#include <stdint.h>

struct kernel_mm;

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

/* 调用者持有MM owner；每个基页片段经权限检查取得实际pin后复制，
 * fault及最后pin之间的映射/临时写许可变化须重查，失败报告精确已复制前缀。
 * pin只保证物理存活；受控磁盘页写借用覆盖复制到归还，快照须排空该页写者。
 * 默认业务仍限CPU0；受控域以MM gate/raw、活动集合和确认失效后回收保护并发。
 * 不能以pin代替内容同步，或把此接口解释为普通用户/所有文件业务的SMP支持。 */
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

/* write/COW解析完成后，对实际pin的对齐32位字执行原生原子比较交换。
 * pin负责页存活；futex键、比较与等待登记的同步由futex桶操作负责。 */
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
