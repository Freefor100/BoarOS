#include <kernel/credentials.h>
#include <kernel/scheduler.h>
#include "private.h"
#include "../../fs/vfs_internal.h"
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
