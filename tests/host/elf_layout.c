#include <kernel/elf_image.h>
#include <kernel/mm_backend.h>
#include <kernel/cpu.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/random.h>
#include <kernel/personality.h>
#include <kernel/exec.h>
#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/tmpfs.h>
#include <kernel/task_state.h>
#include "../../kernel/exec_internal.h"
#include "../../fs/open_file_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
void mm_test_attach(unsigned);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[8*1024*1024] __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct arch_mmu_page_table kernel;
static unsigned close_failures;
static struct kernel_task files_closer;
static unsigned files_cleanup_failures;
enum kernel_files_status kernel_files_release_for_task(struct kernel_files *files,
                                                        struct kernel_task *task)
{
    assert(task==&files_closer);
    if (files_cleanup_failures) {
        files_cleanup_failures--;files->state=KERNEL_FILES_CLEANUP;
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    assert(files->state==KERNEL_FILES_CLEANUP);
    files->state=KERNEL_FILES_RELEASED;return KERNEL_FILES_STATUS_OK;
}
void kernel_task_account_read(uint64_t bytes) { (void)bytes;abort(); }
void kernel_page_cache_put_batch(struct kernel_page_cache *cache,
    struct kernel_page_cache_read_page *pages,size_t count)
{ (void)cache;(void)pages;(void)count;abort(); }
struct kernel_page_cache_prediction kernel_page_cache_readahead(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t first,unsigned count,uint64_t cookie)
{ (void)cache;(void)node;(void)first;(void)count;(void)cookie;abort(); }
int __real_kernel_vfs_close(struct kernel_vfs_file *file);
int __wrap_kernel_vfs_close(struct kernel_vfs_file *file)
{
    if (close_failures) {
        close_failures--;
        return -KERNEL_EIO;
    }
    return __real_kernel_vfs_close(file);
}
__asm__(".pushsection .rodata\n.global arch_elf_vdso_start,arch_elf_vdso_end\narch_elf_vdso_start:\n.byte 0x73,0,0,0\narch_elf_vdso_end:\n.popsection");
static unsigned entropy_seed, entropy_calls;
int kernel_random_available(void) { return entropy_seed != 0; }
enum kernel_random_status kernel_random_fill(void *buffer,size_t size)
{
    entropy_calls++;
    memset(buffer,0,size);
    ((unsigned char *)buffer)[0]=(unsigned char)(entropy_seed+entropy_calls);
    return KERNEL_RANDOM_STATUS_OK;
}
void mm_test_fence(uint64_t address,int instructions)
{ (void)address; (void)instructions; assert(!kernel_cpu_current()->raw_locks); }
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer,uint64_t *address) { *address=(uintptr_t)pointer; return 1; }
static void u16(unsigned char *p,uint16_t v) { for (unsigned i=0;i<2;i++) p[i]=(unsigned char)(v>>(8*i)); }
static void u32(unsigned char *p,uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i)); }
static void u64(unsigned char *p,uint64_t v) { for (unsigned i=0;i<8;i++) p[i]=(unsigned char)(v>>(8*i)); }
static struct kernel_elf64_source *source(unsigned char bytes[256],uint16_t type,uint64_t address,unsigned char payload,int has_interpreter)
{
    memset(bytes,0,256);
    memcpy(bytes,"\177ELF\2\1\1",7);
    u16(bytes+16,type); u16(bytes+18,KERNEL_ELF64_MACHINE_RISCV); u32(bytes+20,1);
    u64(bytes+24,address+184); u64(bytes+32,64); u16(bytes+52,64); u16(bytes+54,56); u16(bytes+56,has_interpreter?2:1);
    u32(bytes+64,KERNEL_ELF64_PROGRAM_LOAD); u32(bytes+68,KERNEL_ELF64_FLAG_READ|KERNEL_ELF64_FLAG_EXECUTE);
    u64(bytes+80,address); u64(bytes+96,256); u64(bytes+104,BOAROS_PAGE_SIZE); u64(bytes+112,BOAROS_PAGE_SIZE);
    if (has_interpreter) {
        u32(bytes+120,KERNEL_ELF64_PROGRAM_INTERPRETER);
        u64(bytes+128,192); u64(bytes+152,sizeof("/ld-layout"));
        memcpy(bytes+192,"/ld-layout",sizeof("/ld-layout"));
    }
    bytes[184]=payload;
    struct kernel_read_source reader;
    struct kernel_elf64_source *result=0;
    assert(!kernel_read_source_from_memory(bytes,256,&reader));
    assert(!kernel_elf64_source_create_reader(&heap,&reader,BOAROS_PAGE_SIZE,KERNEL_ELF64_MACHINE_RISCV,&result));
    return result;
}
static void build_case(uint16_t main_type,uint64_t main_address,uint16_t interpreter_type,
    uint64_t interpreter_address,int overlap)
{
    uint64_t before=physical_page_available(&pages);
    unsigned char main_bytes[256],interpreter_bytes[256];
    struct kernel_elf64_source *main=source(main_bytes,main_type,main_address,0x31,interpreter_type!=0);
    struct kernel_elf64_source *interpreter=interpreter_type?source(interpreter_bytes,interpreter_type,interpreter_address,0x72,0):0;
    struct kernel_exec_string name={"/layout",7};
    struct kernel_exec_image_request request={.executable_source=main,.interpreter_source=interpreter,
        .executable=name,.arguments=&name,.argument_count=1};
    struct kernel_exec_image image={0};
    enum kernel_elf_image_status result=kernel_elf_image_build(&request,&heap,&pages,&kernel,&image);
    if (overlap) assert(result==KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE && image.mm.state==KERNEL_MM_EMPTY);
    else {
        assert(result==KERNEL_ELF_IMAGE_STATUS_OK);
        assert(!kernel_mm_context(&image.mm,&mm_test_context));
        uint64_t vdso;
        assert(!kernel_mm_vdso_address(&image.mm,&vdso) && vdso==BOAROS_PAGE_SIZE);
        struct kernel_vma vma;
        assert(!kernel_mm_vma_lookup(&image.mm,vdso,&vma) && vma.role==KERNEL_VMA_ROLE_VDSO);
        assert(!kernel_mm_vma_lookup(&image.mm,image.entry,&vma) && vma.role==KERNEL_VMA_ROLE_ELF);
        assert(vma.backing==(interpreter?interpreter:main));
        if (interpreter_type==KERNEL_ELF64_TYPE_EXECUTABLE) assert(image.entry==interpreter_address+184);
        if (main_type==KERNEL_ELF64_TYPE_EXECUTABLE) {
            assert(!kernel_mm_vma_lookup(&image.mm,main_address,&vma) && vma.backing==main);
            unsigned char byte; size_t done;
            assert(!kernel_copy_from_user(&image.mm,&byte,main_address+184,1,&done) && done==1 && byte==0x31);
        }
        uint64_t stack[36]; size_t copied;
        assert(!kernel_copy_from_user(&image.mm,stack,image.stack_pointer,sizeof(stack),&copied) && copied==sizeof(stack));
        assert(stack[0]==1 && !stack[2] && !stack[3]);
        uint64_t main_entry=0,phdr=0,interpreter_bias=0,at_random=0;
        for (unsigned i=4;i+1<36 && stack[i];i+=2) {
            if (stack[i]==9) main_entry=stack[i+1];
            if (stack[i]==3) phdr=stack[i+1];
            if (stack[i]==7) interpreter_bias=stack[i+1];
            if (stack[i]==25) at_random=stack[i+1];
        }
        /* Unseeded early exec still supplies libc's mandatory 16-byte input. */
        unsigned char random[16],expected[16]={0};
        expected[0]=(unsigned char)(entropy_seed+entropy_calls);
        assert(at_random && !kernel_copy_from_user(&image.mm,random,at_random,sizeof(random),&copied) &&
            copied==sizeof(random) && !memcmp(random,expected,sizeof(random)));
        assert(main_entry && phdr==main_entry-184+64);
        assert(!kernel_mm_vma_lookup(&image.mm,main_entry,&vma) && vma.backing==main);
        if (interpreter) assert(interpreter_bias==image.entry-interpreter_address-184);
        else assert(!interpreter_bias && image.entry==main_entry);
        unsigned char main_payload; size_t done;
        assert(!kernel_copy_from_user(&image.mm,&main_payload,main_entry,1,&done) && done==1 && main_payload==0x31);
        unsigned char entry;
        assert(!kernel_copy_from_user(&image.mm,&entry,image.entry,1,&done) && done==1 && entry==(interpreter?0x72:0x31));
        mm_test_context=0;
        assert(!kernel_mm_release(&image.mm));
    }
    if (interpreter) assert(!kernel_elf64_source_release(&interpreter));
    assert(!kernel_elf64_source_release(&main));
    assert(physical_page_available(&pages)==before && !physical_page_allocator_audit(&pages));
    fprintf(stderr,"ELF actual build main=%u interp=%u overlap=%d: OK\n",main_type,interpreter_type,overlap);
}

static void personality_layout(void)
{
    uint64_t before=physical_page_available(&pages);
    unsigned char bytes[256];
    struct kernel_elf64_source *main_source=source(bytes,3,0x20000,0x31,0);
    struct kernel_exec_string name={"/layout",7};
    struct kernel_exec_image_request request={.executable_source=main_source,
        .executable=name,.arguments=&name,.argument_count=1,
        .personality=KERNEL_PERSONALITY_ADDR_NO_RANDOMIZE};
    uint64_t entries[2],stacks[2],heaps[2],mappings[2],vdsos[2];
    unsigned char random[2][16];
    for(unsigned pass=0;pass<2;pass++) {
        entropy_seed=pass+1;entropy_calls=0;
        struct kernel_exec_image image={0};
        assert(!kernel_elf_image_build(&request,&heap,&pages,&kernel,&image));
        assert(entropy_calls==1); /* AT_RANDOM survives disabled address randomization. */
        assert(!kernel_mm_context(&image.mm,&mm_test_context));
        entries[pass]=image.entry;stacks[pass]=image.stack_pointer;
        assert(!kernel_mm_brk(&image.mm,0,&heaps[pass]));
        assert(!kernel_mm_mmap_anonymous(&image.mm,0,BOAROS_PAGE_SIZE,KERNEL_MM_READ,0,&mappings[pass]));
        assert(!kernel_mm_vdso_address(&image.mm,&vdsos[pass]));
        uint64_t stack[40],at_random=0;size_t copied;
        assert(!kernel_copy_from_user(&image.mm,stack,image.stack_pointer,sizeof(stack),&copied)&&copied==sizeof(stack));
        for(unsigned i=4;i+1<40 && stack[i];i+=2)if(stack[i]==25)at_random=stack[i+1];
        assert(at_random && !kernel_copy_from_user(&image.mm,random[pass],at_random,16,&copied)&&copied==16);
        /* Per-VMA execute ceilings also apply when execute is implied. */
        uint64_t a=mappings[pass];
        assert(!kernel_mm_resolve_user_fault(&image.mm,a,KERNEL_MM_READ));
        assert(!kernel_mm_mprotect_personality(&image.mm,a,BOAROS_PAGE_SIZE,KERNEL_MM_READ,1));
        struct kernel_vma vma;struct kernel_mm_mapping mapping;
        assert(!kernel_mm_vma_lookup(&image.mm,a,&vma) && (vma.permissions & KERNEL_MM_EXECUTE));
        assert(!kernel_mm_lookup(&image.mm,a,&mapping) && (mapping.permissions & KERNEL_MM_EXECUTE));
        assert(!kernel_mm_mprotect(&image.mm,a,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
        assert(!kernel_mm_vma_lookup(&image.mm,a,&vma) && !(vma.permissions & KERNEL_MM_EXECUTE));
        assert(!kernel_mm_vma_insert_anon(&image.mm,a-BOAROS_PAGE_SIZE,a,KERNEL_MM_READ,
            KERNEL_VMA_ROLE_VVAR,KERNEL_VMA_FAULT_DEMAND_ZERO));
        assert(!kernel_mm_resolve_user_fault(&image.mm,a-BOAROS_PAGE_SIZE,KERNEL_MM_READ));
        assert(!kernel_mm_mprotect_personality(&image.mm,a-BOAROS_PAGE_SIZE,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ,1));
        assert(!kernel_mm_vma_lookup(&image.mm,a-BOAROS_PAGE_SIZE,&vma) && !(vma.permissions & KERNEL_MM_EXECUTE));
        assert(!kernel_mm_lookup(&image.mm,a-BOAROS_PAGE_SIZE,&mapping) && !(mapping.permissions & KERNEL_MM_EXECUTE));
        assert(!kernel_mm_vma_lookup(&image.mm,a,&vma) && (vma.permissions & KERNEL_MM_EXECUTE));
        assert(kernel_mm_mprotect_personality(&image.mm,a-BOAROS_PAGE_SIZE,2*BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_EXECUTE,1)==KERNEL_MM_STATUS_ACCESS);
        mm_test_context=0;assert(!kernel_mm_release(&image.mm));
    }
    assert(entries[0]==entries[1] && stacks[0]==stacks[1] && heaps[0]==heaps[1] &&
        mappings[0]==mappings[1] && vdsos[0]==vdsos[1] && memcmp(random[0],random[1],16));
    entropy_seed=0;
    assert(!kernel_elf64_source_release(&main_source));
    assert(physical_page_available(&pages)==before);
    puts("ELF personality: deterministic layouts, independent AT_RANDOM, implied execute VMA/PTE passed");
}

static void large_stack_layout(uint64_t limit)
{
    uint64_t before=physical_page_available(&pages);
    unsigned char main_bytes[256];
    struct kernel_elf64_source *main=source(main_bytes,2,0x40000000,0x31,0);
    struct kernel_exec_string name={"/layout",7};
    struct kernel_exec_image_request request={.executable_source=main,.executable=name,
        .arguments=&name,.argument_count=1,.stack_limit_valid=1,.stack_limit=limit};
    struct kernel_exec_image image={0};
    enum kernel_elf_image_status result=kernel_elf_image_build(&request,&heap,&pages,&kernel,&image);
    if(result) fprintf(stderr,"large stack limit=%llu image status=%d\n",(unsigned long long)limit,result);
    assert(!result);
    assert(!kernel_mm_context(&image.mm,&mm_test_context));
    struct kernel_vma stack;
    assert(!kernel_mm_vma_lookup(&image.mm,image.stack_pointer,&stack));
    assert(stack.role==KERNEL_VMA_ROLE_STACK && stack.end-stack.start<1024*1024);
    assert(!kernel_mm_resolve_user_fault(&image.mm,stack.end-9*1024*1024,KERNEL_MM_WRITE));
    uint64_t mapped;
    assert(!kernel_mm_mmap_anonymous(&image.mm,0,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,0,&mapped));
    if (limit<stack.end/2) assert(stack.end-mapped>=limit);
    assert(!kernel_mm_munmap(&image.mm,mapped,BOAROS_PAGE_SIZE));
    mm_test_context=0;
    assert(!kernel_mm_release(&image.mm));
    assert(!kernel_elf64_source_release(&main));
    assert(physical_page_available(&pages)==before && !physical_page_allocator_audit(&pages));
}

static void cleanup_transaction(void)
{
    uint64_t before=physical_page_available(&pages);
    struct kernel_vfs_mount *mount=NULL;
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));
    unsigned char bytes[256];
    struct kernel_elf64_source *reader=source(bytes,2,0x40000000,0x31,0);
    assert(!kernel_elf64_source_release(&reader));
    struct kernel_open_file_description *file=NULL;
    int error;
    size_t written;
    assert(!kernel_open_file_create_mode(&heap,mount,"/image",0700,&file,&error) && !error);
    assert(!kernel_vfs_pwrite(&file->file,0,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    struct kernel_exec_transaction transaction={.heap=&heap,.state=KERNEL_EXEC_TRANSACTION_PREPARING};
    transaction.retired_files.state=KERNEL_FILES_LIVE;
    transaction.cleanup_task=&files_closer;
    files_cleanup_failures=1;
    assert(!kernel_elf64_source_create(&heap,&file,BOAROS_PAGE_SIZE,KERNEL_ELF64_MACHINE_RISCV,
        &transaction.executable_source) && !file);
    assert(!kernel_open_file_create_mode(&heap,mount,"/other-owner",0700,
        &transaction.interpreter_file,&error) && !error);
    assert(!kernel_open_file_create_mode(&heap,mount,"/dirfd-owner",0700,
        &transaction.path_file,&error) && !error);
    struct kernel_exec_string name={"/image",6};
    struct kernel_exec_image_request request={.executable_source=transaction.executable_source,
        .executable=name,.arguments=&name,.argument_count=1};
    assert(!kernel_elf_image_build(&request,&heap,&pages,&kernel,&transaction.image));
    assert(!kernel_heap_allocate(&heap,32,(void **)&transaction.original_path));
    assert(!kernel_heap_allocate(&heap,64,(void **)&transaction.string_bytes));
    assert(!kernel_heap_allocate(&heap,32,(void **)&transaction.arguments));
    assert(!kernel_heap_allocate(&heap,32,(void **)&transaction.environment));

    /* 真实VFS关闭错误仍保留source/OFD；纯堆清理不参与重试。 */
    close_failures=3;
    assert(kernel_exec_transaction_cleanup(&transaction)==KERNEL_EXEC_STATUS_CLEANUP_REQUIRED);
    assert(!close_failures && transaction.executable_source && transaction.interpreter_file && transaction.path_file);
    assert(transaction.retired_files.state==KERNEL_FILES_CLEANUP);
    assert(transaction.image.mm.state==KERNEL_MM_RELEASED);
    assert(!transaction.original_path && !transaction.string_bytes &&
        !transaction.arguments && !transaction.environment);
    assert(!kernel_exec_transaction_cleanup(&transaction));
    assert(transaction.state==KERNEL_EXEC_TRANSACTION_EMPTY_CLEANUP &&
        !transaction.executable_source && !transaction.interpreter_file && !transaction.path_file);
    assert(transaction.retired_files.state==KERNEL_FILES_RELEASED);
    assert(!kernel_exec_transaction_cleanup(&transaction));
    assert(!kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats;
    kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==before);
    puts("exec actual cleanup: image released, failed source/OFD retained, heap owners cleared and retry completed");
}
static void rejected_disk_cache_entry(void)
{
    /* The shared VFS link closure must not turn excluded disk work into a
     * successful no-op. A native child keeps the expected abort isolated. */
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct rlimit limit={0,0};if(setrlimit(RLIMIT_CORE,&limit))_exit(2);
        kernel_page_cache_invalidate_range(NULL,NULL,0,1);
        _exit(1);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    puts("ELF fixture: excluded disk-cache invalidation abort verified");
}
int main(void)
{
    rejected_disk_cache_entry();
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) && !physical_page_allocator_bind_access(&pages,page_access) &&
        !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap,&pages,page_address));
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    /* An exec candidate owns its MM before any CPU can activate it. */
    kernel_cpu_current()->logical_id = 1;
    build_case(2,0x40000000,2,0x60000000,0);
    kernel_cpu_current()->logical_id = 0;
    build_case(2,0x40000000,2,0x40000000,1);
    build_case(2,0x40000000,0,0,0);
    build_case(3,0x20000,3,0x10000,0);
    large_stack_layout(UINT64_C(512)*1024*1024);
    large_stack_layout(UINT64_MAX-4096);
    large_stack_layout(UINT64_MAX);
    personality_layout();
    cleanup_transaction();
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    puts("ELF actual source/build/fault: disjoint ET_EXEC, overlap rejection, PIE/vDSO holes and final owners passed");
}
