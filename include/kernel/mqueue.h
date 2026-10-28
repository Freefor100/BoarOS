#ifndef BOAROS_KERNEL_MQUEUE_H
#define BOAROS_KERNEL_MQUEUE_H
#include <stddef.h>
#include <stdint.h>
struct kernel_heap;
struct kernel_task;
struct kernel_mm;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_wait_queue;
struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_vfs_stat;
struct kernel_vfs_timespec;
struct kernel_vfs_timespec;
struct kernel_linux_statfs;
struct kernel_mqueue;
struct kernel_mqueue_request;
struct kernel_mq_attr {
    int64_t flags, maxmsg, msgsize, curmsgs, reserved[4];
};
_Static_assert(sizeof(struct kernel_mq_attr)==64,"mq_attr ABI");
int kernel_mqueue_init(struct kernel_heap *);
int kernel_mqueue_shutdown(void);
int64_t kernel_mqueue_open(struct kernel_task *,struct kernel_mm *,uint64_t name,
    uint32_t flags,uint32_t mode,uint64_t attr);
int64_t kernel_mqueue_unlink(struct kernel_task *,struct kernel_mm *,uint64_t name);
int64_t kernel_mqueue_send(struct kernel_task *,struct kernel_mm *,int64_t fd,
    uint64_t data,uint64_t size,uint32_t priority,uint64_t abstime);
int64_t kernel_mqueue_receive(struct kernel_task *,struct kernel_mm *,int64_t fd,
    uint64_t data,uint64_t size,uint64_t priority,uint64_t abstime);
int64_t kernel_mqueue_notify(struct kernel_task *,struct kernel_mm *,int64_t fd,uint64_t event);
int64_t kernel_mqueue_getsetattr(struct kernel_task *,struct kernel_mm *,int64_t fd,
    uint64_t new_attr,uint64_t old_attr);
void kernel_mqueue_abort(struct kernel_task *);
/* Actual descriptor detach (including dup2, CLOEXEC, group exit), outside the
 * fd-table lock. A transient OFD pin or dup/fork acquisition is not a close. */
void kernel_mqueue_close_fd(struct kernel_open_file_description *,struct kernel_task *);
int kernel_mqueue_close(struct kernel_open_file_description *);
uint32_t kernel_mqueue_poll(struct kernel_open_file_description *,uint32_t,
    struct kernel_wait_queue **);
int kernel_mqueue_stat(struct kernel_open_file_description *,struct kernel_vfs_stat *);
int kernel_mqueue_statfs(struct kernel_open_file_description *,struct kernel_linux_statfs *);
int kernel_mqueue_set_mode(struct kernel_open_file_description *,uint32_t);
int kernel_mqueue_set_owner(struct kernel_open_file_description *,uint32_t,uint32_t);
int kernel_mqueue_set_times(struct kernel_open_file_description *,const struct kernel_vfs_timespec *);
int kernel_mqueue_read(struct kernel_open_file_description *,uint64_t offset,
    void *,size_t,size_t *);
int kernel_mqueue_name(struct kernel_open_file_description *,char *,size_t);
int kernel_mqueue_reopen(struct kernel_open_file_description *,struct kernel_heap *,uint32_t,
    struct kernel_open_file_description **);
/* Filesystem nodes and OFDs own queue references independently of the name.
 * Every mount sees the same registry; unlink only removes name discovery. */
int kernel_mqueue_fs_create(struct kernel_heap *,uint64_t,struct kernel_vfs_mount **);
int kernel_mqueue_fs_is_mount(const struct kernel_vfs_mount *);
/* Success moves the real VFS file into the new OFD and clears the source. */
int kernel_mqueue_fs_open(struct kernel_heap *,struct kernel_vfs_file *,uint32_t,
    struct kernel_open_file_description **);
struct kernel_mqueue *kernel_mqueue_lookup_name(const char *,size_t);
struct kernel_mqueue *kernel_mqueue_lookup_inode(uint64_t);
void kernel_mqueue_get(struct kernel_mqueue *);
void kernel_mqueue_put(struct kernel_mqueue *);
int kernel_mqueue_create_name(struct kernel_task *,const char *,uint32_t,
    const struct kernel_mq_attr *,int exclusive,struct kernel_mqueue **,int *created);
int kernel_mqueue_remove_name(const char *,size_t);
int kernel_mqueue_queue_stat(struct kernel_mqueue *,struct kernel_vfs_stat *);
int kernel_mqueue_queue_set_mode(struct kernel_mqueue *,uint32_t);
int kernel_mqueue_queue_set_owner(struct kernel_mqueue *,uint32_t,uint32_t);
int kernel_mqueue_queue_set_times(struct kernel_mqueue *,const struct kernel_vfs_timespec *);
int kernel_mqueue_queue_read(struct kernel_mqueue *,uint64_t,void *,size_t,size_t *);
int kernel_mqueue_queue_permission(struct kernel_mqueue *,unsigned);
int kernel_mqueue_root_stat(struct kernel_vfs_stat *);
int kernel_mqueue_root_permission(unsigned);
int kernel_mqueue_root_set_mode(uint32_t);
int kernel_mqueue_root_set_owner(uint32_t,uint32_t);
int kernel_mqueue_root_set_times(const struct kernel_vfs_timespec *);
int kernel_mqueue_next(uint64_t *,uint64_t *,char *,size_t);
#endif
