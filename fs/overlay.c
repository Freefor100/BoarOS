#include "vfs_objects.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <string.h>

#define OVERLAY_MAGIC UINT64_C(0x794c7630)
#define UPPER_INODE (UINT64_C(1) << 32)
#define LOWER_COOKIE (UINT64_C(1) << 62)
#define OPAQUE_XATTR "trusted.overlay.opaque"

struct overlay_work {
    struct overlay_work *next;
    bool directory;
    char name[25];
};
struct overlay_mount {
    struct kernel_vfs_instance instance;
    struct kernel_vfs_mount view;
    struct kernel_vfs_path *lower, *upper, *work;
    const struct kernel_credentials *creator;
    struct overlay_mount *next;
    struct overlay_work *cleanup;
    uint64_t temporary;
};
struct overlay_node {
    struct kernel_vfs_node vfs;
    struct kernel_vfs_path *lower, *upper;
    struct kernel_vfs_node *parent;
    uint32_t generation;
    char name[];
};
static struct overlay_mount *active_overlays;

static struct overlay_mount *volume(const struct kernel_vfs_instance *i)
{ return i->backend_data; }
static struct overlay_node *entry(struct kernel_vfs_node *n)
{ return (struct overlay_node *)n; }
static struct kernel_vfs_path *upper(const struct overlay_node *e)
{ return __atomic_load_n(&e->upper,__ATOMIC_ACQUIRE); }
static struct kernel_vfs_path *real(const struct overlay_node *e)
{
    struct kernel_vfs_path *p=upper(e);
    return p ? p : e->lower;
}
static const struct kernel_vfs_file *data_file(struct kernel_vfs_node *n)
{ return &real(entry(n))->file; }
static struct kernel_vfs_path *event_backing(struct kernel_vfs_node *n)
{ return real(entry(n)); }
static void put_path(struct kernel_vfs_path **p)
{ if (*p && kernel_vfs_path_release(p)) __builtin_trap(); }
static void put_node(struct kernel_vfs_node **n)
{ if (*n && kernel_vfs_node_release(n)) __builtin_trap(); }
static void restore_credentials(const struct kernel_credentials **previous)
{ (void)kernel_credentials_override(*previous); }
#define LAYER_CREDS(m) \
    const struct kernel_credentials *previous __attribute__((cleanup(restore_credentials))) = \
        kernel_credentials_override((m)->creator)
#define PATH_OWNER(n) struct kernel_vfs_path *n __attribute__((cleanup(put_path))) = NULL
#define NODE_OWNER(n) struct kernel_vfs_node *n __attribute__((cleanup(put_node)))

static int is_dir(const struct kernel_vfs_path *p)
{ return p && (p->file.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR; }

static int lookup(struct kernel_vfs_path *dir,const char *name,size_t length,
    struct kernel_vfs_path **out)
{
    if (!is_dir(dir)) return 0;
    int error=kernel_vfs_path_lookup_layer(dir,name,length,out);
    return error==-KERNEL_ENOENT ? 0 : error;
}
static int is_whiteout(struct kernel_vfs_path *p)
{
    if (!p || (p->file.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFCHR) return 0;
    struct kernel_vfs_stat st;
    int error=kernel_vfs_path_stat(p,&st);
    return error ? error : st.rdev==0;
}
static int is_opaque(struct kernel_vfs_path *p)
{
    char value;
    int result=kernel_vfs_path_xattr(p,KERNEL_XATTR_GET,OPAQUE_XATTR,&value,1,0);
    if (result==-KERNEL_ENODATA) return 0;
    return result<0 ? result : result==1 && value=='y';
}
static int lookup_layers(struct overlay_node *parent,const char *name,size_t length,
    struct kernel_vfs_path **low,struct kernel_vfs_path **up)
{
    int error=lookup(upper(parent),name,length,up);
    if (error) return error;
    int hidden=is_whiteout(*up);
    if (hidden) return hidden<0 ? hidden : -KERNEL_ENOENT;
    error=lookup(parent->lower,name,length,low);
    if (error) return error;
    if (!*low && !*up) return -KERNEL_ENOENT;
    if (is_dir(*up) && is_dir(*low)) {
        int result=is_opaque(*up);
        if (result<0) return result;
        if (result) put_path(low);
    }
    return 0;
}
static uint64_t inode_number(const struct kernel_vfs_path *p,bool up)
{ return kernel_vfs_path_inode(p) | (up ? UPPER_INODE : 0); }
static int close_node(struct kernel_vfs_node *node)
{
    struct overlay_node *e=entry(node);
    put_path(&e->upper);
    put_path(&e->lower);
    put_node(&e->parent);
    return 0;
}

static int publish(struct kernel_vfs_mount *view,struct kernel_vfs_node *parent,
    const char *name,size_t length,struct kernel_vfs_path **low,struct kernel_vfs_path **up,
    struct kernel_vfs_file *file)
{
    struct kernel_vfs_instance *i=view->private_data;
    struct kernel_vfs_path *path=*up ? *up : *low;
    struct kernel_vfs_node *backing=path->file.private_data;
    unsigned type=backing->mode&KERNEL_VFS_S_IFMT;
    if (type!=KERNEL_VFS_S_IFDIR && type!=KERNEL_VFS_S_IFREG && type!=KERNEL_VFS_S_IFLNK)
        return -KERNEL_EOPNOTSUPP;
    struct overlay_node *e;
    if (kernel_heap_allocate_zeroed(i->heap,1,sizeof(*e)+length+1,(void **)&e))
        return -KERNEL_ENOMEM;
    if (parent && kernel_vfs_node_acquire(parent)) {
        kernel_heap_release(i->heap,e);
        return -KERNEL_EOVERFLOW;
    }
    memcpy(e->name,name,length);
    e->parent=parent;
    e->generation=backing->instance->ops->export->generation(backing);
    e->vfs.inode=parent ? inode_number(path,*up!=NULL) : UINT64_MAX;
    e->vfs.mode=backing->mode;
    e->vfs.uid=backing->uid;
    e->vfs.gid=backing->gid;
    e->vfs.size=kernel_vfs_node_size(backing);
    e->vfs.max_size=backing->max_size;
    e->vfs.attributes=kernel_vfs_node_attributes(backing);
    e->lower=*low; e->upper=*up;
    *low=*up=NULL;
    return kernel_vfs_publish_node(view,&e->vfs,file,false);
}

static int lookup_open(struct kernel_vfs_node *parent,const char *name,size_t length,
    struct kernel_vfs_file *file)
{
    LAYER_CREDS(volume(parent->instance));
    PATH_OWNER(low); PATH_OWNER(up);
    int error=lookup_layers(entry(parent),name,length,&low,&up);
    return error ? error : publish(parent->mount,parent,name,length,&low,&up,file);
}
static int root(struct kernel_vfs_instance *i,uint64_t *inode,uint32_t *mode)
{
    struct overlay_mount *m=volume(i);
    *inode=UINT64_MAX; *mode=kernel_vfs_path_mode(m->upper ? m->upper : m->lower);
    return 0;
}
static int open_root(struct kernel_vfs_mount *view,const char *path,uint64_t inode,
    uint32_t mode,struct kernel_vfs_file *file)
{
    (void)path; (void)mode;
    if (inode!=UINT64_MAX) return -KERNEL_ENOENT;
    struct overlay_mount *m=volume(view->private_data);
    PATH_OWNER(low); PATH_OWNER(up);
    int error=kernel_vfs_path_acquire(m->lower);
    if (error) return error;
    low=m->lower;
    if (m->upper) {
        error=kernel_vfs_path_acquire(m->upper);
        if (error) return error;
        up=m->upper;
        LAYER_CREDS(m);
        int result=is_opaque(up);
        if (result<0) return result;
        if (result) put_path(&low);
    }
    return publish(view,NULL,"",0,&low,&up,file);
}
static int stat_inode(const struct kernel_vfs_file *f,struct kernel_vfs_stat *st)
{
    struct kernel_vfs_node *n=f->private_data;
    int error=kernel_vfs_fstat(data_file(n),st);
    if (!error) { st->dev=n->instance->device_id; st->ino=n->inode; }
    return error;
}
static int statfs(struct kernel_vfs_mount *view,struct kernel_vfs_statfs *st)
{
    struct overlay_mount *m=volume(view->private_data);
    int error=kernel_vfs_mount_statfs((m->upper ? m->upper : m->lower)->file.mount,st);
    if (!error) st->type=OVERLAY_MAGIC;
    return error;
}
static int readlink(struct kernel_vfs_node *n,char *buffer,size_t size,size_t *count)
{ return kernel_vfs_path_readlink(real(entry(n)),buffer,size,count); }
static int permission(struct kernel_vfs_node *n,const struct kernel_credentials *actor,unsigned access)
{
    struct kernel_vfs_node *backing=data_file(n)->private_data;
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(backing,&io,0);
    return backing->instance->ops->acl_permission(backing,actor,access);
}
static void accessed(struct kernel_vfs_file *f)
{
    struct kernel_vfs_file backing=*data_file(f->private_data);
    kernel_vfs_file_accessed(&backing,0);
}
static int mount_error(struct kernel_vfs_instance *i)
{ return i->quiescing ? -KERNEL_EIO : 0; }
static void temp_name(struct overlay_mount *m,char name[25])
{
    static const char hex[]="0123456789abcdef";
    uint64_t number=++m->temporary;
    memcpy(name,".boaros-",8);
    for (unsigned i=0;i<16;i++) name[8+i]=hex[(number>>(60U-4U*i))&15U];
    name[24]=0;
}

static struct overlay_work *prepare_work(struct overlay_mount *m)
{
    struct overlay_work *work;
    if (kernel_heap_allocate_zeroed(m->instance.heap,1,sizeof(*work),(void **)&work))
        return NULL;
    temp_name(m,work->name);
    return work;
}

static int remove_work(struct overlay_mount *,const char *,bool);
static void finish_work(struct overlay_mount *m,struct overlay_work *work,bool remove,bool directory)
{
    int error=remove ? remove_work(m,work->name,directory) : 0;
    if (error && error!=-KERNEL_ENOENT) {
        work->directory=directory;
        work->next=m->cleanup;
        m->cleanup=work;
    } else kernel_heap_release(m->instance.heap,work);
}

static int copy_xattrs(struct overlay_mount *m,struct kernel_vfs_path *from,
    struct kernel_vfs_path *to)
{
    int size=kernel_vfs_path_xattr(from,KERNEL_XATTR_LIST,NULL,NULL,0,0);
    if (size<=0) return size;
    char *names;
    if (kernel_heap_allocate(m->instance.heap,(size_t)size,(void **)&names)) return -KERNEL_ENOMEM;
    int error=kernel_vfs_path_xattr(from,KERNEL_XATTR_LIST,NULL,names,(size_t)size,0);
    if (error<0) goto done;
    char *end=names+error;
    error=0;
    for (const char *name=names;name<end;name+=strlen(name)+1) {
        if (!strncmp(name,"trusted.overlay.",16)) continue;
        int length=kernel_vfs_path_xattr(from,KERNEL_XATTR_GET,name,NULL,0,0);
        if (length<0) { error=length; break; }
        void *value=NULL;
        if (length && kernel_heap_allocate(m->instance.heap,(size_t)length,&value)) {
            error=-KERNEL_ENOMEM;
            break;
        }
        int n=kernel_vfs_path_xattr(from,KERNEL_XATTR_GET,name,value,(size_t)length,0);
        error=n<0 ? n : kernel_vfs_path_xattr(to,KERNEL_XATTR_SET,name,value,(size_t)n,0);
        if (value) kernel_heap_release(m->instance.heap,value);
        if (error) break;
    }
done:
    kernel_heap_release(m->instance.heap,names);
    return error;
}

static int copy_data(struct overlay_mount *m,struct kernel_vfs_path *from,
    struct kernel_vfs_file *to,uint64_t size)
{
    void *buffer;
    if (kernel_heap_allocate(m->instance.heap,BOAROS_PAGE_SIZE,&buffer)) return -KERNEL_ENOMEM;
    struct kernel_vfs_file input=from->file;
    int error=0;
    for (uint64_t pos=0;pos<size;) {
        size_t length=size-pos<BOAROS_PAGE_SIZE ? (size_t)(size-pos) : BOAROS_PAGE_SIZE;
        size_t count, written;
        error=kernel_vfs_pread(&input,pos,buffer,length,&count,NULL);
        if (error) break;
        if (!count) { error=-KERNEL_EIO; break; }
        error=kernel_vfs_pwrite(to,pos,buffer,count,&written);
        if (!error && written!=count) error=-KERNEL_ENOSPC;
        if (error) break;
        pos+=count;
    }
    kernel_heap_release(m->instance.heap,buffer);
    return error;
}

/* The overlay namespace lock covers construction and publication. The old
 * layer path stays owned by the inode after the upper path becomes visible. */
static int copy_one(struct overlay_mount *m,struct overlay_node *e)
{
    struct kernel_vfs_path *parent=upper(entry(e->parent));
    struct kernel_vfs_path *original=e->lower;
    struct kernel_vfs_stat st;
    int error=kernel_vfs_path_stat(original,&st);
    if (error) return error;
    bool directory=is_dir(original);
    if (!directory && ((st.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG || st.nlink!=1))
        return -KERNEL_EOPNOTSUPP;

    struct overlay_work *work=prepare_work(m);
    if (!work) return -KERNEL_ENOMEM;
    const char *name=work->name;
    struct kernel_vfs_file file={0};
    if (directory) error=kernel_vfs_mkdir_at(m->work,m->work,name,0700,0);
    else error=kernel_vfs_create_at(m->work,m->work,name,0600,&file,0);
    if (error) { finish_work(m,work,false,directory); return error; }

    PATH_OWNER(candidate);
    error=lookup(m->work,name,strlen(name),&candidate);
    if (!error && !candidate) error=-KERNEL_EIO;
    if (!error && !directory) {
        file.notify_suppressed=1;
        error=copy_data(m,original,&file,st.size);
    }
    if (!error) error=kernel_vfs_path_set_owner(candidate,st.uid,st.gid);
    if (!error) error=kernel_vfs_path_set_mode(candidate,st.mode&07777U);
    if (!error) error=copy_xattrs(m,original,candidate);
    if (!error) {
        struct kernel_vfs_timespec times[2]={st.atime,st.mtime};
        error=kernel_vfs_path_set_times(candidate,times);
    }
    if (file.private_data) {
        uint64_t observed=0;
        if (!error) error=kernel_vfs_sync(&file,0,&observed);
        if (kernel_vfs_close(&file) && !error) error=-KERNEL_EIO;
    }
    if (!error) error=kernel_vfs_rename_at(m->work,parent,m->work,name,e->name,1);
    finish_work(m,work,error!=0,directory);
    if (error) return error;
    __atomic_store_n(&e->upper,candidate,__ATOMIC_RELEASE);
    candidate=NULL;
    return 0;
}

static int ensure_upper(struct overlay_node *e)
{
    if (upper(e)) return 0;
    struct overlay_mount *m=volume(e->vfs.instance);
    if (!m->upper) return -KERNEL_EROFS;
    KERNEL_LOCK_SCOPE(namespace);
    kernel_vfs_namespace_lock(&m->view,&namespace);
    if (e->vfs.unlinked) return -KERNEL_ENOENT;
    LAYER_CREDS(m);
    struct overlay_node *a=e;
    size_t depth=0;
    while (!upper(a)) {
        if (!a->parent) return -KERNEL_EROFS;
        depth++;
        a=entry(a->parent);
    }
    if (!depth) return 0;
    if (depth==1) return copy_one(m,e);
    struct overlay_node **missing;
    if (kernel_heap_allocate(m->instance.heap,depth*sizeof(*missing),(void **)&missing))
        return -KERNEL_ENOMEM;
    for (size_t j=0;j<depth;j++) {
        missing[j]=e;
        e=entry(e->parent);
    }
    int error=0;
    while (depth && !error) error=copy_one(m,missing[--depth]);
    kernel_heap_release(m->instance.heap,missing);
    return error;
}
static int copy_up(struct kernel_vfs_file *f)
{ return ensure_upper(entry(f->private_data)); }

static int modified(struct kernel_vfs_file *f,uint64_t offset,int append)
{
    if (kernel_vfs_mount_is_readonly(f->mount)) return -KERNEL_EROFS;
    int error=copy_up(f);
    if (error) return error;
    struct kernel_vfs_file file=*data_file(f->private_data);
    return kernel_vfs_file_modified(&file,offset,append);
}

static int prepare_access(struct kernel_vfs_path *path,unsigned access)
{
    struct overlay_node *e=path->file.private_data;
    if ((access&KERNEL_VFS_MAY_WRITE) && kernel_vfs_mount_is_readonly(path->file.mount))
        return -KERNEL_EROFS;
    LAYER_CREDS(volume(e->vfs.instance));
    unsigned real_access=access;
    if ((access&KERNEL_VFS_MAY_WRITE) && !upper(e))
        real_access=(access&~KERNEL_VFS_MAY_WRITE)|KERNEL_VFS_MAY_READ;
    int error=kernel_vfs_path_permission(real(e),real_access);
    if (!error && (access&KERNEL_VFS_MAY_WRITE)) {
        error=ensure_upper(e);
        if (!error) error=kernel_vfs_path_permission(real(e),access);
    }
    return error;
}

static void refresh_ids(struct overlay_node *e)
{
    const struct kernel_vfs_node *backing=upper(e)->file.private_data;
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(&e->vfs,&io,1);
    e->vfs.mode=backing->mode;
    e->vfs.uid=backing->uid;
    e->vfs.gid=backing->gid;
}
static int set_mode(struct kernel_vfs_file *f,uint32_t mode)
{
    struct overlay_node *e=f->private_data;
    const struct kernel_credentials *actor=kernel_credentials_current();
    if (actor->fsuid!=e->vfs.uid && !kernel_credentials_capable(actor,KERNEL_CAP_FOWNER))
        return -KERNEL_EPERM;
    if (!kernel_credentials_in_group(actor,e->vfs.gid) &&
        !kernel_credentials_capable(actor,KERNEL_CAP_FSETID)) mode&=~02000U;
    int error=ensure_upper(e);
    if (error) return error;
    LAYER_CREDS(volume(e->vfs.instance));
    error=kernel_vfs_path_set_mode(upper(e),mode);
    if (!error) refresh_ids(e);
    return error;
}
static int set_owner(struct kernel_vfs_file *f,uint32_t uid,uint32_t gid)
{
    struct overlay_node *e=f->private_data;
    const struct kernel_credentials *actor=kernel_credentials_current();
    if (!kernel_credentials_capable(actor,KERNEL_CAP_CHOWN) &&
        (actor->fsuid!=e->vfs.uid || (uid!=UINT32_MAX && uid!=e->vfs.uid) ||
         (gid!=UINT32_MAX && gid!=e->vfs.gid && !kernel_credentials_in_group(actor,gid))))
        return -KERNEL_EPERM;
    int error=ensure_upper(e);
    if (error) return error;
    LAYER_CREDS(volume(e->vfs.instance));
    error=kernel_vfs_path_set_owner(upper(e),uid,gid);
    if (!error) refresh_ids(e);
    return error;
}
static int set_times(struct kernel_vfs_file *f,const struct kernel_vfs_timespec times[2])
{
    struct overlay_node *e=f->private_data;
    int error=kernel_vfs_inode_times_allowed(&e->vfs,times);
    if (error) return error;
    error=ensure_upper(e);
    if (error) return error;
    LAYER_CREDS(volume(e->vfs.instance));
    return kernel_vfs_path_set_times(upper(e),times);
}
static int xattr(struct kernel_vfs_node *n,enum kernel_xattr_operation op,const char *name,
    void *buffer,size_t size,uint32_t flags)
{
    if (op!=KERNEL_XATTR_LIST && !strncmp(name,"trusted.overlay.",16)) return -KERNEL_EOPNOTSUPP;
    const struct kernel_vfs_file *file=data_file(n);
    struct kernel_vfs_node *backing=file->private_data;
    bool write=op==KERNEL_XATTR_SET || op==KERNEL_XATTR_REMOVE;
    if (write && (!upper(entry(n)) || kernel_vfs_mount_is_readonly(file->mount))) return -KERNEL_EROFS;
    KERNEL_LOCK_SCOPE(mutation);
    if (write) kernel_vfs_mutation_lock(file->mount,&mutation);
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(backing,&io,write);
    int result=backing->instance->ops->xattr(backing,op,name,buffer,size,
        op==KERNEL_XATTR_LIST ? flags&~KERNEL_XATTR_LIST_TRUSTED : flags);
    if (!result && write) n->mode=backing->mode;
    return result;
}

static uint64_t directory_field(const unsigned char *record,size_t offset)
{
    uint64_t value;
    memcpy(&value,record+offset,sizeof(value));
    return value;
}

static int read_directory(struct kernel_vfs_file *f,struct kernel_vfs_dir_batch *output)
{
    struct overlay_node *e=f->private_data;
    LAYER_CREDS(volume(e->vfs.instance));
    struct kernel_vfs_path *up=upper(e);
    bool lower_phase=output->position>=LOWER_COOKIE || !up;
    uint64_t position=output->position&(LOWER_COOKIE-1);
    unsigned char data[512];
    for (;;) {
        struct kernel_vfs_path *layer=lower_phase ? e->lower : up;
        if (!is_dir(layer)) return 0;
        struct kernel_vfs_dir_batch scan={.data=data,.capacity=sizeof(data),.position=position};
        int result=kernel_vfs_read_directory(&layer->file,&scan,NULL);
        if (result<0) return result;
        for (size_t offset=0;offset<scan.used;) {
            const unsigned char *record=data+offset;
            uint16_t length;
            memcpy(&length,record+16,sizeof(length));
            const char *name=(const char *)record+19;
            offset+=length;
            position=directory_field(record,8);
            bool dot=!strcmp(name,".");
            bool dotdot=!strcmp(name,"..");
            uint64_t inode=directory_field(record,0);
            if (dot || dotdot) {
                if (lower_phase && up) continue;
                inode=dotdot && e->parent ? e->parent->inode : e->vfs.inode;
            } else if (lower_phase && up) {
                PATH_OWNER(shadow);
                int error=lookup(up,name,strlen(name),&shadow);
                if (error) return error;
                if (shadow) continue;
            } else if (!lower_phase && record[18]==2) { /* DT_CHR */
                PATH_OWNER(path);
                int error=lookup(up,name,strlen(name),&path);
                if (error) return error;
                int hidden=is_whiteout(path);
                if (hidden<0) return hidden;
                if (hidden) continue;
            }
            if (!lower_phase && !dot && !dotdot) inode|=UPPER_INODE;
            uint64_t cookie=position|(lower_phase ? LOWER_COOKIE : 0);
            if (kernel_vfs_dir_emit(output,cookie,inode,record[18],name,strlen(name))!=1) return 1;
        }
        position=scan.position;
        if (result) continue;
        if (lower_phase) {
            output->position=position|LOWER_COOKIE;
            return 0;
        }
        lower_phase=true;
        position=0;
        output->position=LOWER_COOKIE;
    }
}

/* Check ordinary or merged directories. A directory being removed can have
 * only whiteout entries on disk while still being empty in the overlay. */
static int directory_empty(struct kernel_vfs_file *file,bool merged)
{
    unsigned char bytes[512];
    struct kernel_vfs_dir_batch batch={.data=bytes,.capacity=sizeof(bytes)};
    for (;;) {
        batch.used=0;
        int result=merged ? read_directory(file,&batch) : kernel_vfs_read_directory(file,&batch,NULL);
        if (result<0) return result;
        for (size_t pos=0;pos<batch.used;) {
            const char *name=(const char *)bytes+pos+19;
            uint16_t length;
            memcpy(&length,bytes+pos+16,sizeof(length));
            if (strcmp(name,".") && strcmp(name,"..")) return 0;
            pos+=length;
        }
        if (!result) return 1;
    }
}

static int remove_work(struct overlay_mount *m,const char *name,bool directory)
{
    if (directory) {
        PATH_OWNER(path);
        int error=lookup(m->work,name,strlen(name),&path);
        if (error || !path) return error ? error : -KERNEL_ENOENT;
        /* Only a merged-empty directory reaches this cleanup after exchange. */
        unsigned char bytes[512];
        for (;;) {
            struct kernel_vfs_dir_batch batch={.data=bytes,.capacity=sizeof(bytes)};
            int result=kernel_vfs_read_directory(&path->file,&batch,NULL);
            if (result<0) return result;
            bool removed=false;
            for (size_t pos=0;pos<batch.used;) {
                const char *child=(const char *)bytes+pos+19;
                uint16_t length;
                memcpy(&length,bytes+pos+16,sizeof(length));
                pos+=length;
                if (!strcmp(child,".") || !strcmp(child,"..")) continue;
                PATH_OWNER(marker);
                error=lookup(path,child,strlen(child),&marker);
                if (error) return error;
                int hidden=is_whiteout(marker);
                if (hidden<0) return hidden;
                if (!hidden) return -KERNEL_ENOTEMPTY;
                error=kernel_vfs_unlink_at(path,path,child,0);
                if (error) return error;
                removed=true;
            }
            if (!removed) break;
        }
    }
    return kernel_vfs_unlink_at(m->work,m->work,name,directory);
}

static void filesystem_id(struct kernel_vfs_instance *, unsigned char[16]);

static int create_object(struct kernel_vfs_mount *view,uint64_t parent_inode,
    const char *name,uint32_t mode,const char *link,struct kernel_vfs_file *output,uint32_t umask,
    struct kernel_vfs_handle *handle)
{
    uint32_t type=mode&KERNEL_VFS_S_IFMT;
    if (type!=KERNEL_VFS_S_IFREG && type!=KERNEL_VFS_S_IFDIR && type!=KERNEL_VFS_S_IFLNK)
        return -KERNEL_EOPNOTSUPP;
    struct kernel_vfs_instance *i=view->private_data;
    struct overlay_mount *m=volume(i);
    NODE_OWNER(parent)=kernel_vfs_node_pin(i,parent_inode);
    if (!parent) return -KERNEL_ENOENT;
    struct overlay_node *dir=entry(parent);
    int error=ensure_upper(dir);
    if (error) return error;
    struct kernel_vfs_path *destination=upper(dir);
    uint32_t uid,gid;
    kernel_vfs_creation_identity(parent->mode,parent->gid,&mode,&uid,&gid);
    LAYER_CREDS(m);
    PATH_OWNER(old); PATH_OWNER(candidate); PATH_OWNER(low);
    error=lookup(destination,name,strlen(name),&old);
    if (error) return error;
    int hidden=is_whiteout(old);
    if (hidden<0) return hidden;
    if (old && !hidden) return -KERNEL_EEXIST;

    struct overlay_work *pending=prepare_work(m);
    if (!pending) return -KERNEL_ENOMEM;
    const char *work_name=pending->name;
    bool directory=(mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR;
    struct kernel_vfs_file file={0};
    if (directory) error=kernel_vfs_mkdir_at(m->work,m->work,work_name,mode&07777U,umask);
    else if (link) error=kernel_vfs_symlink_at(m->work,m->work,link,work_name);
    else error=kernel_vfs_create_at(m->work,m->work,work_name,mode&07777U,&file,umask);
    if (error) { finish_work(m,pending,false,directory); return error; }
    if (file.private_data && kernel_vfs_close(&file)) error=-KERNEL_EIO;
    if (!error) error=lookup(m->work,work_name,strlen(work_name),&candidate);
    if (!error && !candidate) error=-KERNEL_EIO;
    if (!error) error=kernel_vfs_path_set_owner(candidate,uid,gid);
    if (!error && !link) error=kernel_vfs_path_set_mode(candidate,(mode&07777U)&~umask);
    if (!error && directory && hidden) {
        char opaque='y';
        error=kernel_vfs_path_xattr(candidate,KERNEL_XATTR_SET,OPAQUE_XATTR,&opaque,1,0);
    }
    if (!error && directory && hidden)
        error=kernel_vfs_exchange_layer_entries(m->work,destination,work_name,name);
    else if (!error)
        error=kernel_vfs_rename_at(m->work,destination,m->work,work_name,name,hidden ? 0 : 1);
    finish_work(m,pending,error!=0 || (directory && hidden),error!=0 && directory);
    if (error) return error;
    if (output) return publish(view,parent,name,strlen(name),&low,&candidate,output);
    if (handle) {
        struct kernel_vfs_node *backing = candidate->file.private_data;
        *handle = (struct kernel_vfs_handle){.inode = inode_number(candidate, true),
            .generation = backing->instance->ops->export->generation(backing)};
        filesystem_id(i, handle->filesystem);
    }
    return 0;
}
static int create(struct kernel_vfs_mount *view,struct kernel_vfs_node *parent,const char *name,
    uint32_t mode,struct kernel_vfs_file *file,uint32_t umask)
{ return create_object(view,parent->inode,name,mode,NULL,file,umask,NULL); }
static int mkdir(struct kernel_vfs_mount *view,uint64_t parent,const char *name,uint32_t mode,uint32_t mask)
{ return create_object(view,parent,name,KERNEL_VFS_S_IFDIR|(mode&07777U),NULL,NULL,mask,NULL); }
static int symlink(struct kernel_vfs_instance *i,uint64_t parent,const char *target,const char *name)
{ return create_object(&i->backend_mount,parent,name,KERNEL_VFS_S_IFLNK|0777U,target,NULL,0,NULL); }
static int create_with_handle(struct kernel_vfs_mount *view, uint64_t parent, const char *name,
    uint32_t mode, uint32_t device, const char *target, uint32_t umask,
    struct kernel_vfs_handle *handle)
{
    (void)device;
    return create_object(view,parent,name,mode,target,NULL,umask,handle);
}

static int remove_resolved(struct kernel_vfs_mount *view,uint64_t number,const char *name,
    struct kernel_vfs_node *node,bool directory,bool exclusive)
{
    (void)exclusive;
    struct overlay_mount *m=volume(view->private_data);
    struct overlay_node *e=entry(node);
    if (directory != (bool)is_dir(real(e))) return directory ? -KERNEL_ENOTDIR : -KERNEL_EISDIR;
    bool last_link=directory;
    if (directory) {
        struct kernel_vfs_file file={.private_data=node,.mode=node->mode};
        int result=directory_empty(&file,true);
        if (result<=0) return result ? result : -KERNEL_ENOTEMPTY;
    } else {
        struct kernel_vfs_stat stat;
        int error=kernel_vfs_path_stat(real(e),&stat);
        if (error) return error;
        last_link=stat.nlink<=1;
    }
    NODE_OWNER(parent)=kernel_vfs_node_pin(&m->instance,number);
    if (!parent) return -KERNEL_ENOENT;
    struct overlay_node *dir=entry(parent);
    /* The overlay namespace excludes another change to this name. Backing
     * locks still serialize removal with accesses to the actual upper inode. */
    int error=ensure_upper(dir);
    if (error) return error;
    LAYER_CREDS(m);
    struct kernel_vfs_path *destination=upper(dir);
    PATH_OWNER(low);
    error=lookup(dir->lower,name,strlen(name),&low);
    if (error) return error;
    if (!low && !directory)
        error=kernel_vfs_unlink_at(destination,destination,name,0);
    else if (!upper(e)) {
        error=kernel_vfs_mknod_at(destination,destination,name,KERNEL_VFS_S_IFCHR,0,0);
    } else {
        struct overlay_work *work=prepare_work(m);
        if (!work) return -KERNEL_ENOMEM;
        if (low) {
            error=kernel_vfs_mknod_at(m->work,m->work,work->name,KERNEL_VFS_S_IFCHR,0,0);
            bool created=error==0;
            if (created)
                error=kernel_vfs_exchange_layer_entries(m->work,destination,work->name,name);
            finish_work(m,work,created,!error && directory);
        } else {
            error=kernel_vfs_rename_at(destination,m->work,destination,name,work->name,1);
            finish_work(m,work,!error,directory);
        }
    }
    if (!error && last_link) {
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node,&io,1);
        node->unlinked=1;
    }
    return error;
}

static void release_unlinked(struct kernel_vfs_node *node)
{
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(node,&io,1);
    if (!node->open_files) node->retired=1;
}

static int rename_inode(struct kernel_vfs_instance *i,uint64_t old_number,const char *old_name,
    uint64_t new_number,const char *new_name,unsigned flags,struct kernel_vfs_rename_result *result)
{
    struct overlay_mount *m=volume(i);
    NODE_OWNER(old_parent)=kernel_vfs_node_pin(i,old_number);
    NODE_OWNER(new_parent)=kernel_vfs_node_pin(i,new_number);
    if (!old_parent || !new_parent) return -KERNEL_ENOENT;
    struct overlay_node *from=entry(old_parent), *to=entry(new_parent);
    LAYER_CREDS(m);
    PATH_OWNER(old_low); PATH_OWNER(old_up); PATH_OWNER(new_low); PATH_OWNER(new_up);
    int error=lookup_layers(from,old_name,strlen(old_name),&old_low,&old_up);
    if (error) return error;
    /* An opaque directory hides its lower directory during lookup. Moving
     * it would still need a whiteout at the original name. */
    if (is_dir(old_up) && !old_low) {
        error=lookup(from->lower,old_name,strlen(old_name),&old_low);
        if (error) return error;
    }
    if (old_low || !old_up) return -KERNEL_EXDEV;
    error=lookup_layers(to,new_name,strlen(new_name),&new_low,&new_up);
    if (error && error!=-KERNEL_ENOENT) return error;
    bool target_exists=error==0;
    if (is_dir(old_up) && is_dir(new_up) && !new_low) {
        error=lookup(to->lower,new_name,strlen(new_name),&new_low);
        if (error) return error;
    }
    if (new_low && is_dir(old_up)) return -KERNEL_EXDEV;
    if ((flags&1U) && target_exists) return -KERNEL_EEXIST;
    uint64_t replaced=target_exists ? inode_number(new_up ? new_up : new_low,new_up!=NULL) : 0;
    bool last_link=false;
    if (target_exists) {
        struct kernel_vfs_path *target=new_up ? new_up : new_low;
        struct kernel_vfs_stat stat;
        error=kernel_vfs_path_stat(target,&stat);
        if (error) return error;
        last_link=is_dir(target) || stat.nlink<=1;
    }
    if (new_low && !new_up) {
        /* The upper rename hides a lower file that remains a live object. */
        if (is_dir(new_low)) return -KERNEL_EISDIR;
        if (is_dir(old_up)) return -KERNEL_ENOTDIR;
    }
    error=ensure_upper(to);
    if (error) return error;
    error=kernel_vfs_rename_at(upper(from),upper(to),upper(from),old_name,new_name,
        replaced==0 && new_up ? 0 : flags);
    if (!error) {
        result->changed=1;
        result->replaced_inode=replaced;
        result->replaced_last_link=last_link;
    }
    return error;
}

static int sync_filesystem(struct kernel_vfs_mount *view)
{
    struct overlay_mount *m=volume(view->private_data);
    uint64_t observed=0;
    return m->upper ? kernel_vfs_sync_mount(m->upper->file.mount,&observed) : 0;
}
static int remount(struct kernel_vfs_mount *view,int read_only)
{
    struct overlay_mount *m=volume(view->private_data);
    return !read_only && (!m->upper || kernel_vfs_mount_is_readonly(m->upper->file.mount)) ?
        -KERNEL_EROFS : 0;
}

static int unmount(struct kernel_vfs_mount *view)
{
    struct overlay_mount *m=volume(view->private_data);
    if (m->instance.external_files || !kernel_vfs_nodes_idle(&m->instance,NULL)) return -KERNEL_EBUSY;
    if (m->cleanup) {
        LAYER_CREDS(m);
        do {
            struct overlay_work *work=m->cleanup;
            int error=remove_work(m,work->name,work->directory);
            if (error && error!=-KERNEL_ENOENT) return error;
            m->cleanup=work->next;
            kernel_heap_release(m->instance.heap,work);
        } while (m->cleanup);
    }
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        struct overlay_mount **link=&active_overlays;
        while (*link!=m) link=&(*link)->next;
        *link=m->next;
    }
    put_path(&m->work);
    put_path(&m->upper);
    put_path(&m->lower);
    kernel_credentials_put(m->creator);
    kernel_heap_release(m->instance.heap,m);
    return 0;
}
static void filesystem_id(struct kernel_vfs_instance *i,unsigned char id[16])
{
    memset(id,0,16);
    memcpy(id,&i->device_id,sizeof(i->device_id));
}
static uint32_t generation(struct kernel_vfs_node *n)
{ return entry(n)->generation; }
static const struct kernel_vfs_export_ops overlay_export={
    .type=KERNEL_VFS_HANDLE_OVERLAY,.filesystem=filesystem_id,.generation=generation,
};
static const struct kernel_vfs_backend overlay_ops={
    .type="overlay",.read_execution=KERNEL_VFS_EXECUTION_CALLER,
    .file_operation_execution=KERNEL_VFS_EXECUTION_CALLER,.namespace_execution=KERNEL_VFS_EXECUTION_CALLER,
    .owns_dentry_names=true,.export=&overlay_export,.error=mount_error,.root=root,.open=open_root,
    .lookup_open=lookup_open,.data_file=data_file,.event_backing=event_backing,
    .copy_range_backing=data_file,
    .prepare_access=prepare_access,.copy_up=copy_up,.close_node=close_node,
    .readlink=readlink,.stat=stat_inode,.statfs=statfs,.acl_permission=permission,
    .accessed=accessed,.modified=modified,.set_mode=set_mode,.set_owner=set_owner,.set_times=set_times,.xattr=xattr,
    .create=create,.mkdir=mkdir,.symlink=symlink,.remove_resolved=remove_resolved,
    .create_with_handle=create_with_handle,
    .release_unlinked=release_unlinked,.rename=rename_inode,
    .read_directory=read_directory,.sync_filesystem=sync_filesystem,.remount=remount,.unmount=unmount,
};

/* The mount topology mutex excludes directory attachment changes. The VFS
 * state lock also stabilizes rename's directory-parent publication. */
static bool same_or_below(const struct kernel_vfs_path *path,const struct kernel_vfs_path *root)
{
    if (!path || !root) return false;
    for (const struct kernel_vfs_dentry *d=path->dentry;d;d=d->parent)
        if (d==root->dentry) return true;
    return false;
}
static bool overlaps(const struct kernel_vfs_path *a,const struct kernel_vfs_path *b)
{ return same_or_below(a,b) || same_or_below(b,a); }

int kernel_overlay_mount(struct kernel_heap *heap,uint64_t flags,const struct kernel_credentials *creator,
    struct kernel_vfs_path *const layers[3],struct kernel_vfs_mount **owner)
{
    struct kernel_vfs_path *low=layers[KERNEL_OVERLAY_LOWER];
    struct kernel_vfs_path *up=layers[KERNEL_OVERLAY_UPPER];
    struct kernel_vfs_path *work=layers[KERNEL_OVERLAY_WORK];
    if (!is_dir(low) || (!!up!=!!work)) return -KERNEL_EINVAL;
    for (unsigned j=0;j<3;j++) if (layers[j]) {
        struct kernel_vfs_instance *i=layers[j]->file.mount->private_data;
        if (strcmp(i->ops->type,"ext4")) return -KERNEL_EOPNOTSUPP;
        if (!is_dir(layers[j])) return -KERNEL_ENOTDIR;
    }
    if (up && work->file.mount!=up->file.mount) return -KERNEL_EINVAL;
    if (up && kernel_vfs_mount_is_readonly(up->file.mount)) return -KERNEL_EROFS;

    struct overlay_mount *m;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*m),(void **)&m)) return -KERNEL_ENOMEM;
    m->creator=kernel_credentials_get(creator);
    m->instance.heap=heap;
    m->instance.ops=&overlay_ops;
    m->instance.backend_data=m;
    m->instance.read_only=!up;
    kernel_rwlock_init(&m->instance.reconfiguration,13,(uintptr_t)&m->instance);
    kernel_mutex_init(&m->instance.namespace_lock,14,(uintptr_t)&m->instance);
    kernel_raw_lock_init(&m->instance.error_lock,KERNEL_RAW_RANK_OBJECT);
    m->view.private_data=&m->instance;
    m->view.source_name="overlay";
    m->view.id=kernel_vfs_allocate_mount_id();
    m->view.state=VFS_MOUNT_STATE_LIVE;

    int error=0;
    struct kernel_vfs_path **slots[3]={&m->lower,&m->upper,&m->work};
    for (unsigned j=0;j<3;j++) if (layers[j]) {
        error=kernel_vfs_path_acquire(layers[j]);
        if (error) goto fail;
        *slots[j]=layers[j];
    }
    if (work) {
        LAYER_CREDS(m);
        int empty=directory_empty(&work->file,false);
        if (empty<=0) { error=empty ? empty : -KERNEL_ENOTEMPTY; goto fail; }
    }
    {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        if (overlaps(low,up) || overlaps(low,work) || overlaps(up,work)) error=-KERNEL_EINVAL;
        for (struct overlay_mount *v=active_overlays;v && !error;v=v->next)
            if (overlaps(up,v->upper) || overlaps(work,v->work) ||
                overlaps(up,v->work) || overlaps(work,v->upper) ||
                overlaps(low,v->upper) || overlaps(low,v->work) ||
                overlaps(up,v->lower) || overlaps(work,v->lower)) error=-KERNEL_EBUSY;
        if (!error) { m->next=active_overlays; active_overlays=m; }
    }
    if (error) goto fail;
    kernel_vfs_mount_initialize(&m->view);
    m->view.read_only=(flags&1U)!=0 || !up;
    *owner=&m->view;
    return 0;
fail:
    put_path(&m->work);
    put_path(&m->upper);
    put_path(&m->lower);
    kernel_credentials_put(m->creator);
    kernel_heap_release(heap,m);
    return error;
}
