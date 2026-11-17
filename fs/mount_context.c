#include "mount_context.h"
#include "cgroup_internal.h"
#include "vfs_internal.h"
#include "open_file_internal.h"
#include "char_device_internal.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/tmpfs.h>
#include <kernel/devpts.h>
#include <kernel/procfs.h>
#include <kernel/mqueue.h>
#include <kernel/ipc_namespace.h>
#include <kernel/bpf.h>
#include <string.h>

/* The context owns one instance view; fsmount gives each returned FD a separate
 * view of that same instance. No namespace connection is owned by the context. */
struct mount_context {
    struct kernel_mount_parameters parameters;
    struct kernel_heap *heap;
    struct kernel_vfs_path *root;
    const struct kernel_credentials *creator;
    struct kernel_mutex lock;
    unsigned source_set;
    unsigned phase; /* 0=create parameters, 1=ready, 2=reconfigure, 3=failed */
};
int kernel_mount_type_supported(const char *type)
{
    return !strcmp(type,"tmpfs") || !strcmp(type,"ext4") || !strcmp(type,"vfat") ||
        !strcmp(type,"devpts") || !strcmp(type,"proc") || !strcmp(type,"mqueue") ||
        !strcmp(type,"bpf") || !strcmp(type,"tracefs") || !strcmp(type,"sysfs") || !strcmp(type,"cgroup2");
}
void kernel_mount_discard(struct kernel_vfs_path **owner)
{
    if (!*owner)return;
    kernel_vfs_close_tree(kernel_vfs_path_mount(*owner));
    if(kernel_vfs_path_release(owner))__builtin_trap();
}
int kernel_mount_create(const struct kernel_fs_context *fs,const struct kernel_mount_parameters *p,
    const struct kernel_credentials *creator,int exclusive,struct kernel_vfs_path **owner)
{
    if(!kernel_mount_type_supported(p->type))return -KERNEL_ENODEV;
    KERNEL_LOCK_SCOPE(topology);kernel_vfs_mount_topology_lock(&topology);
    int disk=!strcmp(p->type,"ext4") || !strcmp(p->type,"vfat");
    uint64_t device=0;
    if(disk) {
        if(!p->source[0])return -KERNEL_EINVAL;
        KERNEL_FS_PATHS(paths,fs);
        struct kernel_vfs_path *source=NULL;
        int error=kernel_vfs_path_resolve(paths.cwd,paths.root,p->source,1,&source);
        if(error)return error;
        struct kernel_vfs_stat st={0};
        error=kernel_vfs_path_stat(source,&st);
        if(!error && (st.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFBLK)error=-KERNEL_ENOTBLK;
        if(!error && kernel_vfs_path_mount(source)->nodev)error=-KERNEL_EACCES;
        device=st.rdev;
        if(kernel_vfs_path_release(&source))__builtin_trap();
        if(error)return error;
    }
    if(strcmp(p->type,"tmpfs") && strcmp(p->type,"devpts") && p->options[0])return -KERNEL_ENOTSUP;
    if(disk) {
        int reused=kernel_vfs_reuse_block_tree(device,p->type,!!(p->flags&1U),exclusive,
            fs->heap,kernel_fs_context_namespace_id(fs),owner);
        if(reused<0)return reused;
        if(!reused){
            struct kernel_vfs_mount *mount=kernel_vfs_path_mount(*owner);
            mount->read_only=!!(p->flags&1U);mount->nosuid=!!(p->flags&2U);
            mount->nodev=!!(p->flags&4U);mount->noexec=!!(p->flags&8U);
            mount->silent=!!(p->flags&32768U);mount->atime_policy=kernel_vfs_atime_policy(p->flags,0,0);
            int error=kernel_vfs_change_propagation(*owner,UINT64_C(262144),0);
            if(error)kernel_mount_discard(owner);
            return error;
        }
    }
    if(!strcmp(p->type,"cgroup2")) {
        int reused=kernel_vfs_reuse_cgroup_tree(fs->heap,
            kernel_fs_context_namespace_id(fs),owner);
        if(reused<0)return reused;
        if(!reused){
            struct kernel_vfs_mount *mount=kernel_vfs_path_mount(*owner);
            mount->read_only=!!(p->flags&1U);mount->nosuid=!!(p->flags&2U);
            mount->nodev=!!(p->flags&4U);mount->noexec=!!(p->flags&8U);
            mount->silent=!!(p->flags&32768U);mount->atime_policy=kernel_vfs_atime_policy(p->flags,0,0);
            int error=kernel_vfs_change_propagation(*owner,UINT64_C(262144),0);
            if(error)kernel_mount_discard(owner);
            return error;
        }
    }
    struct kernel_vfs_mount *mount=NULL;
    const struct kernel_credentials *previous=kernel_credentials_override(creator);
    int error=disk?kernel_vfs_disk_create(fs->heap,device,p->flags&1U,p->source,p->type,&mount):
        !strcmp(p->type,"cgroup2")?kernel_cgroup_mount(fs->heap,p->flags&1U,&mount):
        !strcmp(p->type,"bpf")?kernel_bpffs_mount(fs->heap,p->flags&1U,&mount):
        !strcmp(p->type,"tracefs")?kernel_tracefs_mount(fs->heap,p->flags&1U,&mount):
        !strcmp(p->type,"sysfs")?kernel_sysfs_mount(fs->heap,p->flags&1U,&mount):
        !strcmp(p->type,"devpts")?kernel_devpts_create(fs->heap,p->flags&1U,p->options,&mount):
        !strcmp(p->type,"mqueue")?kernel_mqueue_fs_create(fs->heap,p->flags&1U,p->ipc_namespace?p->ipc_namespace:kernel_task_ipc_namespace(),&mount):
        !strcmp(p->type,"tmpfs")?kernel_tmpfs_create(fs->heap,p->flags&1U,p->options,&mount):
        kernel_procfs_create(fs->heap,p->flags&1U,&mount);
    kernel_credentials_override(previous);

    if(error)return error;
    mount->nosuid=!!(p->flags&2U);mount->nodev=!!(p->flags&4U);
    mount->noexec=!!(p->flags&8U);mount->silent=!!(p->flags&32768U);
    mount->atime_policy=kernel_vfs_atime_policy(p->flags,0,0);
    error=kernel_vfs_detach_created_mount(mount,fs->heap,kernel_fs_context_namespace_id(fs),owner);
    if(error) {
        int cleanup=kernel_vfs_unmount(mount);
        if(cleanup){if(disk)kernel_vfs_disk_defer_cleanup(mount);else __builtin_trap();}
    }
    return error;
}
static void context_release(void *object)
{
    struct mount_context *ctx=object;
    kernel_mount_discard(&ctx->root);
    kernel_credentials_put(ctx->creator);
    if(ctx->parameters.ipc_namespace)kernel_namespace_put(ctx->parameters.ipc_namespace);
    kernel_heap_release(ctx->heap,ctx);
}
static int context_read(void *object,struct kernel_task *task,uint32_t flags,
    void *buffer,size_t size,size_t *count)
{
    (void)object;(void)task;(void)flags;(void)buffer;(void)size;(void)count;
    return -KERNEL_ENODATA;
}
static const struct kernel_char_device context_operations={.release=context_release,.read=context_read,.read_once=1};
int kernel_mount_context_open(struct kernel_heap *heap,const char *type,struct kernel_vfs_path *root,
    struct kernel_open_file_description **owner)
{
    struct mount_context *ctx=NULL;
    struct kernel_open_file_description *file=NULL;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*ctx),(void **)&ctx))return -KERNEL_ENOMEM;
    ctx->heap=heap;ctx->creator=kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&ctx->lock,12,(uintptr_t)ctx);
    if(type)memcpy(ctx->parameters.type,type,strlen(type)+1);
    if(type&&!strcmp(type,"mqueue")&&!root)
        ctx->parameters.ipc_namespace=kernel_namespace_get(kernel_task_ipc_namespace());
    if(root) {
        int error=kernel_vfs_clone_tree(root,0,&ctx->root);
        if(!error)error=kernel_vfs_change_propagation(ctx->root,UINT64_C(262144),0);
        if(error){context_release(ctx);return error;}
        ctx->phase=2;
    }
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*file),(void **)&file)){
        context_release(ctx);return -KERNEL_ENOMEM;
    }
    file->heap=heap;file->references=1;file->kind=KERNEL_OPEN_FILE_KIND_FS_CONTEXT;
    file->open_flags=2;file->credentials=kernel_credentials_get(kernel_credentials_current());
    file->device=&context_operations;file->device_instance=ctx;file->device_opened=1;
    kernel_mutex_init(&file->offset_lock,10,(uintptr_t)file);
    *owner=file;return 0;
}
static int context_parameter(struct mount_context *ctx,unsigned command,const char *key,const char *value,char *candidate)
{
    struct kernel_mount_parameters *p=&ctx->parameters;
    if(!strcmp(key,"ro") || !strcmp(key,"rw")) {
        p->mask|=1U;p->flags=(p->flags&~UINT64_C(1))|(!strcmp(key,"ro")?1U:0U);return 0;
    }
    if(!strcmp(key,"source")) {
        if(command!=1 || ctx->source_set)return -KERNEL_EINVAL;
        size_t n=strlen(value);if(n>=sizeof(p->source))return -KERNEL_ENAMETOOLONG;
        memcpy(p->source,value,n+1);ctx->source_set=1;return 0;
    }
    if(ctx->phase==2 || command>1)return -KERNEL_EINVAL;
    int tmpfs=!strcmp(p->type,"tmpfs"),devpts=!strcmp(p->type,"devpts");
    const char *scalar=command==0?NULL:value;
    int error=tmpfs?kernel_tmpfs_check_mount_parameter(ctx->heap,key,scalar):
        devpts?kernel_devpts_check_mount_parameter(key,scalar):-KERNEL_EINVAL;
    if(error)return error;
    int flag=command==0;
    size_t used=0,keylen=strlen(key);
    for(const char *at=p->options;*at;) {
        const char *end=at;while(*end&&*end!=',')end++;
        if(!((size_t)(end-at)>=keylen && !memcmp(at,key,keylen) && (at[keylen]=='='||at+keylen==end))) {
            size_t n=(size_t)(end-at);if(used)candidate[used++]=',';
            memcpy(candidate+used,at,n);used+=n;
        }
        at=*end?end+1:end;
    }
    size_t n=keylen+(flag?0:1+strlen(value));
    if(used+n+!!used>=sizeof(p->options))return -KERNEL_E2BIG;
    if(used)candidate[used++]=',';
    memcpy(candidate+used,key,keylen);used+=keylen;
    if(!flag){candidate[used++]='=';memcpy(candidate+used,value,strlen(value));used+=strlen(value);}
    candidate[used]=0;
    memcpy(p->options,candidate,used+1);
    return 0;
}
int kernel_mount_context_configure(struct kernel_open_file_description *file,const struct kernel_fs_context *fs,
    unsigned command,const char *key,const char *value,char *candidate)
{
    if(file->kind!=KERNEL_OPEN_FILE_KIND_FS_CONTEXT)return -KERNEL_EINVAL;
    struct mount_context *ctx=file->device_instance;
    KERNEL_LOCK_SCOPE(guard);kernel_mutex_lock(&ctx->lock,&guard);
    if(command==6 || command==8) {
        if(ctx->phase!=0)return -KERNEL_EBUSY;
        if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
        int error=kernel_mount_create(fs,&ctx->parameters,ctx->creator,command==8,&ctx->root);
        ctx->phase=error?3:1;return error;
    }
    if(command==7) {
        if(ctx->phase!=2)return -KERNEL_EBUSY;
        int error;
        if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))error=-KERNEL_EPERM;
        else error=kernel_vfs_mount_reconfigure(ctx->root,ctx->parameters.flags,ctx->parameters.mask,ctx->parameters.options);
        if(error)ctx->phase=3;
        else {ctx->parameters.flags=ctx->parameters.mask=0;ctx->source_set=0;ctx->parameters.options[0]=ctx->parameters.source[0]=0;}
        return error;
    }
    if(ctx->phase!=0 && ctx->phase!=2)return -KERNEL_EBUSY;
    return context_parameter(ctx,command,key,value,candidate);
}
int kernel_mount_context_mount(struct kernel_open_file_description *file,const struct kernel_fs_context *fs,
    unsigned flags,unsigned attributes,struct kernel_open_file_description **owner)
{
    if(file->kind!=KERNEL_OPEN_FILE_KIND_FS_CONTEXT)return -KERNEL_EINVAL;
    struct mount_context *ctx=file->device_instance;
    KERNEL_LOCK_SCOPE(guard);kernel_mutex_lock(&ctx->lock,&guard);
    if(!ctx->root)return -KERNEL_EINVAL;
    if(ctx->phase!=1)return -KERNEL_EBUSY;
    struct kernel_vfs_path *root=NULL;
    int error=kernel_vfs_clone_tree(ctx->root,0,&root);
    if(error)return error;
    struct kernel_vfs_mount *mount=kernel_vfs_path_mount(root);
    mount->detached_origin=kernel_fs_context_namespace_id(fs);
    mount->read_only=!!(attributes&1U);mount->nosuid=!!(attributes&2U);
    mount->nodev=!!(attributes&4U);mount->noexec=!!(attributes&8U);
    mount->atime_policy=((attributes&0x70U)==0x20U?VFS_ATIME_STRICT:attributes&0x10U?VFS_ATIME_NONE:0)|(attributes&0x80U?VFS_ATIME_NODIR:0);
    ctx->phase=2;ctx->parameters.flags=ctx->parameters.mask=0;
    ctx->source_set=0;ctx->parameters.options[0]=ctx->parameters.source[0]=0;
    if(flags&2U)error=kernel_mount_namespace_take_tree(&root,fs->heap,owner);
    else error=kernel_open_file_create_path(fs->heap,root,mount,owner);
    if(error)kernel_mount_discard(&root);
    else {
        if(root && kernel_vfs_path_release(&root))__builtin_trap();
        ctx->phase=2;ctx->parameters.flags=ctx->parameters.mask=0;
        ctx->source_set=0;ctx->parameters.options[0]=ctx->parameters.source[0]=0;
    }
    return error;
}
