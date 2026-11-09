#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <string.h>

static unsigned unprivileged_disabled=2;
int kernel_bpf_unprivileged_get(void)
{return (int)__atomic_load_n(&unprivileged_disabled,__ATOMIC_ACQUIRE);}
int kernel_bpf_unprivileged_set(unsigned value)
{
    if(value>2)return -KERNEL_EINVAL;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))
        return -KERNEL_EPERM;
    unsigned old=__atomic_load_n(&unprivileged_disabled,__ATOMIC_ACQUIRE);
    for(;;){
        if(old==1)return -KERNEL_EPERM;
        if(__atomic_compare_exchange_n(&unprivileged_disabled,&old,value,1,
            __ATOMIC_RELEASE,__ATOMIC_ACQUIRE))return 0;
    }
}
static int privileged(unsigned capability)
{
    const struct kernel_credentials *c=kernel_credentials_current();
    return kernel_credentials_capable(c,capability) ||
        kernel_credentials_capable(c,KERNEL_CAP_SYS_ADMIN);
}
static int zero_tail(const union bpf_attribute *a,size_t end)
{
    const unsigned char *bytes=(const void *)a;
    for(size_t i=end;i<sizeof(*a);i++)if(bytes[i])return -KERNEL_EINVAL;
    return 0;
}
static int object_name(const char name[16])
{
    for(unsigned i=0;i<16;i++){
        unsigned char c=name[i];
        if(!c)return 0;
        if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
            (c>='0'&&c<='9') || c=='_' || c=='.'))return -KERNEL_EINVAL;
    }
    return -KERNEL_EINVAL;
}
static int map_create(struct kernel_files *files,union bpf_attribute *a)
{
    int error=zero_tail(a,BPF_END(union bpf_attribute,map.exclusive_hash_size));
    if(error)return error;
    if(object_name(a->map.name) || a->map.inner_fd || a->map.ifindex ||
        a->map.extra || a->map.vmlinux_type || a->map.value_btf_fd ||
        a->map.token_fd || a->map.exclusive_hash || a->map.exclusive_hash_size)
        return -KERNEL_EINVAL;
    if(kernel_bpf_unprivileged_get() && !privileged(KERNEL_CAP_BPF))return -KERNEL_EPERM;
    struct bpf_map_object *map=NULL;
    if(kernel_heap_allocate_zeroed(files->heap,1,sizeof(*map),(void **)&map))return -KERNEL_ENOMEM;
    bpf_object_initialize(&map->object,files->heap,BPF_OBJECT_MAP);
    kernel_mutex_init(&map->mapping_lock,12,(uintptr_t)map);
    memcpy(map->object.name,a->map.name,16);
    if(a->map.key_type || a->map.value_type){
        map->btf=(void *)bpf_object_fd(files,(int32_t)a->map.btf_fd,BPF_OBJECT_BTF,&error);
        if(!map->btf)goto done;
        if(map->btf->kernel){error=-KERNEL_EACCES;goto done;}
        uint32_t size;
        if((a->map.key_type && (bpf_btf_type_size(map->btf,a->map.key_type,&size) ||
            size!=a->map.key_size)) || !a->map.value_type ||
            bpf_btf_type_size(map->btf,a->map.value_type,&size) || size!=a->map.value_size){
            error=-KERNEL_EINVAL;goto done;
        }
        map->key_type=a->map.key_type;map->value_type=a->map.value_type;
    }
    struct bpf_map_spec spec={a->map.type,a->map.key_size,a->map.value_size,
        a->map.max_entries,a->map.flags};
    error=bpf_map_create(files->heap,files->heap->page_allocator,&spec,&map->storage);
    if(!error)error=bpf_object_publish(&map->object);
    if(!error)error=bpf_object_install(files,&map->object,
        a->map.flags&8?0:a->map.flags&16?1:2,1);
done:
    bpf_object_put(&map->object);return error;
}
static int64_t map_element(struct kernel_files *files,struct kernel_mm *mm,
    unsigned command,union bpf_attribute *a)
{
    size_t last=command==BPF_MAP_FREEZE?sizeof(uint32_t):
        command==BPF_MAP_DELETE_ELEM?BPF_END(union bpf_attribute,element.key):
        command==BPF_MAP_GET_NEXT_KEY?BPF_END(union bpf_attribute,element.value):
        sizeof(a->element);
    int error=zero_tail(a,last);
    if(error)return error;
    if(command==BPF_MAP_LOOKUP_AND_DELETE_ELEM && (a->element.flags&~UINT64_C(4)))
        return -KERNEL_EINVAL;
    unsigned rights=command==BPF_MAP_LOOKUP_ELEM || command==BPF_MAP_GET_NEXT_KEY?BPF_FD_READ:
        command==BPF_MAP_LOOKUP_AND_DELETE_ELEM?BPF_FD_READ|BPF_FD_WRITE:BPF_FD_WRITE;
    struct bpf_map_object *map=bpf_map_fd(files,(int32_t)a->element.fd,rights,&error);
    if(!map)return error;
    if(command==BPF_MAP_FREEZE){
        KERNEL_LOCK_SCOPE(lock);kernel_mutex_lock(&map->mapping_lock,&lock);
        error=__atomic_load_n(&map->writable_bytes,__ATOMIC_ACQUIRE)?-KERNEL_EBUSY:
            bpf_map_freeze(map->storage);
        goto done;
    }
    const struct bpf_map_spec *spec=bpf_map_spec(map->storage);
    size_t value_size=bpf_map_user_value_size(map->storage);
    size_t key_size=spec->key_size;
    if(!key_size){error=-KERNEL_EINVAL;goto done;}
    if((command==BPF_MAP_LOOKUP_ELEM || command==BPF_MAP_LOOKUP_AND_DELETE_ELEM) &&
        a->element.flags){error=-KERNEL_EINVAL;goto done;}
    if(key_size>SIZE_MAX-value_size){error=-KERNEL_ENOMEM;goto done;}
    unsigned char *buffer=NULL;
    if(kernel_heap_allocate(map->object.heap,key_size+value_size,(void **)&buffer)){
        error=-KERNEL_ENOMEM;goto done;
    }
    void *key=buffer,*value=buffer+key_size;
    if(a->element.key || command!=BPF_MAP_GET_NEXT_KEY)
        error=bpf_user_copy(mm,a->element.key,key,key_size,1);
    else key=NULL;
    if(!error)switch(command){
    case BPF_MAP_LOOKUP_ELEM:
    case BPF_MAP_LOOKUP_AND_DELETE_ELEM:
        error=command==BPF_MAP_LOOKUP_AND_DELETE_ELEM?
            bpf_map_lookup_and_delete(map->storage,key,value):bpf_map_lookup(map->storage,key,value,1,0);
        if(!error)error=bpf_user_copy(mm,a->element.value,value,value_size,0);
        break;
    case BPF_MAP_UPDATE_ELEM:
        error=bpf_user_copy(mm,a->element.value,value,value_size,1);
        if(!error)error=bpf_map_update(map->storage,key,value,a->element.flags,1,0);
        break;
    case BPF_MAP_DELETE_ELEM:error=bpf_map_delete(map->storage,key,1);break;
    case BPF_MAP_GET_NEXT_KEY:
        error=bpf_map_next_key(map->storage,key,buffer);
        if(!error)error=bpf_user_copy(mm,a->element.value,buffer,key_size,0);
        break;
    default:__builtin_trap();
    }
    kernel_heap_release(map->object.heap,buffer);
done:
    bpf_object_put(&map->object);return error;
}
static int64_t map_batch(struct kernel_files *files,struct kernel_mm *mm,
    unsigned command,uint64_t user_attribute,union bpf_attribute *a)
{
    int error=zero_tail(a,sizeof(a->batch));
    if(error || a->batch.flags)return -KERNEL_EINVAL;
    unsigned rights=command==BPF_MAP_LOOKUP_BATCH?BPF_FD_READ:
        command==BPF_MAP_LOOKUP_AND_DELETE_BATCH?BPF_FD_READ|BPF_FD_WRITE:BPF_FD_WRITE;
    struct bpf_map_object *map=bpf_map_fd(files,(int32_t)a->batch.map_fd,rights,&error);
    if(!map)return error;
    const struct bpf_map_spec *spec=bpf_map_spec(map->storage);
    size_t value_size=bpf_map_user_value_size(map->storage),key_size=spec->key_size;
    uint32_t count=a->batch.count,completed=0;
    unsigned char *buffer=NULL;
    if(!key_size){error=-KERNEL_EINVAL;goto done;}
    if(a->batch.element_flags){error=-KERNEL_EINVAL;goto done;}
    if(!count){error=0;goto done;}
    /* Linux reports the empty prefix before importing inputs or changing
     * entries, so an unwritable result field cannot consume a batch. */
    error=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,batch.count),&completed,4,0);
    if(error)goto done;
    int lookup=command==BPF_MAP_LOOKUP_BATCH || command==BPF_MAP_LOOKUP_AND_DELETE_BATCH;
    size_t stride=command==BPF_MAP_DELETE_BATCH?key_size:key_size+value_size;
    if(key_size>SIZE_MAX-value_size || (lookup && count>SIZE_MAX/stride)){
        error=-KERNEL_E2BIG;goto done;
    }
    if(kernel_heap_allocate(map->object.heap,lookup?count*stride:stride,(void **)&buffer)){
        error=-KERNEL_ENOMEM;goto done;
    }
    void *keys=buffer,*values=buffer+(lookup?count:1)*key_size;
    if(lookup){
        uint32_t cursor=0,next=0;
        if(a->batch.in_batch){error=bpf_user_copy(mm,a->batch.in_batch,&cursor,4,1);if(error)goto release;}
        completed=count;
        error=bpf_map_lookup_batch(map->storage,a->batch.in_batch?&cursor:NULL,&next,
            keys,values,&completed,command==BPF_MAP_LOOKUP_AND_DELETE_BATCH);
        if(completed){
            int copy=bpf_user_copy(mm,a->batch.keys,keys,completed*key_size,0);
            if(!copy)copy=bpf_user_copy(mm,a->batch.values,values,completed*value_size,0);
            if(!copy)copy=bpf_user_copy(mm,a->batch.out_batch,&next,4,0);
            if(copy)error=copy;
        }
    }else{
        /* Import one element at a time: a later input fault retains the
         * successfully changed prefix and its returned count. */
        for(;completed<count;completed++){
            error=bpf_user_copy(mm,a->batch.keys+(uint64_t)completed*key_size,keys,key_size,1);
            if(error)break;
            if(command==BPF_MAP_DELETE_BATCH)error=bpf_map_delete(map->storage,keys,1);
            else{
                error=bpf_user_copy(mm,a->batch.values+(uint64_t)completed*value_size,values,value_size,1);
                if(!error)error=bpf_map_update(map->storage,keys,values,a->batch.element_flags,1,0);
            }
            if(error)break;
        }
    }
    {int copy=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,batch.count),&completed,4,0);
        if(copy)error=copy;}
release:
    if(buffer)kernel_heap_release(map->object.heap,buffer);
done:
    bpf_object_put(&map->object);return error;
}
static int64_t by_id(struct kernel_files *files,struct kernel_mm *mm,unsigned command,
    uint64_t user_attribute,union bpf_attribute *a)
{
    unsigned kind=command==BPF_PROG_GET_NEXT_ID || command==BPF_PROG_GET_FD_BY_ID?BPF_OBJECT_PROGRAM:
        command==BPF_MAP_GET_NEXT_ID || command==BPF_MAP_GET_FD_BY_ID?BPF_OBJECT_MAP:
        command==BPF_BTF_GET_NEXT_ID || command==BPF_BTF_GET_FD_BY_ID?BPF_OBJECT_BTF:BPF_OBJECT_LINK;
    int next=command==BPF_PROG_GET_NEXT_ID || command==BPF_MAP_GET_NEXT_ID ||
        command==BPF_BTF_GET_NEXT_ID || command==BPF_LINK_GET_NEXT_ID;
    if(zero_tail(a,next?BPF_END(union bpf_attribute,id.next_id):sizeof(a->id)) ||
        (!next && a->id.token_fd))return -KERNEL_EINVAL;
    if(next){
        int error=bpf_object_next_id(kind,a->id.id,&a->id.next_id);
        if(!error)error=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,id.next_id),
            &a->id.next_id,4,0);
        return error;
    }
    if(a->id.flags&~24U)return -KERNEL_EINVAL;
    if((a->id.flags&24U)==24U)return -KERNEL_EINVAL;
    if(!privileged(KERNEL_CAP_BPF))return -KERNEL_EPERM;
    struct bpf_object *object=bpf_object_by_id(kind,a->id.id);
    if(!object)return -KERNEL_ENOENT;
    int result=bpf_object_install(files,object,a->id.flags&8?0:a->id.flags&16?1:2,1);
    bpf_object_put(object);return result;
}
static int64_t btf_load(struct kernel_files *files,struct kernel_mm *mm,
    union bpf_attribute *a)
{
    if(zero_tail(a,sizeof(a->btf)) || a->btf.flags || a->btf.token_fd)return -KERNEL_EINVAL;
    if(!privileged(KERNEL_CAP_BPF))return -KERNEL_EPERM;
    if(!a->btf.size || a->btf.size>UINT32_C(0x1000000))return -KERNEL_E2BIG;
    if(a->btf.log_level>7 || (!a->btf.log_size)!=(!a->btf.log))return -KERNEL_EINVAL;
    unsigned char *data=NULL;
    if(kernel_heap_allocate(files->heap,a->btf.size,(void **)&data))return -KERNEL_ENOMEM;
    int error=bpf_user_copy(mm,a->btf.data,data,a->btf.size,1);
    struct bpf_btf *btf=NULL;
    if(!error)error=bpf_btf_parse(files->heap,data,a->btf.size,&btf);
    if(!btf){kernel_heap_release(files->heap,data);return error;}
    error=bpf_object_publish(&btf->object);
    if(!error)error=bpf_object_install(files,&btf->object,0,1);
    bpf_object_put(&btf->object);return error;
}
static int64_t pin_object(struct kernel_task *task,struct kernel_files *files,
    struct kernel_mm *mm,unsigned command,union bpf_attribute *a)
{
    if(zero_tail(a,BPF_END(union bpf_attribute,pin.path_fd)) || a->pin.flags&~24U || a->pin.path_fd ||
        (a->pin.flags&24U)==24U)return -KERNEL_EINVAL;
    const struct kernel_fs_context *fs;
    if(kernel_task_fs_context_borrow(task,&fs))__builtin_trap();
    char *path=NULL;
    if(kernel_heap_allocate(files->heap,KERNEL_FS_PATH_MAX,(void **)&path))return -KERNEL_ENOMEM;
    size_t copied;
    enum kernel_uaccess_status status=kernel_copy_string_from_user(mm,path,a->pin.path,KERNEL_FS_PATH_MAX,&copied);
    int error=status==KERNEL_UACCESS_STATUS_FAULT?-KERNEL_EFAULT:
        status==KERNEL_UACCESS_STATUS_TOO_LONG?-KERNEL_ENAMETOOLONG:0;
    struct bpf_object *object=NULL;
    if(!error && command==BPF_OBJ_PIN){
        if(a->pin.flags)error=-KERNEL_EINVAL;
        else{
            /* Object kind is intentionally unrestricted; bpffs preserves it. */
            for(unsigned kind=0;kind<BPF_OBJECT_PERF;kind++){
                object=bpf_object_fd(files,(int32_t)a->pin.fd,kind,&error);
                if(object || error==-KERNEL_EBADF)break;
            }
            if(object)error=bpf_pin_object(fs,path,object);
        }
    }else if(!error){
        error=bpf_get_pinned(fs,path,a->pin.flags,&object);
        if(!error)error=bpf_object_install(files,object,a->pin.flags&8?0:a->pin.flags&16?1:2,1);
    }
    if(object)bpf_object_put(object);
    kernel_heap_release(files->heap,path);return error;
}
int64_t kernel_bpf_syscall(struct kernel_task *task,uint32_t command,
    uint64_t user_attribute,uint32_t size)
{
    if(command>38)return -KERNEL_EINVAL;
    if(size>BOAROS_PAGE_SIZE)return -KERNEL_E2BIG;
    struct kernel_files *files;struct kernel_mm *mm;
    if(kernel_task_files_borrow(task,&files) || kernel_task_mm_borrow_mutable(task,&mm))__builtin_trap();
    int initialized=kernel_bpf_initialize(files->heap);
    if(initialized)return initialized;
    union bpf_attribute a={0};
    uint32_t first=size<sizeof(a)?size:sizeof(a);
    int error=bpf_user_copy(mm,user_attribute,&a,first,1);
    if(error)return error;
    for(uint32_t offset=first;offset<size;){
        unsigned char tail[64];uint32_t n=size-offset;if(n>sizeof(tail))n=sizeof(tail);
        error=bpf_user_copy(mm,user_attribute+offset,tail,n,1);
        if(error)return error;
        for(unsigned i=0;i<n;i++)if(tail[i])return -KERNEL_E2BIG;
        offset+=n;
    }
    switch(command){
    case BPF_MAP_CREATE:return map_create(files,&a);
    case BPF_MAP_LOOKUP_ELEM:case BPF_MAP_UPDATE_ELEM:case BPF_MAP_DELETE_ELEM:
    case BPF_MAP_GET_NEXT_KEY:case BPF_MAP_FREEZE:case BPF_MAP_LOOKUP_AND_DELETE_ELEM:
        return map_element(files,mm,command,&a);
    case BPF_MAP_LOOKUP_BATCH:case BPF_MAP_LOOKUP_AND_DELETE_BATCH:
    case BPF_MAP_UPDATE_BATCH:case BPF_MAP_DELETE_BATCH:
        return map_batch(files,mm,command,user_attribute,&a);
    case BPF_PROG_LOAD:return bpf_program_load(task,files,mm,user_attribute,size,&a);
    case BPF_PROG_TEST_RUN:return bpf_program_test(task,files,mm,user_attribute,size,&a);
    case BPF_PROG_GET_NEXT_ID:case BPF_MAP_GET_NEXT_ID:case BPF_BTF_GET_NEXT_ID:
    case BPF_LINK_GET_NEXT_ID:case BPF_PROG_GET_FD_BY_ID:case BPF_MAP_GET_FD_BY_ID:
    case BPF_BTF_GET_FD_BY_ID:case BPF_LINK_GET_FD_BY_ID:
        return by_id(files,mm,command,user_attribute,&a);
    case BPF_BTF_LOAD:return btf_load(files,mm,&a);
    case BPF_OBJ_PIN:case BPF_OBJ_GET:return pin_object(task,files,mm,command,&a);
    case BPF_LINK_CREATE:return bpf_link_create(files,&a);
    case BPF_OBJ_GET_INFO_BY_FD:{
        if(zero_tail(&a,sizeof(a.info)))return -KERNEL_EINVAL;
        struct bpf_object *object=NULL;
        for(unsigned kind=0;kind<BPF_OBJECT_PERF;kind++){
            object=bpf_object_fd(files,(int32_t)a.info.fd,kind,&error);
            if(object || error==-KERNEL_EBADF)break;
        }
        if(!object)return error;
        error=bpf_object_information(mm,object,a.info.data,&a.info.length);
        if(!error || error==-KERNEL_ENOSPC){
            int copy=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,info.length),&a.info.length,4,0);
            if(copy)error=copy;
        }
        bpf_object_put(object);return error;
    }
    case BPF_LINK_DETACH:{
        if(zero_tail(&a,sizeof(a.detach)))return -KERNEL_EINVAL;
        struct bpf_link *link=(void *)bpf_object_fd(files,(int32_t)a.detach.fd,BPF_OBJECT_LINK,&error);
        if(!link)return error;
        /* perf BPF links detach when their last owner is closed; Linux does
         * not provide the explicit LINK_DETACH operation for this link type. */
        error=-KERNEL_EOPNOTSUPP;bpf_object_put(&link->object);return error;
    }
    default:return -KERNEL_EINVAL;
    }
}
