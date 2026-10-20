#include "../../fs/ext4_backend.h"
/* 最后一个目录/文件路径引用可由发起任务归还，不依赖 CPU0。 */
static void namespace_retirement_case(struct kernel_heap *heap,
                                       struct kernel_vfs_mount *mount)
{
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
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-walk",0700));
    namespace_observed_lock=&instance->namespace_lock.lock;
    namespace_lock_acquisitions=0;
    assert(!kernel_vfs_open_at(root,root,"/namespace-walk/file",&request,&file,NULL));
    namespace_observed_lock=NULL;
    assert(namespace_lock_acquisitions==1);
    assert(!kernel_vfs_close(&file));
    struct kernel_vfs_stat stat;
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_stat_at(root,root,"/namespace-walk/file",1,&stat));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==1);
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_rename_at(root,root,root,"/namespace-walk/file","/namespace-walk/new",0));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==1);
    namespace_lock_acquisitions=0;namespace_observed_lock=&instance->namespace_lock.lock;
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-walk/new",0));
    namespace_observed_lock=NULL;assert(namespace_lock_acquisitions==1);
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-walk",1));
    assert(!kernel_vfs_path_release(&root));
    puts("PASS one same-mount namespace qualification per open/create, stat, rename and unlink");
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
