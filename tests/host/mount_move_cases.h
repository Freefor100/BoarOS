static void move_resolve(struct kernel_vfs_path *root,const char *name,struct kernel_vfs_path **owner)
{ assert(!kernel_vfs_path_resolve(root,root,name,1,owner)); }
static void move_name(struct kernel_vfs_path *path,struct kernel_vfs_path *root,const char *expected)
{ char actual[256];assert(!kernel_vfs_path_string(path,root,actual,sizeof(actual))&&!strcmp(actual,expected)); }
static void move_content(struct kernel_vfs_file *file)
{ char bytes[5]={0};size_t done;assert(!kernel_vfs_pread(file,0,bytes,4,&done,NULL)&&done==4&&!strcmp(bytes,"move")); }
static void mount_move(struct kernel_vfs_mount *base)
{
    struct kernel_vfs_path *root=NULL,*source=NULL,*target=NULL,*held=NULL,*path=NULL,*nested=NULL;
    struct kernel_vfs_mount *memory=NULL,*child=NULL,*parent=NULL,*lower=NULL;
    assert(!kernel_vfs_path_root(base,&heap,&root));
    const char *dirs[]={"old","next","parent","layer","failed","failed/point","copy"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++)assert(!kernel_vfs_mkdir_at(root,root,dirs[i],0755,0));
    struct kernel_vfs_file file={0};assert(!kernel_vfs_create_at(root,root,"single",0644,&file,0)&&!kernel_vfs_close(&file));
    move_resolve(root,"old",&target);assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
    assert(!kernel_vfs_mount_attach(memory,target)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"old",&source);assert(!kernel_vfs_mkdir_at(source,root,"sub",0755,0));
    assert(!kernel_vfs_mkdir_at(source,root,"child",0755,0));move_resolve(root,"old/sub",&held);
    assert(!kernel_vfs_create_at(source,root,"data",0644,&file,0));size_t done;
    assert(!kernel_vfs_pwrite(&file,0,"move",4,&done)&&done==4);
    move_resolve(root,"old/child",&target);assert(!kernel_tmpfs_create(&heap,0,NULL,&child));
    assert(!kernel_vfs_mount_attach(child,target)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"old/child",&nested);
    struct kernel_vfs_file token={0};assert(!kernel_vfs_create_at(nested,root,"token",0644,&token,0)&&!kernel_vfs_close(&token));
    assert(!kernel_vfs_remount(source,4096U|2U|4U|8U,NULL));
    move_resolve(root,"copy",&target);assert(!kernel_vfs_bind_mount(source,target,1)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"next",&target);assert(!kernel_vfs_move_mount(source,target)&&!kernel_vfs_path_release(&target));
    move_name(held,root,"/next/sub");move_name(nested,root,"/next/child");move_content(&file);
    assert(kernel_vfs_path_resolve(root,root,"old/data",1,&path)==-KERNEL_ENOENT);
    move_resolve(root,"next/data",&path);assert(kernel_vfs_path_inode(path)==kernel_vfs_file_inode(&file));
    assert(!kernel_vfs_path_release(&path));move_resolve(root,"next",&path);
    struct kernel_vfs_statfs st;assert(!kernel_vfs_mount_statfs(kernel_vfs_path_mount(path),&st)&&(st.flags&15U)==14U);
    assert(!kernel_vfs_path_release(&path));
    move_resolve(root,"next/sub",&target);assert(kernel_vfs_move_mount(source,target)==-KERNEL_ELOOP);
    assert(kernel_vfs_move_mount(held,target)==-KERNEL_EINVAL&&!kernel_vfs_path_release(&target));
    move_resolve(root,"next/child",&target);assert(kernel_vfs_move_mount(source,target)==-KERNEL_ELOOP&&!kernel_vfs_path_release(&target));
    move_resolve(root,"single",&target);assert(kernel_vfs_move_mount(source,target)==-KERNEL_EINVAL);
    assert(kernel_vfs_move_mount(root,target)==-KERNEL_EINVAL&&!kernel_vfs_path_release(&target));
    move_resolve(root,"old",&target);assert(kernel_vfs_move_mount(root,target)==-KERNEL_ELOOP&&!kernel_vfs_path_release(&target));
    /* The held target pins its dentry but not its parent's mount-location
     * object. Failing that preparation must leave the old edge visible. */
    move_resolve(root,"failed/point",&target);struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(&heap,&before);allocation_calls=0;allocation_fail_at=1;
    assert(kernel_vfs_move_mount(source,target)==-KERNEL_ENOMEM&&allocation_calls==1);allocation_fail_at=0;
    kernel_heap_get_statistics(&heap,&after);assert(before.live_allocations==after.live_allocations);
    move_name(held,root,"/next/sub");move_content(&file);assert(!kernel_vfs_path_release(&target));
    move_resolve(root,"layer",&target);assert(!kernel_tmpfs_create(&heap,0,NULL,&lower));
    assert(!kernel_vfs_mount_attach(lower,target));
    /* Target was resolved before this overmount; move must choose its top. */
    assert(!kernel_vfs_move_mount(source,target)&&!kernel_vfs_path_release(&target));move_name(held,root,"/layer/sub");
    move_resolve(root,"next",&target);assert(!kernel_vfs_move_mount(source,target)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"layer",&target);assert(!kernel_vfs_umount(target,0)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"parent",&target);assert(!kernel_tmpfs_create(&heap,0,NULL,&parent));
    assert(!kernel_vfs_mount_attach(parent,target)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"parent",&target);assert(!kernel_vfs_mkdir_at(target,root,"point",0755,0)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"parent/point",&target);assert(!kernel_vfs_move_mount(source,target)&&!kernel_vfs_path_release(&target));
    move_name(held,root,"/parent/point/sub");
    assert(!kernel_vfs_umount(source,2));move_content(&file);
    char cwd_text[64];
    assert(!kernel_vfs_path_cwd_string(source,source,cwd_text,sizeof(cwd_text))&&!strcmp(cwd_text,"/"));
    assert(!kernel_vfs_path_cwd_string(held,source,cwd_text,sizeof(cwd_text))&&!strcmp(cwd_text,"/sub"));
    assert(kernel_vfs_path_cwd_string(held,root,cwd_text,sizeof(cwd_text))==-KERNEL_ENOENT);
    move_resolve(root,"next",&target);assert(kernel_vfs_move_mount(source,target)==-KERNEL_EINVAL&&!kernel_vfs_path_release(&target));
    move_resolve(root,"parent",&target);assert(kernel_vfs_move_mount(target,held)==-KERNEL_ENOENT);
    assert(!kernel_vfs_umount(target,0)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"copy/child/token",&path);assert(!kernel_vfs_path_release(&path));
    move_resolve(root,"copy",&path);assert(!kernel_vfs_umount(path,2)&&!kernel_vfs_path_release(&path));
    assert(!kernel_vfs_close(&file)&&!kernel_vfs_path_release(&nested)&&!kernel_vfs_path_release(&held));
    assert(!kernel_vfs_path_release(&source)&&!kernel_vfs_path_release(&root));
}
static void mount_cwd_unreachable(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*jail=NULL,*cwd=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"jail",0755,0)&&!kernel_vfs_mkdir_at(root,root,"outside",0755,0));
    move_resolve(root,"jail",&jail);move_resolve(root,"outside",&cwd);char text[64];
    assert(!kernel_vfs_path_cwd_string(cwd,jail,text,sizeof(text))&&!strcmp(text,"(unreachable)/outside"));
    assert(kernel_vfs_path_cwd_string(cwd,jail,text,15)==-KERNEL_ERANGE);
    assert(kernel_vfs_path_string(cwd,jail,text,sizeof(text))==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_link_string(cwd,jail,text,sizeof(text))&&!strcmp(text,"/outside"));
    assert(!kernel_vfs_path_link_string(jail,jail,text,sizeof(text))&&!strcmp(text,"/"));
    assert(!kernel_vfs_path_cwd_string(root,jail,text,sizeof(text))&&!strcmp(text,"(unreachable)/"));
    assert(!kernel_vfs_unlink_at(root,root,"outside",1));
    assert(kernel_vfs_path_cwd_string(cwd,jail,text,sizeof(text))==-KERNEL_ENOENT);
    assert(kernel_vfs_path_cwd_string(cwd,cwd,text,sizeof(text))==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_link_string(cwd,jail,text,sizeof(text))&&!strcmp(text,"/outside (deleted)"));
    assert(!kernel_vfs_path_link_string(cwd,cwd,text,sizeof(text))&&!strcmp(text,"/ (deleted)"));
    struct kernel_vfs_mount *detached=NULL;
    struct kernel_vfs_path *point=NULL,*view=NULL,*inside=NULL;
    assert(!kernel_vfs_mkdir_at(root,root,"detached-display",0755,0));move_resolve(root,"detached-display",&point);
    assert(!kernel_tmpfs_create(&heap,0,NULL,&detached)&&!kernel_vfs_mount_attach(detached,point));
    assert(!kernel_vfs_path_release(&point));move_resolve(root,"detached-display",&view);
    assert(!kernel_vfs_mkdir_at(view,root,"inside",0755,0));move_resolve(root,"detached-display/inside",&inside);
    assert(!kernel_vfs_umount(view,2));
    assert(!kernel_vfs_path_link_string(inside,jail,text,sizeof(text))&&!strcmp(text,"/inside"));
    assert(!kernel_vfs_path_link_string(view,jail,text,sizeof(text))&&!strcmp(text,"/"));
    assert(kernel_vfs_path_string(inside,jail,text,sizeof(text))==-KERNEL_ENOENT);
    assert(kernel_vfs_path_cwd_string(inside,jail,text,sizeof(text))==-KERNEL_ENOENT);
    assert(!kernel_vfs_unlink_at(view,view,"inside",1));
    assert(!kernel_vfs_path_link_string(inside,jail,text,sizeof(text))&&!strcmp(text,"/inside (deleted)"));
    assert(!kernel_vfs_path_release(&inside)&&!kernel_vfs_path_release(&view));
    assert(!kernel_vfs_path_release(&cwd)&&!kernel_vfs_path_release(&jail)&&!kernel_vfs_path_release(&root));
}
static void mount_executable_path(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*path=NULL,*target=NULL,*view=NULL,*alias=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"execute-view",0755,0));
    struct kernel_vfs_file created={0},executable={0},writer={0};
    assert(!kernel_vfs_create_at(root,root,"execute-file",0755,&created,0)&&!kernel_vfs_close(&created));
    move_resolve(root,"execute-file",&path);move_resolve(root,"execute-view",&target);
    assert(!kernel_vfs_bind_mount(root,target,0)&&!kernel_vfs_path_release(&target));
    move_resolve(root,"execute-view",&view);move_resolve(root,"execute-view/execute-file",&alias);
    assert(kernel_vfs_path_open_executable(path,&executable)==-KERNEL_EACCES);
    assert(!kernel_vfs_remount(root,0,NULL));
    assert(kernel_vfs_path_open_executable(alias,&executable)==-KERNEL_EACCES);
    assert(!kernel_vfs_path_open_executable(path,&executable));
    assert(!kernel_vfs_path_open(path,&writer)&&kernel_vfs_file_acquire_write(&writer)==-KERNEL_ETXTBSY);
    assert(!kernel_vfs_close(&executable)&&!kernel_vfs_file_acquire_write(&writer));
    assert(kernel_vfs_path_open_executable(path,&executable)==-KERNEL_ETXTBSY&&!executable.private_data);
    assert(!kernel_vfs_close(&writer));
    assert(!kernel_vfs_path_set_mode(path,0644));
    assert(kernel_vfs_path_open_executable(path,&executable)==-KERNEL_EACCES);
    assert(!kernel_vfs_path_set_mode(path,0755));
    assert(!kernel_vfs_remount(view,4096U,NULL));
    assert(!kernel_vfs_unlink_at(root,root,"execute-file",0));
    assert(!kernel_vfs_umount(view,2));
    assert(!kernel_vfs_path_open_executable(alias,&executable));
    assert(!kernel_vfs_path_release(&path)&&!kernel_vfs_path_release(&alias)&&!kernel_vfs_path_release(&view));
    assert(!kernel_vfs_close(&executable)&&!kernel_vfs_path_release(&root));
}
