/* Real VFS/backend pathname semantics; the caller fixture audits final owners. */
struct socket_path_race {
    struct kernel_vfs_path *root,*bound;
    uint64_t original;
};
static void *socket_bind_thread(void *argument)
{
    struct socket_path_race *race=argument;cache_test_attach(1);
    assert(!kernel_vfs_socket_bind_at(race->root,race->root,"/socket-race",0600,&race->bound));
    race->original=kernel_vfs_path_inode(race->bound);
    return NULL;
}
static void *socket_replace_thread(void *argument)
{
    struct socket_path_race *race=argument;cache_test_attach(2);
    assert(!kernel_vfs_unlink_at(race->root,race->root,"/socket-race",0));
    assert(!kernel_vfs_mknod_at(race->root,race->root,"/socket-race",KERNEL_VFS_S_IFSOCK|0666,0));
    __atomic_store_n(&socket_replace_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void vfs_socket_create_interleave(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    struct ext4_journal_runtime runtime={.now_ns=metadata_clock};
    assert(!ext4_journal_group_enable(adapter->mount_point,&runtime,4*1024*1024));
    boaros_lwext4_enable_parallel(&adapter->backend_lock,adapter->device.fs);
    adapter->instance.namespace_parallel=1;
    struct socket_path_race race={0};
    assert(!kernel_vfs_path_root(mount,adapter->instance.heap,&race.root));
    socket_create_pause=1;socket_create_entered=socket_create_resume=socket_replace_done=0;
    pthread_t binder,replacer;
    assert(!pthread_create(&binder,NULL,socket_bind_thread,&race));
    wait_progress(&socket_create_entered,1);
    unsigned parked=mm_test_parked(2);
    assert(!pthread_create(&replacer,NULL,socket_replace_thread,&race));
    while (mm_test_parked(2)==parked) sched_yield();
    assert(!__atomic_load_n(&socket_replace_done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&socket_create_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(binder,NULL) && !pthread_join(replacer,NULL));
    socket_create_pause=0;
    struct kernel_vfs_path *replacement=NULL;
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_path_stat(race.bound,&stat) && !stat.nlink && stat.ino==race.original);
    assert(!kernel_vfs_socket_lookup_at(race.root,race.root,"/socket-race",&replacement));
    assert(kernel_vfs_path_inode(replacement)!=race.original);
    assert(!kernel_vfs_unlink_at(race.root,race.root,"/socket-race",0));
    assert(!kernel_vfs_path_release(&replacement) && !kernel_vfs_path_release(&race.bound) &&
        !kernel_vfs_path_release(&race.root));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    puts("PASS socket bind interleave: replacement blocks during create, bound owner retains original unlinked inode, new name identifies replacement");
}

static void vfs_socket_paths(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*bound=NULL,*lookup=NULL,*alias=NULL,*replacement=NULL;
    struct kernel_vfs_instance *instance=mount->private_data;
    assert(!kernel_vfs_path_root(mount,instance->heap,&root));
    if (!kernel_tmpfs_is_mount(mount) && !lwext4_instance(instance)->device.fs->jbd_journal) {
        assert(kernel_vfs_mknod_at(root,root,"/socket-node",KERNEL_VFS_S_IFSOCK|0620,0)==-KERNEL_EOPNOTSUPP);
        assert(kernel_vfs_socket_bind_at(root,root,"/socket-node",0620,&bound)==-KERNEL_EOPNOTSUPP && !bound);
        assert(kernel_vfs_socket_lookup_at(root,root,"/socket-node",&lookup)==-KERNEL_ENOENT && !lookup);
        assert(!kernel_vfs_path_release(&root));
        puts("PASS socket pathname: nonjournal creation refusal leaves no node or owner");
        return;
    }
    assert(!kernel_vfs_mknod_at(root,root,"/socket-node",KERNEL_VFS_S_IFSOCK|0620,0));
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_stat_at(root,root,"/socket-node",1,&stat));
    assert((stat.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFSOCK && (stat.mode & 0777)==0620);
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-node",&lookup));
    assert(!kernel_vfs_path_release(&lookup));
    struct kernel_vfs_file file={0};
    assert(kernel_vfs_open(mount,"/socket-node",&file)==-KERNEL_ENXIO && !file.private_data && !file.path);
    assert(!kernel_vfs_unlink_at(root,root,"/socket-node",0));

    assert(!kernel_vfs_socket_bind_at(root,root,"/socket-node",0620,&bound));
    assert(!kernel_vfs_path_stat(bound,&stat));
    uint64_t original=stat.ino;
    assert((stat.mode & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFSOCK && (stat.mode & 0777)==0620);
    assert(kernel_vfs_socket_bind_at(root,root,"/socket-node",0600,&replacement)==-KERNEL_EEXIST && !replacement);
    assert(!kernel_vfs_path_stat(bound,&stat) && stat.ino==original && (stat.mode & 0777)==0620);
    assert(!kernel_vfs_link_at(bound,root,root,"/socket-alias"));
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-alias",&alias));
    assert(kernel_vfs_path_inode(alias)==original && kernel_vfs_path_mount(alias)==mount);
    assert(!kernel_vfs_symlink_at(root,root,"socket-alias","/socket-link"));
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-link",&lookup));
    assert(kernel_vfs_path_inode(lookup)==original && !kernel_vfs_path_release(&lookup));
    assert(!kernel_vfs_rename_at(root,root,root,"/socket-node","/socket-moved",0));
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-moved",&lookup));
    assert(kernel_vfs_path_inode(lookup)==original && !kernel_vfs_path_release(&lookup));
    assert(!kernel_vfs_unlink_at(root,root,"/socket-moved",0));
    assert(!kernel_vfs_unlink_at(root,root,"/socket-alias",0));
    assert(!kernel_vfs_path_stat(bound,&stat) && stat.ino==original && !stat.nlink);
    assert(kernel_vfs_socket_lookup_at(root,root,"/socket-link",&lookup)==-KERNEL_ENOENT && !lookup);
    assert(!kernel_vfs_socket_bind_at(root,root,"/socket-moved",0600,&replacement));
    assert(kernel_vfs_path_inode(replacement)!=original);
    assert(!kernel_vfs_path_release(&bound) && !kernel_vfs_path_release(&alias));
    assert(!kernel_vfs_unlink_at(root,root,"/socket-moved",0));
    assert(!kernel_vfs_path_release(&replacement));
    assert(!kernel_vfs_unlink_at(root,root,"/socket-link",0));

    assert(!kernel_vfs_create_at(root,root,"/regular",0666,&file));
    assert(!kernel_vfs_close(&file));
    assert(kernel_vfs_socket_lookup_at(root,root,"/regular",&lookup)==-KERNEL_ECONNREFUSED && !lookup);
    assert(!kernel_vfs_unlink_at(root,root,"/regular",0));
    assert(!kernel_vfs_mkdir_at(root,root,"/socket-dir",02777));
    struct kernel_vfs_path *directory=NULL;
    assert(!kernel_vfs_path_resolve(root,root,"/socket-dir",1,&directory));
    assert(!kernel_vfs_path_set_owner(directory,UINT32_MAX,200));
    assert(!kernel_vfs_path_set_mode(directory,02777));
    const struct kernel_credentials *privileged=kernel_credentials_current();
    struct kernel_credentials user=*privileged,other;
    user.uid=user.euid=user.suid=user.fsuid=1000;
    user.gid=user.egid=user.sgid=user.fsgid=100;
    user.effective=user.permitted=user.inheritable=user.ambient=0;
    other=user;other.uid=other.euid=other.suid=other.fsuid=1001;
    (void)kernel_credentials_override(&user);
    assert(!kernel_vfs_socket_bind_at(root,root,"/socket-dir/owned",0600,&bound));
    assert(!kernel_vfs_path_stat(bound,&stat) && stat.uid==1000 && stat.gid==200 && (stat.mode&0777)==0600);
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-dir/owned",&lookup));
    assert(!kernel_vfs_path_release(&lookup));
    (void)kernel_credentials_override(&other);
    assert(kernel_vfs_socket_lookup_at(root,root,"/socket-dir/owned",&lookup)==-KERNEL_EACCES && !lookup);
    (void)kernel_credentials_override(privileged);
    assert(!kernel_vfs_path_set_mode(bound,0666));
    assert(!kernel_vfs_path_set_mode(directory,0700));
    (void)kernel_credentials_override(&other);
    assert(kernel_vfs_socket_lookup_at(root,root,"/socket-dir/owned",&lookup)==-KERNEL_EACCES && !lookup);
    assert(kernel_vfs_socket_bind_at(root,root,"/socket-dir/denied",0600,&replacement)==-KERNEL_EACCES && !replacement);
    (void)kernel_credentials_override(privileged);
    assert(!kernel_vfs_path_set_mode(directory,0555));
    (void)kernel_credentials_override(&user);
    assert(kernel_vfs_socket_bind_at(root,root,"/socket-dir/denied",0600,&replacement)==-KERNEL_EACCES && !replacement);
    (void)kernel_credentials_override(privileged);
    instance->read_only=1;
    assert(!kernel_vfs_socket_lookup_at(root,root,"/socket-dir/owned",&lookup));
    assert(!kernel_vfs_path_release(&lookup));
    assert(kernel_vfs_socket_bind_at(root,root,"/readonly-socket",0600,&replacement)==-KERNEL_EROFS && !replacement);
    instance->read_only=0;
    assert(!kernel_vfs_unlink_at(root,root,"/socket-dir/owned",0));
    assert(!kernel_vfs_path_release(&bound) && !kernel_vfs_path_release(&directory));
    assert(!kernel_vfs_unlink_at(root,root,"/socket-dir",1));
    assert(!kernel_vfs_path_release(&root));
    if (!kernel_tmpfs_is_mount(mount)) vfs_socket_create_interleave(mount);
    puts("PASS socket pathname: mknod/stat/ENXIO, owned bind, duplicate, hardlink/symlink/rename/unlink identity, replacement, DAC/search/SGID/readonly");
}
