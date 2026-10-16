#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/cost.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include <kernel/file_mapping.h>
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_errno.h>
#include <ext4_journal.h>
#include <string.h>

static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct riscv_virtio_mmio_block device;
static unsigned completed;
static int journaled;
static unsigned char bytes[4096], actual[4096];
static struct kernel_vfs_file *hot_file;
static unsigned hot_read_done;
static struct kernel_wait_queue hot_read_completion;
static struct kernel_wait_queue checkpoint_held_wait, checkpoint_release_wait;
static unsigned checkpoint_hold_armed, checkpoint_held, checkpoint_release;
static int (*checkpoint_write_phase)(struct ext4_blockdev *, const struct ext4_block_write *);
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("journal group failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *pointer, uint64_t *address) { *address=(uintptr_t)pointer;return 1; }
static int dma(const void *pointer, uint64_t size, uint64_t *address) { (void)size;return physical(pointer,address); }
static void hot_read_access(void *argument)
{
    (void)argument;
    kernel_vfs_file_accessed(hot_file);
    uintptr_t irq=riscv_interrupt_save();
    hot_read_done=1;
    check(kernel_wait_queue_wake_one(&hot_read_completion)==KERNEL_SCHEDULER_STATUS_OK,47);
    riscv_interrupt_restore(irq);
}
static int hold_checkpoint_phase(struct ext4_blockdev *bdev,const struct ext4_block_write *first)
{
    struct jbd_journal *journal=bdev->fs->jbd_journal;
    if(checkpoint_hold_armed && journal->checkpointing && first==journal->checkpoint_io) {
        uintptr_t irq=riscv_interrupt_save();
        checkpoint_hold_armed=0; checkpoint_held=1;
        check(kernel_wait_queue_wake_all(&checkpoint_held_wait)==KERNEL_SCHEDULER_STATUS_OK,78);
        while(!checkpoint_release) {
            enum kernel_wait_wake_reason reason;
            check(kernel_scheduler_block_current(&checkpoint_release_wait,0,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,79);
        }
        riscv_interrupt_restore(irq);
    }
    return checkpoint_write_phase(bdev,first);
}
static void exercise_independent_checkpoint(struct lwext4_mount_adapter *adapter,ext4_file *file)
{
    check(ext4_journal_group_drain(adapter->mount_point)==0,80);
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    uint64_t checkpoint=journal->checkpoint_sequence;
    kernel_wait_queue_init(&checkpoint_held_wait);
    kernel_wait_queue_init(&checkpoint_release_wait);
    checkpoint_write_phase=adapter->interface.bwrite_phase;
    checkpoint_hold_armed=1;
    adapter->interface.bwrite_phase=hold_checkpoint_phase;
    const struct ext4_timestamp older[3]={{411,11},{422,22},{433,33}};
    const struct ext4_timestamp newer[3]={{511,11},{522,22},{533,33}};
    check(ext4_file_set_times(file,7,older)==0 && ext4_file_sync_metadata(file)==0,81);
    uint64_t deadline;
    check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+2000000000,&deadline)==KERNEL_TIME_STATUS_OK,82);
    while(!checkpoint_held && riscv_time_read()<deadline) {
        enum kernel_wait_wake_reason reason;
        check(kernel_scheduler_block_current(&checkpoint_held_wait,deadline,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,83);
    }
    check(checkpoint_held && journal->checkpointing && journal->checkpoint_sequence==checkpoint,84);
    struct jbd_trans *held=journal->checkpointing;
    uint64_t durable=journal->durable_sequence;
    check(ext4_file_set_times(file,7,newer)==0 && ext4_file_sync_metadata(file)==0,85);
    check(journal->checkpointing==held && journal->checkpoint_sequence==checkpoint &&
        journal->durable_sequence>durable && journal->durable_sequence==journal->accepted_sequence,86);
    uintptr_t irq=riscv_interrupt_save();
    checkpoint_release=1;
    check(kernel_wait_queue_wake_all(&checkpoint_release_wait)==KERNEL_SCHEDULER_STATUS_OK,87);
    riscv_interrupt_restore(irq);
    check(ext4_journal_group_drain(adapter->mount_point)==0 && !journal->checkpointing &&
        journal->checkpoint_sequence==journal->durable_sequence,88);
    adapter->interface.bwrite_phase=checkpoint_write_phase;
    struct ext4_inode inode;
    check(ext4_fraw_inode_fill(file,&inode)==0 && inode.modification_time==522 && inode.mtime_extra==(22U<<2),89);
    virt_uart_puts("BoarOS: held checkpoint allowed later durable fsync\n");
}
static void exercise_nojournal_times(struct lwext4_mount_adapter *adapter)
{
    struct kernel_vfs_file file={0};
    struct kernel_vfs_stat expected, observed;
    size_t count=0;
    check(kernel_vfs_open(&mount,"/background-atime",&file)==0 &&
        kernel_vfs_pread(&file,0,actual,1,&count)==0 && count==1 && actual[0]=='R',65);
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    uint32_t inode=node->inode;
    uint32_t block_size=ext4_sb_get_block_size(adapter->superblock);
    struct ext4_inode_ref reference;
    adapter->locks.lock(adapter->locks.context);
    check(ext4_fs_get_inode_ref(adapter->device.fs,inode,&reference)==0,66);
    uint64_t block=reference.block.lb_id;
    size_t inode_offset=(unsigned char *)reference.inode-reference.block.data;
    check(ext4_fs_put_inode_ref(&reference)==0,66);
    adapter->locks.unlock(adapter->locks.context);
    check(kernel_page_cache_invalidate_node(&cache,node)==KERNEL_PAGE_CACHE_STATUS_OK,67);
    kernel_vfs_file_accessed(&file);
    check(kernel_vfs_fstat(&file,&expected)==0 &&
        (expected.atime.seconds || expected.atime.nanoseconds) &&
        !node->dirty_cache_pages.count &&
        ((struct lwext4_node *)node)->dirty_fields==EXT4_TIME_ATIME &&
        kernel_vfs_close(&file)==0,68);
    /* The clean data page was removed above. Only the dirty timestamp pin
     * keeps this inode alive until the existing mount worker has stored it. */
    check(adapter->journal_started && adapter->dirty_times,69);
    uint64_t deadline;
    check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+2000000000,&deadline)==KERNEL_TIME_STATUS_OK,70);
    enum kernel_wait_wake_reason reason;
    while(adapter->dirty_times && riscv_time_read()<deadline)
        check(kernel_scheduler_block_current(&adapter->journal_progress,deadline,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,71);
    check(!adapter->dirty_times,72);
    for(node=adapter->instance.nodes;node;node=node->next) check(node->inode!=inode,73);
    /* Read the actual inode-table block through the device, bypassing lwext4's
     * metadata cache, before any explicit sync or unmount can save the value. */
    check(block_size==sizeof(actual) && kernel_block_read_at(&device.block,
        block*block_size,actual,block_size)==KERNEL_BLOCK_STATUS_OK,74);
    struct ext4_inode disk;
    memcpy(&disk,actual+inode_offset,sizeof(disk));
    check((int32_t)disk.access_time==expected.atime.seconds &&
        (disk.atime_extra>>2)==expected.atime.nanoseconds,75);
    check(kernel_vfs_unmount(&mount)==0 &&
        kernel_vfs_mount_root(&mount,&device.block,&heap,&cache)==0 &&
        kernel_vfs_open(&mount,"/background-atime",&file)==0 &&
        kernel_vfs_fstat(&file,&observed)==0 &&
        !memcmp(&expected.atime,&observed.atime,sizeof(expected.atime)) &&
        kernel_vfs_close(&file)==0 && kernel_vfs_unmount(&mount)==0,76);
    check(kernel_page_cache_destroy(&cache)==KERNEL_PAGE_CACHE_STATUS_OK &&
        riscv_virtio_mmio_block_destroy(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK,77);
    completed=1;
}

static void exercise(void *argument)
{
    (void)argument;
    uintptr_t irq=riscv_interrupt_save();
    int startup=kernel_vfs_start_journal_worker(&mount);
    if(startup){virt_uart_puts("journal startup errno=");virt_uart_put_hex((uint64_t)(int64_t)startup);virt_uart_putc('\n');
        struct lwext4_mount_adapter *a=mount.private_data;
        if(a->device.fs->jbd_journal){virt_uart_puts("journal state error/memory/limit=");virt_uart_put_hex(a->device.fs->jbd_journal->error);virt_uart_putc(' ');virt_uart_put_hex(a->device.fs->jbd_journal->memory_used);virt_uart_putc(' ');virt_uart_put_hex(a->device.fs->jbd_journal->memory_limit);virt_uart_putc('\n');}}
    check(startup==0,20);
    struct lwext4_mount_adapter *adapter=mount.private_data;
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    journaled=journal!=NULL;
    if(!journaled){exercise_nojournal_times(adapter);riscv_interrupt_restore(irq);return;}
    struct kernel_vfs_file noatime={0};
    const struct kernel_vfs_timespec noatime_old[2]={{0,0},{0,0}};
    struct kernel_vfs_stat noatime_stat;
    check(kernel_vfs_open(&mount,"/noatime",&noatime)==0 &&
        kernel_vfs_file_set_times(&noatime,noatime_old)==0,63);
    uint64_t noatime_sequence=journal->accepted_sequence;
    kernel_vfs_file_accessed(&noatime);
    check(kernel_vfs_fstat(&noatime,&noatime_stat)==0 && noatime_stat.atime.seconds==0 &&
        noatime_stat.atime.nanoseconds==0 && journal->accepted_sequence==noatime_sequence &&
        kernel_vfs_close(&noatime)==0 && kernel_vfs_unlink(&mount,"/noatime")==0,64);
    struct kernel_vfs_file file={0};
    check(kernel_vfs_create(&mount,"/group",0600,&file)==0,21);
    size_t count;uint64_t error=0;
    for(unsigned page=0;page<128;page++) {
        memset(bytes,(int)(page+1),sizeof(bytes));
        check(kernel_vfs_file_modified(&file, 0, 1)==0 &&
            kernel_vfs_pwrite(&file,(uint64_t)page*sizeof(bytes),bytes,sizeof(bytes),&count)==0 && count==sizeof(bytes),22);
    }
    int synced=kernel_vfs_sync(&file,1,&error);
    if(synced){virt_uart_puts("sync errno/journal memory/limit/accepted/durable/cp=");
        virt_uart_put_hex((uint64_t)(int64_t)synced);virt_uart_putc(' ');
        virt_uart_put_hex(journal->memory_used);virt_uart_putc(' ');virt_uart_put_hex(journal->memory_limit);virt_uart_putc(' ');
        virt_uart_put_hex(journal->accepted_sequence);virt_uart_putc(' ');virt_uart_put_hex(journal->durable_sequence);virt_uart_putc(' ');virt_uart_put_hex(journal->checkpoint_sequence);virt_uart_putc('\n');}
    check(synced==0,23);
    check(journal->durable_sequence==journal->accepted_sequence && journal->memory_peak<=journal->memory_limit,24);
    struct riscv_virtio_mmio_block_statistics statistics;
    riscv_virtio_mmio_block_get_statistics(&device,&statistics);
    check(statistics.max_inflight>1 && statistics.max_inflight<=8,42);
    kernel_vfs_file_accessed(&file);
    hot_file=&file;
    kernel_wait_queue_init(&hot_read_completion);
    struct kernel_thread_join reader={0};
    {
        KERNEL_LOCK_SCOPE(shared);
        kernel_vfs_node_lock(kernel_vfs_file_node(&file),&shared,0);
        check(kernel_thread_create_joinable(hot_read_access,NULL,&reader)==KERNEL_SCHEDULER_STATUS_OK,43);
        uint64_t limit;
        enum kernel_wait_wake_reason reason;
        check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+1000000000,&limit)==KERNEL_TIME_STATUS_OK,44);
        while(!hot_read_done && riscv_time_read()<limit)
            check(kernel_scheduler_block_current(&hot_read_completion,limit,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,44);
        check(hot_read_done,45);
    }
    kernel_thread_join(&reader);
    for(unsigned page=0;page<128;page++) {
        memset(bytes,(int)(page+1),sizeof(bytes));
        check(kernel_vfs_pread(&file,(uint64_t)page*sizeof(bytes),actual,sizeof(actual),&count)==0 && count==sizeof(actual) && !memcmp(bytes,actual,sizeof(bytes)),25);
    }
    const struct kernel_vfs_timespec old_times[2]={{1,0},{1,0}};
    check(kernel_vfs_file_set_times(&file,old_times)==0,46);
    struct kernel_vfs_file alias={0};
    check(kernel_vfs_open(&mount,"/group",&alias)==0 &&
        kernel_vfs_file_node(&alias)==kernel_vfs_file_node(&file),48);
    struct ext4_inode disk_before, disk_after;
    ext4_file *handle=lwext4_node_file(kernel_vfs_file_node(&file));
    exercise_independent_checkpoint(adapter,handle);
    check(ext4_fraw_inode_fill(handle,&disk_before)==0,49);
    uint64_t before=journal->accepted_sequence;
    for(unsigned repeat=0;repeat<64;repeat++) {
        check(kernel_vfs_file_modified(&file, 0, 1)==0,26);
        kernel_vfs_file_accessed(&file);
    }
    check(journal->accepted_sequence==before,50);
    struct kernel_vfs_stat stat={0}, alias_stat={0};
    check(kernel_vfs_fstat(&file,&stat)==0 && kernel_vfs_fstat(&alias,&alias_stat)==0 &&
        (stat.mtime.seconds!=1 || stat.mtime.nanoseconds!=0) &&
        !memcmp(&stat.atime,&alias_stat.atime,sizeof(stat.atime)) &&
        !memcmp(&stat.mtime,&alias_stat.mtime,sizeof(stat.mtime)) &&
        !memcmp(&stat.ctime,&alias_stat.ctime,sizeof(stat.ctime)) &&
        ext4_fraw_inode_fill(handle,&disk_after)==0 &&
        disk_after.modification_time==disk_before.modification_time &&
        disk_after.mtime_extra==disk_before.mtime_extra,51);
    /* A real metadata abort must preserve the earlier dirty runtime values. */
    const struct ext4_timestamp explicit[3]={{77,17},{78,18},{79,19}};
    check(kernel_vfs_file_modified(&file,0,0)==0 && kernel_vfs_fstat(&file,&stat)==0 &&
        ext4_transaction_begin(adapter->mount_point)==0 &&
        ext4_file_set_times(handle,7,explicit)==0 &&
        ext4_transaction_abort(adapter->mount_point,ECANCELED)==ECANCELED &&
        kernel_vfs_fstat(&alias,&alias_stat)==0 &&
        !memcmp(&stat.mtime,&alias_stat.mtime,sizeof(stat.mtime)) &&
        !memcmp(&stat.ctime,&alias_stat.ctime,sizeof(stat.ctime)),52);
    /* A read after a tentative explicit set owns a newer atime. Acceptance
     * must not overwrite it with the earlier transaction's staged value. */
    const struct kernel_vfs_timespec stale_atime[2]={{0,0},{0,KERNEL_VFS_UTIME_OMIT}};
    check(kernel_vfs_file_set_times(&file,stale_atime)==0 && kernel_vfs_fstat(&file,&stat)==0 &&
        ext4_transaction_begin(adapter->mount_point)==0 &&
        ext4_file_set_times(handle,EXT4_TIME_ATIME,explicit)==0,60);
    kernel_vfs_file_accessed(&file);
    check(ext4_transaction_end(adapter->mount_point)==0 &&
        kernel_vfs_fstat(&file,&alias_stat)==0 && alias_stat.atime.seconds!=77,61);
    /* A metadata-only fsync must encode the current RAM values even though
     * every data page was already clean before these writes were announced. */
    check(kernel_vfs_sync(&file,0,&error)==0 &&
        ext4_fraw_inode_fill(handle,&disk_after)==0 &&
        (int32_t)disk_after.modification_time==stat.mtime.seconds &&
        (disk_after.mtime_extra>>2)==stat.mtime.nanoseconds &&
        (int32_t)disk_after.change_inode_time==stat.ctime.seconds &&
        (disk_after.ctime_extra>>2)==stat.ctime.nanoseconds,53);
    /* Explicit atime with OMIT keeps the dirty runtime mtime authoritative. */
    check(kernel_vfs_file_modified(&file,0,0)==0 && kernel_vfs_fstat(&file,&stat)==0,54);
    const struct kernel_vfs_timespec partial_times[2]={{123,456},{0,KERNEL_VFS_UTIME_OMIT}};
    check(kernel_vfs_file_set_times(&alias,partial_times)==0 &&
        kernel_vfs_fstat(&file,&alias_stat)==0 && alias_stat.atime.seconds==123 &&
        alias_stat.atime.nanoseconds==456 &&
        !memcmp(&stat.mtime,&alias_stat.mtime,sizeof(stat.mtime)) &&
        kernel_vfs_sync(&file,0,&error)==0,55);
    /* A dirty timestamp owns the inode after the last descriptor disappears.
     * Wait on actual journal progress with a deadline, then reopen it. */
    check(kernel_vfs_close(&alias)==0,57);
    struct kernel_vfs_file time_only={0};
    check(kernel_vfs_create(&mount,"/time-only",0600,&time_only)==0 &&
        kernel_vfs_file_modified(&time_only,0,0)==0 &&
        kernel_vfs_fstat(&time_only,&stat)==0 && kernel_vfs_close(&time_only)==0,58);
    uint64_t deadline;
    check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+2000000000,&deadline)==KERNEL_TIME_STATUS_OK,27);
    enum kernel_wait_wake_reason reason;
    while((adapter->dirty_times || journal->checkpoint_sequence!=journal->accepted_sequence) && riscv_time_read()<deadline)
        check(kernel_scheduler_block_current(&adapter->journal_progress,deadline,0,&reason)==KERNEL_SCHEDULER_STATUS_OK,28);
    check(!adapter->dirty_times && journal->checkpoint_sequence==journal->accepted_sequence,29);
    check(kernel_vfs_open(&mount,"/time-only",&time_only)==0 &&
        kernel_vfs_fstat(&time_only,&alias_stat)==0 &&
        !memcmp(&stat.mtime,&alias_stat.mtime,sizeof(stat.mtime)) &&
        kernel_vfs_close(&time_only)==0,59);
    check(kernel_vfs_ftruncate(&file,4097)==0 && kernel_vfs_sync(&file,0,&error)==0,30);
    check(kernel_vfs_unlink(&mount,"/group")==0,31);
    memset(bytes,0x92,sizeof(bytes));
    check(kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&count)==0 && count==sizeof(bytes) && kernel_vfs_sync(&file,0,&error)==0,32);
    check(kernel_vfs_close(&file)==0,33);
    check(kernel_vfs_unmount(&mount)==0,34);
    check(kernel_vfs_mount_root(&mount,&device.block,&heap,&cache)==0 &&
        kernel_vfs_open(&mount,"/time-only",&time_only)==0 &&
        kernel_vfs_fstat(&time_only,&alias_stat)==0 &&
        !memcmp(&stat.mtime,&alias_stat.mtime,sizeof(stat.mtime)) &&
        kernel_vfs_close(&time_only)==0 && kernel_vfs_unlink(&mount,"/time-only")==0 &&
        kernel_vfs_unmount(&mount)==0,62);
    check(kernel_page_cache_destroy(&cache)==KERNEL_PAGE_CACHE_STATUS_OK,35);
    check(riscv_virtio_mmio_block_destroy(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK,36);
    completed=1;
    riscv_interrupt_restore(irq);
}

void kernel_main(unsigned long hart,const void *dtb)
{
    struct dtb_boot_info info;struct dtb_irq_info routing;
    struct boot_memory_layout layout={0};
    check(dtb_read_boot_info(dtb,&info)==DTB_STATUS_OK && dtb_read_irq_info(dtb,hart,&routing)==DTB_STATUS_OK,1);
    layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    check(physical_page_allocator_init(&allocator,&layout)==PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_bind_access(&allocator,access_page)==PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_finalize(&allocator)==PHYSICAL_PAGE_STATUS_OK &&
        kernel_heap_init(&heap,&allocator,physical)==KERNEL_HEAP_STATUS_OK &&
        kernel_page_cache_init(&cache,&heap,&allocator)==KERNEL_PAGE_CACHE_STATUS_OK,2);
    int found=0;
    for(unsigned i=0;i<info.virtio_mmio_count;i++)
        if(riscv_virtio_mmio_block_init(&device,(void*)(uintptr_t)info.virtio_mmio[i].base,info.virtio_mmio[i].size,&allocator,dma,info.timebase_frequency)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK){found=1;break;}
    check(found && kernel_vfs_mount_root(&mount,&device.block,&heap,&cache)==0,3);
    check(kernel_scheduler_init(&allocator,(uintptr_t)__boot_stack_bottom,(uintptr_t)__boot_stack_top)==KERNEL_SCHEDULER_STATUS_OK,4);
    check(riscv_plic_init((void*)(uintptr_t)routing.plic.base,routing.plic.size,routing.context,routing.source_count),5);
    uint32_t source=0;
    for(unsigned i=0;i<routing.route_count;i++) if(routing.routes[i].base==(uintptr_t)riscv_virtio_mmio_block_base(&device))source=routing.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device,source),6);
    check(kernel_time_init(info.timebase_frequency,0)==KERNEL_TIME_STATUS_OK && riscv_timer_start(info.timebase_frequency,100)==RISCV_TIMER_STATUS_OK,7);
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(1,info.timebase_frequency,1,0)==0,40);
#endif
    check(kernel_thread_create(exercise,NULL)==KERNEL_SCHEDULER_STATUS_OK,8);
    while(!completed) {
        uintptr_t irq=riscv_interrupt_save();
        check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,9);
        riscv_interrupt_restore(irq|RISCV_SSTATUS_SIE);
    }
#if BOAROS_COST_DIAGNOSTICS
    uint64_t groups, data_writes, resumes;
    check(kernel_cost_end(1,0)==0 &&
        kernel_cost_read(1,COST_JOURNAL_GROUPS,&groups)==0 && (journaled ? groups>0 : groups==0) &&
        kernel_cost_read(1,COST_DEVICE_OTHER_PRECOMMIT_BODY,&data_writes)==0 && (!journaled || data_writes>0) &&
        kernel_cost_read(1,COST_IO_COMPLETE_TO_RESUME,&resumes)==0 && (!journaled || resumes>0),41);
#endif
    virt_uart_puts("BoarOS: journal group tests passed\n");sbi_shutdown();
}
