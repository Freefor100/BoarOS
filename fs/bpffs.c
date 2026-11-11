#include "bpf_fs.h"
#include "../kernel/bpf/private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/time.h>
#include <kernel/memory_object.h>
#include <kernel/tmpfs.h>
#include <string.h>

#define BPF_FS_MAGIC 0xcafe4a11U
struct bpf_inode {
    struct bpf_inode *next;
    struct kernel_vfs_stat stat;
    uint64_t parent;
    uint32_t references;
    uint8_t linked;
    char name[256];
    struct bpf_object *object;
    struct kernel_memory_object *memory;
};
struct bpf_fs {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_raw_lock lock;
    struct bpf_inode *entries;
    uint64_t next_inode,magic;
};
static struct kernel_vfs_backend bpf_ops;
static struct bpf_fs *fs(struct kernel_vfs_instance *i){return i->backend_data;}
static struct bpf_inode *inode(struct bpf_fs *f,uint64_t id)
{for(struct bpf_inode *n=f->entries;n;n=n->next)if(n->stat.ino==id)return n;return 0;}
static struct bpf_inode *child(struct bpf_fs *f,uint64_t parent,const char *name,size_t length)
{for(struct bpf_inode *n=f->entries;n;n=n->next)if(n->parent==parent && n->stat.ino!=1 && strlen(n->name)==length && !memcmp(n->name,name,length))return n;return 0;}
static int allocate(struct kernel_heap *h,size_t size,void **out)
{return kernel_heap_allocate_zeroed(h,1,size,out)?-KERNEL_ENOMEM:0;}
static void destroy_inode(struct bpf_fs *f,struct bpf_inode *n)
{if(n->object)bpf_object_put(n->object);if(n->memory)kernel_memory_object_release(&n->memory);kernel_heap_release(f->instance.heap,n);}
static struct kernel_vfs_timespec now(void)
{uint64_t n=kernel_time_realtime_ns();return (struct kernel_vfs_timespec){n/1000000000,n%1000000000};}
static int add(struct bpf_fs *f,uint64_t parent,const char *name,uint32_t mode,
    struct bpf_object *object,const void *data,size_t size)
{
    size_t length=strlen(name);if(length>255)return -KERNEL_ENAMETOOLONG;
    if(child(f,parent,name,length))return -KERNEL_EEXIST;
    if(f->next_inode==UINT64_MAX)return -KERNEL_EOVERFLOW;
    struct bpf_inode *n=0;int r=allocate(f->instance.heap,sizeof(*n),(void **)&n);if(r)return r;
    if(!object && (mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFREG) {
        enum kernel_memory_object_status status=kernel_memory_object_create(
            f->instance.heap,f->instance.heap->page_allocator,&n->memory);
        if(status)r=status==KERNEL_MEMORY_OBJECT_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
        /* The inode owns allocator pages, not image/heap fragments. Ordinary
         * reads and mmap borrow this same initialized source through the OFD. */
        for(size_t offset=0;!r && offset<size;) {
            uint64_t physical;int created;
            status=kernel_memory_object_get_page(n->memory,offset>>BOAROS_PAGE_SHIFT,&physical,&created);
            if(status){r=status==KERNEL_MEMORY_OBJECT_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;break;}
            void *page;
            if(!created || physical_page_resolve(f->instance.heap->page_allocator,physical,&page))__builtin_trap();
            size_t length=size-offset;if(length>BOAROS_PAGE_SIZE)length=BOAROS_PAGE_SIZE;
            memcpy(page,(const unsigned char *)data+offset,length);
            if(physical_page_release(f->instance.heap->page_allocator,physical))__builtin_trap();
            offset+=length;
        }
        if(r){destroy_inode(f,n);return r;}
    }
    struct kernel_vfs_timespec time=now();const struct kernel_credentials *c=kernel_credentials_current();
    n->stat=(struct kernel_vfs_stat){.dev=f->mount.id,.ino=f->next_inode++,
        .mode=mode,.uid=c->fsuid,.gid=c->fsgid,.nlink=(mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR?2:1,
        .size=size,.blksize=4096,.atime=time,.mtime=time,.ctime=time};
    memcpy(n->name,name,length+1);n->parent=parent;n->linked=1;n->object=object;
    if(object)bpf_object_get(object);
    struct bpf_inode *p=inode(f,parent);
    if(p && p!=n){if((mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)p->stat.nlink++;p->stat.mtime=p->stat.ctime=time;}
    n->next=f->entries;f->entries=n;return 0;
}
static int error(struct kernel_vfs_instance *i){(void)i;return 0;}
static int root(struct kernel_vfs_instance *i,uint64_t *id,uint32_t *mode)
{*id=1;*mode=inode(fs(i),1)->stat.mode;return 0;}
static int lookup(struct kernel_vfs_instance *i,uint64_t parent,const char *name,size_t size,uint64_t *id,uint32_t *mode)
{if(fs(i)->magic==BPF_FS_MAGIC)for(size_t j=0;j<size;j++)if(name[j]=='.')return -KERNEL_EPERM;
    struct bpf_inode *n=child(fs(i),parent,name,size);if(!n)return -KERNEL_ENOENT;*id=n->stat.ino;*mode=n->stat.mode;return 0;}
static int open_inode(struct kernel_vfs_mount *m,const char *path,uint64_t id,uint32_t mode,struct kernel_vfs_file *file)
{
    (void)path;(void)mode;struct kernel_vfs_instance *i=m->private_data;struct bpf_inode *n=inode(fs(i),id);
    if(!n)return -KERNEL_ENOENT;
    struct kernel_vfs_node *node=0;int r=allocate(i->heap,sizeof(*node),(void **)&node);if(r)return r;
    node->inode=id;node->backend_data=n;node->mode=n->stat.mode;node->uid=n->stat.uid;node->gid=n->stat.gid;
    node->size=n->stat.size;node->max_size=n->stat.size;node->memory=n->memory;
    if(n->memory)node->file_seals=KERNEL_SEAL_SEAL|KERNEL_SEAL_SHRINK|KERNEL_SEAL_GROW|KERNEL_SEAL_WRITE;
    if(n->object)node->generated_control=1;
    KERNEL_RAW_SCOPE(guard,&fs(i)->lock);n->references++;
    kernel_raw_lock_release(&guard);
    return kernel_vfs_publish_node(m,node,file,0);
}
static int close_node(struct kernel_vfs_node *node)
{
    struct bpf_fs *f=fs(node->instance);struct bpf_inode *n=node->backend_data;
    KERNEL_RAW_SCOPE(guard,&f->lock);
    if(!n->references)__builtin_trap();
    n->references--;int collect=!n->references&&!n->linked;
    kernel_raw_lock_release(&guard);if(collect)destroy_inode(f,n);return 0;
}
static int mutable(struct bpf_fs *f)
{return f->instance.read_only?-KERNEL_EROFS:f->magic!=BPF_FS_MAGIC?-KERNEL_EPERM:0;}
static int valid_name(const char *name)
{if(!*name)return 0;for(const char *p=name;*p;p++)if(*p=='.'||*p=='/')return 0;return 1;}
int kernel_bpffs_pin_inode(struct kernel_vfs_instance *i,uint64_t parent,const char *name,struct bpf_object *object,uint32_t umask)
{
    if(i->ops!=&bpf_ops || fs(i)->magic!=BPF_FS_MAGIC)return -KERNEL_EPERM;
    int r=mutable(fs(i));if(r)return r;
    if(!valid_name(name))return -KERNEL_EPERM;
    return add(fs(i),parent,name,KERNEL_VFS_S_IFREG|(0600&~umask),object,0,0);
}
static int mkdir_inode(struct kernel_vfs_mount *m,uint64_t parent,const char *name,uint32_t mode,uint32_t umask)
{
    struct bpf_fs *f=fs(m->private_data);int r=mutable(f);if(r)return r;
    if(!valid_name(name))return -KERNEL_EPERM;
    return add(f,parent,name,KERNEL_VFS_S_IFDIR|(mode&07777&~umask),0,0,0);
}
static int empty(struct bpf_fs *f,uint64_t id)
{for(struct bpf_inode *n=f->entries;n;n=n->next)if(n->parent==id && n->stat.ino!=id)return 0;return 1;}
static void remove_inode(struct bpf_fs *f,struct bpf_inode *n)
{
    struct bpf_inode **p=&f->entries;while(*p!=n)p=&(*p)->next;*p=n->next;
    struct bpf_inode *parent=inode(f,n->parent);
    if((n->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)parent->stat.nlink--;
    parent->stat.mtime=parent->stat.ctime=now();
    KERNEL_RAW_SCOPE(guard,&f->lock);
    n->linked=0;n->stat.nlink=0;int collect=!n->references;
    kernel_raw_lock_release(&guard);if(collect)destroy_inode(f,n);
}
static int remove_path(struct kernel_vfs_mount *m,uint64_t parent,const char *name,int directory)
{
    struct bpf_fs *f=fs(m->private_data);int r=mutable(f);if(r)return r;
    struct bpf_inode *n=child(f,parent,name,strlen(name));if(!n)return -KERNEL_ENOENT;
    int dir=(n->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR;
    if(directory!=dir)return directory?-KERNEL_ENOTDIR:-KERNEL_EISDIR;
    if(dir&&!empty(f,n->stat.ino))return -KERNEL_ENOTEMPTY;
    remove_inode(f,n);return 0;
}
static int unlink_inode(struct kernel_vfs_mount *m,uint64_t p,const char *n){return remove_path(m,p,n,0);}
static int rmdir_inode(struct kernel_vfs_mount *m,uint64_t p,const char *n){return remove_path(m,p,n,1);}
static int rename_inode(struct kernel_vfs_instance *i,uint64_t op,const char *old,uint64_t np,const char *name,unsigned flags,struct kernel_vfs_rename_result *out)
{
    struct bpf_fs *f=fs(i);int r=mutable(f);if(r)return r;
    if(flags&~1U)return -KERNEL_EINVAL;
    if(!valid_name(name))return -KERNEL_EPERM;
    struct bpf_inode *n=child(f,op,old,strlen(old)),*dest=child(f,np,name,strlen(name));
    if(!n)return -KERNEL_ENOENT;
    if(dest==n)return 0;
    if(dest&&(flags&1))return -KERNEL_EEXIST;
    int dir=(n->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR;
    if(dir)for(struct bpf_inode *p=inode(f,np);;p=inode(f,p->parent)){if(p==n)return -KERNEL_EINVAL;if(p->stat.ino==1)break;}
    if(dest){int dd=(dest->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR;
        if(dir!=dd)return dir?-KERNEL_ENOTDIR:-KERNEL_EISDIR;
        if(dd&&!empty(f,dest->stat.ino))return -KERNEL_ENOTEMPTY;
        out->replaced_inode=dest->stat.ino;out->replaced_last_link=1;remove_inode(f,dest);}
    if(dir&&op!=np){inode(f,op)->stat.nlink--;inode(f,np)->stat.nlink++;}
    n->parent=np;memcpy(n->name,name,strlen(name)+1);n->stat.ctime=now();out->changed=1;return 0;
}
static int stat_inode(const struct kernel_vfs_file *file,struct kernel_vfs_stat *s)
{struct kernel_vfs_node *n=file->private_data;*s=((struct bpf_inode *)n->backend_data)->stat;return 0;}
static int statfs_inode(struct kernel_vfs_mount *m,struct kernel_vfs_statfs *s)
{struct bpf_fs *f=fs(m->private_data);*s=(struct kernel_vfs_statfs){.type=f->magic,.block_size=4096,.name_length=255,.fsid=m->id,.flags=f->instance.read_only?1U:0U};return 0;}
static int set_mode(struct kernel_vfs_file *file,uint32_t mode)
{struct kernel_vfs_node *n=file->private_data;int r=mutable(fs(n->instance));if(r)return r;struct bpf_inode *e=n->backend_data;e->stat.mode=(e->stat.mode&KERNEL_VFS_S_IFMT)|(mode&07777);n->mode=e->stat.mode;return 0;}
static int set_owner(struct kernel_vfs_file *file,uint32_t uid,uint32_t gid)
{struct kernel_vfs_node *n=file->private_data;int r=mutable(fs(n->instance));if(r)return r;struct bpf_inode *e=n->backend_data;if(uid!=UINT32_MAX)n->uid=e->stat.uid=uid;if(gid!=UINT32_MAX)n->gid=e->stat.gid=gid;return 0;}
static int set_times(struct kernel_vfs_file *file,const struct kernel_vfs_timespec t[2])
{struct kernel_vfs_node *n=file->private_data;int r=mutable(fs(n->instance));if(r)return r;struct bpf_inode *e=n->backend_data;e->stat.atime=t[0];e->stat.mtime=t[1];e->stat.ctime=now();return 0;}
static void accessed(struct kernel_vfs_file *file){(void)file;}
static int modified(struct kernel_vfs_file *file,uint64_t offset,int append){(void)file;(void)offset;(void)append;return -KERNEL_EINVAL;}
static int no_truncate(struct kernel_vfs_node *n,uint64_t s,uint64_t *a,int *c){(void)n;(void)s;(void)a;*c=0;return -KERNEL_EINVAL;}
static void release_unlinked(struct kernel_vfs_node *n){(void)n;}
static int dir_entry(struct kernel_vfs_file *file,uint64_t position,uint64_t *next,uint64_t *id,uint8_t *type,char *name,size_t size)
{
    struct kernel_vfs_node *node=file->private_data;struct bpf_inode *d=node->backend_data;
    if((d->stat.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR)return -KERNEL_ENOTDIR;
    if(position<2){const char *s=position?"..":".";if(size<strlen(s)+1)return -KERNEL_ERANGE;strcpy(name,s);*id=position?d->parent:d->stat.ino;*next=position+1;*type=4;return 1;}
    struct bpf_inode *found=0;
    for(struct bpf_inode *n=fs(node->instance)->entries;n;n=n->next)
        if(n->parent==d->stat.ino && n!=d && n->stat.ino>=position && (!found||n->stat.ino<found->stat.ino))found=n;
    if(!found)return 0;
    if(size<=strlen(found->name))return -KERNEL_ERANGE;
    strcpy(name,found->name);*id=found->stat.ino;*next=*id+1;*type=(found->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR?4:8;return 1;
}
static int pread_inode(struct kernel_vfs_node *node,uint64_t offset,void *data,size_t size,size_t *read)
{
    struct bpf_inode *n=node->backend_data;*read=0;
    if((n->stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)return -KERNEL_EISDIR;
    if(n->object)return -KERNEL_EINVAL;
    while(*read<size && offset<n->stat.size) {
        uint64_t physical;
        if(kernel_memory_object_find_page(n->memory,offset>>BOAROS_PAGE_SHIFT,&physical))__builtin_trap();
        void *page;
        if(physical_page_resolve(node->instance->heap->page_allocator,physical,&page))__builtin_trap();
        size_t within=offset&BOAROS_PAGE_MASK,count=BOAROS_PAGE_SIZE-within;
        if(count>size-*read)count=size-*read;
        if(count>n->stat.size-offset)count=n->stat.size-offset;
        memcpy((unsigned char *)data+*read,(const unsigned char *)page+within,count);
        if(physical_page_release(node->instance->heap->page_allocator,physical))__builtin_trap();
        *read+=count;offset+=count;
    }
    return 0;
}
static int check_domain(const struct kernel_vfs_node *node,const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if(!node || !node->instance || node->instance->ops!=&bpf_ops ||
        node->instance->heap!=heap || !heap || heap->page_allocator!=allocator ||
        !node->backend_data || (node->mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG)return -KERNEL_EINVAL;
    const struct bpf_inode *n=node->backend_data;
    return n->stat.ino!=node->inode || n->memory!=node->memory ||
        kernel_memory_object_check_domain(n->memory,heap,allocator)?-KERNEL_EINVAL:0;
}
static int pin_control(struct kernel_vfs_node *n,int write,uint64_t offset,char *bytes,size_t size,size_t *count)
{(void)n;(void)write;(void)offset;(void)bytes;(void)size;*count=0;return -KERNEL_EINVAL;}
static int unmount(struct kernel_vfs_mount *m)
{
    struct bpf_fs *f=fs(m->private_data);struct kernel_vfs_instance *i=&f->instance;
    if(m->covered_path||m->root_path||m->child_mounts||i->external_files||!kernel_vfs_nodes_idle(i,0)||i->paths)return -KERNEL_EBUSY;
    while(f->entries){struct bpf_inode *n=f->entries;f->entries=n->next;destroy_inode(f,n);}
    kernel_heap_release(i->heap,f);return 0;
}
static struct kernel_vfs_backend bpf_ops={
    .read_execution=KERNEL_VFS_EXECUTION_CALLER,.file_operation_execution=KERNEL_VFS_EXECUTION_CALLER,.namespace_execution=KERNEL_VFS_EXECUTION_CALLER,
    .check_domain=check_domain,.control=pin_control,
    .error=error,.root=root,.lookup=lookup,.open=open_inode,.close_node=close_node,
    .mkdir=mkdir_inode,.unlink=unlink_inode,.rmdir=rmdir_inode,.rename=rename_inode,
    .stat=stat_inode,.statfs=statfs_inode,.set_mode=set_mode,.set_owner=set_owner,.set_times=set_times,
    .accessed=accessed,.modified=modified,.truncate=no_truncate,.writeback_allowed=error,.flush=error,
    .release_unlinked=release_unlinked,.dir_entry=dir_entry,.pread=pread_inode,.unmount=unmount,
};
int bpf_metadata_mount(struct kernel_heap *h,uint64_t flags,uint64_t magic,const struct bpf_fs_entry *entries,size_t count,struct kernel_vfs_mount **out)
{
    if(!h || !out || *out)return -KERNEL_EINVAL;
    struct bpf_fs *f=0;int r=allocate(h,sizeof(*f),(void **)&f);if(r)return r;
    f->magic=magic;f->next_inode=1;f->instance.heap=h;f->instance.ops=&bpf_ops;f->instance.backend_data=f;f->instance.read_only=!!(flags&1);
    kernel_rwlock_init(&f->instance.reconfiguration,19,(uintptr_t)&f->instance);
    kernel_mutex_init(&f->instance.namespace_lock,20,(uintptr_t)&f->instance);
    kernel_raw_lock_init(&f->instance.error_lock,KERNEL_RAW_RANK_OBJECT);kernel_raw_lock_init(&f->lock,KERNEL_RAW_RANK_OBJECT);
    f->mount.private_data=&f->instance;f->mount.id=kernel_vfs_allocate_mount_id();f->mount.state=VFS_MOUNT_STATE_LIVE;
    r=add(f,1,"",KERNEL_VFS_S_IFDIR|(magic==BPF_FS_MAGIC?01777:0555),0,0,0);
    for(size_t j=0;!r&&j<count;j++)r=add(f,entries[j].parent,entries[j].name,entries[j].mode,0,entries[j].data,entries[j].size);
    if(r){unmount(&f->mount);return r;}
    kernel_vfs_mount_initialize(&f->mount);*out=&f->mount;return 0;
}
int kernel_bpffs_mount(struct kernel_heap *h,uint64_t flags,struct kernel_vfs_mount **out)
{return bpf_metadata_mount(h,flags,BPF_FS_MAGIC,0,0,out);}
int kernel_vfs_bpf_pin_at(struct kernel_vfs_path *,struct kernel_vfs_path *,const char *,struct bpf_object *,uint32_t);
int bpf_pin_object(const struct kernel_fs_context *context,const char *path,struct bpf_object *object)
{KERNEL_FS_PATHS(paths,context);return kernel_vfs_bpf_pin_at(paths.cwd,paths.root,path,object,paths.umask);}
int bpf_get_pinned(const struct kernel_fs_context *context,const char *name,uint32_t flags,struct bpf_object **out)
{
    if(flags&~(8U|16U) || (flags&24)==24)return -KERNEL_EINVAL;
    KERNEL_FS_PATHS(paths,context);struct kernel_vfs_path *path=0;
    int r=kernel_vfs_path_resolve(paths.cwd,paths.root,name,1,&path);if(r)return r;
    unsigned access=(flags&8)?KERNEL_VFS_MAY_READ:(flags&16)?KERNEL_VFS_MAY_WRITE:KERNEL_VFS_MAY_READ|KERNEL_VFS_MAY_WRITE;
    r=kernel_vfs_path_permission(path,access);
    struct kernel_vfs_instance *i=kernel_vfs_path_mount(path)->private_data;
    if(!r && (i->ops!=&bpf_ops || fs(i)->magic!=BPF_FS_MAGIC))r=-KERNEL_EINVAL;
    if(!r){struct bpf_inode *n=((struct kernel_vfs_node *)path->file.private_data)->backend_data;
        if(!n->object)r=-KERNEL_EINVAL;else{bpf_object_get(n->object);*out=n->object;}}
    if(kernel_vfs_path_release(&path))__builtin_trap();
    return r;
}

const char *kernel_bpf_filesystem_type(const struct kernel_vfs_mount *mount)
{
    const struct kernel_vfs_instance *i=mount?mount->private_data:0;
    if(!i || i->ops!=&bpf_ops)return 0;
    const struct bpf_fs *f=i->backend_data;
    return f->magic==BPF_FS_MAGIC?"bpf":f->magic==0x74726163?"tracefs":"sysfs";
}
