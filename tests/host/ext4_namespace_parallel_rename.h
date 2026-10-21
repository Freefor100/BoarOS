/* A single caller still needs a globally ordered set of actual inode owners. */
static void rename_order_case(struct lwext4_mount_adapter *adapter,struct kernel_vfs_path *root)
{
    struct kernel_vfs_file source={0},victim={0};
    assert(!kernel_vfs_create_at(root,root,"rename-order-source",0600,&source) &&
           !kernel_vfs_create_at(root,root,"rename-order-victim",0600,&victim));
    assert(!kernel_vfs_mkdir_at(root,root,"rename-order-left",0700) &&
           !kernel_vfs_mkdir_at(root,root,"rename-order-right",0700));
    struct kernel_vfs_path *parents[2]={NULL,NULL};
    assert(!kernel_vfs_path_lookup(root,"rename-order-left",17,&parents[0]) &&
           !kernel_vfs_path_lookup(root,"rename-order-right",18,&parents[1]));
    if(kernel_vfs_path_inode(parents[0])>kernel_vfs_path_inode(parents[1])) {
        struct kernel_vfs_path *swap=parents[0];parents[0]=parents[1];parents[1]=swap;
    }
    uint64_t source_inode=kernel_vfs_file_inode(&source),victim_inode=kernel_vfs_file_inode(&victim);
    assert(source_inode<kernel_vfs_path_inode(parents[0]) &&
           victim_inode<kernel_vfs_path_inode(parents[0]));
    assert(!kernel_vfs_link_at(source.path,parents[0],root,"first") &&
           !kernel_vfs_link_at(victim.path,parents[1],root,"replace") &&
           !kernel_vfs_unlink_at(root,root,"rename-order-source",0) &&
           !kernel_vfs_unlink_at(root,root,"rename-order-victim",0) &&
           !ext4_journal_group_drain(adapter->mount_point));
    struct {
        unsigned from,to;
        const char *old_name,*new_name;
    } cases[]={
        {0,0,"first","second"},{0,0,"second","first"},
        {0,1,"first","second"},{1,0,"second","first"},
        {0,1,"first","replace"},
    };
    unsigned aborts=0,retries=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        rename_order_aborts=rename_order_retries=0;rename_order_watch=1;
        int result=kernel_vfs_rename_at(parents[cases[i].from],parents[cases[i].to],root,
            cases[i].old_name,cases[i].new_name,0);
        rename_order_watch=0;
        assert(!result);
        aborts+=rename_order_aborts;retries+=rename_order_retries;
        struct kernel_vfs_path *found=NULL;
        assert(!kernel_vfs_path_lookup(parents[cases[i].to],cases[i].new_name,
            strlen(cases[i].new_name),&found) && kernel_vfs_path_inode(found)==source_inode &&
            !kernel_vfs_path_release(&found));
        printf("rename known inode order: old=%llu new=%llu source=%llu replaced=%llu aborts=%u retries=%u\n",
            (unsigned long long)kernel_vfs_path_inode(parents[cases[i].from]),
            (unsigned long long)kernel_vfs_path_inode(parents[cases[i].to]),
            (unsigned long long)source_inode,(unsigned long long)(i==4?victim_inode:0),
            rename_order_aborts,rename_order_retries);
    }
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(&source,&stat) && stat.ino==source_inode && stat.nlink==1);
    assert(!kernel_vfs_fstat(&victim,&stat) && stat.ino==victim_inode && !stat.nlink);
    assert(!kernel_vfs_close(&source) && !kernel_vfs_close(&victim) &&
           !kernel_vfs_unlink_at(parents[1],root,"replace",0));
    for(unsigned i=0;i<2;i++)assert(!kernel_vfs_path_release(&parents[i]));
    assert(!kernel_vfs_unlink_at(root,root,"rename-order-left",1) &&
           !kernel_vfs_unlink_at(root,root,"rename-order-right",1) &&
           !ext4_journal_group_drain(adapter->mount_point));
    assert(!aborts && !retries);
    puts("PASS rename known inode set: same/cross, ascending/descending parents, lower source and replacement complete without resource abort/replay");
}

static void rename_order_directory_case(struct lwext4_mount_adapter *adapter,
                                        struct kernel_vfs_path *root)
{
    const char *names[]={"rename-order-tree","rename-order-empty",
                         "rename-order-parent-a","rename-order-parent-b"};
    struct kernel_vfs_path *paths[4]={0};
    const char *ordered_names[4];
    for(unsigned i=0;i<4;i++) {
        assert(!kernel_vfs_mkdir_at(root,root,names[i],0700));
        assert(!kernel_vfs_path_lookup(root,names[i],strlen(names[i]),&paths[i]));
        ordered_names[i]=names[i];
    }
    for(unsigned i=0;i<4;i++)for(unsigned j=i+1;j<4;j++) {
        if(kernel_vfs_path_inode(paths[i])<kernel_vfs_path_inode(paths[j]))continue;
        struct kernel_vfs_path *swap=paths[i];paths[i]=paths[j];paths[j]=swap;
        const char *name=ordered_names[i];ordered_names[i]=ordered_names[j];ordered_names[j]=name;
    }
    struct kernel_vfs_path *source=paths[0],*victim=paths[1];
    struct kernel_vfs_path **parents=&paths[2];
    uint64_t source_inode=kernel_vfs_path_inode(source);
    assert(source_inode<kernel_vfs_path_inode(parents[0]));
    assert(!kernel_vfs_rename_at(root,parents[0],root,ordered_names[0],"tree",0) &&
           !kernel_vfs_rename_at(root,parents[1],root,ordered_names[1],"empty",0));
    const unsigned from[]={0,1,0},to[]={1,0,1};
    const char *old[]={"tree","tree","tree"},*new[]={"tree","tree","empty"};
    for(unsigned i=0;i<3;i++) {
        rename_order_aborts=rename_order_retries=0;rename_order_watch=1;
        int r=kernel_vfs_rename_at(parents[from[i]],parents[to[i]],root,old[i],new[i],0);
        rename_order_watch=0;
        assert(!r && !rename_order_aborts && !rename_order_retries);
        struct kernel_vfs_path *found=NULL;
        assert(!kernel_vfs_path_lookup(parents[to[i]],new[i],strlen(new[i]),&found) &&
               found==source && kernel_vfs_path_inode(found)==source_inode &&
               !kernel_vfs_path_release(&found));
    }
    assert(!kernel_vfs_path_release(&source) && !kernel_vfs_path_release(&victim) &&
           !kernel_vfs_unlink_at(parents[1],root,"empty",1));
    for(unsigned i=2;i<4;i++)assert(!kernel_vfs_path_release(&paths[i]));
    assert(!kernel_vfs_unlink_at(root,root,ordered_names[2],1) &&
           !kernel_vfs_unlink_at(root,root,ordered_names[3],1) &&
           !ext4_journal_group_drain(adapter->mount_point));
    puts("PASS rename directory known set: lower source, ascending/descending parents, ancestor reads and empty replacement complete without resource abort/replay");
}
