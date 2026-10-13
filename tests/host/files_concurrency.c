#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/physical_page.h>
#include <kernel/cpu.h>
#include <kernel/open_file.h>
#include <kernel/mm_backend.h>
#include <kernel/signal.h>
#include <kernel/tmpfs.h>
#include <kernel/fs_context.h>
#include <kernel/uaccess.h>
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/pipe_internal.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
void mm_test_bind_files(struct kernel_files *);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out)
{ *out = (uintptr_t)p; return 1; }
static unsigned watch_pin, pin_entered, close_started, release_pin;
static struct kernel_files files;
static struct kernel_open_file_description *held;
static struct kernel_mm domain_mm;
static struct kernel_open_file_description *path_description;
static struct kernel_vfs_path *watched_path;
static unsigned path_acquired;
int __real_kernel_vfs_path_acquire(struct kernel_vfs_path *);
int __wrap_kernel_vfs_path_acquire(struct kernel_vfs_path *path)
{
    if (path==watched_path) {
        assert(path_description && "OFD pin ended before the borrowed path acquired its own owner");
        path_acquired=1;
    }
    return __real_kernel_vfs_path_acquire(path);
}
enum kernel_open_file_status __real_kernel_open_file_release(struct kernel_open_file_description **);
enum kernel_open_file_status __wrap_kernel_open_file_release(struct kernel_open_file_description **owner)
{
    if (path_description && *owner==path_description) {
        /* 精确交错：guard归还前关闭槽owner，让pin成为最后OFD引用。 */
        path_description=0;
        int64_t result;
        assert(!kernel_files_close(&files,5,&result) && !result);
    }
    return __real_kernel_open_file_release(owner);
}
static void directory_pin_handoff(struct kernel_heap *heap,int metadata)
{
    files=(struct kernel_files){0};
    struct kernel_vfs_mount *mount=0;
    struct kernel_vfs_path *root=0,*path=0;
    struct kernel_open_file_description *description=0;
    struct kernel_fs_context fs={0};
    int result;
    assert(!kernel_tmpfs_create(heap,0,"",&mount));
    assert(!kernel_vfs_path_root(mount,heap,&root));
    if (metadata) assert(!kernel_fs_context_create(&fs,mount,heap));
    assert(!kernel_open_file_create_at(heap,root,root,"/",KERNEL_OPEN_PATH_FOLLOW,0,&description,&result) && !result);
    assert(!kernel_files_create(&files,heap));
    watched_path=description->file.path; path_description=description; path_acquired=0;
    assert(!kernel_files_install_new_owned_at(&files,5,0,&description));
    if (metadata) {
        uint64_t address;
        size_t copied;
        int64_t linux_result;
        mm_test_bind(&domain_mm);
        assert(!kernel_mm_context(&domain_mm,&mm_test_context));
        assert(!kernel_mm_mmap_anonymous(&domain_mm,0x300000,BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
        assert(!kernel_copy_to_user(&domain_mm,address,"",1,&copied) && copied==1);
        assert(!kernel_files_utimensat(&files,&fs,&domain_mm,5,address,0,
            KERNEL_FILES_AT_EMPTY_PATH,&linux_result) && !linux_result);
        assert(!kernel_mm_munmap(&domain_mm,address,BOAROS_PAGE_SIZE));
        mm_test_bind(0);
        mm_test_context=0;
    } else assert(!kernel_files_path_start(&files,0,5,"child",&path) && path==watched_path);
    assert(path_acquired && !path_description);
    watched_path=0;
    if (path) assert(!kernel_vfs_path_release(&path));
    if (metadata) assert(!kernel_fs_context_release(&fs));
    assert(!kernel_files_release(&files));
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(mount));
    files=(struct kernel_files){0};
}
#ifdef PIPE_CONCURRENCY_CASES
static unsigned signal_calls;
enum kernel_signal_status kernel_signal_send_task(struct kernel_task *task,uint32_t sig,kernel_pid_t sender)
{ (void)task; (void)sig; (void)sender; signal_calls++; return KERNEL_SIGNAL_STATUS_INVALID_STATE; }
void kernel_signal_note_syscall_restart(struct kernel_task *task)
{ (void)task; assert(!"pipe core invoked user signal context"); }
static void broken_kernel_writer(struct kernel_heap *heap)
{
    struct kernel_pipe *pipe=0; struct kernel_open_file_description *a=0,*b=0;
    assert(!kernel_pipe_create(heap,&pipe));
    assert(!kernel_open_file_create_pipe(heap,pipe,KERNEL_PIPE_ENDPOINT_READ,0,&a));
    assert(!kernel_open_file_create_pipe(heap,pipe,KERNEL_PIPE_ENDPOINT_WRITE,0,&b));
    assert(!kernel_open_file_release(&a));
    struct kernel_pipe_result result;
    assert(!kernel_pipe_write_buffer(pipe,"x",1,1,&result) && result.bytes==-32 && result.broken && !signal_calls);
    assert(!kernel_open_file_release(&b));
}
#endif
static struct kernel_files views[4];
static void forbidden_pipe_callback(struct kernel_wait_node *node,uint32_t reason)
{ (void)node; (void)reason; assert(!"publication notified a domain-external observer"); }
static void reject_external_pipe_owner(struct kernel_heap *heap)
{
    files=(struct kernel_files){0};
    struct kernel_files ordinary={0};
    struct kernel_pipe *pipe=0;
    struct kernel_open_file_description *a=0,*b=0,*external=0;
    assert(!kernel_files_create(&files,heap));
    assert(!kernel_pipe_create(heap,&pipe));
    assert(!kernel_open_file_create_pipe(heap,pipe,1,0,&a));
    assert(!kernel_open_file_create_pipe(heap,pipe,2,0,&b));
    assert(!kernel_files_install_new_owned_at(&files,0,0,&a));
    assert(!kernel_files_install_new_owned_at(&files,1,0,&b));
    assert(!kernel_files_fork(&ordinary,&files));
    assert(kernel_files_enable_concurrency(&files,&domain_mm)==KERNEL_FILES_STATUS_STATE &&
        !files.record->concurrent && !pipe->concurrent);
    assert(!kernel_files_release(&ordinary));
    struct kernel_open_file_pipe_pin pin={0};
    assert(!kernel_open_file_pipe_pin(files.record->slots[0].description,0,&pin));
    assert(!kernel_open_file_pipe_finish(heap,&pin,0,&external));
    assert(kernel_files_enable_concurrency(&files,&domain_mm)==KERNEL_FILES_STATUS_STATE &&
        !files.record->concurrent && !pipe->concurrent);
    assert(!kernel_open_file_release(&external));
    struct kernel_wait_node observer={0};
    kernel_wait_node_init_callback(&observer,forbidden_pipe_callback,0);
    kernel_wait_queue_add(&pipe->read_queue,&observer);
    assert(kernel_files_enable_concurrency(&files,&domain_mm)==KERNEL_FILES_STATUS_STATE &&
        !files.record->concurrent && !pipe->concurrent);
    assert(!kernel_wait_node_remove_sync(&observer));
    /* 本表内dup有多个槽owner，却只对应一个端点，仍可启用。 */
    int64_t result;
    assert(!kernel_files_dup(&files,0,&result) && result==2);
    assert(!kernel_files_enable_concurrency(&files,&domain_mm));
    assert(!kernel_files_release(&files));
    puts("PASS controlled pipe publication rejects ordinary aliases and independent OFDs");
}
static unsigned allocated[4][192], group_ready, group_release;
static _Thread_local unsigned fail_growth;
static unsigned maximum_copy, metadata_acquisitions;
void *__real_memcpy(void *,const void *,size_t);
void *__wrap_memcpy(void *dst,const void *src,size_t length)
{
    struct kernel_raw_guard *guard=kernel_cpu_current()->raw_locks;
    if (guard && guard->lock->rank==KERNEL_RAW_RANK_OBJECT) {
        assert(length<=8192 && "FD expansion copied more than 8KiB under raw lock");
        unsigned old=__atomic_load_n(&maximum_copy,__ATOMIC_RELAXED);
        while (old<length && !__atomic_compare_exchange_n(&maximum_copy,&old,
            (unsigned)length,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    }
    return __real_memcpy(dst,src,length);
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    if (lock->rank==KERNEL_RAW_RANK_OBJECT)
        __atomic_fetch_add(&metadata_acquisitions,1,__ATOMIC_RELAXED);
    __real_kernel_raw_lock_acquire(lock,guard);
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t size,void **owner)
{
    assert(!kernel_cpu_current()->raw_locks && "FD candidate allocation under raw protection");
    if (fail_growth && size==sizeof(struct kernel_file_slot) && count>32) {
        fail_growth=0; return KERNEL_HEAP_STATUS_EMPTY;
    }
    return __real_kernel_heap_allocate_zeroed(heap,count,size,owner);
}
static void *dup_group(void *p)
{
    unsigned id=(unsigned)(uintptr_t)p;
    mm_test_attach(id); mm_test_bind(&domain_mm); mm_test_bind_files(&views[id]);
    int64_t result;
    for (unsigned i=0;i<192;i++) {
        assert(!kernel_files_dup(&views[id],1,&result) && result>=0 && result<1024);
        allocated[id][i]=(unsigned)result;
    }
    __atomic_fetch_add(&group_ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&group_release,__ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i=0;i<192;i++)
        assert(!kernel_files_close(&views[id],allocated[id][i],&result) && !result);
    assert(!kernel_files_release(&views[id]));
    mm_test_bind_files(0); mm_test_bind(0);
    return 0;
}
static void concurrent_dup_case(struct kernel_heap *heap,unsigned actors)
{
    files=(struct kernel_files){0};
    assert(!kernel_files_create(&files,heap));
    struct kernel_pipe *pipe=0; struct kernel_open_file_description *a=0,*b=0;
    assert(!kernel_pipe_create(heap,&pipe));
    assert(!kernel_open_file_create_pipe(heap,pipe,KERNEL_PIPE_ENDPOINT_READ,0,&a));
    assert(!kernel_open_file_create_pipe(heap,pipe,KERNEL_PIPE_ENDPOINT_WRITE,0,&b));
    assert(!kernel_files_install_new_owned_at(&files,0,0,&a));
    assert(!kernel_files_install_new_owned_at(&files,1,0,&b));
    assert(!kernel_files_enable_concurrency(&files,&domain_mm));
    group_ready=group_release=0;
    pthread_t threads[4];
    for (unsigned i=0;i<actors;i++) {
        views[i]=(struct kernel_files){0};
        assert(!kernel_files_acquire(&views[i],&files));
        assert(!pthread_create(&threads[i],0,dup_group,(void *)(uintptr_t)i));
    }
    while (__atomic_load_n(&group_ready,__ATOMIC_ACQUIRE)!=actors) sched_yield();
    unsigned owner[1024]={0}; owner[0]=owner[1]=1;
    for (unsigned i=0;i<actors;i++) for (unsigned j=0;j<192;j++)
        assert(!owner[allocated[i][j]]++ && "FD handed to two callers");
    struct kernel_files_statistics stats;
    kernel_files_get_statistics(&files,&stats);
    assert(stats.current_open_fds==2+actors*192 && stats.capacity==(actors==2?512:1024));
    __atomic_store_n(&group_release,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<actors;i++) assert(!pthread_join(threads[i],0));
    kernel_files_get_statistics(&files,&stats);
    assert(stats.current_open_fds==2);
    /* 预留不可被dup3接管；取消后恢复最低可用编号。 */
    uint32_t fd; int error; int64_t result;
    assert(!kernel_files_find_free_fd(&files,&fd,&error) && !error && fd==2);
    struct kernel_files_fd_reservation reservation={&files,fd};
    assert(!kernel_files_dup3(&files,1,fd,0,&result) && result==-16);
    kernel_files_cancel_reservation(&reservation);
    assert(!kernel_files_dup(&files,1,&result) && result==2);
    assert(!kernel_files_fcntl(&files,2,KERNEL_FILES_F_SETFD,1,&result) && !result);
    assert(!kernel_files_fcntl(&files,2,KERNEL_FILES_F_SETFL,KERNEL_FILES_O_NONBLOCK,&result) && !result);
    assert(!kernel_files_fcntl(&files,1,KERNEL_FILES_F_GETFL,0,&result) &&
        result==(KERNEL_FILES_O_NONBLOCK|1));
    assert(!kernel_files_close_on_exec(&files));
    kernel_files_get_statistics(&files,&stats);
    assert(stats.current_open_fds==2 && !stats.close_on_exec_fds);
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        files.record->references=0;
        struct kernel_open_file_description *invalid=0;
        kernel_files_pin(&files,0,&invalid,&result);
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGILL && "live FD record lost ownership without a fatal trap");
    assert(!kernel_files_release(&files));
    printf("PASS %u threads shared FD growth, unique owners, reservations and status flags\n",actors);
}
enum kernel_open_file_status __real_kernel_open_file_acquire(struct kernel_open_file_description *);
enum kernel_open_file_status __wrap_kernel_open_file_acquire(struct kernel_open_file_description *file)
{
    if (__atomic_exchange_n(&watch_pin, 0, __ATOMIC_ACQ_REL)) {
        /* lookup和取得引用之间必须有实际互斥，关本核IRQ不能覆盖另一CPU。 */
        assert(kernel_cpu_current()->raw_locks && "FD lookup-to-OFD acquire is unprotected");
        __atomic_store_n(&pin_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&release_pin, __ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_open_file_acquire(file);
}
static void *pin_thread(void *p)
{
    (void)p; mm_test_attach(0);
    int64_t result;
    assert(!kernel_files_pin(&files, 0, &held, &result) && !result && held);
    return 0;
}
static void *close_thread(void *p)
{
    (void)p; mm_test_attach(0);
    __atomic_store_n(&close_started, 1, __ATOMIC_RELEASE);
    int64_t result;
    assert(!kernel_files_close(&files, 0, &result) && !result);
    return 0;
}
#ifdef PIPE_CONCURRENCY_CASES
#include "pipe_cases.h"
#endif
int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0); mm_test_attach(0);
    struct physical_page_allocator pages = {0}; struct kernel_heap heap = {0};
    struct boot_memory_layout layout = {.usable_count=1,
        .usable={{(uintptr_t)pool, sizeof(pool)}}};
    assert(!physical_page_allocator_init(&pages, &layout) &&
        !physical_page_allocator_bind_access(&pages, access_page) &&
        !physical_page_allocator_finalize(&pages) &&
        !kernel_heap_init(&heap, &pages, page_address));
    uint64_t baseline=physical_page_available(&pages);
    directory_pin_handoff(&heap,0);
    assert(!kernel_files_create(&files, &heap));
    struct kernel_pipe *pipe = 0;
    struct kernel_open_file_description *read = 0, *write = 0;
    assert(!kernel_pipe_create(&heap, &pipe));
    assert(!kernel_open_file_create_pipe(&heap, pipe, KERNEL_PIPE_ENDPOINT_READ, 0, &read));
    assert(!kernel_open_file_create_pipe(&heap, pipe, KERNEL_PIPE_ENDPOINT_WRITE, 0, &write));
    assert(!kernel_files_install_new_owned_at(&files, 0, 0, &read));
    assert(!kernel_files_install_new_owned_at(&files, 1, 0, &write));
    watch_pin=1;
    pthread_t pin, close;
    assert(!pthread_create(&pin, 0, pin_thread, 0));
    while (!__atomic_load_n(&pin_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&close, 0, close_thread, 0));
    while (!__atomic_load_n(&close_started, __ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&release_pin, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(pin, 0) && !pthread_join(close, 0));
    assert(kernel_files_fd_matches(&files,0,0));
    assert(!kernel_open_file_release(&held));
    int64_t result;
    fail_growth=1;
    assert(!kernel_files_dup3(&files,1,63,0,&result) && result==-12 && !fail_growth);
    struct kernel_files_statistics before;
    kernel_files_get_statistics(&files,&before);
    assert(before.capacity==32 && before.current_open_fds==1);
    watch_pin=1;
    assert(!kernel_files_dup3(&files, 1, 1023, 0, &result) && result==1023);
    assert(!kernel_files_close(&files, 1023, &result) && !result);
    assert(!kernel_files_close(&files, 1, &result) && !result);
    assert(!kernel_files_release(&files));
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) &&
        !kernel_mm_create(&domain_mm,&space) && !kernel_mm_vma_enable(&domain_mm,&heap));
    assert(!kernel_mm_brk_initialize(&domain_mm,0x10000,0x20000));
    assert(!kernel_mm_enable_concurrency(&domain_mm));
    directory_pin_handoff(&heap,1);
    reject_external_pipe_owner(&heap);
    concurrent_dup_case(&heap,2); concurrent_dup_case(&heap,4);
#ifdef PIPE_CONCURRENCY_CASES
    broken_kernel_writer(&heap);
    pipe_cases(&heap);
#endif
    assert(!kernel_mm_release(&domain_mm));
    assert(!physical_page_release(&pages,kernel.root_address));
    assert(!physical_page_allocator_audit(&pages));
    struct kernel_heap_statistics final;
    kernel_heap_get_statistics(&heap,&final);
    assert(!final.live_allocations);
    assert(!final.current_pages && physical_page_available(&pages)==baseline);
    assert(sizeof(struct kernel_files_record)<=256 && sizeof(struct kernel_pipe)<=512);
    assert(maximum_copy && maximum_copy<=8192 && metadata_acquisitions);
    printf("FD record=%zu pipe=%zu max raw copy=%u acquisitions=%u\n",
        sizeof(struct kernel_files_record),sizeof(struct kernel_pipe),maximum_copy,metadata_acquisitions);
    puts("PASS FD lookup pins owner before concurrent close");
}
