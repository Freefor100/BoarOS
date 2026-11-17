#include "private.h"
#include "ptrace_internal.h"
#include <arch/elf.h>
#include <kernel/credentials.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/vfs.h>
#include <string.h>

/* First-stage file dumper. No worker or per-task allocation on normal exit.
 * Only an exclusive MM with one live group member is supported: dumping a
 * concurrently running sibling would turn the register/map image into a lie. */
struct core_header {
    unsigned char ident[16];
    uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct core_status {
    int32_t info[3]; uint16_t signal; uint16_t pad;
    uint64_t pending, blocked;
    int32_t pid, ppid, pgrp, sid;
    int64_t times[8];
    /* The architecture gregset immediately follows this common prefix. */
};
_Static_assert(sizeof(struct core_header)==64,"ELF header ABI");
_Static_assert(sizeof(struct core_status)==112,"Linux prstatus prefix");

static int core_write(struct kernel_vfs_file *file,uint64_t *offset,
    const void *data,size_t size,uint64_t limit)
{
    if (*offset>limit || size>limit-*offset) return 0;
    size_t done=0;
    int error=kernel_vfs_pwrite(file,*offset,data,size,&done);
    *offset+=done;
    return !error && done==size;
}

int process_core_dump(struct kernel_task *task,unsigned signal)
{
    struct kernel_rlimit64 core, fsize;
    if (kernel_exec_security_dumpable(task->exec_security)!=1 ||
        kernel_task_get_rlimit(task,KERNEL_RLIMIT_CORE,&core) ||
        kernel_task_get_rlimit(task,KERNEL_RLIMIT_FSIZE,&fsize) ||
        core.current<BOAROS_PAGE_SIZE || !fsize.current) return 0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (task->group_leader!=task || task->group_members!=1 ||
            task->group_exiting || task->group_execing || task->terminate_requested ||
            kernel_mm_user_count(&task->mm)!=1) return 0;
    }
    uint64_t limit=core.current<fsize.current?core.current:fsize.current;
    struct kernel_heap *heap=task->files.heap;
    struct kernel_mm_proc_map *maps=0;
    uint32_t count=0;
    if (kernel_mm_proc_maps_snapshot(&task->mm,heap,&maps,&count)) return 0;
    int dumped=0,opened=0;
    uint64_t offset=0;
    struct kernel_vfs_file file={0};
    struct kernel_fs_paths paths={0};
    struct kernel_task_io_buffer scratch={0};
    if (count>=65534 || kernel_task_io_buffer_acquire(&scratch,task->mm.allocator)) goto out;
    /* Linux PRSTATUS: common fields, native gregset, fpvalid and tail padding. */
    unsigned char note[512]={0};
    uint32_t *nh=(void *)note;
    size_t registers=arch_ptrace_regset_size(1);
    size_t descsize=(sizeof(struct core_status)+registers+4+7)&~(size_t)7;
    nh[0]=5;nh[1]=(uint32_t)descsize;nh[2]=1;
    memcpy(note+12,"CORE",5);
    struct core_status status_storage={0};
    struct core_status *status=&status_storage;
    status->info[0]=(int32_t)signal;status->signal=(uint16_t)signal;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        status->blocked=task->signal_blocked;status->pid=task->tid;
        status->ppid=task->parent?task->parent->tid:0;
        status->pgrp=process_identity_number(task,KERNEL_PID_PGID);
        status->sid=process_identity_number(task,KERNEL_PID_SID);
    }
    uint64_t original=0;
    /* note+20 is only 4-byte aligned, so copy from aligned register storage. */
    uint64_t regs[45]={0};
    arch_ptrace_regset(task,1,regs,&original,0);
    memcpy(note+20+sizeof(*status),regs,registers);
    memcpy(note+20,status,sizeof(*status));
    size_t notesize=20+descsize;
    struct core_header header={.ident={0x7f,'E','L','F',2,1,1},.type=4,
        .machine=ARCH_ELF_MACHINE,.version=1,.phoff=64,.ehsize=64,
        .phentsize=56,.phnum=(uint16_t)(count+1)};
    uint64_t noteoff=64+(uint64_t)(count+1)*56;
    uint64_t dataoff=(noteoff+notesize+BOAROS_PAGE_SIZE-1)&~(uint64_t)(BOAROS_PAGE_SIZE-1);
    if (dataoff>limit) goto out;
    paths=kernel_fs_context_snapshot(&task->fs);
    struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_CREATE|KERNEL_VFS_OPEN_REGULAR,
        .mode=0600,.access=KERNEL_VFS_MAY_WRITE,.resolve=KERNEL_VFS_RESOLVE_NO_SYMLINKS};
    struct kernel_vfs_open_result result={0};
    int error=kernel_vfs_open_at(paths.cwd,paths.root,"core",&request,&file,&result);
    if (result.pathless_link) kernel_vfs_path_release(&result.pathless_link);
    if (error) goto out;
    opened=1;
    struct kernel_vfs_stat stat;
    const struct kernel_credentials *credentials=kernel_credentials_current();
    if (kernel_vfs_fstat(&file,&stat) || stat.nlink!=1 ||
        stat.uid!=credentials->fsuid || (stat.mode&07777)!=0600 ||
        (stat.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG ||
        kernel_vfs_file_acquire_write(&file) || kernel_vfs_ftruncate(&file,0)) goto out;
    if (!core_write(&file,&offset,&header,sizeof(header),limit)) goto out;
    struct kernel_elf64_program_header ph={.type=4,.offset=noteoff,.file_size=notesize,.alignment=4};
    if (!core_write(&file,&offset,&ph,sizeof(ph),limit)) goto out;
    uint64_t next=dataoff;
    for (uint32_t i=0;i<count;i++) {
        struct kernel_mm_proc_map *map=&maps[i];
        uint64_t bytes=(map->flags&KERNEL_VMA_DONTDUMP) || map->role==KERNEL_VMA_ROLE_VVAR ? 0 : map->end-map->start;
        if (bytes>UINT64_MAX-next) goto out;
        ph=(struct kernel_elf64_program_header){.type=1,
            .flags=((map->permissions&KERNEL_MM_READ)?4U:0U)|
                ((map->permissions&KERNEL_MM_WRITE)?2U:0U)|((map->permissions&KERNEL_MM_EXECUTE)?1U:0U),
            .offset=next,.virtual_address=map->start,.file_size=bytes,
            .memory_size=map->end-map->start,.alignment=BOAROS_PAGE_SIZE};
        if (!core_write(&file,&offset,&ph,sizeof(ph),limit)) goto out;
        next+=bytes;
    }
    if (!core_write(&file,&offset,note,notesize,limit)) goto out;
    dumped=1;offset=dataoff;
    for (uint32_t i=0;i<count;i++) {
        struct kernel_mm_proc_map *map=&maps[i];
        if ((map->flags&KERNEL_VMA_DONTDUMP) || map->role==KERNEL_VMA_ROLE_VVAR) continue;
        for (uint64_t address=map->start;address<map->end;address+=BOAROS_PAGE_SIZE) {
            if (__atomic_load_n(&task->terminate_requested,__ATOMIC_RELAXED) || offset>limit || BOAROS_PAGE_SIZE>limit-offset) goto out;
            size_t copied=0;
            /* Debugger read preserves protection bits and handles PROT_NONE.
             * Inaccessible backing (e.g. a truncated file) is an ELF hole. */
            memset(scratch.data,0,BOAROS_PAGE_SIZE);
            unsigned char resident=1;unsigned completed=0;
            if (map->kind==KERNEL_VMA_KIND_ANONYMOUS)
                (void)kernel_mm_mincore(&task->mm,address,1,&resident,&completed);
            if (resident || !completed)
                (void)kernel_mm_remote_transfer(&task->mm,address,scratch.data,BOAROS_PAGE_SIZE,0,1,&copied);
            /* Untouched anonymous and inaccessible pages need neither fault
             * allocation nor data writes. Extend the sparse file at cleanup. */
            const uint64_t *words=scratch.data;uint64_t nonzero=0;
            for (size_t j=0;j<BOAROS_PAGE_SIZE/sizeof(*words);j++) nonzero|=words[j];
            if (!nonzero) offset+=BOAROS_PAGE_SIZE;
            else if (!core_write(&file,&offset,scratch.data,BOAROS_PAGE_SIZE,limit)) goto out;
        }
    }
out:
    kernel_fs_paths_release(&paths);
    if (opened) {
        if (dumped) (void)kernel_vfs_ftruncate(&file,offset);
        (void)kernel_vfs_close(&file);
    }
    if (scratch.allocator) kernel_task_io_buffer_release(&scratch);
    if (maps) {
        for (uint32_t i=0;i<count;i++) if (maps[i].file) kernel_open_file_release(&maps[i].file);
        kernel_heap_release(heap,maps);
    }
    return dumped;
}
