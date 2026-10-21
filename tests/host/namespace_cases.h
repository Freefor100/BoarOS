#include "../../fs/ext4_backend.h"
#ifdef NAMESPACE_PARALLEL_CASES
#include <ext4_fs.h>
static struct kernel_vfs_instance *namespace_parallel_instance;
static _Thread_local unsigned namespace_parallel_actor;
static unsigned namespace_parallel_pause, namespace_parallel_resume;
static unsigned namespace_parallel_entered[2], namespace_parallel_closed;
static unsigned namespace_parallel_created[2], namespace_parallel_creates,namespace_parallel_moved;
static struct kernel_vfs_node *namespace_stat_node;
static _Thread_local unsigned namespace_stat_reader;
static unsigned namespace_stat_entered,namespace_stat_resume,namespace_stat_written;
static struct kernel_rwlock *namespace_directory_resource;
int __real_ext4_fs_get_inode_ref(struct ext4_fs *,uint32_t,struct ext4_inode_ref *);
int __wrap_ext4_fs_get_inode_ref(struct ext4_fs *fs,uint32_t index,struct ext4_inode_ref *ref)
{
    int result=__real_ext4_fs_get_inode_ref(fs,index,ref);
    if(!result && (namespace_stat_reader==4 || namespace_stat_reader==5) &&
        namespace_stat_node && index==namespace_stat_node->inode) {
        struct kernel_io_context *context=kernel_io_context_current();
        assert(ext4_current_operation(fs));
        struct kernel_lock_guard *guard=context->locks;
        while(guard && (guard->lock->rank!=31 || guard->lock->key!=index))guard=guard->previous;
        assert(guard && guard->write && guard->owner==context);
        namespace_directory_resource=guard->lock;
        __atomic_store_n(&namespace_stat_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&namespace_stat_resume,__ATOMIC_ACQUIRE))sched_yield();
        namespace_stat_reader=0;
    }
    return result;
}
int __real_ext4_fraw_inode_fill(const ext4_file *,struct ext4_inode *);
int __wrap_ext4_fraw_inode_fill(const ext4_file *file,struct ext4_inode *inode)
{
    int result=__real_ext4_fraw_inode_fill(file,inode);
    if(namespace_stat_reader && namespace_stat_node && file==lwext4_node_file(namespace_stat_node)) {
        if((namespace_stat_node->mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFREG)
            assert(kernel_lock_held(&namespace_stat_node->io_lock,0));
        __atomic_store_n(&namespace_stat_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&namespace_stat_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    return result;
}
int __real_ext4_file_set_times(ext4_file *,unsigned,const struct ext4_timestamp [3]);
int __wrap_ext4_file_set_times(ext4_file *file,unsigned fields,const struct ext4_timestamp times[3])
{
    if(namespace_stat_reader==2 && namespace_stat_node && file==lwext4_node_file(namespace_stat_node)) {
        assert(kernel_lock_held(&namespace_stat_node->io_lock,1));
        __atomic_store_n(&namespace_stat_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&namespace_stat_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    return __real_ext4_file_set_times(file,fields,times);
}
struct namespace_stat_request {
    struct kernel_vfs_file *file;
    struct kernel_vfs_path *directory;
    struct kernel_vfs_stat stat;
    unsigned writer;
};
static void *namespace_stat_thread(void *argument)
{
    struct namespace_stat_request *request=argument;
    mm_test_attach(request->writer+1);mm_test_activate_task();kernel_cpu_current()->logical_id=!request->writer;
    if(request->writer==3 || request->writer==5) {
        namespace_stat_reader=request->writer;
        assert(!kernel_vfs_file_set_mode(request->file,0711));
        __atomic_store_n(&namespace_stat_written,1,__ATOMIC_RELEASE);
    } else if(request->writer==2 || request->writer==4) {
        namespace_stat_reader=request->writer;
        const struct kernel_vfs_timespec times[2]={{1234,0},{2345,0}};
        assert(!kernel_vfs_file_set_times(request->file,times));
    } else if(request->writer) {
        if(request->directory)assert(!kernel_vfs_mkdir_at(request->directory,request->directory,"child",0700));
        else assert(!kernel_vfs_ftruncate(request->file,1));
        __atomic_store_n(&namespace_stat_written,1,__ATOMIC_RELEASE);
    } else {
        namespace_stat_reader=1;
        assert(!kernel_vfs_fstat(request->file,&request->stat));
    }
    mm_test_finish();return NULL;
}
static void namespace_stat_qualification_case(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create(mount,"/namespace-stat",0600,&file));
    namespace_stat_node=file.private_data;namespace_stat_entered=namespace_stat_resume=namespace_stat_written=0;
    struct namespace_stat_request requests[2]={{.file=&file},{.file=&file,.writer=1}};
    pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,namespace_stat_thread,&requests[0]));
    while(!__atomic_load_n(&namespace_stat_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(!pthread_create(&threads[1],NULL,namespace_stat_thread,&requests[1]));
    for(;;) {
        unsigned waiting;
        {KERNEL_RAW_SCOPE(guard,&namespace_stat_node->io_lock.metadata);
         waiting=namespace_stat_node->io_lock.pending_head!=NULL;}
        if(waiting)break;
        sched_yield();
    }
    assert(!__atomic_load_n(&namespace_stat_written,__ATOMIC_ACQUIRE));
    __atomic_store_n(&namespace_stat_resume,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    namespace_stat_node=NULL;
    struct kernel_vfs_stat stat;
    assert(!requests[0].stat.size && !kernel_vfs_fstat(&file,&stat) && stat.size==1);
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unlink(mount,"/namespace-stat"));
    puts("PASS actual ext4 fstat holds inode read qualification through raw inode fill and final snapshot; ftruncate waits");
}
static void namespace_directory_stat_qualification_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    if (!((struct kernel_vfs_instance *)mount->private_data)->namespace_parallel) return;
    struct kernel_vfs_path *root=NULL,*directory=NULL;
    assert(!kernel_vfs_path_root(mount,heap,&root) &&
           !kernel_vfs_mkdir_at(root,root,"namespace-directory-stat",0700) &&
           !kernel_vfs_path_lookup(root,"namespace-directory-stat",24,&directory));
    for(unsigned setter=0;setter<3;setter++) {
        namespace_stat_node=directory->file.private_data;
        namespace_stat_entered=namespace_stat_resume=namespace_stat_written=0;
        struct namespace_stat_request requests[2]={{.file=&directory->file,.writer=setter==1?2:0},
            {.file=&directory->file,.directory=directory,.writer=setter==2?3:1}};
        pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,namespace_stat_thread,&requests[0]));
        while(!__atomic_load_n(&namespace_stat_entered,__ATOMIC_ACQUIRE))sched_yield();
        assert(!pthread_create(&threads[1],NULL,namespace_stat_thread,&requests[1]));
        unsigned waiting=0;
        for(;;) {
            if(__atomic_load_n(&namespace_stat_written,__ATOMIC_ACQUIRE))break;
            {KERNEL_RAW_SCOPE(guard,&namespace_stat_node->io_lock.metadata);
             waiting=namespace_stat_node->io_lock.pending_head!=NULL;}
            if(waiting)break;
            sched_yield();
        }
        __atomic_store_n(&namespace_stat_resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        namespace_stat_node=NULL;
        assert(!waiting); /* A copied directory snapshot need not pin future edits. */
        struct kernel_vfs_stat stat;
        assert(!kernel_vfs_fstat(&directory->file,&stat));
        if(!setter)assert(stat.nlink==requests[0].stat.nlink+1);
        else if(setter==1)assert(stat.atime.seconds==1234);
        else assert((stat.mode&07777)==0711 && (requests[0].stat.mode&07777)==0700);
        if(setter!=2)assert(!kernel_vfs_unlink_at(directory,directory,"child",1));
    }
    for(unsigned setter=4;setter<=5;setter++) {
        namespace_stat_node=directory->file.private_data;
        namespace_stat_entered=namespace_stat_resume=namespace_stat_written=0;
        namespace_directory_resource=NULL;
        struct kernel_vfs_stat before,after;
        assert(!kernel_vfs_fstat(&directory->file,&before));
        struct namespace_stat_request requests[2]={{.file=&directory->file,.writer=setter},
            {.file=&directory->file,.directory=directory,.writer=1}};
        pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,namespace_stat_thread,&requests[0]));
        while(!__atomic_load_n(&namespace_stat_entered,__ATOMIC_ACQUIRE))sched_yield();
        assert(namespace_directory_resource);
        assert(!pthread_create(&threads[1],NULL,namespace_stat_thread,&requests[1]));
        for(;;) {
            unsigned waiting;
            {KERNEL_RAW_SCOPE(guard,&namespace_directory_resource->metadata);
             waiting=namespace_directory_resource->pending_head!=NULL;}
            if(waiting)break;
            {KERNEL_RAW_SCOPE(guard,&namespace_stat_node->io_lock.metadata);
             assert(!namespace_stat_node->io_lock.pending_head);}
            assert(!__atomic_load_n(&namespace_stat_written,__ATOMIC_ACQUIRE));
            sched_yield();
        }
        __atomic_store_n(&namespace_stat_resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        namespace_stat_node=NULL;namespace_directory_resource=NULL;
        assert(!kernel_vfs_fstat(&directory->file,&after) && after.nlink==before.nlink+1);
        if(setter==4)assert(after.atime.seconds==1234);
        else assert((after.mode&07777)==0711);
        uint64_t observed=kernel_vfs_error_sequence(&directory->file);
        assert(!kernel_vfs_sync_borrowing(&directory->file,0,&observed,NULL));
        assert(!kernel_vfs_unlink_at(directory,directory,"child",1));
    }
    assert(!kernel_vfs_path_release(&directory) &&
           !kernel_vfs_unlink_at(root,root,"namespace-directory-stat",1) && !kernel_vfs_path_release(&root));
    puts("PASS directory stat retains its copied snapshot while mkdir/chmod progress; FD utimens merges the latest nlink");
    puts("PASS directory FD utimens/chmod and mkdir serialize through actual backend inode owner; fsync and final owners return");
}
static int namespace_parallel_truncate(struct kernel_vfs_node *node,uint64_t size,
    uint64_t *actual,int *changed)
{
    assert(!size && kernel_lock_held(&node->io_lock,1));
    __atomic_store_n(&namespace_parallel_entered[0],1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&namespace_parallel_resume,__ATOMIC_ACQUIRE)) sched_yield();
    *actual=size;*changed=1;return 0;
}
static void namespace_parallel_link_enter(struct kernel_vfs_instance *instance,uint64_t inode)
{
    __atomic_store_n(&namespace_parallel_entered[1],1,__ATOMIC_RELEASE);
    struct kernel_vfs_node *node=kernel_vfs_node_pin(instance,inode);
    assert(node && kernel_lock_held(&node->io_lock,1));
    assert(!kernel_vfs_node_release(&node));
}
static int namespace_parallel_unlink(struct kernel_vfs_mount *mount,uint64_t parent,const char *name)
{
    assert(parent==20 && !strcmp(name,"created"));
    unsigned inode=__atomic_load_n(&namespace_parallel_created[1],__ATOMIC_ACQUIRE);
    namespace_parallel_link_enter(mount->private_data,inode);
    assert(inode && inode==namespace_parallel_created[0]);
    __atomic_store_n(&namespace_parallel_created[1],0,__ATOMIC_RELEASE);return 0;
}
static int namespace_parallel_link(struct kernel_vfs_instance *instance,uint64_t inode,
    uint64_t parent,const char *name)
{
    assert(parent==20 && !strcmp(name,"created") && inode==namespace_parallel_created[0]);
    namespace_parallel_link_enter(instance,inode);
    assert(!__atomic_exchange_n(&namespace_parallel_created[1],inode,__ATOMIC_ACQ_REL));return 0;
}

void __real_kernel_file_mapping_init(struct kernel_vfs_node *);
void __wrap_kernel_file_mapping_init(struct kernel_vfs_node *node)
{
    __real_kernel_file_mapping_init(node);
    if (namespace_parallel_pause == 1 && node->instance == namespace_parallel_instance) {
        __atomic_store_n(&namespace_parallel_entered[namespace_parallel_actor], 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&namespace_parallel_resume, __ATOMIC_ACQUIRE)) sched_yield();
    }
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count, size_t size, void **owner)
{
    enum kernel_heap_status result=__real_kernel_heap_allocate_zeroed(heap,count,size,owner);
    if (!result && namespace_parallel_pause==2 && count==1 &&
        size==sizeof(struct kernel_vfs_path)+5) {
        __atomic_store_n(&namespace_parallel_entered[namespace_parallel_actor],1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&namespace_parallel_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
static int namespace_parallel_close(struct kernel_vfs_node *node)
{
    assert(node->instance==namespace_parallel_instance);
    __atomic_fetch_add(&namespace_parallel_closed,1,__ATOMIC_RELAXED);
    return 0;
}
static int namespace_parallel_root(struct kernel_vfs_instance *instance,uint64_t *inode,uint32_t *mode)
{
    assert(instance==namespace_parallel_instance);
    *inode=1;*mode=KERNEL_VFS_S_IFDIR|0755;return 0;
}
static int namespace_parallel_lookup(struct kernel_vfs_instance *instance,uint64_t parent,
    const char *name,size_t length,uint64_t *inode,uint32_t *mode)
{
    assert(instance==namespace_parallel_instance);
    if (parent==1 && ((length==1 && name[0]=='b') ||
        (!namespace_parallel_moved && length==1 && name[0]=='a') ||
        (namespace_parallel_moved && length==5 && !memcmp(name,"moved",5)))) {
        *inode=length==1 && name[0]=='b'?20:10;*mode=KERNEL_VFS_S_IFDIR|0700;return 0;
    }
    if (parent==1 && length==4 && !memcmp(name,"leaf",4)) {
        *inode=2;*mode=KERNEL_VFS_S_IFREG|0600;return 0;
    }
    if ((parent==10 || parent==20) && length==7 && !memcmp(name,"created",7) &&
        __atomic_load_n(&namespace_parallel_created[parent==20],__ATOMIC_ACQUIRE)) {
        *inode=__atomic_load_n(&namespace_parallel_created[parent==20],__ATOMIC_ACQUIRE);
        *mode=KERNEL_VFS_S_IFREG|0600;return 0;
    }
    return -KERNEL_ENOENT;
}
static int namespace_parallel_open(struct kernel_vfs_mount *mount,const char *path,
    uint64_t inode,uint32_t mode,struct kernel_vfs_file *file)
{
    (void)path;struct kernel_vfs_node *node=NULL;
    struct kernel_vfs_instance *instance=mount->private_data;
    assert(!kernel_heap_allocate_zeroed(instance->heap,1,sizeof(*node),(void **)&node));
    node->inode=inode;node->mode=mode;
    return kernel_vfs_publish_node(mount,node,file,0);
}
static int namespace_parallel_create(struct kernel_vfs_mount *mount,uint64_t parent,
    const char *name,uint32_t mode,struct kernel_vfs_file *file)
{
    assert((parent==10 || parent==20) && !strcmp(name,"created"));
    struct kernel_vfs_node *directory=kernel_vfs_node_pin(mount->private_data,parent);
    assert(directory && kernel_lock_held(&directory->namespace_lock,1));
    assert(!kernel_vfs_node_release(&directory));
    if (namespace_parallel_pause==3) {
        __atomic_store_n(&namespace_parallel_entered[namespace_parallel_actor],1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&namespace_parallel_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __atomic_fetch_add(&namespace_parallel_creates,1,__ATOMIC_RELAXED);
    if (__atomic_exchange_n(&namespace_parallel_created[parent==20],parent+1,__ATOMIC_ACQ_REL))
        return -KERNEL_EEXIST;
    return namespace_parallel_open(mount,NULL,parent+1,KERNEL_VFS_S_IFREG|mode,file);
}
static void namespace_parallel_assert_writer(struct kernel_vfs_instance *instance,uint64_t inode)
{
    struct kernel_vfs_node *node=kernel_vfs_node_pin(instance,inode);
    assert(node && kernel_lock_held(&node->namespace_lock,1));
    assert(!kernel_vfs_node_release(&node));
}
static int namespace_parallel_rmdir(struct kernel_vfs_mount *mount,uint64_t parent,const char *name)
{
    assert(parent==1 && !strcmp(name,"a"));
    namespace_parallel_assert_writer(mount->private_data,1);
    namespace_parallel_assert_writer(mount->private_data,10);
    assert(__atomic_load_n(&namespace_parallel_created[0],__ATOMIC_ACQUIRE));
    return -KERNEL_ENOTEMPTY;
}
static int namespace_parallel_rename(struct kernel_vfs_instance *instance,uint64_t old_parent,
    const char *old_name,uint64_t new_parent,const char *new_name,unsigned flags,
    struct kernel_vfs_rename_result *result)
{
    if(old_parent==10 && new_parent==20) {
        assert(!strcmp(old_name,"created") && !strcmp(new_name,"created") && !flags);
        unsigned source=namespace_parallel_created[0],target=namespace_parallel_created[1];
        assert(source && target);
        if(source==target)return 0;
        namespace_parallel_link_enter(instance,target);
        namespace_parallel_created[0]=0;namespace_parallel_created[1]=source;
        result->replaced_inode=target;result->replaced_last_link=1;result->changed=1;return 0;
    }
    assert(old_parent==1 && new_parent==1 && !strcmp(old_name,"a") &&
           !strcmp(new_name,"moved") && !flags);
    namespace_parallel_assert_writer(instance,1);
    namespace_parallel_assert_writer(instance,10);
    namespace_parallel_moved=1;result->changed=1;return 0;
}
static void namespace_parallel_release_unlinked(struct kernel_vfs_node *node)
{
    KERNEL_LOCK_SCOPE(guard);kernel_vfs_node_lock(node,&guard,1);
    if (!node->open_files)node->retired=1;
}
struct namespace_parallel_request {
    struct kernel_vfs_mount *mount;
    struct kernel_vfs_path *root,*found;
    struct kernel_vfs_file file;
    unsigned actor,operation,done;
    int error;
    struct kernel_vfs_file *borrowed;
    struct kernel_vfs_path *source;
    struct kernel_vfs_path *destination;
};
static void *namespace_parallel_thread(void *argument)
{
    struct namespace_parallel_request *request=argument;
    mm_test_attach(request->actor+1);mm_test_activate_task();
    kernel_cpu_current()->logical_id=request->actor;
    namespace_parallel_actor=request->actor;
    if (request->operation==8)
        request->error=kernel_vfs_rename_at(request->source,request->destination,
            request->root,"created","created",0);
    else if (request->operation==5)
        request->error=kernel_vfs_ftruncate(request->borrowed,0);
    else if (request->operation==6)
        request->error=kernel_vfs_unlink_at(request->root,request->root,"created",0);
    else if (request->operation==7)
        request->error=kernel_vfs_link_at(request->source,request->root,request->root,"created");
    else if (request->operation==3)
        request->error=kernel_vfs_unlink_at(request->root,request->root,"a",1);
    else if (request->operation==4)
        request->error=kernel_vfs_rename_at(request->root,request->root,request->root,"a","moved",0);
    else if (request->operation==2) {
        struct kernel_vfs_open_request open={.flags=KERNEL_VFS_OPEN_CREATE|KERNEL_VFS_OPEN_FOLLOW,.mode=0600};
        assert(!kernel_vfs_open_at(request->root,request->root,"created",&open,&request->file,NULL));
    } else if (request->root) assert(!kernel_vfs_path_lookup(request->root,"leaf",4,&request->found));
    else assert(!namespace_parallel_open(request->mount,NULL,2,KERNEL_VFS_S_IFREG|0600,&request->file));
    mm_test_finish();
    __atomic_store_n(&request->done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void namespace_parallel_publication_case(struct kernel_heap *heap)
{
    struct kernel_vfs_backend ops={.namespace_execution=KERNEL_VFS_EXECUTION_CALLER,
        .close_node=namespace_parallel_close,
        .root=namespace_parallel_root,.lookup=namespace_parallel_lookup,.open=namespace_parallel_open,
        .create=namespace_parallel_create,.rmdir=namespace_parallel_rmdir,.rename=namespace_parallel_rename,
        .unlink=namespace_parallel_unlink,.link=namespace_parallel_link,.truncate=namespace_parallel_truncate,
        .release_unlinked=namespace_parallel_release_unlinked};
    struct kernel_vfs_instance instance={.ops=&ops,.heap=heap,.namespace_parallel=1};
    struct kernel_vfs_mount mount={.private_data=&instance,.state=VFS_MOUNT_STATE_LIVE,.id=900};
    kernel_mutex_init(&instance.namespace_lock,20,(uintptr_t)&instance);
    namespace_parallel_instance=&instance;namespace_parallel_closed=0;
    struct namespace_parallel_request requests[2]={{.mount=&mount,.actor=0},{.mount=&mount,.actor=1}};
    pthread_t threads[2];
    namespace_parallel_pause=1;namespace_parallel_resume=0;
    memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,namespace_parallel_thread,&requests[i]));
    while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&namespace_parallel_entered[1],__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert(requests[0].file.private_data==requests[1].file.private_data && instance.node_count==1 &&
           instance.external_files==2 && namespace_parallel_closed==1);
    namespace_parallel_pause=0;
    struct kernel_vfs_path *root=NULL;
    assert(!kernel_vfs_path_root(&mount,heap,&root));
    for(unsigned i=0;i<2;i++)requests[i].root=root;
    namespace_parallel_pause=2;namespace_parallel_resume=0;
    memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
    assert(!pthread_create(&threads[0],NULL,namespace_parallel_thread,&requests[0]));
    while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&threads[1],NULL,namespace_parallel_thread,&requests[1]));
    unsigned parallel=0;
    for (;;) {
        parallel=__atomic_load_n(&namespace_parallel_entered[1],__ATOMIC_ACQUIRE);
        if (parallel) break;
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&instance.namespace_lock.lock.metadata);
          waiting=instance.namespace_lock.lock.pending_head!=NULL; }
        if (waiting) break;
        sched_yield();
    }
    __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert(parallel && requests[0].found==requests[1].found && instance.node_count==2);
    namespace_parallel_pause=0;
    for(unsigned i=0;i<2;i++)assert(!kernel_vfs_path_release(&requests[i].found) &&
                                  !kernel_vfs_close(&requests[i].file));
    assert(namespace_parallel_closed==2);
    struct kernel_vfs_path *parents[2]={NULL,NULL};
    assert(!kernel_vfs_path_lookup(root,"a",1,&parents[0]) &&
           !kernel_vfs_path_lookup(root,"b",1,&parents[1]));
    for (unsigned shared=0;shared<2;shared++) {
        memset(namespace_parallel_created,0,sizeof(namespace_parallel_created));
        memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
        namespace_parallel_creates=0;namespace_parallel_pause=3;namespace_parallel_resume=0;
        for(unsigned i=0;i<2;i++) requests[i]=(struct namespace_parallel_request){
            .mount=&mount,.root=parents[shared?0:i],.actor=i,.operation=2};
        assert(!pthread_create(&threads[0],NULL,namespace_parallel_thread,&requests[0]));
        while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE)) sched_yield();
        assert(!pthread_create(&threads[1],NULL,namespace_parallel_thread,&requests[1]));
        unsigned overlap=0;
        for (;;) {
            overlap=__atomic_load_n(&namespace_parallel_entered[1],__ATOMIC_ACQUIRE);
            if (overlap) break;
            struct kernel_vfs_node *directory=parents[shared?0:1]->file.private_data;
            unsigned waiting;
            { KERNEL_RAW_SCOPE(guard,&directory->namespace_lock.metadata);
              waiting=directory->namespace_lock.pending_head!=NULL; }
            if (waiting) break;
            sched_yield();
        }
        assert(overlap==!shared);
        __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        assert(namespace_parallel_creates==(shared?1U:2U));
        assert((requests[0].file.private_data==requests[1].file.private_data)==!!shared);
        namespace_parallel_pause=0;
        for(unsigned i=0;i<2;i++)assert(!kernel_vfs_close(&requests[i].file));
    }
    for(unsigned operation=3;operation<=4;operation++) {
        memset(namespace_parallel_created,0,sizeof(namespace_parallel_created));
        memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
        namespace_parallel_pause=3;namespace_parallel_resume=0;
        requests[0]=(struct namespace_parallel_request){.mount=&mount,.root=parents[0],.actor=0,.operation=2};
        requests[1]=(struct namespace_parallel_request){.mount=&mount,.root=root,.actor=1,.operation=operation};
        assert(!pthread_create(&threads[0],NULL,namespace_parallel_thread,&requests[0]));
        while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE)) sched_yield();
        assert(!pthread_create(&threads[1],NULL,namespace_parallel_thread,&requests[1]));
        for (;;) {
            struct kernel_vfs_node *directory=parents[0]->file.private_data;unsigned waiting;
            { KERNEL_RAW_SCOPE(guard,&directory->namespace_lock.metadata);
              waiting=directory->namespace_lock.pending_head!=NULL; }
            if (waiting) break;
            sched_yield();
        }
        __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        assert(requests[1].error==(operation==3?-KERNEL_ENOTEMPTY:0));
        if (operation==4) {
            char text[64];
            assert(!kernel_vfs_path_string(requests[0].file.path,root,text,sizeof(text)) &&
                   !strcmp(text,"/moved/created"));
        }
        namespace_parallel_pause=0;
        assert(!kernel_vfs_close(&requests[0].file));
    }
    for(unsigned operation=6;operation<=7;operation++) {
        memset(namespace_parallel_created,0,sizeof(namespace_parallel_created));
        struct kernel_vfs_file held={0};
        struct kernel_vfs_open_request open={.flags=KERNEL_VFS_OPEN_CREATE|KERNEL_VFS_OPEN_FOLLOW,.mode=0600};
        assert(!kernel_vfs_open_at(parents[0],root,"created",&open,&held,NULL));
        unsigned inode=(unsigned)kernel_vfs_file_inode(&held);
        if(operation==6)namespace_parallel_created[1]=inode;
        memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
        namespace_parallel_resume=0;
        requests[0]=(struct namespace_parallel_request){.actor=0,.operation=5,.borrowed=&held};
        requests[1]=(struct namespace_parallel_request){.root=parents[1],.actor=1,.operation=operation,.source=held.path};
        assert(!pthread_create(&threads[0],NULL,namespace_parallel_thread,&requests[0]));
        while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE)) sched_yield();
        assert(!pthread_create(&threads[1],NULL,namespace_parallel_thread,&requests[1]));
        unsigned waiting=0;
        for (;;) {
            if (__atomic_load_n(&namespace_parallel_entered[1],__ATOMIC_ACQUIRE)) break;
            struct kernel_vfs_node *node=held.private_data;
            { KERNEL_RAW_SCOPE(guard,&node->io_lock.metadata);waiting=node->io_lock.pending_head!=NULL; }
            if(waiting)break;
            sched_yield();
        }
        assert(waiting && namespace_parallel_created[1]==(operation==6?inode:0));
        __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL) && !requests[i].error);
        assert(namespace_parallel_created[1]==(operation==6?0:inode));
        assert(!kernel_vfs_close(&held));
    }
    for(unsigned alias=0;alias<2;alias++) for(unsigned held=0;held<3;held++) {
        memset(namespace_parallel_created,0,sizeof(namespace_parallel_created));
        struct kernel_vfs_file source={0},target={0};
        struct kernel_vfs_open_request open={.flags=KERNEL_VFS_OPEN_CREATE|KERNEL_VFS_OPEN_FOLLOW,.mode=0600};
        assert(!kernel_vfs_open_at(parents[0],root,"created",&open,&source,NULL));
        uint64_t source_inode=kernel_vfs_file_inode(&source);
        if(alias)namespace_parallel_created[1]=(unsigned)source_inode;
        assert(!kernel_vfs_open_at(parents[1],root,"created",&open,&target,NULL));
        uint64_t target_inode=kernel_vfs_file_inode(&target);
        if (!held) {
            assert(!kernel_vfs_rename_at(parents[0],parents[1],root,"created","created",0));
        } else {
            memset(namespace_parallel_entered,0,sizeof(namespace_parallel_entered));
            namespace_parallel_resume=0;
            requests[0]=(struct namespace_parallel_request){.actor=0,.operation=5,
                .borrowed=held==1?&source:&target};
            requests[1]=(struct namespace_parallel_request){.actor=1,.operation=8,
                .source=parents[0],.destination=parents[1],.root=root};
            assert(!pthread_create(&threads[0],NULL,namespace_parallel_thread,&requests[0]));
            while (!__atomic_load_n(&namespace_parallel_entered[0],__ATOMIC_ACQUIRE)) sched_yield();
            assert(!pthread_create(&threads[1],NULL,namespace_parallel_thread,&requests[1]));
            struct kernel_vfs_node *busy=requests[0].borrowed->private_data;
            unsigned waiting=0;
            /* Observe the actual completed rename or registration on the
             * held data lock, rather than deciding progress by elapsed time. */
            while (!__atomic_load_n(&requests[1].done,__ATOMIC_ACQUIRE)) {
                { KERNEL_RAW_SCOPE(guard,&busy->io_lock.metadata);
                  waiting=busy->io_lock.pending_head!=NULL; }
                if (waiting) break;
                sched_yield();
            }
            if (waiting!=(held==2 && !alias))
                fprintf(stderr,"rename/truncate alias=%u held=%u waiting=%u done=%u error=%d\n",
                    alias,held,waiting,__atomic_load_n(&requests[1].done,__ATOMIC_ACQUIRE),requests[1].error);
            assert(waiting==(held==2 && !alias));
            if (!waiting) assert(!requests[1].error);
            __atomic_store_n(&namespace_parallel_resume,1,__ATOMIC_RELEASE);
            for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL) && !requests[i].error);
        }
        assert(kernel_vfs_file_inode(&source)==source_inode && kernel_vfs_file_inode(&target)==target_inode);
        struct kernel_vfs_path *found=NULL;
        assert(!kernel_vfs_path_lookup(parents[1],"created",7,&found));
        assert(found==(alias?target.path:source.path));
        assert(!kernel_vfs_path_release(&found));
        if(!alias)assert(kernel_vfs_path_lookup(parents[0],"created",7,&found)==-KERNEL_ENOENT &&
                         ((struct kernel_vfs_node *)target.private_data)->unlinked);
        assert(!kernel_vfs_close(&target) && !kernel_vfs_close(&source));
    }
    /* Model the retained object after a deleted dirfd's disk number is reused.
     * It is a separate identity and cannot share the live endpoint's lock. */
    struct kernel_vfs_node retired={.instance=&instance,.mount=&mount,.inode=20,
        .references=1,.mode=KERNEL_VFS_S_IFDIR|0700,.unlinked=1,.retired=1};
    kernel_rwlock_init(&retired.namespace_lock,21,20);
    struct kernel_vfs_path retired_path={.file={.mount=&mount,.private_data=&retired,
        .mode=KERNEL_VFS_S_IFDIR|0700,.state=VFS_FILE_STATE_LIVE},.heap=heap,
        .references=1,.name="retired"};
    assert(kernel_vfs_rename_at(&retired_path,parents[1],root,"created","different",0)==-KERNEL_ENOENT &&
           retired_path.references==1 && retired.references==1);
    for(unsigned i=0;i<2;i++)assert(!kernel_vfs_path_release(&parents[i]));
    assert(!kernel_vfs_path_release(&root) && !instance.nodes && !instance.paths &&
           !instance.node_count && !instance.external_files && !instance.retiring_nodes);
    namespace_parallel_instance=NULL;
    puts("PASS parallel shared lookup and unique inode/path candidate publication; losing candidates and all owners returned");
    puts("PASS independent directory creates overlap; shared directory creation waits, rechecks the name and opens one inode");
    puts("PASS rmdir and rename wait for the child's directory qualification; moved descendant path identity survives");
    puts("PASS deleted parent identity and reused live disk inode return ENOENT before equal-key qualification");
    puts("PASS cross-parent unlink/link of one regular inode wait for actual ftruncate before changing links");
    puts("PASS rename progresses during source truncate, waits for replacement truncate and preserves open/hard-link identities");
}
#endif
/* 最后一个目录/文件路径引用可由发起任务归还，不依赖 CPU0。 */
static void namespace_retirement_case(struct kernel_heap *heap,
                                       struct kernel_vfs_mount *mount)
{
#ifdef NAMESPACE_PARALLEL_CASES
    namespace_stat_qualification_case(mount);
    namespace_directory_stat_qualification_case(heap,mount);
#endif
    struct kernel_vfs_file created={0};
    struct kernel_vfs_path *root=NULL,*child=NULL;
    assert(!kernel_vfs_create(mount,"/namespace-retirement",0600,&created));
    assert(!kernel_vfs_close(&created));
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(kernel_vfs_unlink_at(root,root,".",0)==-KERNEL_EISDIR);
    assert(kernel_vfs_unlink_at(root,root,"..",0)==-KERNEL_EISDIR);
    assert(kernel_vfs_unlink_at(root,root,"missing/..",0)==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_lookup(root,"namespace-retirement",20,&child));
    kernel_cpu_current()->logical_id=1;
    assert(!kernel_vfs_path_release(&child) && !child);
    kernel_cpu_current()->logical_id=0;
    assert(!kernel_vfs_path_release(&root));
    assert(!kernel_vfs_unlink(mount,"/namespace-retirement"));
    puts("PASS namespace final path/node retirement on CPU1");
}

struct namespace_cwd_change {
    struct kernel_fs_context *shared;
    struct kernel_vfs_path *next;
    pthread_barrier_t gate;
};
static void *namespace_change_cwd(void *argument)
{
    struct namespace_cwd_change *change=argument;
    mm_test_attach(1);mm_test_activate_task();
    kernel_cpu_current()->logical_id=1;
    pthread_barrier_wait(&change->gate);
    assert(!kernel_fs_context_set_cwd(change->shared,change->next));
    pthread_barrier_wait(&change->gate);
    mm_test_finish();return NULL;
}
static void namespace_snapshot_case(struct kernel_heap *heap,
                                    struct kernel_vfs_mount *mount)
{
    struct kernel_fs_context original={0},shared={0},forked={0},moved={0};
    assert(!kernel_vfs_mkdir(mount,"/namespace-cwd-a",0700));
    assert(!kernel_vfs_mkdir(mount,"/namespace-cwd-b",0700));
    assert(!kernel_fs_context_create(&original,mount,heap));
    struct kernel_fs_paths initial=kernel_fs_context_snapshot(&original);
    struct kernel_vfs_path *a=NULL,*b=NULL;
    assert(!kernel_vfs_path_lookup(initial.root,"namespace-cwd-a",15,&a));
    assert(!kernel_vfs_path_lookup(initial.root,"namespace-cwd-b",15,&b));
    assert(!kernel_fs_context_set_cwd(&original,a));
    assert(!kernel_fs_context_acquire(&shared,&original));
    assert(!kernel_fs_context_fork(&forked,&original));
    struct kernel_fs_paths held=kernel_fs_context_snapshot(&shared);
    struct namespace_cwd_change change={.shared=&shared,.next=b};
    pthread_t thread;
    assert(!pthread_barrier_init(&change.gate,NULL,2));
    assert(!pthread_create(&thread,NULL,namespace_change_cwd,&change));
    pthread_barrier_wait(&change.gate);
    pthread_barrier_wait(&change.gate);
    assert(!pthread_join(thread,NULL) && !pthread_barrier_destroy(&change.gate));
    char text[128];
    assert(!kernel_vfs_path_string(held.cwd,held.root,text,sizeof(text)) &&
           !strcmp(text,"/namespace-cwd-a"));
    struct kernel_vfs_path *selected=NULL;
    assert(!kernel_files_path_start(NULL,&held,KERNEL_FS_AT_FDCWD,"child",&selected) && selected==a);
    assert(!kernel_vfs_path_release(&selected));
    struct kernel_fs_paths changed=kernel_fs_context_snapshot(&original);
    assert(changed.cwd==b);
    struct kernel_fs_paths isolated=kernel_fs_context_snapshot(&forked);
    assert(isolated.cwd==a);
    assert(!kernel_fs_context_move(&moved,&shared));
    assert(!kernel_fs_context_snapshot(&shared).root);
    assert(!kernel_fs_context_release(&original));
    assert(!kernel_fs_context_release(&forked));
    assert(!kernel_fs_context_release(&moved));
    kernel_fs_paths_release(&initial);kernel_fs_paths_release(&held);
    kernel_fs_paths_release(&changed);kernel_fs_paths_release(&isolated);
    assert(!kernel_vfs_path_release(&a) && !kernel_vfs_path_release(&b));
    assert(!kernel_vfs_rmdir(mount,"/namespace-cwd-a"));
    assert(!kernel_vfs_rmdir(mount,"/namespace-cwd-b"));
    puts("PASS shared cwd replacement: owned snapshot, fork isolation, move and final context release");
}

struct namespace_creator {
    struct kernel_vfs_path *root;
    const char *name;
    pthread_barrier_t *gate;
    unsigned actor,exclusive;
    int error,created;
    uint64_t inode;
};
static void *namespace_create_competing(void *argument)
{
    struct namespace_creator *creator=argument;
    mm_test_attach(creator->actor+1);mm_test_activate_task();
    kernel_cpu_current()->logical_id=creator->actor;
    struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW |
        KERNEL_VFS_OPEN_CREATE | (creator->exclusive?KERNEL_VFS_OPEN_EXCLUSIVE:0),.mode=0600};
    struct kernel_vfs_file file={0};
    pthread_barrier_wait(creator->gate);
    creator->error=kernel_vfs_open_at(creator->root,creator->root,creator->name,
                                     &request,&file,&creator->created);
    if (!creator->error) {
        creator->inode=kernel_vfs_file_inode(&file);
        assert(!kernel_vfs_close(&file));
    }
    mm_test_finish();return NULL;
}
static void namespace_create_competing_case(struct kernel_heap *heap,
                                            struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    for(unsigned exclusive=0;exclusive<2;exclusive++) {
        pthread_barrier_t gate;pthread_t threads[2];
        struct namespace_creator creators[2];
        assert(!pthread_barrier_init(&gate,NULL,3));
        for(unsigned i=0;i<2;i++) {
            creators[i]=(struct namespace_creator){.root=root,.name="/namespace-competing",
                .gate=&gate,.actor=i,.exclusive=exclusive};
            assert(!pthread_create(&threads[i],NULL,namespace_create_competing,&creators[i]));
        }
        pthread_barrier_wait(&gate);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        assert(!pthread_barrier_destroy(&gate));
        assert(creators[0].created+creators[1].created==1);
        if(exclusive)assert((!creators[0].error && creators[1].error==-KERNEL_EEXIST) ||
                            (!creators[1].error && creators[0].error==-KERNEL_EEXIST));
        else assert(!creators[0].error && !creators[1].error &&
                    creators[0].inode==creators[1].inode);
        assert(!kernel_vfs_unlink_at(root,root,"/namespace-competing",0));
    }
    assert(!kernel_vfs_path_release(&root));
    puts("PASS competing opens: one creator, shared existing inode, exclusive loser and final owners");
}

static void namespace_open_request_case(struct kernel_heap *heap,
                                        struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL;
    struct kernel_vfs_file file={0};int created=-1;
    struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW |
        KERNEL_VFS_OPEN_CREATE,.mode=0640};
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_open_at(root,root,"/namespace-open",&request,&file,&created) && created==1);
    uint64_t inode=kernel_vfs_file_inode(&file);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_open_at(root,root,"/namespace-open",&request,&file,&created) && !created);
    assert(kernel_vfs_file_inode(&file)==inode && !kernel_vfs_close(&file));
    request.flags|=KERNEL_VFS_OPEN_EXCLUSIVE;
    request.flags&=~KERNEL_VFS_OPEN_FOLLOW;
    assert(kernel_vfs_open_at(root,root,"/namespace-open",&request,&file,&created)==-KERNEL_EEXIST && !created);
    assert(!kernel_vfs_symlink_at(root,root,"namespace-open-target","/namespace-open-link"));
    assert(kernel_vfs_open_at(root,root,"/namespace-open-link",&request,&file,&created)==-KERNEL_EEXIST);
    request.flags=KERNEL_VFS_OPEN_CREATE;
    assert(kernel_vfs_open_at(root,root,"/namespace-open-link",&request,&file,&created)==-KERNEL_ELOOP);
    request.flags|=KERNEL_VFS_OPEN_FOLLOW;
    assert(!kernel_vfs_open_at(root,root,"/namespace-open-link",&request,&file,&created) && created);
    assert(!kernel_vfs_close(&file));
    struct kernel_vfs_instance *instance=mount->private_data;
    unsigned read_only=instance->read_only;instance->read_only=1;
    request.flags|=KERNEL_VFS_OPEN_EXCLUSIVE;
    request.flags&=~KERNEL_VFS_OPEN_FOLLOW;
    assert(kernel_vfs_open_at(root,root,"/namespace-open",&request,&file,&created)==-KERNEL_EEXIST);
    assert(kernel_vfs_open_at(root,root,"/namespace-open-missing",&request,&file,&created)==-KERNEL_EROFS);
    instance->read_only=read_only;
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-open-link",0));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-open-target",0));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-open",0));
    assert(!kernel_vfs_path_release(&root));
    puts("PASS unified open/create: created identity, exclusive/symlink and read-only precedence");
}

static void namespace_walk_qualification_case(struct kernel_heap *heap,
                                              struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL;struct kernel_vfs_file file={0};
    struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW |
        KERNEL_VFS_OPEN_CREATE | KERNEL_VFS_OPEN_EXCLUSIVE,.mode=0600};
    struct kernel_vfs_instance *instance=mount->private_data;
    unsigned ordinary_qualifications=instance->namespace_parallel?0:1;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-walk",0700));
    namespace_observed_lock=&instance->namespace_lock.lock;
    namespace_lock_acquisitions=0;
    assert(!kernel_vfs_open_at(root,root,"/namespace-walk/file",&request,&file,NULL));
    namespace_observed_lock=NULL;
    assert(namespace_lock_acquisitions==ordinary_qualifications);
    assert(!kernel_vfs_close(&file));
    struct kernel_vfs_stat stat;
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_stat_at(root,root,"/namespace-walk/file",1,&stat));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==ordinary_qualifications);
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_rename_at(root,root,root,"/namespace-walk/file","/namespace-walk/new",0));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==1);
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-walk/new",0));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==ordinary_qualifications);
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-walk",1));
    assert(!kernel_vfs_path_release(&root));
    puts(instance->namespace_parallel?
        "PASS directory-qualified open/create, stat and unlink; rename takes mount topology once":
        "PASS one same-mount namespace qualification per open/create, stat, rename and unlink");
}

static void namespace_walk_workspace_case(struct kernel_heap *heap,
                                          struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL;struct kernel_vfs_file file={0};struct kernel_vfs_stat stat;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_create_at(root,root,"/namespace-workspace",0600,&file));
    uint64_t inode=kernel_vfs_file_inode(&file);assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_symlink_at(root,root,"namespace-workspace","/namespace-expansion-a"));
    assert(!kernel_vfs_symlink_at(root,root,"namespace-expansion-a","/namespace-expansion-b"));
    path_expansion_observed=path_expansion_failed=1;path_expansion_allocations=0;
    assert(!kernel_vfs_stat_at(root,root,"/namespace-workspace",1,&stat) && stat.ino==inode);
    assert(!path_expansion_allocations);
    assert(kernel_vfs_stat_at(root,root,"/namespace-expansion-b",1,&stat)==-KERNEL_ENOMEM);
    path_expansion_failed=0;path_expansion_allocations=0;
    assert(!kernel_vfs_stat_at(root,root,"/namespace-expansion-b",1,&stat) && stat.ino==inode);
    assert(path_expansion_allocations==1);
    path_expansion_observed=0;
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-expansion-a",0));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-expansion-b",0));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-workspace",0));
    assert(!kernel_vfs_path_release(&root));
    puts("PASS plain path needs no expansion allocation; two text links share one workspace; OOM returns owners");
}

/* An object-link lookup may drop the final OFD pin after another CPU closes
 * its owner. Exercise the real close path while returning the acquired path. */
struct namespace_link_close {
    pthread_barrier_t gate;
    struct kernel_open_file_description *file;
    unsigned callbacks;
};
static struct namespace_link_close *namespace_link_closing;
static int namespace_follow_last_pin(struct kernel_vfs_node *node,
                                     struct kernel_vfs_path **owner)
{
    (void)node;
    struct namespace_link_close *race=namespace_link_closing;
    struct kernel_open_file_description *pin=race->file;
    assert(kernel_open_file_acquire(pin)==KERNEL_OPEN_FILE_STATUS_OK);
    pthread_barrier_wait(&race->gate);
    pthread_barrier_wait(&race->gate);
    struct kernel_vfs_path *target=kernel_open_file_path(pin);
    assert(target && !kernel_vfs_path_acquire(target));
    *owner=target;
    assert(kernel_open_file_release(&pin)==KERNEL_OPEN_FILE_STATUS_OK && !pin);
    race->callbacks++;
    return 0;
}
static int namespace_readlink_last_pin(struct kernel_vfs_node *node,
                                       char *buffer,size_t size,size_t *count)
{
    struct kernel_vfs_path *target=NULL;
    int result=namespace_follow_last_pin(node,&target);
    if (result) return result;
    assert(!kernel_vfs_path_release(&target) && size);
    buffer[0]='T';*count=1;return 0;
}
static void *namespace_close_link_target(void *argument)
{
    struct namespace_link_close *race=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    pthread_barrier_wait(&race->gate);
    assert(kernel_open_file_release(&race->file)==KERNEL_OPEN_FILE_STATUS_OK && !race->file);
    pthread_barrier_wait(&race->gate);
    mm_test_finish();return NULL;
}
static void namespace_object_link_close_case(struct kernel_heap *heap)
{
    for (unsigned text=0;text<2;text++) {
        struct kernel_vfs_mount *mounts[2]={NULL,NULL};
        assert(!kernel_tmpfs_create(heap,0,"",&mounts[0]) && !kernel_tmpfs_create(heap,0,"",&mounts[1]));
        struct kernel_vfs_instance *a=mounts[0]->private_data,*b=mounts[1]->private_data;
        /* Either allocation order: follow from the higher real lock key to lower. */
        struct kernel_vfs_mount *source=a->namespace_lock.lock.key>b->namespace_lock.lock.key ? mounts[0]:mounts[1];
        struct kernel_vfs_mount *target=source==mounts[0] ? mounts[1]:mounts[0];
        struct kernel_vfs_path *from=NULL,*to=NULL,*found=NULL;
        assert(!kernel_vfs_path_root(source,heap,&from) && !kernel_vfs_path_root(target,heap,&to));
        struct namespace_link_close race={0};int error=0;
        struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW | KERNEL_VFS_OPEN_CREATE,.mode=0600};
        assert(kernel_open_file_create_at(heap,to,to,"/last-object",&request,&race.file,NULL,&error)==KERNEL_OPEN_FILE_STATUS_OK && !error);
        uint64_t inode=kernel_vfs_file_inode(kernel_open_file_vfs_borrow(race.file));
        assert(!kernel_vfs_unlink_at(to,to,"/last-object",0));
        assert(!kernel_vfs_symlink_at(from,from,"unused-display-text","/object-link"));
        struct kernel_vfs_instance *instance=source->private_data;
        const struct kernel_vfs_backend *original=instance->ops;
        struct kernel_vfs_backend callbacks=*original;callbacks.follow_link=namespace_follow_last_pin;
        callbacks.readlink=namespace_readlink_last_pin;
        instance->ops=&callbacks;namespace_link_closing=&race;
        pthread_t closer;assert(!pthread_barrier_init(&race.gate,NULL,2));
        assert(!pthread_create(&closer,NULL,namespace_close_link_target,&race));
        if (text) {
            char display[4];size_t size=0;
            assert(!kernel_vfs_readlink_at(from,from,"/object-link",display,sizeof(display),&size) && size==1 && display[0]=='T');
        } else assert(!kernel_vfs_path_resolve(from,from,"/object-link",1,&found));
        assert(!pthread_join(closer,NULL) && !pthread_barrier_destroy(&race.gate));
        assert(race.callbacks==1 && !race.file);
        if (!text) {
            assert(kernel_vfs_path_mount(found)==target);
            struct kernel_vfs_stat stat;assert(!kernel_vfs_path_stat(found,&stat) && stat.ino==inode && !stat.nlink);
            assert(!kernel_vfs_path_release(&found));
        }
        namespace_link_closing=NULL;instance->ops=original;
        assert(!kernel_vfs_unlink_at(from,from,"/object-link",0));
        assert(!kernel_vfs_path_release(&from) && !kernel_vfs_path_release(&to));
        assert(!kernel_vfs_unmount(mounts[0]) && !kernel_vfs_unmount(mounts[1]));
    }
    puts("PASS follow/readlink vs CPU1 last close: foreign unlinked OFD retires without nested namespaces; path identity and owners");
}

struct namespace_parent_move {
    struct kernel_vfs_path *root;
    pthread_barrier_t gate;
};
static void *namespace_move_child(void *argument)
{
    struct namespace_parent_move *move=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    pthread_barrier_wait(&move->gate);
    assert(!kernel_vfs_rename_at(move->root,move->root,move->root,
        "/namespace-old-parent/child","/namespace-new-parent/child",0));
    assert(!kernel_vfs_unlink_at(move->root,move->root,"/namespace-old-parent",1));
    mm_test_finish();return NULL;
}
static void namespace_parent_snapshot_case(struct kernel_heap *heap,
                                           struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*child=NULL,*parent;
    struct kernel_vfs_file file={0};struct kernel_vfs_stat stat;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-old-parent",0700));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-new-parent",0700));
    assert(!kernel_vfs_create_at(root,root,"/namespace-old-parent/child",0600,&file));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_resolve(root,root,"/namespace-old-parent/child",1,&child));
    parent=kernel_vfs_path_parent_pin(child);assert(parent);
    uint64_t inode=kernel_vfs_path_inode(parent);
    struct namespace_parent_move move={.root=root};pthread_t thread;
    assert(!pthread_barrier_init(&move.gate,NULL,2));
    assert(!pthread_create(&thread,NULL,namespace_move_child,&move));
    pthread_barrier_wait(&move.gate);
    assert(!pthread_join(thread,NULL) && !pthread_barrier_destroy(&move.gate));
    assert(!kernel_vfs_path_stat(parent,&stat) && stat.ino==inode && !stat.nlink);
    struct kernel_vfs_path *next=kernel_vfs_path_parent_pin(child);
    assert(next && kernel_vfs_path_inode(next)!=inode);
    assert(!kernel_vfs_path_release(&next) && !kernel_vfs_path_release(&parent));
    assert(!kernel_vfs_path_release(&child));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-new-parent/child",0));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-new-parent",1));
    assert(!kernel_vfs_path_release(&root));
    puts("PASS owned parent snapshot survives remote child rename and old-parent removal");
}

/* 目录身份由 dirfd/path 引用固定，改名后直接在同一个父 inode 下操作。 */
static void namespace_parent_case(struct kernel_heap *heap,
                                  struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*parent=NULL,*child=NULL;
    struct kernel_vfs_file file={0};struct kernel_vfs_stat stat;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-parent",02770));
    assert(!kernel_vfs_path_lookup(root,"namespace-parent",16,&parent));
    assert(kernel_vfs_unlink_at(root,root,"namespace-parent/.",0)==-KERNEL_EISDIR);
    assert(!kernel_vfs_rename_at(root,root,root,"/namespace-parent","/namespace-renamed",0));
    char basename[12];
    assert(!kernel_vfs_path_copy_name(parent,basename,sizeof(basename)) && !strcmp(basename,"namespace-r"));
    assert(!kernel_vfs_create_at(parent,root,"data",0640,&file));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_mkdir_at(parent,root,"nested",0750));
    assert(!kernel_vfs_stat_at(parent,root,"nested",1,&stat) && (stat.mode&02000));
    assert(!kernel_vfs_symlink_at(parent,root,"data","alias"));
    char text[16];size_t count;
    assert(!kernel_vfs_readlink_at(parent,root,"alias",text,sizeof(text),&count) &&
        count==4 && !memcmp(text,"data",4));
    assert(!kernel_vfs_path_lookup(parent,"data",4,&child));
    assert(!kernel_vfs_link_at(child,parent,root,"hard"));
    assert(!kernel_vfs_mknod_at(parent,root,"fifo",KERNEL_VFS_S_IFIFO|0600,0));
    assert(!kernel_vfs_unlink_at(parent,root,"data",0));
    assert(!kernel_vfs_path_stat(child,&stat) && stat.nlink==1);
    assert(!kernel_vfs_path_release(&child));
    assert(!kernel_vfs_unlink_at(parent,root,"hard",0));
    assert(!kernel_vfs_unlink_at(parent,root,"alias",0));
    assert(!kernel_vfs_unlink_at(parent,root,"fifo",0));
    struct kernel_vfs_node *directory_node=kernel_vfs_file_node(&parent->file);
    ext4_dir iterator={.f=*lwext4_node_file(directory_node),.next_off=UINT64_MAX-3};
    iterator.f.fsize=0;ext4_direntry entry;uint64_t offset;
    assert(!ext4_dir_entry_next_status(&iterator,&entry,&offset) && offset==UINT64_MAX && iterator.f.fsize>0);
    assert(!kernel_vfs_unlink_at(parent,root,"nested",1));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-renamed",1));
    assert(kernel_vfs_create_at(parent,root,"gone",0600,&file)==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_release(&parent) && !kernel_vfs_path_release(&root));
    puts("PASS parent inode namespace: renamed dirfd, setgid, link/symlink/fifo, deletion and dead directory");
}

static unsigned namespace_retire_entered,namespace_retire_resume;
static int (*namespace_original_close)(struct kernel_vfs_node *);
static int namespace_pause_close(struct kernel_vfs_node *node)
{
    __atomic_store_n(&namespace_retire_entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&namespace_retire_resume,__ATOMIC_ACQUIRE)) sched_yield();
    return namespace_original_close(node);
}
static void *namespace_retire_node(void *argument)
{
    struct kernel_vfs_node **owner=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    assert(!kernel_vfs_node_release(owner));mm_test_finish();return NULL;
}
static void namespace_retiring_mount_case(struct kernel_heap *heap)
{
#ifdef NAMESPACE_PARALLEL_CASES
    namespace_parallel_publication_case(heap);
#endif
    struct kernel_vfs_mount *mount=NULL;
    struct kernel_vfs_file file={0};
    assert(!kernel_tmpfs_create(heap,0,"",&mount));
    assert(!kernel_vfs_create(mount,"/retiring",0600,&file));
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    assert(!kernel_vfs_node_acquire(node) && !kernel_vfs_close(&file));
    struct kernel_vfs_instance *instance=mount->private_data;
    const struct kernel_vfs_backend *original=instance->ops;
    struct kernel_vfs_backend observed=*original;
    namespace_original_close=observed.close_node;observed.close_node=namespace_pause_close;
    instance->ops=&observed;
    namespace_retire_entered=namespace_retire_resume=0;pthread_t thread;
    assert(!pthread_create(&thread,NULL,namespace_retire_node,&node));
    while (!__atomic_load_n(&namespace_retire_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* The weak index is empty, but close still borrows the backend and heap. */
    assert(!instance->nodes && !instance->external_files);
    assert(kernel_vfs_unmount(mount)==-KERNEL_EBUSY);
    __atomic_store_n(&namespace_retire_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,NULL) && !node);
    instance->ops=original;
    assert(!kernel_vfs_unmount(mount));
    puts("PASS unmount retains backend/heap until final node retirement actually ends");
}

struct namespace_publication_race {
    struct kernel_fs_context *fs;
    struct kernel_vfs_path *renamed;
    pthread_barrier_t gate;
};
static void *namespace_copy_publication(void *argument)
{
    struct namespace_publication_race *race=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    for (unsigned round=0;round<64;round++) {
        pthread_barrier_wait(&race->gate);
        struct kernel_fs_paths paths=kernel_fs_context_snapshot(race->fs);
        char name[32];
        assert(!kernel_vfs_path_copy_name(race->renamed,name,sizeof(name)) &&
            (!strcmp(name,"publication-a") || !strcmp(name,"publication-b")));
        kernel_fs_paths_release(&paths);
        pthread_barrier_wait(&race->gate);
    }
    mm_test_finish();return NULL;
}
static void namespace_publication_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_mount *child=NULL;
    struct kernel_fs_context fs={0};struct kernel_vfs_path *root=NULL,*covered=NULL,*renamed=NULL;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"publication-mount",0700) &&
           !kernel_vfs_mkdir_at(root,root,"publication-a",0700));
    assert(!kernel_vfs_path_lookup(root,"publication-mount",17,&covered) &&
           !kernel_vfs_path_lookup(root,"publication-a",13,&renamed));
    assert(!kernel_fs_context_create(&fs,mount,heap) &&
           !kernel_fs_context_set_cwd(&fs,covered));
    uint32_t references=covered->references;
    struct namespace_publication_race race={.fs=&fs,.renamed=renamed};pthread_t thread;
    assert(!pthread_barrier_init(&race.gate,NULL,2) && !pthread_create(&thread,NULL,namespace_copy_publication,&race));
    for (unsigned round=0;round<64;round++) {
        pthread_barrier_wait(&race.gate);
        assert(!kernel_tmpfs_create(heap,0,"",&child) && !kernel_vfs_mount_attach(child,covered));
        assert(!kernel_vfs_rename_at(root,root,root,round&1?"publication-b":"publication-a",
            round&1?"publication-a":"publication-b",0));
        assert(!kernel_vfs_mount_prepare_detach(child,NULL) && !kernel_vfs_mount_detach(child,NULL));
        assert(!kernel_vfs_unmount(child));child=NULL;
        pthread_barrier_wait(&race.gate);
    }
    assert(!pthread_join(thread,NULL) && !pthread_barrier_destroy(&race.gate));
    assert(covered->references==references);
    assert(!kernel_fs_context_release(&fs));
    assert(!kernel_vfs_path_release(&renamed) && !kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_unlink_at(root,root,"publication-a",1) &&
           !kernel_vfs_unlink_at(root,root,"publication-mount",1) && !kernel_vfs_path_release(&root));
    puts("PASS mount edge acquire/detach vs owned cwd snapshots; rename vs bounded comm name copies; final refs");
}

struct namespace_slot_race { struct kernel_vfs_path *root; pthread_barrier_t gate; };
static void *namespace_lookup_later_slot(void *argument)
{
    struct namespace_slot_race *race=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    for (unsigned round=0;round<32;round++) {
        pthread_barrier_wait(&race->gate);
        for (unsigned i=0;i<8;i++) {
            struct kernel_vfs_path *found=NULL;
            assert(!kernel_vfs_path_lookup(race->root,"lost+found",10,&found));
            struct kernel_vfs_stat st;
            assert(!kernel_vfs_path_stat(found,&st) && (st.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR);
            assert(!kernel_vfs_path_release(&found));
        }
        pthread_barrier_wait(&race->gate);
    }
    mm_test_finish();return NULL;
}
static enum kernel_block_status namespace_slot_read(void *unused,uint64_t offset,void *data,size_t size)
{
    (void)unused;assert(!kernel_cpu_current()->raw_locks);
    return pread(image,data,size,offset)==(ssize_t)size?KERNEL_BLOCK_STATUS_OK:KERNEL_BLOCK_STATUS_IO;
}
static void namespace_mount_slot_case(struct kernel_heap *heap,struct kernel_heap *backend_heap,
    struct physical_page_allocator *pages,uint64_t capacity)
{
    /* Independent read-only device identities read one immutable fixture. */
    struct kernel_block_device disks[2]={0};struct kernel_page_cache caches[2]={0};
    struct kernel_vfs_mount mounts[2]={0};
    for (unsigned i=0;i<2;i++) {
        disks[i]=(struct kernel_block_device){.read=namespace_slot_read,.capacity_bytes=capacity,.logical_block_size=512};
        assert(!kernel_block_register(&disks[i],KERNEL_BLOCK_DEVICE_NUMBER(101+i)) &&
               !kernel_page_cache_init(&caches[i],heap,pages) &&
               !kernel_vfs_mount_ext4(&mounts[i],&disks[i],backend_heap,&caches[i],1));
    }
    struct namespace_slot_race race={0};pthread_t thread;
    assert(!kernel_vfs_path_root(&mounts[1],heap,&race.root) &&
           !pthread_barrier_init(&race.gate,NULL,2) && !pthread_create(&thread,NULL,namespace_lookup_later_slot,&race));
    for (unsigned round=0;round<32;round++) {
        pthread_barrier_wait(&race.gate);
        assert(!kernel_vfs_unmount(&mounts[0]) &&
               !kernel_vfs_mount_ext4(&mounts[0],&disks[0],backend_heap,&caches[0],1));
        pthread_barrier_wait(&race.gate);
    }
    assert(!pthread_join(thread,NULL) && !pthread_barrier_destroy(&race.gate) && !kernel_vfs_path_release(&race.root));
    for (unsigned i=0;i<2;i++) assert(!kernel_vfs_unmount(&mounts[i]) &&
        !kernel_page_cache_destroy(&caches[i]) && !kernel_block_unregister(&disks[i]));
    puts("PASS CPU1 lookup of later ext4 mount while CPU0 unmounts and reuses earlier slot; final owners");
}
