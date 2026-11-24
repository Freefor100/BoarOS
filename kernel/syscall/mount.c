#include <kernel/credentials.h>
#include <kernel/scheduler.h>
#include "private.h"
#include "../../fs/vfs_internal.h"
#include "../../fs/vfs_objects.h"
#include "../../fs/files/private.h"
#include "../../fs/mount_context.h"
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/mqueue.h>
#include <kernel/bpf.h>

#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/procfs.h>
#include <kernel/process_acct.h>
#include <kernel/devpts.h>
#include <kernel/tmpfs.h>
#include <kernel/loop.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define LINUX_MS_RDONLY UINT64_C(1)
#define LINUX_MS_NOSUID UINT64_C(2)
#define LINUX_MS_NODEV UINT64_C(4)
#define LINUX_MS_NOEXEC UINT64_C(8)
#define LINUX_MS_REMOUNT UINT64_C(32)
#define LINUX_MS_BIND UINT64_C(4096)
#define LINUX_MS_MOVE UINT64_C(8192)
#define LINUX_MS_REC UINT64_C(16384)
#define LINUX_MS_SILENT UINT64_C(32768)
#define LINUX_MS_PRIVATE UINT64_C(262144)

static int import_string(struct kernel_mm *mm, char *buffer, size_t capacity,
                         uint64_t address)
{
    size_t length = 0;
    enum kernel_uaccess_status status = kernel_copy_string_from_user(
        mm, buffer, address, capacity, &length);
    if (status == KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status == KERNEL_UACCESS_STATUS_TOO_LONG) return -KERNEL_ENAMETOOLONG;
    if (status != KERNEL_UACCESS_STATUS_OK) return -KERNEL_EIO;
    return 0;
}

struct mount_arguments {
    char target[KERNEL_FS_PATH_MAX];
    struct kernel_mount_parameters parameters;
};
static void mount_arguments_initialize(struct mount_arguments *storage)
{
    storage->parameters.type[0]=storage->parameters.options[0]=storage->parameters.source[0]=0;
    storage->parameters.ipc_namespace=NULL;
    /* Legacy mount chooses the caller view at creation, unlike fsopen's
     * retained view. Heap contents are not guaranteed to start at zero. */
    storage->parameters.pid_view=NULL;
    memset(storage->parameters.overlay_layers,0,sizeof(storage->parameters.overlay_layers));
}
static __attribute__((noinline)) int mount_filesystem(struct kernel_mm *mm, const struct kernel_fs_context *fs,
                      const struct kernel_syscall_request *request)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    KERNEL_LOCK_SCOPE(topology);
    uint64_t flags = request->arguments[3];
    struct kernel_vfs_path *covered = NULL;
    struct kernel_vfs_path *created=NULL;
    struct mount_arguments *storage=NULL;
    enum kernel_heap_status allocation=kernel_heap_allocate(fs->heap,sizeof(*storage),(void **)&storage);
    if(allocation!=KERNEL_HEAP_STATUS_OK)
        return allocation==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    char *target=storage->target,*source_name=storage->parameters.source,*type=storage->parameters.type,*options=storage->parameters.options;
    mount_arguments_initialize(storage);
    int result;
    if (request->arguments[2]) {
        result=import_string(mm,(flags&(LINUX_MS_BIND|LINUX_MS_MOVE))?target:type,
            (flags&(LINUX_MS_BIND|LINUX_MS_MOVE))?KERNEL_FS_PATH_MAX:sizeof(storage->parameters.type),request->arguments[2]);
        if(result)goto Finish;
    }
    if (request->arguments[0]) {
        result = import_string(mm, source_name, KERNEL_FS_PATH_MAX,
                               request->arguments[0]);
        if (result) goto Finish;
    }
    if (!(flags & (LINUX_MS_BIND|LINUX_MS_MOVE)) && request->arguments[4]) {
        size_t capacity=!strcmp(type,"overlay")?KERNEL_MOUNT_OPTIONS_MAX:256U;
        result = import_string(mm, options, capacity,
                               request->arguments[4]);
        if (result) goto Finish;
    }
    if ((flags & (LINUX_MS_BIND|LINUX_MS_MOVE)) && request->arguments[4]) {
        unsigned char first;
        size_t copied = 0;
        /* copy_mount_options接受可读前缀；未使用的选项无需复制其余页。 */
        enum kernel_uaccess_status status = kernel_copy_from_user(mm, &first,
            request->arguments[4], sizeof(first), &copied);
        if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(first)) {
            result = status == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT : -KERNEL_EIO;
            goto Finish;
        }
    }
    result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                           request->arguments[1]);
    if (result) goto Finish;
    result = kernel_vfs_path_resolve(fs_paths.cwd,
              fs_paths.root, target, 1, &covered);
    if (result) goto Finish;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN)) {result=-KERNEL_EPERM;goto Finish;}
    if(flags & ~(LINUX_MS_RDONLY|LINUX_MS_NOSUID|LINUX_MS_NODEV|LINUX_MS_NOEXEC|VFS_MS_NOSYMFOLLOW|LINUX_MS_SILENT|LINUX_MS_REMOUNT|LINUX_MS_BIND|LINUX_MS_MOVE|LINUX_MS_REC|UINT64_C(1966080)|UINT64_C(1024)|2048U|2097152U|16777216U)) {result=-KERNEL_ENOTSUP;goto Finish;}
    if (!(flags & (LINUX_MS_BIND | LINUX_MS_MOVE | LINUX_MS_REMOUNT)) && !strcmp(type, "devtmpfs")) {
        result = kernel_loop_initialize(fs->heap);
        if (result) goto Finish;
    }
    kernel_vfs_mount_topology_lock(&topology);
    struct kernel_vfs_mount *top=kernel_fs_context_mount_root(fs);
    if (!kernel_vfs_mount_contains(top,covered)) {result=-KERNEL_EINVAL;goto Finish;}
    if (flags & LINUX_MS_REMOUNT) {
        result=kernel_vfs_remount(covered,flags,options);
        goto Finish;
    }
    if (flags & LINUX_MS_BIND) {
        struct kernel_vfs_path *source=NULL;
        if (!request->arguments[0]) {result=-KERNEL_EINVAL;goto Finish;}
        result=kernel_vfs_path_resolve(fs_paths.cwd,fs_paths.root,source_name,1,&source);
        if (!result && !kernel_vfs_mount_contains(top,source)) result=-KERNEL_EINVAL;
        if (!result) result=kernel_vfs_bind_mount(source,covered,!!(flags & LINUX_MS_REC));
        if (source && kernel_vfs_path_release(&source)) __builtin_trap();
        goto Finish;
    }
    if(flags&UINT64_C(1966080)) {
        uint64_t type=flags&UINT64_C(1966080);
        result=(flags & ~(UINT64_C(1966080)|LINUX_MS_REC|LINUX_MS_SILENT)) ? -KERNEL_EINVAL :
            kernel_vfs_change_propagation(covered,type,!!(flags&LINUX_MS_REC));
        goto Finish;
    }
    if (flags & LINUX_MS_MOVE) {
        struct kernel_vfs_path *source=NULL;
        if (!request->arguments[0] || !source_name[0]) {result=-KERNEL_EINVAL;goto Finish;}
        result=kernel_vfs_path_resolve(fs_paths.cwd,fs_paths.root,source_name,1,&source);
        if (!result && !kernel_vfs_mount_contains(top,source)) result=-KERNEL_EINVAL;
        if (!result) result=kernel_vfs_move_mount(source,covered,0);
        if (source && kernel_vfs_path_release(&source)) __builtin_trap();
        goto Finish;
    }
    if (flags & LINUX_MS_REC) {result=-KERNEL_ENOTSUP;goto Finish;}
    if(!kernel_mount_type_supported(type)){result=-KERNEL_ENODEV;goto Finish;}
    storage->parameters.flags=flags;
    result=kernel_mount_create(fs,&storage->parameters,kernel_credentials_current(),0,&created);
    /* Legacy mount checks the new root type after creating the filesystem;
     * move_mount has its own EINVAL rule for mismatched existing trees. */
    if(!result && (kernel_vfs_path_mode(covered)&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR)result=-KERNEL_ENOTDIR;
    if(!result)result=kernel_vfs_move_mount(created,covered,0);
    kernel_mount_discard(&created);
Finish:
    if (covered && kernel_vfs_path_release(&covered)) __builtin_trap();
    kernel_heap_release(fs->heap, target);
    return result;
}

/* Do not retain unmount's operation-specific frame in every mount syscall. */
static __attribute__((noinline)) int unmount_filesystem(struct kernel_mm *mm,
                        const struct kernel_fs_context *fs,
                        const struct kernel_syscall_request *request)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    KERNEL_LOCK_SCOPE(accounting);
    KERNEL_LOCK_SCOPE(topology);
    enum { MNT_FORCE_FLAG = 1U, UMOUNT_NOFOLLOW_FLAG = 8U };
    unsigned flags=(uint32_t)request->arguments[1];
    if (flags & ~(MNT_FORCE_FLAG|KERNEL_VFS_MNT_DETACH|KERNEL_VFS_MNT_EXPIRE|UMOUNT_NOFOLLOW_FLAG))
        return -KERNEL_ENOTSUP;
    if ((flags&MNT_FORCE_FLAG) && !(flags&KERNEL_VFS_MNT_EXPIRE)) return -KERNEL_ENOTSUP;
    if (!request->arguments[0]) return -KERNEL_EFAULT;
    char *target = 0;
    struct kernel_vfs_path *root = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(fs->heap,
        KERNEL_FS_PATH_MAX, (void **)&target);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    int result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                               request->arguments[0]);
    if (result) goto Finish;
    result = kernel_vfs_path_resolve(fs_paths.cwd,
             fs_paths.root, target, !(flags & UMOUNT_NOFOLLOW_FLAG), &root);
    if (result) goto Finish;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN)) {result=-KERNEL_EPERM;goto Finish;}
    kernel_process_acct_lock(&accounting);
    kernel_vfs_mount_topology_lock(&topology);
    if (!kernel_vfs_mount_contains(kernel_fs_context_mount_root(fs),root)) {
        result=-KERNEL_EINVAL;goto Finish;
    }
    flags &= ~UMOUNT_NOFOLLOW_FLAG;
    struct kernel_vfs_mount *mount = kernel_vfs_path_mount(root);
    if (!kernel_vfs_path_is_mount_root(root)) {
        result = -KERNEL_EINVAL;
        goto Finish;
    }
    if ((flags&KERNEL_VFS_MNT_EXPIRE) &&
        ((flags&(MNT_FORCE_FLAG|KERNEL_VFS_MNT_DETACH)) || mount==kernel_vfs_path_mount(fs_paths.root))) {
        result = -KERNEL_EINVAL;
        goto Finish;
    }
    if (!flags && mount==kernel_vfs_path_mount(fs_paths.root)) {
        uint64_t flags=1U|(mount->nosuid?2U:0U)|(mount->nodev?4U:0U)|
            (mount->noexec?8U:0U)|(mount->nosymfollow?VFS_MS_NOSYMFOLLOW:0U)|(mount->silent?32768U:0U);
        result=kernel_vfs_remount(root,flags,NULL);
        goto Finish;
    }
    result=kernel_vfs_umount(root,flags);
Finish:
    if (root && kernel_vfs_umount_path_release(&root)) __builtin_trap();
    kernel_heap_release(fs->heap, target);
    return result;
}

/* Path snapshots and FD pins are real owners; no FD-table lock crosses lookup
 * or mount publication. Empty names preserve the identity of the opened view. */
static int mount_api_path(struct kernel_files *files,const struct kernel_fs_paths *paths,
    int fd,const char *name,int empty,int follow,struct kernel_vfs_path **owner,
    struct kernel_files_pin_guard *pin)
{
    if(!*name && !empty)return -KERNEL_ENOENT;
    struct kernel_vfs_path *start;
    if(*name=='/')start=paths->root;
    else if(fd==KERNEL_FS_AT_FDCWD)start=paths->cwd;
    else {
        struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,pin);
        if(!file)return -KERNEL_EBADF;
        start=kernel_open_file_path(file);
        if(!start)return -KERNEL_EINVAL;
        if(*name && (kernel_open_file_mode(file)&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR)return -KERNEL_ENOTDIR;
    }
    if(*name)return kernel_vfs_path_resolve(start,paths->root,name,follow,owner);
    int error=kernel_vfs_path_acquire(start);if(!error)*owner=start;return error;
}

static struct kernel_vfs_mount *mount_tree_top(const struct kernel_vfs_path *path)
{
    struct kernel_vfs_mount *mount=kernel_vfs_path_mount(path);
    while(mount->parent)mount=mount->parent;
    return mount;
}
static int anonymous_tree(const struct kernel_vfs_path *path,uint64_t namespace_id)
{
    struct kernel_vfs_mount *top=mount_tree_top(path);
    return top->detached_tree && !__atomic_load_n(&top->namespace_retiring,__ATOMIC_ACQUIRE) && !top->retired && top->detached_origin==namespace_id;
}
struct native_mount_attributes {uint64_t set,clear,propagation,userns;};
/* User import is complete before the tree publication path begins; its
 * bounded copy probe need not occupy every nested filesystem call frame. */
static __attribute__((noinline)) int import_mount_attributes(struct kernel_mm *mm,
    const struct kernel_syscall_request *request,struct native_mount_attributes *attr)
{
    unsigned flags=(unsigned)request->arguments[2];
    if(flags&~(0x1000U|0x8000U|0x100U|0x800U))return -KERNEL_EINVAL;
    uint64_t size=request->arguments[4];
    if(size>BOAROS_PAGE_SIZE)return -KERNEL_E2BIG;
    if(size<sizeof(*attr))return -KERNEL_EINVAL;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
    size_t copied;
    if(kernel_copy_from_user(mm,attr,request->arguments[3],sizeof(*attr),&copied))return -KERNEL_EFAULT;
    unsigned char tail[64];
    for(size_t offset=sizeof(*attr);offset<size;) {
        size_t bytes=size-offset;if(bytes>sizeof(tail))bytes=sizeof(tail);
        if(kernel_copy_from_user(mm,tail,request->arguments[3]+offset,bytes,&copied))return -KERNEL_EFAULT;
        for(size_t i=0;i<bytes;i++)if(tail[i])return -KERNEL_E2BIG;
        offset+=bytes;
    }

    if((attr->set|attr->clear)&~(UINT64_C(0x1000ff)|VFS_MOUNT_ATTR_NOSYMFOLLOW))return -KERNEL_EINVAL;
    if(attr->propagation&~UINT64_C(1966080) || (attr->propagation&(attr->propagation-1)))return -KERNEL_EINVAL;
    if((attr->clear&0x70U) && (attr->clear&0x70U)!=0x70U)return -KERNEL_EINVAL;
    if((attr->set&0x70U) && !(attr->clear&0x70U))return -KERNEL_EINVAL;
    if((attr->set&0x70U)!=0 && (attr->set&0x70U)!=0x10U && (attr->set&0x70U)!=0x20U)return -KERNEL_EINVAL;
    if((attr->set|attr->clear)&0x100000U)return -KERNEL_EOPNOTSUPP;
    return 0;
}
struct mount_api_context {
    struct kernel_heap *heap;
    struct kernel_fs_paths paths;
    struct kernel_vfs_path *source,*target;
    struct kernel_files_pin_guard source_fd,target_fd;
    struct kernel_lock_guard topology;
    char name[KERNEL_FS_PATH_MAX];
};
static void mount_api_finish(struct mount_api_context **owner)
{
    struct mount_api_context *call=*owner;
    if(!call)return;
    kernel_lock_scope_release(&call->topology);
    kernel_files_path_guard_release(&call->target);kernel_files_path_guard_release(&call->source);
    kernel_files_pin_guard_release(&call->target_fd);kernel_files_pin_guard_release(&call->source_fd);
    kernel_fs_paths_release(&call->paths);
    kernel_heap_release(call->heap,call);
}
static int mount_api(struct kernel_task *caller,struct kernel_mm *mm,
    const struct kernel_fs_context *fs,const struct kernel_syscall_request *request)
{
    struct kernel_files *files;
    if(kernel_task_files_borrow(caller,&files))__builtin_trap();
    unsigned nr=request->number,flags=nr==41?0:(unsigned)request->arguments[nr==429?4:2];
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    if(nr==428) {
        uint32_t fd;int error;
        if(kernel_files_find_free_fd(files,&fd,&error))__builtin_trap();
        if(error)return error;
        reservation=(struct kernel_files_fd_reservation){files,fd};
    }
    struct native_mount_attributes attr={0};
    if(nr==428) {
        if(flags&~(0x1000U|0x800U|0x8000U|0x100U|02000000U|3U))return -KERNEL_EINVAL;
        if((flags&0x8000U) && !(flags&3U))return -KERNEL_EINVAL;
        if((flags&3U)==3U)return -KERNEL_EINVAL;
    }
    if(nr==429) {
        if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
        if(flags&~0x377U || (flags&0x300U)==0x300U)return -KERNEL_EINVAL;
    }
    if(nr==442) {
        int error=import_mount_attributes(mm,request,&attr);
        if(error)return error;
        if(!attr.set && !attr.clear && !attr.propagation)return 0;
    }
    if(nr==428 && (flags&3U) &&
       !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
    /* These owners span lookup and publication. Reuse the existing pathname
     * allocation for their storage instead of retaining a deep stack frame. */
    struct mount_api_context *call __attribute__((cleanup(mount_api_finish)))=NULL;
    if(kernel_heap_allocate_zeroed(fs->heap,1,sizeof(*call),(void **)&call))return -KERNEL_ENOMEM;
    call->heap=fs->heap;call->paths=kernel_fs_context_snapshot(fs);
    char *names=call->name;
    int error;
    kernel_vfs_mount_topology_lock(&call->topology);
    if(nr==429) {
        if(!request->arguments[3] && (flags&64U))names[0]=0;
        else if((error=import_string(mm,names,KERNEL_FS_PATH_MAX,request->arguments[3])))goto done;
        error=mount_api_path(files,&call->paths,(int)request->arguments[2],names,flags&64U,flags&16U,&call->target,&call->target_fd);
        if(error)goto done;
    }
    uint64_t address=request->arguments[nr==41?0:1];
    if(nr==429 && !address && (flags&4U))names[0]=0;
    else if((error=import_string(mm,names,KERNEL_FS_PATH_MAX,address)))goto done;
    error=mount_api_path(files,&call->paths,nr==41?KERNEL_FS_AT_FDCWD:(int)request->arguments[0],names,
        nr==429?flags&4U:flags&0x1000U,nr==429?flags&1U:!(flags&0x100U),&call->source,&call->source_fd);
    if(error)goto done;
    struct kernel_vfs_mount *top=kernel_fs_context_mount_root(fs);
    uint64_t namespace_id=kernel_fs_context_namespace_id(fs);
    int source_here=kernel_vfs_mount_contains(top,call->source);
    int source_anon=anonymous_tree(call->source,namespace_id);
    if(nr==428) {
        if((flags&3U) && !source_here && !source_anon){error=-KERNEL_EINVAL;goto done;}
        struct kernel_open_file_description *file=NULL;
        if(flags&2U)error=kernel_mount_namespace_open_tree(call->source,fs->heap,!!(flags&0x8000U),&file);
        else {
            struct kernel_vfs_path *clone=NULL;
            if(flags&1U) {
                error=kernel_vfs_clone_tree(call->source,!!(flags&0x8000U),&clone);if(error)goto done;
                kernel_vfs_path_mount(clone)->detached_origin=namespace_id;
            }
            error=kernel_open_file_create_path(fs->heap,clone?clone:call->source,
                clone?kernel_vfs_path_mount(clone):NULL,&file);
            if(error && clone)kernel_vfs_close_tree(kernel_vfs_path_mount(clone));
            if(clone && kernel_vfs_path_release(&clone))__builtin_trap();
        }
        if(error)goto done;
        unsigned fd=reservation.fd;
        if(kernel_files_install_new_owned_at(files,fd,!!(flags&02000000U),&file))__builtin_trap();
        reservation.files=NULL;error=(int)fd;
    } else if(nr==442) {
        error=(!source_here && !(source_anon && mount_tree_top(call->source)==kernel_vfs_path_mount(call->source)))?
            -KERNEL_EINVAL:kernel_vfs_set_mount_attributes(call->source,attr.set,attr.clear,attr.propagation,flags&0x8000U);
    } else if(nr==429) {
        int target_here=kernel_vfs_mount_contains(top,call->target);
        if(flags&0x100U) {
            struct kernel_vfs_mount *from=mount_tree_top(call->source),*to=mount_tree_top(call->target);
            error=(from->retired||__atomic_load_n(&from->namespace_retiring,__ATOMIC_ACQUIRE)||
                to->retired||__atomic_load_n(&to->namespace_retiring,__ATOMIC_ACQUIRE))?-KERNEL_EINVAL:
                kernel_vfs_move_mount(call->source,call->target,flags&0x300U);
        } else if(source_here)error=target_here?kernel_vfs_move_mount(call->source,call->target,flags&0x300U):-KERNEL_EINVAL;
        else error=source_anon && mount_tree_top(call->source)==kernel_vfs_path_mount(call->source) &&
            (target_here || (anonymous_tree(call->target,namespace_id) && mount_tree_top(call->source)!=mount_tree_top(call->target)))?
            kernel_vfs_move_mount(call->source,call->target,flags&0x300U):-KERNEL_EINVAL;
    }
    else {
        error=((kernel_vfs_path_mode(call->source)&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR)?0:-KERNEL_ENOTDIR;
        if(!error)error=import_string(mm,names,KERNEL_FS_PATH_MAX,request->arguments[1]);
        if(!error)error=mount_api_path(files,&call->paths,KERNEL_FS_AT_FDCWD,names,0,1,&call->target,&call->target_fd);
        if(!error && !((kernel_vfs_path_mode(call->target)&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR))error=-KERNEL_ENOTDIR;
        if(!error && !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))error=-KERNEL_EPERM;
        if(!error && (!source_here || !kernel_vfs_mount_contains(top,call->target)))error=-KERNEL_EINVAL;
        if(!error)error=kernel_vfs_pivot_root(fs,call->source,call->target);
    }
done:
    return error;
}

static int install_context_file(struct kernel_files *files,struct kernel_open_file_description **owner,int cloexec)
{
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    uint32_t fd;int error;
    if(kernel_files_find_free_fd(files,&fd,&error))__builtin_trap();
    if(error){if(kernel_open_file_release(owner))__builtin_trap();return error;}
    reservation=(struct kernel_files_fd_reservation){files,fd};
    if(kernel_files_install_new_owned_at(files,fd,cloexec,owner))__builtin_trap();
    reservation.files=NULL;return (int)fd;
}
struct fsconfig_input {char key[256],value[KERNEL_FS_PATH_MAX],options[KERNEL_MOUNT_OPTIONS_MAX];};
static __attribute__((noinline)) int configure_context(struct kernel_task *caller,struct kernel_mm *mm,
    const struct kernel_fs_context *fs,const struct kernel_syscall_request *request)
{
    int fd=(int32_t)request->arguments[0],aux=(int32_t)request->arguments[4];
    unsigned command=(unsigned)request->arguments[1];
    uint64_t key=request->arguments[2],value=request->arguments[3];
    if(fd<0)return -KERNEL_EINVAL;
    switch(command){
    case 0:if(!key||value||aux)return -KERNEL_EINVAL;break;
    case 1:if(!key||!value||aux)return -KERNEL_EINVAL;break;
    case 2:if(!key||!value||aux<=0||aux>1048576)return -KERNEL_EINVAL;break;
    case 3:case 4:if(!key||!value||(aux!=KERNEL_FS_AT_FDCWD&&aux<0))return -KERNEL_EINVAL;break;
    case 5:if(!key||value||aux<0)return -KERNEL_EINVAL;break;
    case 6:case 7:case 8:if(key||value||aux)return -KERNEL_EINVAL;break;
    default:return -KERNEL_EOPNOTSUPP;
    }
    struct kernel_files *files;if(kernel_task_files_borrow(caller,&files))__builtin_trap();
    KERNEL_FILES_PIN_SCOPE(pin);KERNEL_FILES_PIN_SCOPE(value_pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    if(!file)return -KERNEL_EBADF;
    if(kernel_open_file_kind(file)!=KERNEL_OPEN_FILE_KIND_FS_CONTEXT)return -KERNEL_EINVAL;
    if(command>=6)return kernel_mount_context_configure(file,fs,command,NULL,NULL,NULL);
    struct fsconfig_input *input=NULL;
    if(kernel_heap_allocate(fs->heap,sizeof(*input),(void **)&input))return -KERNEL_ENOMEM;
    int error=import_string(mm,input->key,sizeof(input->key),key);
    if(error==-KERNEL_ENAMETOOLONG)error=-KERNEL_EINVAL;
    void *blob=NULL;
    if(!error && command==2){
        if(kernel_heap_allocate(fs->heap,(size_t)aux,(void **)&blob))error=-KERNEL_ENOMEM;
        else {size_t copied;if(kernel_copy_from_user(mm,blob,value,(size_t)aux,&copied))error=-KERNEL_EFAULT;}
    } else if(!error && (command==1||command==3||command==4)){
        error=import_string(mm,input->value,command==1?256:sizeof(input->value),value);
        if(command==1&&error==-KERNEL_ENAMETOOLONG)error=-KERNEL_EINVAL;
        if(!error&&command==3&&!input->value[0])error=-KERNEL_ENOENT;
    } else if(!error && command==5 && !kernel_files_hold_fd(files,aux,&value_pin))error=-KERNEL_EBADF;
    if(!error)error=kernel_mount_context_configure(file,fs,command,input->key,
        command==1||command==3||command==4?input->value:NULL,input->options);
    if(blob)kernel_heap_release(fs->heap,blob);
    kernel_heap_release(fs->heap,input);return error;
}
static __attribute__((noinline)) int filesystem_api(struct kernel_task *caller,struct kernel_mm *mm,
    const struct kernel_fs_context *fs,const struct kernel_syscall_request *request)
{
    if(request->number==431)return configure_context(caller,mm,fs,request);
    struct kernel_files *files;if(kernel_task_files_borrow(caller,&files))__builtin_trap();
    unsigned flags=(unsigned)request->arguments[request->number==430?1:request->number==432?1:2];
    if(request->number==432){
        if(flags&~3U)return -KERNEL_EINVAL;
        if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
        unsigned attrs=(unsigned)request->arguments[2];
        if(attrs&~(0xffU|VFS_MOUNT_ATTR_NOSYMFOLLOW))return -KERNEL_EINVAL;
        if((attrs&0x70U)!=0 && (attrs&0x70U)!=0x10U && (attrs&0x70U)!=0x20U)return -KERNEL_EINVAL;
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *file=kernel_files_hold_fd(files,(int32_t)request->arguments[0],&pin);
        if(!file)return -KERNEL_EBADF;
        struct kernel_open_file_description *result=NULL;
        int error=kernel_mount_context_mount(file,fs,flags,attrs,&result);
        return error?error:install_context_file(files,&result,flags&1U);
    }
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))return -KERNEL_EPERM;
    if(flags&~(request->number==430?1U:15U))return -KERNEL_EINVAL;
    if(request->number==430){
        char *type=NULL;
        if(kernel_heap_allocate(fs->heap,BOAROS_PAGE_SIZE,(void **)&type))return -KERNEL_ENOMEM;
        int error=import_string(mm,type,BOAROS_PAGE_SIZE,request->arguments[0]);
        struct kernel_open_file_description *file=NULL;
        if(error==-KERNEL_ENAMETOOLONG)error=-KERNEL_EINVAL;
        if(!error && !kernel_mount_type_supported(type))error=-KERNEL_ENODEV;
        if(!error)error=kernel_mount_context_open(fs->heap,type,NULL,&file);
        kernel_heap_release(fs->heap,type);
        return error?error:install_context_file(files,&file,flags&1U);
    }

    struct mount_api_context *call __attribute__((cleanup(mount_api_finish)))=NULL;
    if(kernel_heap_allocate_zeroed(fs->heap,1,sizeof(*call),(void **)&call))return -KERNEL_ENOMEM;
    call->heap=fs->heap;call->paths=kernel_fs_context_snapshot(fs);
    int error=import_string(mm,call->name,sizeof(call->name),request->arguments[1]);
    if(error)return error;
    error=mount_api_path(files,&call->paths,(int32_t)request->arguments[0],call->name,flags&8U,!(flags&2U),&call->source,&call->source_fd);
    if(error)return error;
    if(!kernel_vfs_path_is_mount_root(call->source))return -KERNEL_EINVAL;
    struct kernel_open_file_description *file=NULL;
    error=kernel_mount_context_open(fs->heap,NULL,call->source,&file);
    return error?error:install_context_file(files,&file,flags&1U);
}

enum kernel_syscall_status syscall_handle_mount(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded, int unmount)
{
    KERNEL_SERVICE_SCOPE(service);
    const struct kernel_fs_context *fs;
    struct kernel_mm *mm;
    if (kernel_task_fs_context_borrow(caller, &fs) != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = request->number>=430 && request->number<=433 ? filesystem_api(caller,mm,fs,request) : (request->number==41 || request->number==428 || request->number==429 || request->number==442)
        ? mount_api(caller,mm,fs,request) : unmount ? unmount_filesystem(mm, fs, request)
                             : mount_filesystem(mm, fs, request);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum {
    STATMOUNT_SB_BASIC=1U,
    STATMOUNT_MNT_BASIC=2U,
    STATMOUNT_PROPAGATE_FROM=4U,
    STATMOUNT_MNT_ROOT=8U,
    STATMOUNT_MNT_POINT=16U,
    STATMOUNT_FS_TYPE=32U,
    STATMOUNT_MNT_NS_ID=64U,
    STATMOUNT_MNT_OPTS=128U,
    STATMOUNT_FS_SUBTYPE=256U,
    STATMOUNT_SB_SOURCE=512U,
    STATMOUNT_OPT_ARRAY=1024U,
    STATMOUNT_OPT_SEC_ARRAY=2048U,
    STATMOUNT_SUPPORTED_MASK=4096U,
    STATMOUNT_MNT_UIDMAP=8192U,
    STATMOUNT_MNT_GIDMAP=16384U,
    STATMOUNT_BY_FD=1U,
    LISTMOUNT_REVERSE=1U,
};
#define STATMOUNT_STRINGS (STATMOUNT_MNT_ROOT|STATMOUNT_MNT_POINT|STATMOUNT_FS_TYPE|STATMOUNT_SB_SOURCE)
#define STATMOUNT_LINUX_STRINGS (STATMOUNT_STRINGS|STATMOUNT_MNT_OPTS|STATMOUNT_FS_SUBTYPE| \
    STATMOUNT_OPT_ARRAY|STATMOUNT_OPT_SEC_ARRAY|STATMOUNT_MNT_UIDMAP|STATMOUNT_MNT_GIDMAP)
#define STATMOUNT_SUPPORTED (STATMOUNT_SB_BASIC|STATMOUNT_MNT_BASIC|STATMOUNT_PROPAGATE_FROM| \
    STATMOUNT_STRINGS|STATMOUNT_MNT_NS_ID|STATMOUNT_SUPPORTED_MASK)
#define LISTMOUNT_ROOT UINT64_MAX
#define STATMOUNT_STRING_CAPACITY (3U*KERNEL_FS_PATH_MAX+256U)

struct native_mount_query {
    uint32_t size,fd;
    uint64_t id,param,namespace_id;
};
struct native_statmount {
    uint32_t size,mount_options;
    uint64_t mask;
    uint32_t device_major,device_minor;
    uint64_t magic;
    uint32_t sb_flags,fs_type;
    uint64_t id,parent_id;
    uint32_t legacy_id,legacy_parent_id;
    uint64_t attributes,propagation,peer,master,propagate_from;
    uint32_t mount_root,mount_point;
    uint64_t namespace_id;
    uint32_t fs_subtype,source,option_count,option_array,security_count,security_array;
    uint64_t supported;
    uint32_t uid_count,uid_map,gid_count,gid_map;
    uint64_t spare[43];
};
_Static_assert(offsetof(struct native_mount_query,namespace_id)==24 &&
    sizeof(struct native_mount_query)==32,"native mount query request");
_Static_assert(sizeof(struct native_statmount)==512,"native statmount result");

static int import_mount_query(struct kernel_mm *mm,uint64_t address,bool by_fd,
    struct native_mount_query *request)
{
    uint32_t size;
    size_t copied;
    if (kernel_copy_from_user(mm,&size,address,sizeof(size),&copied)) return -KERNEL_EFAULT;
    if (size>BOAROS_PAGE_SIZE) return -KERNEL_E2BIG;
    if (size<24U) return -KERNEL_EINVAL;
    if (kernel_user_range_check(address,size)) return -KERNEL_EFAULT;

    /* The Linux structure import validates future bytes before known fields. */
    for (size_t offset=sizeof(*request);offset<size;) {
        unsigned char bytes[64];
        size_t count=size-offset;
        if (count>sizeof(bytes)) count=sizeof(bytes);
        size_t page_left=BOAROS_PAGE_SIZE-((address+offset)&BOAROS_PAGE_MASK);
        if (count>page_left) count=page_left;
        if (kernel_copy_from_user(mm,bytes,address+offset,count,&copied)) return -KERNEL_EFAULT;
        for (size_t i=0;i<count;i++) if (bytes[i]) return -KERNEL_E2BIG;
        offset+=count;
    }
    *request=(struct native_mount_query){0};
    size_t count=size<sizeof(*request)?size:sizeof(*request);
    if (kernel_copy_from_user(mm,request,address,count,&copied)) return -KERNEL_EFAULT;
    if (by_fd) return request->id || request->namespace_id ? -KERNEL_EINVAL : 0;
    if (request->fd && request->namespace_id) return -KERNEL_EINVAL;
    return request->id<=KERNEL_VFS_MOUNT_UNIQUE_ID_BASE ? -KERNEL_EINVAL : 0;
}

static struct kernel_vfs_mount *find_mount_id(struct kernel_vfs_mount *top,uint64_t id)
{
    for (struct kernel_vfs_mount *m=top;m;m=kernel_vfs_mount_next(m,top))
        if (kernel_vfs_unique_mount_id(m->id)==id) return m;
    return NULL;
}

/* The pin covers querying another namespace while its original members exit. */
static int resolve_mount_query_fd(struct kernel_task *caller,const struct native_mount_query *request,
    bool by_fd,struct kernel_files_pin_guard *pin,struct kernel_mount_namespace **namespace,
    struct kernel_vfs_mount **mount)
{
    if (!by_fd && !request->fd) return 0;
    struct kernel_files *files;
    if (kernel_task_files_borrow(caller,&files)) return -KERNEL_EBADF;
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,(int32_t)request->fd,pin);
    if (!file) return -KERNEL_EBADF;
    if (by_fd) {
        struct kernel_vfs_path *path=kernel_open_file_path(file);
        if (!path) return -KERNEL_EOPNOTSUPP;
        *mount=kernel_vfs_path_mount(path);
    } else {
        if (kernel_open_file_kind(file)==KERNEL_OPEN_FILE_KIND_PATH) return -KERNEL_EBADF;
        *namespace=kernel_mount_namespace_file(file);
        if (!*namespace) return -KERNEL_EINVAL;
    }
    return 0;
}

static int mount_query_root(const struct kernel_fs_mounts *current,struct kernel_mount_namespace *ns,
    uint64_t namespace_id,struct kernel_vfs_path **foreign,struct kernel_vfs_path **root,
    struct kernel_vfs_mount **top)
{
    if (namespace_id && namespace_id!=kernel_mount_namespace_id(current->namespace))
        return -KERNEL_EOPNOTSUPP;
    if (!ns || ns==current->namespace) {
        *root=current->root;
        *top=kernel_vfs_path_mount(current->top);
        return 0;
    }
    int result=kernel_mount_namespace_top_pin(ns,foreign);
    if (result) return result;
    *top=kernel_vfs_path_mount(*foreign);
    *root=*foreign;
    return 0;
}

static int mount_query_visible(struct kernel_vfs_mount *mount,const struct kernel_vfs_path *root)
{
    return kernel_vfs_mount_reachable(mount,root) ||
        kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN);
}

static uint64_t statmount_attributes(const struct kernel_vfs_mount *mount)
{
    uint64_t attrs=(mount->read_only?1U:0U) | (mount->nosuid?2U:0U) |
        (mount->nodev?4U:0U) | (mount->noexec?8U:0U) |
        (mount->nosymfollow?VFS_MOUNT_ATTR_NOSYMFOLLOW:0U);
    if (mount->atime_policy&VFS_ATIME_NONE) attrs|=0x10U;
    else if (mount->atime_policy&VFS_ATIME_STRICT) attrs|=0x20U;
    if (mount->atime_policy&VFS_ATIME_NODIR) attrs|=0x80U;
    return attrs;
}

struct statmount_output {
    struct native_statmount header;
    size_t string_bytes,string_capacity,user_capacity;
    char strings[];
};

static int statmount_string(struct statmount_output *output,uint32_t flag,uint32_t *offset,
    const char *literal,const struct kernel_vfs_mount *mount,const struct kernel_vfs_path *root)
{
    size_t start=output->string_bytes?output->string_bytes:1U;
    char *destination=output->strings+start;
    size_t available=output->string_capacity-start;
    if (literal) {
        size_t bytes=strlen(literal)+1;
        if (bytes>available) return -KERNEL_EOVERFLOW;
        memcpy(destination,literal,bytes);
    } else {
        int result=kernel_vfs_mount_root_string(mount,root,destination,available);
        if (result==-KERNEL_ENOENT && flag==STATMOUNT_MNT_POINT) return 0;
        if (result) return result==-KERNEL_ERANGE ? -KERNEL_EOVERFLOW : result;
    }
    if (!destination[0]) return 0;
    size_t end=start+strlen(destination)+1;
    if (sizeof(output->header)+end>output->user_capacity) return -KERNEL_EOVERFLOW;
    if (!output->string_bytes) output->strings[0]=0;
    *offset=(uint32_t)start;
    output->string_bytes=end;
    output->header.mask|=flag;
    return 0;
}

static void statmount_fill_basic(struct native_statmount *header,const struct kernel_vfs_mount *mount,
    uint64_t mask)
{
    const struct kernel_vfs_instance *instance=mount->private_data;
    KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
    if (mask&STATMOUNT_SB_BASIC) {
        uint64_t device=instance->device_id;
        header->device_major=(uint32_t)(((device>>8)&0xfff)|((device>>32)&0xfffff000));
        header->device_minor=(uint32_t)((device&0xff)|((device>>12)&0xffffff00));
        header->sb_flags=instance->read_only?1U:0U;
    }
    if (mask&STATMOUNT_MNT_BASIC) {
        const struct kernel_vfs_mount *parent=mount->parent?mount->parent:mount;
        header->mask|=STATMOUNT_MNT_BASIC;
        header->id=kernel_vfs_unique_mount_id(mount->id);
        header->parent_id=kernel_vfs_unique_mount_id(parent->id);
        header->legacy_id=(uint32_t)mount->id;
        header->legacy_parent_id=(uint32_t)parent->id;
        header->attributes=statmount_attributes(mount);
        header->peer=mount->propagation_group;
        header->master=mount->master?mount->master->propagation_group:0;
        header->propagation=(header->peer?1048576U:0U) |
            (mount->master?524288U:0U) | (mount->unbindable?131072U:0U);
        if (!header->propagation) header->propagation=262144U;
    }
}

static int statmount_capture(const struct kernel_fs_context *fs,const struct native_mount_query *request,
    struct kernel_mount_namespace *ns,struct kernel_vfs_mount *mount,struct statmount_output *output,
    struct kernel_vfs_mount **superblock_view)
{
    struct kernel_fs_mounts current __attribute__((cleanup(kernel_fs_mounts_release)))={0};
    struct kernel_vfs_path *foreign __attribute__((cleanup(kernel_files_path_guard_release)))=NULL;
    KERNEL_LOCK_SCOPE(topology);
    kernel_vfs_mount_topology_lock(&topology);
    current=kernel_fs_context_mounts_snapshot(fs);
    struct kernel_vfs_path *root=current.root;
    uint64_t namespace_id;
    if (mount) {
        namespace_id=kernel_mount_namespace_of_view(mount,&foreign);
        if (namespace_id!=kernel_mount_namespace_id(current.namespace)) root=foreign;
    } else {
        struct kernel_vfs_mount *top;
        int result=mount_query_root(&current,ns,request->namespace_id,&foreign,&root,&top);
        if (result) return result;
        mount=find_mount_id(top,request->id);
        if (!mount) return -KERNEL_ENOENT;
        if (!mount_query_visible(mount,root)) return -KERNEL_EPERM;
        namespace_id=kernel_mount_namespace_id(ns?ns:current.namespace);
    }

    uint64_t mask=request->param&STATMOUNT_SUPPORTED;
    struct native_statmount *header=&output->header;
    statmount_fill_basic(header,mount,mask);
    if (mask&STATMOUNT_PROPAGATE_FROM) {
        header->mask|=STATMOUNT_PROPAGATE_FROM;
        header->propagate_from=kernel_vfs_mount_dominating_group(mount,current.root);
    }
    if ((mask&STATMOUNT_MNT_NS_ID) && namespace_id) {
        header->mask|=STATMOUNT_MNT_NS_ID;
        header->namespace_id=namespace_id;
    }
    if (mask&STATMOUNT_SUPPORTED_MASK) {
        header->mask|=STATMOUNT_SUPPORTED_MASK;
        header->supported=STATMOUNT_SUPPORTED;
    }
    int result=0;
    if (mask&STATMOUNT_FS_TYPE)
        result=statmount_string(output,STATMOUNT_FS_TYPE,&header->fs_type,kernel_vfs_mount_type(mount),NULL,NULL);
    if (!result && (mask&STATMOUNT_MNT_ROOT))
        result=statmount_string(output,STATMOUNT_MNT_ROOT,&header->mount_root,NULL,mount,NULL);
    if (!result && root && (mask&STATMOUNT_MNT_POINT))
        result=statmount_string(output,STATMOUNT_MNT_POINT,&header->mount_point,NULL,mount,root);
    if (!result && (mask&STATMOUNT_SB_SOURCE)) {
        const char *source=mount->source_name?mount->source_name:"none";
        result=statmount_string(output,STATMOUNT_SB_SOURCE,&header->source,source,NULL,NULL);
    }
    if (!result && (mask&STATMOUNT_SB_BASIC)) {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        result=kernel_vfs_mount_view_acquire_locked(mount);
        if (!result) *superblock_view=mount;
    }
    return result;
}

static int statmount_query(struct kernel_task *caller,struct kernel_mm *mm,const struct kernel_fs_context *fs,
    const uint64_t args[])
{
    unsigned flags=(uint32_t)args[3];
    if (flags&~STATMOUNT_BY_FD) return -KERNEL_EINVAL;
    struct native_mount_query request;
    int result=import_mount_query(mm,args[0],flags&STATMOUNT_BY_FD,&request);
    if (result) return result;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_mount_namespace *ns=NULL;
    struct kernel_vfs_mount *mount=NULL;
    result=resolve_mount_query_fd(caller,&request,flags&STATMOUNT_BY_FD,&pin,&ns,&mount);
    if (result) return result;
    if (kernel_user_range_check(args[1],args[2])) return -KERNEL_EFAULT;
    if (args[2]==sizeof(struct native_statmount) && (request.param&STATMOUNT_LINUX_STRINGS))
        return -KERNEL_EOVERFLOW;

    size_t string_capacity=request.param&STATMOUNT_STRINGS ? STATMOUNT_STRING_CAPACITY : 0;
    struct statmount_output *output;
    if (kernel_heap_allocate(fs->heap,sizeof(*output)+string_capacity,(void **)&output))
        return -KERNEL_ENOMEM;
    *output=(struct statmount_output){.string_capacity=string_capacity,.user_capacity=args[2]};
    struct kernel_vfs_mount *view=NULL;
    result=statmount_capture(fs,&request,ns,mount,output,&view);
    if (view) {
        struct kernel_vfs_statfs stat;
        /* Backends may sleep. A view reference keeps the instance alive. */
        result=kernel_vfs_mount_statfs(view,&stat);
        if (!result) {
            output->header.mask|=STATMOUNT_SB_BASIC;
            output->header.magic=stat.type;
        }
        kernel_vfs_mount_view_release(view);
    }
    if (!result) {
        size_t fixed=output->user_capacity<sizeof(output->header)?output->user_capacity:sizeof(output->header);
        size_t copied;
        output->header.size=(uint32_t)(fixed+output->string_bytes);
        /* Linux writes the strings first, then copies the fixed prefix. */
        if (output->string_bytes &&
            kernel_copy_to_user(mm,args[1]+sizeof(output->header),output->strings,output->string_bytes,&copied))
            result=-KERNEL_EFAULT;
        if (!result && fixed && kernel_copy_to_user(mm,args[1],&output->header,fixed,&copied))
            result=-KERNEL_EFAULT;
    }
    kernel_heap_release(fs->heap,output);
    return result;
}

static int collect_mount_ids(const struct kernel_fs_context *fs,const struct native_mount_query *request,
    struct kernel_mount_namespace *ns,uint64_t *ids,size_t capacity,size_t *count,bool reverse,bool empty)
{
    struct kernel_fs_mounts current __attribute__((cleanup(kernel_fs_mounts_release)))={0};
    struct kernel_vfs_path *foreign __attribute__((cleanup(kernel_files_path_guard_release)))=NULL;
    KERNEL_LOCK_SCOPE(topology);
    kernel_vfs_mount_topology_lock(&topology);
    current=kernel_fs_context_mounts_snapshot(fs);
    struct kernel_vfs_path *root;
    struct kernel_vfs_mount *top;
    int result=mount_query_root(&current,ns,request->namespace_id,&foreign,&root,&top);
    if (result) return result;
    *count=0;
    struct kernel_vfs_mount *base=kernel_vfs_path_mount(root);
    struct kernel_vfs_path *origin=root;
    if (request->id!=LISTMOUNT_ROOT) {
        base=find_mount_id(top,request->id);
        if (!base) return -KERNEL_ENOENT;
        if (!mount_query_visible(base,root)) return -KERNEL_EPERM;
        origin=NULL;
    } else if (!kernel_vfs_mount_contains(top,root)) return 0;

    if (empty) return 0;
    uint64_t cursor=request->param;
    if (!reverse && cursor==UINT64_MAX) cursor=0;
    for (struct kernel_vfs_mount *m=base;m;m=kernel_vfs_mount_next(m,base)) {
        uint64_t id=kernel_vfs_unique_mount_id(m->id);
        if (id==request->id || (cursor && (reverse?id>=cursor:id<=cursor))) continue;
        if (origin && !kernel_vfs_mount_reachable(m,origin)) continue;
        if (*count<capacity) ids[*count]=id;
        (*count)++;
    }
    return 0;
}

static int compare_mount_ids(const void *one,const void *two)
{
    uint64_t a=*(const uint64_t *)one,b=*(const uint64_t *)two;
    return (a>b)-(a<b);
}

static long listmount_query(struct kernel_task *caller,struct kernel_mm *mm,const struct kernel_fs_context *fs,
    const uint64_t args[])
{
    unsigned flags=(uint32_t)args[3];
    size_t limit=args[2];
    if (flags&~LISTMOUNT_REVERSE) return -KERNEL_EINVAL;
    if (limit>1000000U) return -KERNEL_EOVERFLOW;
    if (kernel_user_range_check(args[1],limit*sizeof(uint64_t))) return -KERNEL_EFAULT;
    struct native_mount_query request;
    int result=import_mount_query(mm,args[0],false,&request);
    if (result) return result;
    if (request.param && request.param<=KERNEL_VFS_MOUNT_UNIQUE_ID_BASE) return -KERNEL_EINVAL;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_mount_namespace *ns=NULL;
    struct kernel_vfs_mount *unused=NULL;
    result=resolve_mount_query_fd(caller,&request,false,&pin,&ns,&unused);
    if (result) return result;

    uint64_t *ids=NULL;
    size_t capacity=0,count=0;
    for (;;) {
        result=collect_mount_ids(fs,&request,ns,ids,capacity,&count,flags&LISTMOUNT_REVERSE,!limit);
        if (result || count<=capacity) break;
        if (ids) kernel_heap_release(fs->heap,ids);
        ids=NULL;
        if (count>SIZE_MAX/sizeof(*ids) ||
            kernel_heap_allocate(fs->heap,count*sizeof(*ids),(void **)&ids)) return -KERNEL_ENOMEM;
        capacity=count;
    }
    if (!result && count) {
        qsort(ids,count,sizeof(*ids),compare_mount_ids);
        size_t output_count=count<limit?count:limit;
        uint64_t *source=ids;
        if (flags&LISTMOUNT_REVERSE) {
            source=ids+count-output_count;
            for (size_t i=0;i<output_count/2;i++) {
                size_t tail=output_count-1-i;
                uint64_t value=source[i];
                source[i]=source[tail];
                source[tail]=value;
            }
        }
        size_t copied;
        result=kernel_copy_to_user(mm,args[1],source,output_count*sizeof(*ids),&copied) ?
            -KERNEL_EFAULT : (int)output_count;
    }
    if (ids) kernel_heap_release(fs->heap,ids);
    return result;
}

static enum kernel_syscall_status mount_query_syscall(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded,bool list)
{
    const struct kernel_fs_context *fs;
    struct kernel_mm *mm;
    if (kernel_task_fs_context_borrow(caller,&fs) || kernel_task_mm_borrow_mutable(caller,&mm))
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=list?listmount_query(caller,mm,fs,request->arguments):
        statmount_query(caller,mm,fs,request->arguments);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_statmount(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{ return mount_query_syscall(caller,request,decoded,false); }

enum kernel_syscall_status syscall_handle_listmount(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{ return mount_query_syscall(caller,request,decoded,true); }
