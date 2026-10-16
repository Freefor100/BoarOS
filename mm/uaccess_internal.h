#ifndef BOAROS_MM_UACCESS_INTERNAL_H
#define BOAROS_MM_UACCESS_INTERNAL_H

#include <kernel/mm.h>
#include <kernel/uaccess.h>

/* 实际复制方持有MM owner；成功返回的物理pin由同一调用现场归还。
 * fault可睡眠，调用前不得持有目标inode I/O或缓存写入借用。 */
enum kernel_uaccess_status kernel_uaccess_pin_user_page(
    struct kernel_mm *, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *);

/* 同一批已取得的数据片段共用当前用户页；换页前先归还，fault不持旧pin。
 * cursor不可复制，调用者必须在下一次存储等待和退出前finish。 */
struct kernel_uaccess_write_cursor {
    struct kernel_mm_user_pin pin;
    uint64_t virtual_page;
#if BOAROS_USER_SMP
    /* 缺页/撤权/COW只报告RETRY及已复制前缀，交回CPU0完整路径。 */
    unsigned resident_only;
#endif
};
enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(
    struct kernel_mm *, struct kernel_uaccess_write_cursor *,
    uint64_t destination, const void *source, size_t size, size_t *copied);
void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *);

#if BOAROS_USER_SMP
/* 小iovec导入等只读resident页；RETRY可带内部复制前缀，调用者重导完整输入。 */
enum kernel_uaccess_status kernel_copy_from_user_resident(
    struct kernel_mm *, void *destination, uint64_t source, size_t size,
    size_t *copied);
#endif

#endif
