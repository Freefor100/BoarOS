#include "vfs_objects.h"
#include <kernel/mqueue.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include <string.h>

struct mq_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    uint64_t flags;
};
static struct kernel_vfs_backend mq_ops;
int kernel_mqueue_fs_is_mount(const struct kernel_vfs_mount *mount)
{
    const struct kernel_vfs_instance *i=mount?mount->private_data:0;
    return i&&i->ops==&mq_ops;
}
struct kernel_mqueue *kernel_mqueue_fs_file_queue(const struct kernel_vfs_file *file)
{
    if(!file||!kernel_mqueue_fs_is_mount(file->mount))return 0;
    const struct kernel_vfs_node *node=file->private_data;
    return node&&node->inode!=1?node->backend_data:0;
}
static int error(struct kernel_vfs_instance *i){(void)i;return 0;}
static int root(struct kernel_vfs_instance *i,uint64_t *id,uint32_t *mode)
{(void)i;struct kernel_vfs_stat stat;kernel_mqueue_root_stat(&stat);*id=1;*mode=stat.mode;return 0;}
static int lookup(struct kernel_vfs_instance *i,uint64_t parent,const char *name,
    size_t n,uint64_t *id,uint32_t *mode)
{
    (void)i;if(parent!=1)return -KERNEL_ENOTDIR;
    struct kernel_mqueue *q=kernel_mqueue_lookup_name(name,n);if(!q)return -KERNEL_ENOENT;
    struct kernel_vfs_stat stat;kernel_mqueue_queue_stat(q,&stat);kernel_mqueue_put(q);
    *id=stat.ino;*mode=stat.mode;return 0;
}
static int open_inode(struct kernel_vfs_mount *mount,const char *path,uint64_t id,
    uint32_t mode,struct kernel_vfs_file *file)
{
    (void)path;(void)mode;struct kernel_vfs_instance *i=mount->private_data;
    struct kernel_mqueue *q=id==1?0:kernel_mqueue_lookup_inode(id);
    if(id!=1&&!q)return -KERNEL_ENOENT;
    struct kernel_vfs_node *node=0;
    enum kernel_heap_status s=kernel_heap_allocate_zeroed(i->heap,1,sizeof(*node),(void **)&node);
    if(s){if(q)kernel_mqueue_put(q);return s==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;}
    node->inode=id;node->backend_data=q;node->max_size=80;
    struct kernel_vfs_stat st;if(q)kernel_mqueue_queue_stat(q,&st);else kernel_mqueue_root_stat(&st);
    node->mode=st.mode;node->uid=st.uid;node->gid=st.gid;node->size=st.size;
    return kernel_vfs_publish_node(mount,node,file,0);
}
static int close_node(struct kernel_vfs_node *node)
{if(node->backend_data)kernel_mqueue_put(node->backend_data);return 0;}
static int create(struct kernel_vfs_mount *mount,uint64_t parent,const char *name,
    uint32_t mode,struct kernel_vfs_file *file)
{
    if(parent!=1)return -KERNEL_ENOTDIR;
    if((mode&KERNEL_VFS_S_IFMT)&&(mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG)return -KERNEL_EPERM;
    struct kernel_mqueue *q=0;int made=0;
    int result=kernel_mqueue_create_name(kernel_task_current(),name,mode,0,1,&q,&made);
    if(result)return result;
    struct kernel_vfs_stat st;kernel_mqueue_queue_stat(q,&st);
    result=open_inode(mount,name,st.ino,st.mode,file);kernel_mqueue_put(q);return result;
}
static int unlink_file(struct kernel_vfs_mount *mount,uint64_t parent,const char *name)
{(void)mount;return parent==1?kernel_mqueue_remove_name(name,strlen(name)):-KERNEL_ENOTDIR;}
static int stat_inode(const struct kernel_vfs_file *file,struct kernel_vfs_stat *stat)
{
    struct kernel_mqueue *q=kernel_mqueue_fs_file_queue(file);
    if(q)kernel_mqueue_queue_stat(q,stat);
    else kernel_mqueue_root_stat(stat);
    stat->dev=0x19800202U;return 0;
}
static int statfs(struct kernel_vfs_mount *mount,struct kernel_vfs_statfs *stat)
{
    struct mq_mount *m=(void *)mount;
    *stat=(struct kernel_vfs_statfs){.type=0x19800202U,.block_size=BOAROS_PAGE_SIZE,
        .fsid=0x19800202U,.name_length=255,.flags=m->instance.read_only?1U:0U};return 0;
}
static int permission(struct kernel_vfs_node *node,const struct kernel_credentials *c,unsigned access)
{
    (void)c;if(!node->backend_data)return kernel_mqueue_root_permission(access);
    /* q metadata is shared across mount-local node caches. Permission always
     * consults that authoritative queue, including chmod from another mount. */
    return kernel_mqueue_queue_permission(node->backend_data,access);
}
static int set_mode(struct kernel_vfs_file *file,uint32_t mode)
{
    if(((struct kernel_vfs_instance *)file->mount->private_data)->read_only)return -KERNEL_EROFS;
    struct kernel_mqueue *q=kernel_mqueue_fs_file_queue(file);
    return q?kernel_mqueue_queue_set_mode(q,mode):kernel_mqueue_root_set_mode(mode);
}
static int set_owner(struct kernel_vfs_file *file,uint32_t uid,uint32_t gid)
{
    struct kernel_mqueue *q=kernel_mqueue_fs_file_queue(file);
    return q?kernel_mqueue_queue_set_owner(q,uid,gid):kernel_mqueue_root_set_owner(uid,gid);
}
static int set_times(struct kernel_vfs_file *file,const struct kernel_vfs_timespec times[2])
{
    if(((struct kernel_vfs_instance *)file->mount->private_data)->read_only)return -KERNEL_EROFS;
    struct kernel_mqueue *q=kernel_mqueue_fs_file_queue(file);
    return q?kernel_mqueue_queue_set_times(q,times):kernel_mqueue_root_set_times(times);
}
static int no_mkdir(struct kernel_vfs_mount *mount,uint64_t parent,const char *name,uint32_t mode)
{(void)mount;(void)parent;(void)name;(void)mode;return -KERNEL_EPERM;}
static int no_rmdir(struct kernel_vfs_mount *mount,uint64_t parent,const char *name)
{(void)mount;(void)parent;(void)name;return -KERNEL_ENOTDIR;}
static int no_truncate(struct kernel_vfs_node *node,uint64_t size,uint64_t *actual,int *changed)
{(void)node;(void)size;(void)actual;*changed=0;return -KERNEL_EINVAL;}
static void accessed(struct kernel_vfs_file *file){(void)file;}
static int modified(struct kernel_vfs_file *file,uint64_t offset,int append)
{(void)file;(void)offset;(void)append;return -KERNEL_EINVAL;}
static int no_writeback(struct kernel_vfs_instance *i){(void)i;return 0;}
static void release_unlinked(struct kernel_vfs_node *node){(void)node;}
static int dir_entry(struct kernel_vfs_file *file,uint64_t position,uint64_t *next,
    uint64_t *id,uint8_t *type,char *name,size_t size)
{
    const struct kernel_vfs_node *node=file->private_data;if(node->inode!=1)return -KERNEL_ENOTDIR;
    if(position<2){
        const char *s=position?"..":".";size_t n=strlen(s)+1;if(size<n)return -KERNEL_ERANGE;
        memcpy(name,s,n);*next=position+1;*id=1;*type=4;return 1;
    }
    uint64_t cursor=position-1;int result=kernel_mqueue_next(&cursor,id,name,size);
    if(result>0){*next=cursor+1;*type=8;}return result;
}
static int pread_inode(struct kernel_vfs_node *node,uint64_t offset,void *buffer,size_t size,size_t *read)
{
    if(!node->backend_data)return -KERNEL_EISDIR;
    return kernel_mqueue_queue_read(node->backend_data,offset,buffer,size,read);
}
static int unmount(struct kernel_vfs_mount *mount)
{
    struct mq_mount *m=(void *)mount;struct kernel_vfs_instance *i=&m->instance;
    if(mount->covered_path||mount->root_path||mount->child_mounts||i->external_files||
       !kernel_vfs_nodes_idle(i,0)||i->paths)return -KERNEL_EBUSY;
    kernel_heap_release(i->heap,m);return 0;
}
static void initialize_ops(void)
{
    volatile struct kernel_vfs_backend *o=&mq_ops;
    o->read_execution=KERNEL_VFS_EXECUTION_CALLER;o->file_metadata_execution=KERNEL_VFS_EXECUTION_CALLER;
    o->namespace_execution=KERNEL_VFS_EXECUTION_CALLER;
    o->error=error;o->root=root;o->lookup=lookup;o->open=open_inode;o->close_node=close_node;
    o->create=create;o->unlink=unlink_file;o->stat=stat_inode;o->statfs=statfs;o->permission=permission;
    o->set_mode=set_mode;o->set_owner=set_owner;o->set_times=set_times;
    o->mkdir=no_mkdir;o->rmdir=no_rmdir;o->truncate=no_truncate;
    o->accessed=accessed;o->modified=modified;o->writeback_allowed=no_writeback;o->flush=error;
    o->release_unlinked=release_unlinked;
    o->dir_entry=dir_entry;o->pread=pread_inode;o->unmount=unmount;
}
int kernel_mqueue_fs_create(struct kernel_heap *heap,uint64_t flags,struct kernel_vfs_mount **owner)
{
    if(!heap||!owner||*owner)return -KERNEL_EINVAL;
    int result=kernel_mqueue_init(heap);if(result)return result;
    struct mq_mount *m=0;
    enum kernel_heap_status s=kernel_heap_allocate_zeroed(heap,1,sizeof(*m),(void **)&m);
    if(s)return s==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    initialize_ops();m->flags=flags;m->instance.heap=heap;m->instance.ops=&mq_ops;
    m->instance.backend_data=m;m->instance.read_only=!!(flags&1U);
    kernel_rwlock_init(&m->instance.reconfiguration, 19, (uintptr_t)&m->instance);
    kernel_mutex_init(&m->instance.namespace_lock,20,(uintptr_t)&m->instance);
    kernel_raw_lock_init(&m->instance.error_lock,KERNEL_RAW_RANK_OBJECT);
    m->mount.private_data=&m->instance;m->mount.id=kernel_vfs_allocate_mount_id();
    m->mount.state=VFS_MOUNT_STATE_LIVE;m->mount.nosuid=!!(flags&2U);
    *owner=&m->mount;return 0;
}
