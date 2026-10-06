#include "init-config.h"
#include <platform/loongarch_virt.h>
#include <platform/loongarch_pci.h>
#include <arch/task.h>
#include <arch/timer.h>
#include <arch/elf.h>
#include <kernel/elf_image.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/vfs.h>
#include <kernel/procfs.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/open_file.h>
#include <kernel/shm.h>
#include <kernel/errno.h>
#include <kernel/virtio_pci_block.h>

#define ROOT_DEVICES 8
static struct {
    struct kernel_heap heap;
    struct kernel_page_cache cache;
    struct kernel_vfs_mount mount;
    struct virtio_pci_block devices[ROOT_DEVICES];
    unsigned count,detected;
    struct kernel_exec_image image;
    struct kernel_files files;
    struct kernel_fs_context fs;
    struct kernel_elf64_source *source;
    struct kernel_open_file_description *file;
    struct kernel_elf64_source *interpreter_source;
    struct kernel_open_file_description *interpreter_file;
    uint64_t baseline;
    int live;
} root;
static struct arch_mmu_page_table table;
static int is_block(uint32_t id)
{ return id==0x10421af4 || id==0x10011af4; }
static int dma(const void *p,uint64_t size,uint64_t *address)
{ return arch_direct_map_va_to_pa((uintptr_t)p,size,address)==ARCH_DIRECT_MAP_STATUS_OK; }

/* 文件、映像和 mount 仍是 I/O owner 时保留原字段，下一次只重试未完成项。 */
static int release_root(void)
{
    if((root.files.state==KERNEL_FILES_LIVE || root.files.state==KERNEL_FILES_CLEANUP) &&
       kernel_files_release(&root.files)!=KERNEL_FILES_STATUS_OK) return -KERNEL_EIO;
    if((root.fs.state==KERNEL_FS_CONTEXT_LIVE || root.fs.state==KERNEL_FS_CONTEXT_CLEANUP) &&
       kernel_fs_context_release(&root.fs)!=KERNEL_FS_CONTEXT_STATUS_OK) return -KERNEL_EIO;
    if((root.image.mm.state==KERNEL_MM_LIVE || root.image.mm.state==KERNEL_MM_CLEANUP) &&
       kernel_exec_image_cleanup(&root.heap,&root.image)!=KERNEL_EXEC_IMAGE_STATUS_OK) return -KERNEL_EIO;
    if(root.interpreter_source && kernel_elf64_source_release(&root.interpreter_source)!=KERNEL_ELF64_SOURCE_STATUS_OK) return -KERNEL_EIO;
    if(root.interpreter_file && kernel_open_file_release(&root.interpreter_file)!=KERNEL_OPEN_FILE_STATUS_OK) return -KERNEL_EIO;
    if(root.source && kernel_elf64_source_release(&root.source)!=KERNEL_ELF64_SOURCE_STATUS_OK) return -KERNEL_EIO;
    if(root.file && kernel_open_file_release(&root.file)!=KERNEL_OPEN_FILE_STATUS_OK) return -KERNEL_EIO;
    kernel_page_cache_stop_worker(&root.cache);
    int error=kernel_vfs_disk_cleanup_pending();
    if(error) return error;
    if(root.mount.private_data) {
        error=kernel_procfs_unmount_children(&root.mount);
        if(!error) error=kernel_vfs_unmount(&root.mount);
        if(error) return error;
    }
    if((root.cache.state==KERNEL_PAGE_CACHE_LIVE || root.cache.state==KERNEL_PAGE_CACHE_CLEANUP) &&
       kernel_page_cache_destroy(&root.cache)!=KERNEL_PAGE_CACHE_STATUS_OK) return -KERNEL_EIO;
    while(root.count) {
        struct virtio_pci_block *device=&root.devices[root.count-1];
        if(device->device.block.registered && kernel_block_unregister(&device->device.block)) return -KERNEL_EBUSY;
        if(virtio_pci_block_destroy(device)!=VIRTIO_BLOCK_DRIVER_STATUS_OK) return -KERNEL_EIO;
        root.count--;
    }
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&root.heap,&stats);
    uint64_t available=physical_page_available(root.heap.page_allocator);
    if(stats.live_allocations || stats.current_pages || available!=root.baseline || pci_host_claimed(la_virt_pci_host())) {
        la_virt_puts("LA root baseline available=");la_virt_hex(available);
        la_virt_puts(" expected=");la_virt_hex(root.baseline);la_virt_puts(" heap-live=");la_virt_hex(stats.live_allocations);
        la_virt_puts(" heap-pages=");la_virt_hex(stats.current_pages);la_virt_puts("\n");
        la_virt_fatal("root ownership baseline");
    }
    la_virt_puts("LA root owners released; pages=");la_virt_hex(available);la_virt_puts(" heap-live=0\n");
    return 0;
}
static int start_root(void);
static void cleanup_worker(void *unused)
{
    (void)unused; (void)arch_interrupt_save(); kernel_scheduler_register_cleanup();
    int boot_error=start_root();
    if(boot_error) {
        la_virt_puts("LA root boot errno=");la_virt_hex((uint64_t)(int64_t)boot_error);la_virt_puts("\n");
        for(unsigned attempt=0;attempt<3;attempt++) {
            int cleanup=release_root();if(!cleanup) la_virt_shutdown();
            la_virt_puts("LA root cleanup errno=");la_virt_hex((uint64_t)(int64_t)cleanup);la_virt_puts("\n");
            if(attempt<2) kernel_scheduler_wait_cleanup(arch_time_read()+la_timer_frequency()/100);
        }
        la_virt_shutdown();
    }
    struct kernel_thread_completion init={0}; int reaped=0;unsigned cleanup_attempts=0;
    for(;;) {
        uintptr_t irq=arch_interrupt_save();
        struct kernel_thread_completion completion;enum kernel_scheduler_status status;
        do {
            status=kernel_scheduler_reap_one(&completion);
            if(status==KERNEL_SCHEDULER_STATUS_OK && root.live && !reaped &&
               completion.kind==KERNEL_THREAD_KIND_USER && completion.tid==1 && completion.tgid==1) { init=completion; reaped=1; }
        } while(status==KERNEL_SCHEDULER_STATUS_OK ||
                (status==KERNEL_SCHEDULER_STATUS_EMPTY && kernel_scheduler_reap_pending()));
        int pending=reaped && kernel_scheduler_stop_users();
        arch_interrupt_restore(irq);
        if(reaped && !pending) {
            int error=release_root();
            if(!error) {
                la_virt_puts("LA PID 1 exited reason=");la_virt_hex(init.reason);
                la_virt_puts(" status=");la_virt_hex(init.status);la_virt_puts("\n");la_virt_shutdown();
            }
            la_virt_puts("LA root cleanup errno=");la_virt_hex((uint64_t)(int64_t)error);la_virt_puts("\n");
            if(++cleanup_attempts==3) {
                /* 永久 I/O 错误不靠无界重试恢复；保留仍有效的 mount/DMA owner 并明确停止。 */
                la_virt_puts("LA root cleanup retained mount=");la_virt_hex(root.mount.private_data!=0);
                la_virt_puts(" cache=");la_virt_hex(root.cache.record!=0);
                la_virt_puts(" devices=");la_virt_hex(root.count);
                la_virt_puts(" claims=");la_virt_hex(pci_host_claimed(la_virt_pci_host()));la_virt_puts("\n");
                la_virt_shutdown();
            }
            irq=arch_interrupt_save();kernel_scheduler_wait_cleanup(arch_time_read()+la_timer_frequency()/100);arch_interrupt_restore(irq);
            continue;
        }
        if(status!=KERNEL_SCHEDULER_STATUS_EMPTY && status!=KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP) la_virt_fatal("root reaper");
        irq=arch_interrupt_save();kernel_scheduler_wait_cleanup(status==KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP ?
            arch_time_read()+la_timer_frequency()/100 : 0);arch_interrupt_restore(irq);
    }
}
static int prepare_init(void)
{
    if(kernel_fs_context_create(&root.fs,&root.mount,&root.heap)!=KERNEL_FS_CONTEXT_STATUS_OK) return -KERNEL_ENOMEM;
    int error=0; enum kernel_open_file_status opened=kernel_open_file_create_executable(&root.heap,&root.mount,init_path,&root.file,&error);
    if(opened!=KERNEL_OPEN_FILE_STATUS_OK) return opened==KERNEL_OPEN_FILE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    if(error) return error;
    enum kernel_elf64_source_status source=kernel_elf64_source_create(&root.heap,&root.file,BOAROS_PAGE_SIZE,ARCH_ELF_MACHINE,&root.source);
    if(source!=KERNEL_ELF64_SOURCE_STATUS_OK) return source==KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
        source==KERNEL_ELF64_SOURCE_STATUS_IO ? -KERNEL_EIO : -KERNEL_ENOEXEC;
    const char *interpreter=kernel_elf64_source_interpreter(root.source,0);
    if(interpreter) {
        opened=kernel_open_file_create_at(&root.heap,kernel_fs_context_cwd(&root.fs),
            kernel_fs_context_root(&root.fs),interpreter,KERNEL_OPEN_PATH_EXECUTABLE,0,
            &root.interpreter_file,&error);
        if(opened!=KERNEL_OPEN_FILE_STATUS_OK)return opened==KERNEL_OPEN_FILE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        if(error)return error;
        source=kernel_elf64_source_create_interpreter(&root.heap,&root.interpreter_file,BOAROS_PAGE_SIZE,
            ARCH_ELF_MACHINE,&root.interpreter_source);
        if(source!=KERNEL_ELF64_SOURCE_STATUS_OK)return source==KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
            source==KERNEL_ELF64_SOURCE_STATUS_IO ? -KERNEL_EIO : -KERNEL_ELIBBAD;
        if(kernel_elf64_source_interpreter(root.interpreter_source,0))return -KERNEL_ELIBBAD;
    }
    struct kernel_exec_image_request request={.executable_source=root.source,.interpreter_source=root.interpreter_source,
        .executable={init_path,sizeof(init_path)-1},.arguments=init_arguments,.argument_count=INIT_ARGUMENTS_COUNT,
        .environment=init_environment,.environment_count=INIT_ENVIRONMENT_COUNT};
    int64_t result=0;
    enum kernel_exec_image_status image=kernel_exec_image_prepare(&request,&root.heap,&root.image,&result);
    if(image!=KERNEL_EXEC_IMAGE_STATUS_OK) return result ? (int)result : -KERNEL_EIO;
    if(kernel_files_create(&root.files,&root.heap)!=KERNEL_FILES_STATUS_OK) return -KERNEL_ENOMEM;
    for(unsigned i=0;i<3;i++) {
        if(kernel_files_open_boot_console(&root.files,kernel_fs_context_root(&root.fs),i,&result)!=KERNEL_FILES_STATUS_OK) return -KERNEL_ENOMEM;
        if(result) return (int)result;
    }
    return 0;
}
static int start_root(void)
{
    struct physical_page_allocator *allocator=root.heap.page_allocator;
    struct pci_host *host=la_virt_pci_host();
    int error=root.detected>ROOT_DEVICES ? -KERNEL_ENOSPC : 0;
    for(unsigned bdf=0;!error && bdf<256;bdf++) {
        uint32_t id=host->read(host->context,bdf,0,4);
        if(!is_block(id)) continue;
        if(id!=0x10421af4) {error=-KERNEL_ENOTSUP;break;}
        struct virtio_pci_block *device=&root.devices[root.count];
        enum virtio_block_status status=virtio_pci_block_init(device,host,bdf,allocator,dma,la_timer_frequency());
        if(status!=VIRTIO_BLOCK_DRIVER_STATUS_OK) { error=status==VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO; break; }
        unsigned number=root.count++;
        if(kernel_block_register(&device->device.block,KERNEL_BLOCK_DEVICE_NUMBER(number))) error=-KERNEL_EBUSY;
    }
    if(!error && kernel_page_cache_init(&root.cache,&root.heap,allocator)!=KERNEL_PAGE_CACHE_STATUS_OK) error=-KERNEL_ENOMEM;
    if(!error) error=kernel_vfs_mount_root(&root.mount,&root.devices[0].device.block,&root.heap,&root.cache);
    if(!error) error=prepare_init();
    if(!error) error=kernel_page_cache_start_worker(&root.cache);
    if(!error) error=kernel_vfs_start_journal_worker(&root.mount);
    if(error) return error;
    for(unsigned i=0;i<root.count;i++)
        if(!virtio_block_enable_irq(&root.devices[i].device,root.devices[i].irq)) return -KERNEL_EIO;
    /* 后台 I/O owner 就绪后才原子发布 PID 1，启动 OOM 从未留下半成品用户任务。 */
    enum kernel_scheduler_status created=kernel_user_thread_create(&root.image.mm,&root.files,&root.fs,
        root.image.entry,root.image.stack_pointer,root.image.thread_pointer);
    if(created!=KERNEL_SCHEDULER_STATUS_OK)
        return created==KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    root.live=1;return 0;
}
int la_root_boot(struct physical_page_allocator *allocator)
{
    struct pci_host *host=la_virt_pci_host();
    for(unsigned bdf=0;bdf<256;bdf++) if(is_block(host->read(host->context,bdf,0,4))) root.detected++;
    if(!root.detected) return 0;
    table.allocator=allocator;table.state=ARCH_MMU_STATE_ACTIVE;
    if(kernel_heap_init(&root.heap,allocator,la_virt_physical_address)!=KERNEL_HEAP_STATUS_OK ||
       kernel_exec_image_bind(allocator,&table) || kernel_shm_init(&root.heap,allocator)!=KERNEL_SHM_STATUS_OK ||
       !la_virt_irq_initialize()) la_virt_fatal("root environment");
    root.baseline=physical_page_available(allocator);
    if(kernel_thread_create(cleanup_worker,0)!=KERNEL_SCHEDULER_STATUS_OK) {
        la_virt_puts("LA root boot errno=0xfffffffffffffff4\n");
        if(release_root()) la_virt_fatal("root initial ownership");
        la_virt_shutdown();
    }
    /* 该永久可信栈在基线前分配，所有可停止的 I/O worker 与用户栈均计入回收。 */
    root.baseline=physical_page_available(allocator);
    if(la_timer_start(la_timer_frequency(),100)!=ARCH_TIMER_STATUS_OK) la_virt_fatal("root timer");
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    for(;;) arch_cpu_wait();
}
void la_user_contract(struct physical_page_allocator *);
void la_boot_tasks(struct physical_page_allocator *allocator)
{ if(!la_root_boot(allocator)) la_user_contract(allocator); }
