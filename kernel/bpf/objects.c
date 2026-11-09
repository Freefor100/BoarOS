#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <string.h>
#include "../../fs/open_file_internal.h"
#include "../../fs/char_device_internal.h"
#include "../../fs/files/private.h"
#include "../../fs/uaccess_iov_internal.h"

static struct kernel_raw_lock objects_lock={
    .rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED
};
static struct bpf_object *roots[BPF_OBJECT_KINDS];
static uint32_t next_ids[BPF_OBJECT_KINDS]={1,1,1,1,1};
static struct kernel_mutex initialize_mutex;
static unsigned initialize_mutex_ready;
int kernel_bpf_initialize(struct kernel_heap *heap)
{
    extern const unsigned char __boaros_btf_start[] __attribute__((weak));
    extern const unsigned char __boaros_btf_end[] __attribute__((weak));
    if(bpf_btf_kernel())return 0;
    {
        KERNEL_RAW_SCOPE(guard,&objects_lock);
        if(!initialize_mutex_ready){
            /* sysfs creation holds mount topology (rank 18). The immutable
             * kernel BTF is constructed before any object is published. */
            kernel_mutex_init(&initialize_mutex,19,(uintptr_t)&initialize_mutex);
            initialize_mutex_ready=1;
        }
    }
    KERNEL_LOCK_SCOPE(lock);kernel_mutex_lock(&initialize_mutex,&lock);
    if(bpf_btf_kernel())return 0;
    if(!__boaros_btf_start || !__boaros_btf_end ||
        (uintptr_t)__boaros_btf_end<=(uintptr_t)__boaros_btf_start)return -KERNEL_ENODATA;
    size_t size=(size_t)(__boaros_btf_end-__boaros_btf_start);
    if(size>UINT32_MAX)return -KERNEL_E2BIG;
    return bpf_btf_kernel_initialize(heap,__boaros_btf_start,(uint32_t)size);
}
int kernel_bpf_shutdown(struct kernel_heap *heap)
{
    if(!bpf_btf_kernel())return 0;
    KERNEL_LOCK_SCOPE(lock);kernel_mutex_lock(&initialize_mutex,&lock);
    struct bpf_btf *kernel=bpf_btf_kernel();
    if(!kernel)return 0;
    if(kernel->object.heap!=heap)return -KERNEL_EBUSY;
    {
        KERNEL_RAW_SCOPE(guard,&objects_lock);
        for(unsigned kind=0;kind<BPF_OBJECT_KINDS;kind++){
            struct bpf_object *expected=kind==BPF_OBJECT_BTF?&kernel->object:NULL;
            if(roots[kind]!=expected || (expected && (expected->left || expected->right)))
                return -KERNEL_EBUSY;
        }
        if(__atomic_load_n(&kernel->object.references,__ATOMIC_ACQUIRE)!=1)return -KERNEL_EBUSY;
    }
    bpf_btf_kernel_retire();
    {KERNEL_RAW_SCOPE(guard,&objects_lock);for(unsigned i=0;i<BPF_OBJECT_KINDS;i++)next_ids[i]=1;}
    return 0;
}
static unsigned height(struct bpf_object *n) {return n?n->height:0;}
static void set_height(struct bpf_object *n)
{unsigned a=height(n->left),b=height(n->right);n->height=1+(a>b?a:b);}
static struct bpf_object *rotate(struct bpf_object *n,int left)
{
    struct bpf_object *p=left?n->right:n->left;
    if(left){n->right=p->left;p->left=n;}else{n->left=p->right;p->right=n;}
    set_height(n);set_height(p);return p;
}
static struct bpf_object *balance(struct bpf_object *n)
{
    if(!n)return n;
    set_height(n);
    if(height(n->left)>height(n->right)+1){
        if(height(n->left->right)>height(n->left->left))n->left=rotate(n->left,1);
        return rotate(n,0);
    }
    if(height(n->right)>height(n->left)+1){
        if(height(n->right->left)>height(n->right->right))n->right=rotate(n->right,0);
        return rotate(n,1);
    }
    return n;
}
static struct bpf_object *insert(struct bpf_object *n,struct bpf_object *value)
{
    if(!n)return value;
    if(value->id<n->id)n->left=insert(n->left,value);
    else if(value->id>n->id)n->right=insert(n->right,value);
    else __builtin_trap();
    return balance(n);
}
static struct bpf_object *remove_min(struct bpf_object *n,struct bpf_object **minimum)
{
    if(!n->left){*minimum=n;return n->right;}
    n->left=remove_min(n->left,minimum);return balance(n);
}
static struct bpf_object *remove_id(struct bpf_object *n,uint32_t id)
{
    if(!n)__builtin_trap();
    if(id<n->id)n->left=remove_id(n->left,id);
    else if(id>n->id)n->right=remove_id(n->right,id);
    else {
        if(!n->right)return n->left;
        struct bpf_object *p;
        struct bpf_object *right=remove_min(n->right,&p);
        p->left=n->left;p->right=right;n=p;
    }
    return balance(n);
}
static int try_get(struct bpf_object *object)
{
    uint32_t old=__atomic_load_n(&object->references,__ATOMIC_RELAXED);
    while(old){
        if(old==UINT32_MAX)__builtin_trap();
        if(__atomic_compare_exchange_n(&object->references,&old,old+1,1,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    }
    return 0;
}
void bpf_object_initialize(struct bpf_object *object,struct kernel_heap *heap,unsigned kind)
{
    if(kind>=BPF_OBJECT_KINDS)__builtin_trap();
    object->heap=heap;object->kind=kind;object->references=1;object->height=1;
    object->uid=kernel_credentials_current()->uid;
}
int bpf_object_publish(struct bpf_object *object)
{
    KERNEL_RAW_SCOPE(guard,&objects_lock);
    if(object->published)__builtin_trap();
    uint32_t id=next_ids[object->kind];
    if(!id)return -KERNEL_ENOSPC;
    next_ids[object->kind]=id+1;
    object->id=id;object->published=1;
    roots[object->kind]=insert(roots[object->kind],object);
    return 0;
}
void bpf_object_get(struct bpf_object *object)
{if(!object || !try_get(object))__builtin_trap();}
struct bpf_object *bpf_object_by_id(unsigned kind,uint32_t id)
{
    if(kind>=BPF_OBJECT_KINDS)return NULL;
    KERNEL_RAW_SCOPE(guard,&objects_lock);
    struct bpf_object *n=roots[kind];
    while(n && n->id!=id)n=id<n->id?n->left:n->right;
    return n && try_get(n)?n:NULL;
}
int bpf_object_next_id(unsigned kind,uint32_t after,uint32_t *id)
{
    if(kind>=BPF_OBJECT_KINDS)return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(guard,&objects_lock);
    struct bpf_object *n=roots[kind],*best=NULL;
    while(n){if(n->id>after){best=n;n=n->left;}else n=n->right;}
    if(!best)return -KERNEL_ENOENT;
    *id=best->id;return 0;
}
void bpf_object_put(struct bpf_object *object)
{
    if(!object)return;
    uint32_t old=__atomic_fetch_sub(&object->references,1,__ATOMIC_ACQ_REL);
    if(!old)__builtin_trap();
    if(old!=1)return;
    if(object->published){
        KERNEL_RAW_SCOPE(guard,&objects_lock);
        roots[object->kind]=remove_id(roots[object->kind],object->id);
        object->published=0;
    }
    switch(object->kind){
    case BPF_OBJECT_PROGRAM:{
        struct kernel_bpf_program *p=(void *)object;
        bpf_runtime_release(p);
        for(uint32_t i=0;i<p->map_count;i++)bpf_object_put(&p->maps[i]->object);
        if(p->maps)kernel_heap_release(object->heap,p->maps);
        if(p->bindings)kernel_heap_release(object->heap,p->bindings);
        if(p->btf)bpf_object_put(&p->btf->object);
        if(p->function_info)kernel_heap_release(object->heap,p->function_info);
        if(p->line_info)kernel_heap_release(object->heap,p->line_info);
        if(p->classic)kernel_heap_release(object->heap,p->classic);
        if(p->instructions)kernel_heap_release(object->heap,p->instructions);
        if(p->verification_log)kernel_heap_release(object->heap,p->verification_log);
        break;
    }
    case BPF_OBJECT_MAP:{
        struct bpf_map_object *map=(void *)object;
        if(map->storage)bpf_map_put(map->storage);
        if(map->btf)bpf_object_put(&map->btf->object);
        break;
    }
    case BPF_OBJECT_BTF:{
        struct bpf_btf *btf=(void *)object;
        if(btf->offsets)kernel_heap_release(object->heap,btf->offsets);
        if(btf->data && !btf->kernel)kernel_heap_release(object->heap,btf->data);
        break;
    }
    case BPF_OBJECT_LINK:{
        struct bpf_link *link=(void *)object;
        (void)bpf_link_detach(link);
        if(link->program)kernel_bpf_program_put(link->program);
        if(link->event)bpf_object_put(&link->event->object);
        break;
    }
    case BPF_OBJECT_PERF:bpf_perf_destroy((void *)object);break;
    default:__builtin_trap();
    }
    kernel_heap_release(object->heap,object);
}
void kernel_bpf_program_get(struct kernel_bpf_program *program)
{bpf_object_get(&program->object);}
void kernel_bpf_program_put(struct kernel_bpf_program *program)
{if(program)bpf_object_put(&program->object);}
uint32_t kernel_bpf_program_classic(const struct kernel_bpf_program *program,
    const struct kernel_bpf_classic_insn **instructions)
{
    if(!program){*instructions=NULL;return 0;}
    *instructions=program->classic;return program->classic_count;
}
int bpf_user_copy(struct kernel_mm *mm,uint64_t address,void *buffer,size_t size,int from)
{
    size_t copied=0;
    enum kernel_uaccess_status status=from?
        kernel_copy_from_user(mm,buffer,address,size,&copied):
        kernel_copy_to_user(mm,address,buffer,size,&copied);
    if(status==KERNEL_UACCESS_STATUS_FAULT)return -KERNEL_EFAULT;
    if(status || copied!=size)__builtin_trap();
    return 0;
}
static void release_file(void *instance)
{
    struct bpf_object *object=instance;
    if(object->kind==BPF_OBJECT_PERF)bpf_perf_close_file((void *)object);
    bpf_object_put(object);
}
static enum kernel_files_status read_file(void *instance,struct kernel_task *task,
    struct kernel_files *files,struct kernel_open_file_description **owner,void **iov_owner,
    struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t iov_count,
    uint64_t count,uint32_t flags,int64_t *result)
{
    (void)task;(void)files;(void)owner;(void)iov_owner;(void)flags;
    struct bpf_object *object=instance;
    if(object->kind!=BPF_OBJECT_PERF){*result=-KERNEL_EINVAL;return 0;}
    unsigned char buffer[64];size_t length=0;
    *result=bpf_perf_read((void *)object,buffer,count<sizeof(buffer)?(size_t)count:sizeof(buffer),&length);
    if(*result)return 0;
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};size_t copied=0;
    enum kernel_uaccess_status status=kernel_copy_to_user_iov(mm,&cursor,buffer,length,&copied);
    if(status==KERNEL_UACCESS_STATUS_FAULT)*result=-KERNEL_EFAULT;
    else if(status || copied!=length)__builtin_trap();
    else *result=(int64_t)length;
    return 0;
}
static uint32_t poll_file(void *instance,uint32_t flags,uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)flags;struct bpf_object *object=instance;
    if(queue)*queue=NULL;
    if(object->kind!=BPF_OBJECT_MAP)return 0;
    struct bpf_map *map=((struct bpf_map_object *)object)->storage;
    struct kernel_wait_queue *wait=bpf_map_wait_queue(map);
    if(queue)*queue=wait;
    if(!wait)return 8U;
    return bpf_map_poll_readable(map)?requested&1U:0;
}
static int ioctl_file(void *instance,struct kernel_task *task,struct kernel_files *files,
    struct kernel_open_file_description **owner,struct kernel_mm *mm,uint64_t command,uint64_t argument)
{
    (void)owner;(void)mm;struct bpf_object *object=instance;
    return object->kind==BPF_OBJECT_PERF?
        bpf_perf_ioctl((void *)object,task,files,command,argument):-KERNEL_ENOTTY;
}
static const struct kernel_char_device object_ops={
    .kind=KERNEL_OPEN_FILE_KIND_BPF,.release=release_file,
    .poll=poll_file,.ioctl=ioctl_file,.readv=read_file,
};
static const struct kernel_char_device nonpoll_object_ops={
    .kind=KERNEL_OPEN_FILE_KIND_BPF,.release=release_file,
    .ioctl=ioctl_file,.readv=read_file,
};
struct bpf_object *bpf_object_from_file(const struct kernel_open_file_description *file)
{return file && file->kind==KERNEL_OPEN_FILE_KIND_BPF?file->device_instance:NULL;}
struct bpf_object *bpf_object_fd(struct kernel_files *files,int32_t fd,unsigned kind,int *error)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    struct bpf_object *object=bpf_object_from_file(file);
    *error=!file?-KERNEL_EBADF:!object || object->kind!=kind?-KERNEL_EINVAL:0;
    if(*error)return NULL;
    bpf_object_get(object);return object;
}
int kernel_bpf_program_fd(struct kernel_files *files,int32_t fd,uint32_t type,
    struct kernel_bpf_program **out)
{
    int error;
    struct bpf_object *o=bpf_object_fd(files,fd,BPF_OBJECT_PROGRAM,&error);
    if(!o)return error;
    struct kernel_bpf_program *program=(void *)o;
    if(program->type!=type){bpf_object_put(o);return -KERNEL_EINVAL;}
    *out=program;return 0;
}
struct bpf_map_object *bpf_map_fd(struct kernel_files *files,int32_t fd,unsigned access,int *error)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    struct bpf_object *object=bpf_object_from_file(file);
    *error=!file?-KERNEL_EBADF:!object || object->kind!=BPF_OBJECT_MAP?-KERNEL_EINVAL:0;
    if(!*error){
        unsigned mode=kernel_open_file_flags(file)&3;
        unsigned flags=bpf_map_spec(((struct bpf_map_object *)object)->storage)->map_flags;
        if(((access&BPF_FD_WRITE) && (mode==0 || (flags&8))) ||
            ((access&BPF_FD_READ) && (mode==1 || (flags&16))))*error=-KERNEL_EPERM;
    }
    if(*error)return NULL;
    bpf_object_get(object);return (void *)object;
}
int bpf_object_file(struct kernel_heap *heap,struct bpf_object *object,uint32_t flags,
    struct kernel_open_file_description **out)
{
    struct kernel_open_file_description *file=NULL;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*file),(void **)&file))return -KERNEL_ENOMEM;
    file->heap=heap;file->references=1;file->kind=KERNEL_OPEN_FILE_KIND_BPF;
    file->device=object->kind==BPF_OBJECT_MAP || object->kind==BPF_OBJECT_PERF?
        &object_ops:&nonpoll_object_ops;
    file->device_instance=object;
    file->open_flags=flags;file->credentials=kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&file->offset_lock,10,(uintptr_t)file);
    bpf_object_get(object);*out=file;return 0;
}
int bpf_object_install(struct kernel_files *files,struct bpf_object *object,uint32_t flags,uint32_t fd_flags)
{
    uint32_t fd;int error;
    if(kernel_files_find_free_fd(files,&fd,&error))__builtin_trap();
    if(error)return error;
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation.files=files;reservation.fd=fd;
    struct kernel_open_file_description *file=NULL;
    error=bpf_object_file(files->heap,object,flags,&file);
    if(error)return error;
    if(kernel_files_install_new_owned_at(files,fd,fd_flags,&file)){
        if(kernel_open_file_release(&file))__builtin_trap();
        __builtin_trap();
    }
    reservation.files=NULL;return (int)fd;
}
const char *kernel_bpf_fd_name(const struct kernel_open_file_description *file)
{
    struct bpf_object *o=bpf_object_from_file(file);
    if(!o)return NULL;
    switch(o->kind){
    case BPF_OBJECT_PROGRAM:return "anon_inode:bpf-prog";
    case BPF_OBJECT_MAP:return "anon_inode:bpf-map";
    case BPF_OBJECT_BTF:return "anon_inode:bpf-btf";
    case BPF_OBJECT_LINK:return "anon_inode:bpf-link";
    case BPF_OBJECT_PERF:return "anon_inode:[perf_event]";
    default:__builtin_trap();
    }
}
int kernel_bpf_mmap_begin(struct kernel_open_file_description *file,uint64_t offset,
    uint64_t length,int writable,struct kernel_lock_guard *guard,uint64_t **write_bytes)
{
    struct bpf_object *object=bpf_object_from_file(file);
    if(!object || object->kind!=BPF_OBJECT_MAP)return -KERNEL_ENODEV;
    struct bpf_map_object *map=(void *)object;
    kernel_mutex_lock(&map->mapping_lock,guard);
    int error=bpf_map_mmap_check(map->storage,offset,length,writable);
    if(error){kernel_lock_release(guard);return error;}
    *write_bytes=writable?&map->writable_bytes:NULL;
    return 0;
}
int kernel_bpf_mmap_page(struct kernel_open_file_description *file,uint64_t offset,
    int writable,uint64_t *physical)
{
    struct bpf_object *object=bpf_object_from_file(file);
    if(!object || object->kind!=BPF_OBJECT_MAP)return -KERNEL_ENODEV;
    return bpf_map_page_get(((struct bpf_map_object *)object)->storage,offset,writable,physical);
}
