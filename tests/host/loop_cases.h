#include <kernel/loop.h>
#include <stdlib.h>
#include <kernel/time.h>
#include "../../fs/char_device_internal.h"
enum kernel_time_status __wrap_kernel_time_deadline_from_monotonic(uint64_t target,uint64_t *deadline)
{
    *deadline=target;
    return target<=kernel_time_monotonic_ns()?KERNEL_TIME_STATUS_DEADLINE_PASSED:KERNEL_TIME_STATUS_OK;
}
static int loop_test_open(struct kernel_files *local,struct kernel_fs_context *local_fs,const char *path,unsigned flags)
{
    size_t copied; int64_t result;
    assert(!kernel_copy_to_user(&mm,0x200000,path,strlen(path)+1,&copied));
    assert(!kernel_files_openat(local,local_fs,&mm,-100,0x200000,flags,0600,&result));
    return (int)result;
}
static int loop_test_ioctl(struct kernel_files *local,int fd,uint64_t command,uint64_t argument)
{
    int64_t result; assert(!kernel_files_ioctl(local,&mm,fd,command,argument,&result));
    return (int)result;
}
static void loop_test_close(struct kernel_files *local,int fd)
{ int64_t result;assert(!kernel_files_close(local,fd,&result) && !result); }
static int loop_backing_image(struct kernel_files *local,struct kernel_fs_context *local_fs,
    const char *path,const char *image_path)
{
    int source=open(image_path,O_RDONLY);assert(source>=0);
    struct stat st;assert(!fstat(source,&st));
    unsigned char *bytes=malloc(st.st_size);assert(bytes);
    assert(read(source,bytes,st.st_size)==st.st_size && !close(source));
    int fd=loop_test_open(local,local_fs,path,0102|2);
    if (fd<0) {
        struct kernel_vfs_stat stat={0};
        struct kernel_vfs_mount *back_mount=kernel_vfs_path_mount(kernel_fs_context_root(local_fs));
        int error=kernel_vfs_stat_path(back_mount,path,1,&stat);
        fprintf(stderr,"loop backing open %s: %d stat=%d mode=%o size=%llu\n",path,fd,error,stat.mode,(unsigned long long)stat.size);
    }
    assert(fd>=0);
    struct kernel_open_file_description *file=NULL;int64_t result;size_t done;
    assert(!kernel_files_pin_data(local,fd,&file,&result) && !result);
    assert(!kernel_vfs_pwrite(&file->file,0,bytes,st.st_size,&done) && done==(size_t)st.st_size);
    assert(!kernel_open_file_release(&file));free(bytes);return fd;
}
static void loop_mount_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_files *local,struct kernel_fs_context *local_fs)
{
    const char *outer_path=getenv("BOAROS_LOOP_OUTER_IMAGE"),*inner_path=getenv("BOAROS_LOOP_INNER_IMAGE");
    assert(outer_path && inner_path);
    struct kernel_vfs_path *root=kernel_fs_context_root(local_fs);
    assert(!kernel_vfs_mknod_at(root,root,"/loop1",KERNEL_VFS_S_IFBLK|0600,KERNEL_LOOP_DEVICE_NUMBER(1)));
    int backing=loop_backing_image(local,local_fs,"/outer-backing",outer_path);
    int first=loop_test_open(local,local_fs,"/loop0",2);assert(first>=0);
    struct kernel_block_device *probe=kernel_block_get(KERNEL_LOOP_DEVICE_NUMBER(0));assert(probe);
    int claim_owner;
    assert(!kernel_block_claim(probe,&claim_owner));
    assert(loop_test_ioctl(local,first,0x4c00,backing)==-KERNEL_EBUSY);
    kernel_block_release_claim(probe,&claim_owner);
    unsigned char super[28];int source=open(outer_path,O_RDONLY);assert(source>=0);
    assert(pread(source,super,sizeof(super),1024)==sizeof(super) && !close(source));
    unsigned fs_block=1024U<<super[24];
    if (fs_block<BOAROS_PAGE_SIZE) {
        struct kernel_loop_config bad={.fd=(uint32_t)backing,.block_size=fs_block*2};size_t copied;
        assert(!kernel_copy_to_user(&mm,0x200400,&bad,sizeof(bad),&copied));
        assert(!loop_test_ioctl(local,first,0x4c0a,0x200400));
        struct kernel_page_cache cache={0};struct kernel_vfs_mount rejected={0};
        assert(!kernel_page_cache_init(&cache,heap,heap->page_allocator));
        assert(kernel_vfs_mount_ext4(&rejected,probe,heap,&cache,0)==-KERNEL_EINVAL);
        assert(!rejected.private_data && !kernel_page_cache_destroy(&cache));
        assert(!loop_test_ioctl(local,first,0x4c01,0));loop_test_close(local,first);
        kernel_block_put(probe);probe=NULL;
        assert(kernel_loop_release_unused()>=0);
        first=loop_test_open(local,local_fs,"/loop0",2);assert(first>=0);
    }
    if (probe) kernel_block_put(probe);
    const char *sector_setting=getenv("BOAROS_LOOP_SECTOR");
    unsigned sector=sector_setting?(unsigned)strtoul(sector_setting,NULL,10):512;
    struct kernel_loop_config config={.fd=(uint32_t)backing,.block_size=sector};
    size_t copied;
    assert(!kernel_copy_to_user(&mm,0x200400,&config,sizeof(config),&copied));
    assert(!loop_test_ioctl(local,first,0x4c0a,0x200400));loop_test_close(local,backing);
    assert(!loop_test_ioctl(local,first,0x1268,0x200400));
    unsigned reported_sector=0;
    assert(!kernel_copy_from_user(&mm,&reported_sector,0x200400,sizeof(reported_sector),&copied));
    assert(reported_sector==sector);
    struct kernel_page_cache caches[2]={{0}};
    struct kernel_vfs_mount children[2]={{0}};
    struct kernel_block_device *device=kernel_block_get(KERNEL_LOOP_DEVICE_NUMBER(0));assert(device);
    assert(!kernel_page_cache_init(&caches[0],heap,heap->page_allocator));
    assert(!kernel_vfs_mount_ext4(&children[0],device,heap,&caches[0],0));kernel_block_put(device);
    assert(!kernel_page_cache_start_worker(&caches[0]));
    assert(!kernel_vfs_start_journal_worker(&children[0]));
    struct kernel_fs_context inside={0};assert(!kernel_fs_context_create(&inside,&children[0],heap));
    backing=loop_backing_image(local,&inside,"/inner-backing",inner_path);
    int second=loop_test_open(local,local_fs,"/loop1",2);assert(second>=0);
    assert(!loop_test_ioctl(local,second,0x4c00,backing));loop_test_close(local,backing);
    device=kernel_block_get(KERNEL_LOOP_DEVICE_NUMBER(1));assert(device);
    assert(!kernel_page_cache_init(&caches[1],heap,heap->page_allocator));
    assert(!kernel_vfs_mount_ext4(&children[1],device,heap,&caches[1],0));kernel_block_put(device);
    assert(!kernel_page_cache_start_worker(&caches[1]));
    assert(!kernel_vfs_start_journal_worker(&children[1]));
    struct kernel_vfs_file file={0};size_t done;uint64_t observed=0;
    assert(!kernel_vfs_create(&children[1],"/payload",0600,&file));
    assert(!kernel_vfs_pwrite(&file,0,"nested-loop-durable",19,&done) && done==19);
    assert(!kernel_vfs_sync(&file,0,&observed));
    assert(!kernel_page_cache_invalidate_node(&caches[1],kernel_vfs_file_node(&file)));
    struct kernel_page_cache_concurrency_state before_read,after_read;
    kernel_page_cache_get_concurrency_state(&caches[1],&before_read);
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    char actual[19];assert(!kernel_vfs_pread(&file,0,actual,sizeof(actual),&done) &&
        done==sizeof(actual) && !memcmp(actual,"nested-loop-durable",sizeof(actual)));
    assert(kernel_cpu_current()->logical_id==1);
    kernel_page_cache_get_concurrency_state(&caches[1],&after_read);
    assert(before_read.requests_queued==after_read.requests_queued);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS loop upper cold read on caller CPU1: worker backing I/O and mount/loop owners preserved");
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_mkdir(mount,"/outer",0755) && !kernel_vfs_mkdir(mount,"/inner",0755));
    struct kernel_vfs_path *covered=NULL;
    /* Put the busy backing mount first, so shutdown must try another leaf. */
    assert(!kernel_vfs_path_resolve(root,root,"/inner",1,&covered));
    assert(!kernel_vfs_mount_attach(&children[1],covered) && !kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"/outer",1,&covered));
    assert(!kernel_vfs_mount_attach(&children[0],covered) && !kernel_vfs_path_release(&covered));
    assert(!kernel_fs_context_release(&inside));
    assert(!loop_test_ioctl(local,first,0x4c01,0));
    loop_test_close(local,first);loop_test_close(local,second);
    assert(!kernel_loop_release_unused()); /* Both mount claims retain binding. */
    assert(!kernel_loop_shutdown_mounts(mount) && !mount->first_child);
    assert(!kernel_page_cache_destroy(&caches[0]) && !kernel_page_cache_destroy(&caches[1]));
    /* Save the actual outer backing bytes after both stacked mounts flushed. */
    struct kernel_vfs_file saved={0};assert(!kernel_vfs_open(mount,"/outer-backing",&saved));
    const char *output=getenv("BOAROS_LOOP_OUTER_RESULT");assert(output);
    int destination=open(output,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(destination>=0);
    unsigned char *buffer=malloc(65536);assert(buffer);
    uint64_t size=kernel_vfs_file_size(&saved);
    for (uint64_t offset=0;offset<size;offset+=done) {
        size_t chunk=size-offset<65536?(size_t)(size-offset):65536;
        assert(!kernel_vfs_pread(&saved,offset,buffer,chunk,&done) && done==chunk);
        assert(write(destination,buffer,done)==(ssize_t)done);
    }
    free(buffer);assert(!fsync(destination) && !close(destination) && !kernel_vfs_close(&saved));
    assert(!kernel_vfs_unlink(mount,"/outer-backing"));
    puts("PASS loop stack: two real ext4 mounts, cache reload and persistent payload; mounted claims retain loops; shutdown handles busy backing leaf before root");
}

static void loop_readonly_case(struct kernel_files *local,struct kernel_fs_context *local_fs)
{
    int backing=loop_test_open(local,local_fs,"/readonly-backing",0102|2);assert(backing>=0);
    int64_t result;assert(!kernel_files_ftruncate(local,backing,2*1024*1024,&result) && !result);
    int loop=loop_test_open(local,local_fs,"/loop0",0);assert(loop>=0);
    struct kernel_loop_config config={.fd=(uint32_t)backing,.info={.offset=128,.sizelimit=4096}};
    size_t copied;
    config.reserved[1]=1;
    assert(!kernel_copy_to_user(&mm,0x200400,&config,sizeof(config),&copied));
    assert(loop_test_ioctl(local,loop,0x4c0a,0x200400)==-KERNEL_EINVAL);
    config.reserved[1]=0;
    assert(!kernel_copy_to_user(&mm,0x200400,&config,sizeof(config),&copied));
    assert(!loop_test_ioctl(local,loop,0x4c0a,0x200400));
    assert(!loop_test_ioctl(local,loop,0x4c05,0x200400));
    struct kernel_loop_info64 info;
    assert(!kernel_copy_from_user(&mm,&info,0x200400,sizeof(info),&copied));
    assert(info.offset==128 && info.sizelimit==4096 && (info.flags&KERNEL_LOOP_READ_ONLY));
    const unsigned writer_modes[]={2,1};
    for (unsigned i=0;i<sizeof(writer_modes)/sizeof(writer_modes[0]);i++) {
        int writer=loop_test_open(local,local_fs,"/loop0",writer_modes[i]);assert(writer>=0);
        assert(!kernel_files_write(local,&mm,writer,0x200800,1,&result) && result==-KERNEL_EPERM);
        assert(!kernel_files_pwrite(local,&mm,writer,0x200800,1,0,&result) && result==-KERNEL_EPERM);
        loop_test_close(local,writer);
    }
    assert(!kernel_files_pread(local,&mm,loop,0x200800,512,0,&result) && result==512);
    assert(!loop_test_ioctl(local,loop,0x4c01,0));
    assert(loop_test_ioctl(local,loop,0x4c05,0x200400)==-KERNEL_ENXIO);
    assert(loop_test_open(local,local_fs,"/loop0",0)==-KERNEL_ENXIO);
    assert(loop_test_ioctl(local,loop,0x4c00,backing)==-KERNEL_EBUSY);
    loop_test_close(local,loop);loop_test_close(local,backing);
    assert(kernel_loop_release_unused()>=0);
    puts("PASS loop CONFIGURE: reserved validation, read-only loop fd inference, unaligned backing offset, and sole-opener rundown rejecting new use");
}

static void loop_partial_sector_case(struct kernel_vfs_mount *mount,
    struct kernel_files *local,struct kernel_fs_context *local_fs)
{
    for (unsigned limited=0;limited<2;limited++) {
        const char *path=limited?"/limited-sector-backing":"/short-sector-backing";
        size_t backing_size=limited?8192:4608;
        uint64_t backing_offset=limited?128:0;
        unsigned char *expected=malloc(backing_size),*actual=malloc(backing_size);
        assert(expected && actual);
        for (size_t i=0;i<backing_size;i++) expected[i]=(unsigned char)(i*29+limited*7+3);
        int backing=loop_test_open(local,local_fs,path,0102|2);assert(backing>=0);
        struct kernel_open_file_description *backing_file=NULL,*loop_file=NULL;
        int64_t result;size_t done;
        assert(!kernel_files_pin_data(local,backing,&backing_file,&result) && !result);
        assert(!kernel_vfs_pwrite(&backing_file->file,0,expected,backing_size,&done) && done==backing_size);
        int loop=loop_test_open(local,local_fs,"/loop0",2);assert(loop>=0);
        struct kernel_loop_config config={.fd=(uint32_t)backing,.block_size=4096,
            .info={.offset=backing_offset,.sizelimit=limited?4608:0}};
        assert(!kernel_copy_to_user(&mm,0x200400,&config,sizeof(config),&done));
        assert(!loop_test_ioctl(local,loop,0x4c0a,0x200400));
        assert(!loop_test_ioctl(local,loop,0x80081272,0x200400));
        uint64_t capacity;
        assert(!kernel_copy_from_user(&mm,&capacity,0x200400,sizeof(capacity),&done) && capacity==4608);
        assert(!loop_test_ioctl(local,loop,0x1268,0x200400));
        unsigned sector;
        assert(!kernel_copy_from_user(&mm,&sector,0x200400,sizeof(sector),&done) && sector==4096);
        assert(!kernel_files_pin_data(local,loop,&loop_file,&result) && !result);
        uint32_t references,opens;
        kernel_block_usage(loop_file->block,&references,&opens);
        assert(references==1 && opens==1);
        assert(!kernel_files_pread(local,&mm,loop,0x201000,1,4096,&result));
        if (result!=1) fprintf(stderr,"loop partial sector: limited=%u capacity=%llu sector=%u tail pread=%lld expected=1\n",
            limited,(unsigned long long)capacity,sector,(long long)result);
        assert(result==1);
        assert(!kernel_copy_from_user(&mm,actual,0x201000,1,&done) && actual[0]==expected[backing_offset+4096]);
        assert(!kernel_copy_to_user(&mm,0x200800,"tail-rmw",8,&done));
        assert(!kernel_files_pwrite(local,&mm,loop,0x200800,8,4096,&result) && result==8);
        memcpy(expected+backing_offset+4096,"tail-rmw",8);
        assert(!kernel_block_file_pwrite(loop_file,4599,"last-nine-and-more",18,&done) && done==9);
        memcpy(expected+backing_offset+4599,"last-nine",9);
        memset(actual,0xa5,1024);
        assert(!kernel_open_file_pread(loop_file,4094,actual,1024,&done) && done==514 &&
            !memcmp(actual,expected+backing_offset+4094,514));
        for (size_t i=514;i<1024;i++) assert(actual[i]==0xa5);
        assert(!kernel_files_dup(local,loop,&result) && result>=0);int alias=(int)result;
        assert(!kernel_files_lseek(local,loop,4094,0,&result) && result==4094);
        assert(!kernel_files_read(local,&mm,alias,0x201000,1024,&result) && result==514);
        assert(!kernel_copy_from_user(&mm,actual,0x201000,514,&done) &&
            !memcmp(actual,expected+backing_offset+4094,514));
        assert(!kernel_files_lseek(local,loop,0,1,&result) && result==4608);
        assert(!kernel_files_lseek(local,alias,-1,2,&result) && result==4607);
        assert(!kernel_files_write(local,&mm,loop,0x200800,8,&result) && result==1);
        expected[backing_offset+4607]='t';
        assert(!kernel_files_lseek(local,alias,0,1,&result) && result==4608);
        assert(!kernel_files_write(local,&mm,alias,0x200800,1,&result) && result==-KERNEL_ENOSPC);
        assert(!kernel_files_pread(local,&mm,loop,0x201000,1,4608,&result) && result==0);
        assert(!kernel_files_pwrite(local,&mm,loop,0x200800,1,4608,&result) && result==-KERNEL_ENOSPC);
        assert(!kernel_files_sync(local,loop,0,&result) && !result);
        uint32_t after_references,after_opens;
        kernel_block_usage(loop_file->block,&after_references,&after_opens);
        assert(after_references==references && after_opens==opens);
        assert(!kernel_open_file_release(&loop_file));loop_test_close(local,alias);
        assert(!loop_test_ioctl(local,loop,0x4c01,0));loop_test_close(local,loop);
        assert(kernel_loop_release_unused()>=0 && backing_file->references==2);
        assert(kernel_open_file_size(backing_file)==backing_size);
        assert(!kernel_open_file_pread(backing_file,0,actual,backing_size,&done) && done==backing_size &&
            !memcmp(actual,expected,backing_size));
        assert(!kernel_open_file_release(&backing_file));loop_test_close(local,backing);
        assert(!kernel_vfs_unlink(mount,path));free(actual);free(expected);
    }
    puts("PASS loop partial final sector: 4096-byte logical sector with 4608-byte backing/view, tail prefixes, shared offset and outside-view bytes preserved");
}

static void loop_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_fs_context local_fs={0};struct kernel_files local={0};
    assert(!kernel_fs_context_create(&local_fs,mount,heap) && !kernel_files_create(&local,heap));
    struct kernel_vfs_path *root=kernel_fs_context_root(&local_fs);
    assert(!kernel_vfs_mknod_at(root,root,"/loop0",KERNEL_VFS_S_IFBLK|0600,KERNEL_LOOP_DEVICE_NUMBER(0)));
    int backing=loop_test_open(&local,&local_fs,"/loop-backing",0102|2); assert(backing>=0);
    int64_t result;assert(!kernel_files_ftruncate(&local,backing,2*1024*1024,&result) && !result);
    int loop=loop_test_open(&local,&local_fs,"/loop0",2); assert(loop>=0);
    assert(!loop_test_ioctl(&local,loop,0x4c00,backing));
    /* Binding must retain the real OFD after both namespace and fd owners go. */
    loop_test_close(&local,backing);assert(!kernel_vfs_unlink(mount,"/loop-backing"));
    size_t copied;assert(!kernel_copy_to_user(&mm,0x200200,"loop-data",9,&copied));
    assert(!kernel_files_pwrite(&local,&mm,loop,0x200200,9,509,&result) && result==9);
    assert(!kernel_files_sync(&local,loop,0,&result) && !result);
    assert(!kernel_files_pread(&local,&mm,loop,0x200300,9,509,&result) && result==9);
    char bytes[9];assert(!kernel_copy_from_user(&mm,bytes,0x200300,9,&copied) && !memcmp(bytes,"loop-data",9));
    assert(!loop_test_ioctl(&local,loop,0x4c05,0x200400));
    struct kernel_loop_info64 info;
    assert(!kernel_copy_from_user(&mm,&info,0x200400,sizeof(info),&copied));
    assert(info.number==0 && !info.offset && !info.sizelimit && info.inode);
    unsigned char legacy_bytes[176];memset(legacy_bytes,0xa5,sizeof(legacy_bytes));
    assert(!kernel_copy_to_user(&mm,0x200800,legacy_bytes,sizeof(legacy_bytes),&copied));
    assert(!loop_test_ioctl(&local,loop,0x4c03,0x200800));
    assert(!kernel_copy_from_user(&mm,legacy_bytes,0x200800,sizeof(legacy_bytes),&copied));
    struct kernel_loop_info legacy;memcpy(&legacy,legacy_bytes,sizeof(legacy));
    assert(legacy.number==0 && legacy.inode==info.inode && legacy.device==info.device);
    for (size_t i=sizeof(legacy);i<sizeof(legacy_bytes);i++) assert(legacy_bytes[i]==0xa5);
    legacy.offset=512;legacy.flags=KERNEL_LOOP_AUTOCLEAR;
    assert(!kernel_copy_to_user(&mm,0x200800,&legacy,sizeof(legacy),&copied));
    assert(!loop_test_ioctl(&local,loop,0x4c02,0x200800));
    assert(loop_test_ioctl(&local,loop,0x4c04,ARCH_MMU_USER_LIMIT+1)==-KERNEL_EFAULT);
    info.offset=512;info.sizelimit=4096;info.flags=KERNEL_LOOP_AUTOCLEAR;
    assert(!kernel_copy_to_user(&mm,0x200400,&info,sizeof(info),&copied));
    assert(!loop_test_ioctl(&local,loop,0x4c04,0x200400));
    assert(!loop_test_ioctl(&local,loop,0x80081272,0x200400));
    uint64_t capacity;assert(!kernel_copy_from_user(&mm,&capacity,0x200400,8,&copied) && capacity==4096);
    int second=loop_test_open(&local,&local_fs,"/loop0",2);assert(second>=0);
    assert(!loop_test_ioctl(&local,loop,0x4c01,0));
    assert(!loop_test_ioctl(&local,second,0x4c05,0x200400));
    loop_test_close(&local,loop);loop_test_close(&local,second);
    assert(kernel_loop_release_unused()>=0);
    loop=loop_test_open(&local,&local_fs,"/loop0",2);assert(loop>=0);
    assert(loop_test_ioctl(&local,loop,0x4c05,0x200400)==-KERNEL_ENXIO);
    assert(loop_test_ioctl(&local,loop,0x4c00,loop)==-KERNEL_ELOOP);
    assert(!kernel_vfs_mknod_at(root,root,"/loop7",KERNEL_VFS_S_IFBLK|0600,KERNEL_LOOP_DEVICE_NUMBER(7)));
    int unbound=loop_test_open(&local,&local_fs,"/loop7",2);assert(unbound>=0);
    assert(loop_test_ioctl(&local,loop,0x4c00,unbound)==-KERNEL_EINVAL);
    loop_test_close(&local,unbound);loop_test_close(&local,loop);
    loop_readonly_case(&local,&local_fs);
    loop_partial_sector_case(mount,&local,&local_fs);
    assert(kernel_loop_control_device.ioctl(NULL,NULL,&local,NULL,&mm,0x4c80,12)==12);
    assert(kernel_loop_control_device.ioctl(NULL,NULL,&local,NULL,&mm,0x4c81,12)==0);
    assert(kernel_loop_control_device.ioctl(NULL,NULL,&local,NULL,&mm,0x4c82,0)==0);
    assert(!kernel_loop_shutdown());
    loop_mount_case(heap,mount,&local,&local_fs);
    assert(!kernel_files_release(&local) && !kernel_fs_context_release(&local_fs));
    assert(!kernel_loop_shutdown());
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS loop real OFD: bind, close/unlink backing, unaligned I/O, flush, status, offset/limit, lazy detach, autoclear, direct cycle rejection and control lifetime");
}
