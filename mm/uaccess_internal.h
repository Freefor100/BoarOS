#ifndef BOAROS_MM_UACCESS_INTERNAL_H
#define BOAROS_MM_UACCESS_INTERNAL_H

#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/cost.h>

/* Share fault/retry preparation while allowing GUP-style callers to retain
 * ENOMEM. Ordinary copy callers still translate a failed pin to EFAULT. */
static inline enum kernel_mm_status kernel_uaccess_pin_resolved(
    struct kernel_mm *mm,uint64_t address,uint64_t second,uint32_t access,
    struct kernel_mm_user_pin *pin,unsigned *count)
{
    COST_ADD(USER_RESOLUTIONS,1);COST_IO_ADD(4,1);
    enum kernel_mm_status status;
    for (;;) {
        status=count?kernel_mm_pin_user_pair(mm,address,second,access,pin,count):
            kernel_mm_pin_user_page(mm,address,access,pin);
        if(status!=KERNEL_MM_STATUS_NOT_MAPPED && status!=KERNEL_MM_STATUS_ACCESS)break;
        status=kernel_mm_resolve_user_fault(mm,address,access);
        if(status!=KERNEL_MM_STATUS_OK)break;
        /* fault结束后快照可再次撤W；最终pin必须重新检查实际权限。 */
    }
    if(!status && count){COST_ADD(USER_RESOLUTIONS,1);COST_IO_ADD(4,1);}
    return status;
}

/* 实际复制方持有MM owner；成功返回的物理pin由同一调用现场归还。
 * fault可睡眠，调用前不得持有目标inode I/O或缓存写入借用。 */
enum kernel_uaccess_status kernel_uaccess_pin_user_page(
    struct kernel_mm *, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *);
enum kernel_uaccess_status kernel_uaccess_pin_user_pair(
    struct kernel_mm *, uint64_t first,uint64_t second,uint32_t access,
    struct kernel_mm_user_pin pins[2],unsigned *count);

/* 同一批已取得的数据片段共用当前用户页；换页前先归还，fault不持旧pin。
 * cursor不可复制，调用者必须在下一次存储等待和退出前finish。 */
struct kernel_uaccess_write_cursor {
    struct kernel_mm_user_pin pin;
    uint64_t virtual_page;
};
enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(
    struct kernel_mm *, struct kernel_uaccess_write_cursor *,
    uint64_t destination, const void *source, size_t size, size_t *copied);
void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *);


#endif
