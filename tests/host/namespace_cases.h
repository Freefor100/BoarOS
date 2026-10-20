/* 最后一个目录/文件路径引用可由发起任务归还，不依赖 CPU0。 */
static void namespace_retirement_case(struct kernel_heap *heap,
                                       struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file created={0};
    struct kernel_vfs_path *root=NULL,*child=NULL;
    assert(!kernel_vfs_create(mount,"/namespace-retirement",0600,&created));
    assert(!kernel_vfs_close(&created));
    assert(!kernel_vfs_path_root(mount,heap,&root));
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

/* 目录身份由 dirfd/path 引用固定，改名后直接在同一个父 inode 下操作。 */
static void namespace_parent_case(struct kernel_heap *heap,
                                  struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*parent=NULL,*child=NULL;
    struct kernel_vfs_file file={0};struct kernel_vfs_stat stat;
    assert(!kernel_vfs_path_root(mount,heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/namespace-parent",02770));
    assert(!kernel_vfs_path_lookup(root,"namespace-parent",16,&parent));
    assert(!kernel_vfs_rename_at(root,root,root,"/namespace-parent","/namespace-renamed",0));
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
    assert(!kernel_vfs_unlink_at(parent,root,"nested",1));
    assert(!kernel_vfs_unlink_at(root,root,"/namespace-renamed",1));
    assert(kernel_vfs_create_at(parent,root,"gone",0600,&file)==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_release(&parent) && !kernel_vfs_path_release(&root));
    puts("PASS parent inode namespace: renamed dirfd, setgid, link/symlink/fifo, deletion and dead directory");
}
