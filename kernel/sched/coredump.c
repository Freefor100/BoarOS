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
#include <kernel/time.h>
#include <string.h>

/* No worker or per-task allocation on normal exit. */
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

/* A terminal rendezvous, not a reusable stop state. All members retain their
 * MM and user registers until the owner releases the session; robust-list and
 * clear_child_tid writes happen afterwards. Nothing is added to switch paths. */
struct process_core_session {
    struct kernel_task *owner;
    struct kernel_wait_queue arrivals;
    uint32_t count, arrived;
    unsigned signal;
    size_t notesize;
    unsigned char notes[];
};

static size_t core_note(struct kernel_task *task,unsigned signal,unsigned char *note)
{
    size_t registers=arch_ptrace_regset_size(1);
    size_t descsize=(sizeof(struct core_status)+registers+4+7)&~(size_t)7;
    uint32_t nh[3]={5,(uint32_t)descsize,1};
    memcpy(note,nh,sizeof(nh));memcpy(note+12,"CORE",5);
    struct core_status status={0};
    status.info[0]=(int32_t)signal;status.signal=(uint16_t)signal;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_pid_registry *view=process_pid_active(task);
        status.blocked=task->signal_blocked;
        status.pid=kernel_pid_number_in(process_identity(task,KERNEL_PID_TID),view);
        /* Linux records the real parent's TID, not getppid()'s TGID.
         * All threads inherit this relationship from their group. */
        status.ppid=kernel_pid_number_in(task->group_leader->child_creator,view);
        status.pgrp=kernel_pid_number_in(process_identity(task,KERNEL_PID_PGID),view);
        status.sid=kernel_pid_number_in(process_identity(task,KERNEL_PID_SID),view);
    }
    uint64_t original=0,regs[45]={0};
    arch_ptrace_regset(task,1,regs,&original,0);
    memcpy(note+20,&status,sizeof(status));
    memcpy(note+20+sizeof(status),regs,registers);
    return 20+descsize;
}

static int core_pending(struct kernel_task *leader,struct process_core_session *session)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return leader->core_session==session;
}

void process_core_rendezvous(struct kernel_task *task)
{
    struct kernel_task *leader=task->group_leader;
    struct process_core_session *session;
    uint32_t slot;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        session=leader?leader->core_session:0;
        if (!session || session->owner==task) return;
        slot=session->arrived;
        if (slot>=session->count) __builtin_trap();
        /* Exit and all MM service run on the service CPU. The session cannot
         * disappear while this nonblocking register copy is in progress. */
    }
    core_note(task,session->signal,session->notes+slot*session->notesize);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        session->arrived++;
    }
    (void)kernel_wait_queue_wake_one(&session->arrivals);
    while (core_pending(leader,session)) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&leader->group_wait_queue,0,0,&reason,
                               core_pending(leader,session))) __builtin_trap();
    }
}

static int core_arriving(struct process_core_session *session)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return session->arrived!=session->count;
}

static void core_release(struct kernel_task *leader,struct process_core_session *session,int dumped)
{
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (leader->core_session!=session) __builtin_trap();
        leader->completion.detail=dumped;
        leader->core_session=0;
    }
    (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
}

static struct process_core_session *core_freeze(struct kernel_task *task,unsigned signal)
{
    struct kernel_task *leader=task->group_leader;
    struct process_core_session *session=0;
    uint32_t count;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (leader->group_exiting || leader->group_execing || task->terminate_requested) return 0;
        count=leader->group_members;
    }
    size_t notesize=20+((sizeof(struct core_status)+arch_ptrace_regset_size(1)+4+7)&~(size_t)7);
    if (!count || count>(SIZE_MAX-sizeof(*session))/notesize ||
        kernel_heap_allocate_zeroed(task->files.heap,1,sizeof(*session)+count*notesize,(void **)&session)) return 0;
    kernel_wait_queue_init(&session->arrivals);
    session->owner=task;session->count=count;session->signal=signal;session->notesize=notesize;
    core_note(task,signal,session->notes);session->arrived=1;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (leader->group_exiting || leader->group_execing || task->terminate_requested ||
            leader->group_members!=count || kernel_mm_user_count(&task->mm)!=count) goto reject;
        struct kernel_task *member=leader;
        do {
            /* Tracers may hold a stopped task's registers across sleeping
             * accesses. Defer traced dumps rather than race that ownership. */
            if (member->proc_exiting || member->retirement || member->terminate_requested ||
                member->mm.record!=task->mm.record ||
                (member->ptrace && (member->ptrace->tracer || member->ptrace->access_target))) goto reject;
            member=member->group_next;
        } while (member!=leader);
        leader->core_session=session;
        leader->group_exiting=1;
        leader->completion.reason=KERNEL_THREAD_EXIT_SIGNAL;
        leader->completion.status=signal;
        leader->completion.detail=0;
    }
    /* Publication above closes clone/exec; now request peer exit/IPIs. It deliberately
     * retains the first fatal signal even if another member faults meanwhile. */
    process_group_request_exit(task,KERNEL_THREAD_EXIT_SIGNAL,signal,0);
    uint64_t deadline;
    if (kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+UINT64_C(5000000000),&deadline)) goto failed;
    while (core_arriving(session)) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&session->arrivals,deadline,0,&reason,core_arriving(session)) ||
            reason==KERNEL_WAIT_TIMEOUT) goto failed;
    }
    return session;
failed:
    core_release(leader,session,0);
reject:
    kernel_heap_release(task->files.heap,session);
    return 0;
}

static int core_write(struct kernel_vfs_file *file,uint64_t *offset,
    const void *data,size_t size,uint64_t limit)
{
    if (*offset>limit || size>limit-*offset) return 0;
    size_t done=0;
    int error=kernel_vfs_pwrite(file,*offset,data,size,&done);
    *offset+=done;
    return !error && done==size;
}

/* Snapshot each decision once: neither a racing proc write nor inode metadata
 * changes may make ELF p_filesz disagree with the emitted page range. */
static uint64_t core_map_size(struct kernel_mm *mm,struct kernel_mm_proc_map *map,unsigned filter)
{
    uint64_t length=map->end-map->start;
    /* Linux always_dump_vma exempts vDSO even from MADV_DONTDUMP. */
    if (map->role==KERNEL_VMA_ROLE_VDSO) return length;
    if ((map->flags & KERNEL_VMA_DONTDUMP) || map->role==KERNEL_VMA_ROLE_VVAR) return 0;
    if (map->kind==KERNEL_VMA_KIND_HUGETLB_PRIVATE)
        return (filter & (1U<<5)) ? length : 0;
    if (map->kind==KERNEL_VMA_KIND_HUGETLB_SHARED)
        return (filter & (1U<<6)) ? length : 0;
    struct kernel_vfs_stat stat={0};
    if (map->file && kernel_vfs_fstat((struct kernel_vfs_file *)kernel_open_file_vfs_borrow(map->file),&stat))
        return UINT64_MAX;
    if (map->shared)
        return (filter & (1U << ((!map->file || !stat.nlink) ? 1 : 3))) ? length : 0;
    if ((filter & 1U) && (map->flags & KERNEL_VMA_ANON_HISTORY)) return length;
    if (!map->file) return 0;
    if (filter & (1U<<2)) return length;
    if ((filter & (1U<<4)) && !map->offset && (map->permissions & KERNEL_MM_READ)) {
        if (stat.mode & 0111U) return BOAROS_PAGE_SIZE;
        unsigned char magic[4]={0};size_t copied=0;
        if (!kernel_mm_remote_transfer(mm,map->start,magic,sizeof(magic),0,1,&copied) &&
            copied==sizeof(magic) && magic[0]==0x7f && magic[1]=='E' && magic[2]=='L' && magic[3]=='F')
            return BOAROS_PAGE_SIZE;
    }
    return 0;
}

int process_core_dump(struct kernel_task *task,unsigned signal)
{
    struct kernel_rlimit64 core, fsize;
    if (kernel_exec_security_dumpable(task->exec_security)!=1 ||
        kernel_task_get_rlimit(task,KERNEL_RLIMIT_CORE,&core) ||
        kernel_task_get_rlimit(task,KERNEL_RLIMIT_FSIZE,&fsize) ||
        core.current<BOAROS_PAGE_SIZE || !fsize.current) return 0;
    struct process_core_session *session=core_freeze(task,signal);
    if (!session) return 0;
    int remote_locked=kernel_mm_core_begin(&task->mm);
    uint64_t limit=core.current<fsize.current?core.current:fsize.current;
    struct kernel_heap *heap=task->files.heap;
    struct kernel_mm_proc_map *maps=0;
    uint32_t count=0;

    int dumped=0,opened=0;
    uint64_t offset=0;
    struct kernel_vfs_file file={0};
    struct kernel_fs_paths paths={0};
    struct kernel_task_io_buffer scratch={0};
    if (!remote_locked || kernel_mm_proc_maps_snapshot(&task->mm,heap,&maps,&count)) goto out;
    if (count>=65534 || kernel_task_io_buffer_acquire(&scratch,task->mm.allocator)) goto out;
    unsigned filter=kernel_mm_coredump_filter_get(&task->mm);
    for (uint32_t i=0;i<count;i++) {
        maps[i].dump_size=core_map_size(&task->mm,&maps[i],filter);
        if (maps[i].dump_size==UINT64_MAX) goto out;
    }
    size_t notesize=session->count*session->notesize;
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
        uint64_t bytes=map->dump_size;
        if (bytes>UINT64_MAX-next) goto out;
        ph=(struct kernel_elf64_program_header){.type=1,
            .flags=((map->permissions&KERNEL_MM_READ)?4U:0U)|
                ((map->permissions&KERNEL_MM_WRITE)?2U:0U)|((map->permissions&KERNEL_MM_EXECUTE)?1U:0U),
            .offset=next,.virtual_address=map->start,.file_size=bytes,
            .memory_size=map->end-map->start,.alignment=BOAROS_PAGE_SIZE};
        if (!core_write(&file,&offset,&ph,sizeof(ph),limit)) goto out;
        next+=bytes;
    }
    if (!core_write(&file,&offset,session->notes,notesize,limit)) goto out;
    dumped=1;offset=dataoff;
    for (uint32_t i=0;i<count;i++) {
        struct kernel_mm_proc_map *map=&maps[i];
        for (uint64_t address=map->start;address<map->start+map->dump_size;address+=BOAROS_PAGE_SIZE) {
            if (offset>limit || BOAROS_PAGE_SIZE>limit-offset) goto out;
            size_t copied=0;
            /* Debugger read preserves protection bits and handles PROT_NONE.
             * Inaccessible backing (e.g. a truncated file) is an ELF hole.
             * Another MM may still write shared backing during this copy;
             * the group rendezvous does not promise a global snapshot. */
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
    if (remote_locked) kernel_mm_core_end(&task->mm);
    core_release(task->group_leader,session,dumped);
    kernel_heap_release(heap,session);
    return dumped;
}
