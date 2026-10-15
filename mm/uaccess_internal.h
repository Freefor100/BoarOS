#ifndef BOAROS_MM_UACCESS_INTERNAL_H
#define BOAROS_MM_UACCESS_INTERNAL_H

#include <kernel/mm.h>
#include <kernel/uaccess.h>

/* 实际复制方持有MM owner；成功返回的物理pin由同一调用现场归还。
 * fault可睡眠，调用前不得持有目标inode I/O或缓存写入借用。 */
enum kernel_uaccess_status kernel_uaccess_pin_user_page(
    struct kernel_mm *, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *);

#endif
