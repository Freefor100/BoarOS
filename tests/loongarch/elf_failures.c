#include <arch/elf.h>
#include <kernel/elf_image.h>
#include <kernel/heap.h>
#include <kernel/syscall.h>
#include <platform/loongarch_virt.h>
#include <string.h>
extern const unsigned char la_user_elf_start[],la_user_elf_end[];
static int fail_after=-1, fault_armed;
void la_fault_injection_set(int value) { fail_after=value; }
void la_fault_injection_arm(void) { fault_armed=1; }
enum kernel_syscall_status __real_kernel_syscall_dispatch(struct kernel_task *, const struct kernel_syscall_request *, struct kernel_syscall_result *);
enum kernel_syscall_status __wrap_kernel_syscall_dispatch(struct kernel_task *task,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *result)
{
    enum kernel_syscall_status status=__real_kernel_syscall_dispatch(task,request,result);
    if (fault_armed && request->number==222 && status==KERNEL_SYSCALL_STATUS_OK &&
        result->action==KERNEL_SYSCALL_ACTION_RETURN && result->value>=0) {
        fault_armed=0;fail_after=0;
    }
    return status;
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __real_physical_page_allocate_order(struct physical_page_allocator *,uint32_t,uint64_t *);
static int exhaust(void)
{ if (fail_after<0) return 0;if (!fail_after) return 1;fail_after--;return 0; }
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *p)
{ return exhaust() ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate(a,p); }
enum physical_page_status __wrap_physical_page_allocate_order(struct physical_page_allocator *a,uint32_t order,uint64_t *p)
{ return exhaust() ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate_order(a,order,p); }
static uint64_t read64(const unsigned char *p)
{ uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(i*8);return v; }
static void write64(unsigned char *p,uint64_t v)
{ for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(i*8)); }
void la_elf_failure_contract(struct kernel_heap *heap)
{
    uint64_t before=physical_page_available(heap->page_allocator);
    size_t size=la_user_elf_end-la_user_elf_start;
    unsigned char *copy;
    if (kernel_heap_allocate(heap,size,(void **)&copy)!=KERNEL_HEAP_STATUS_OK) la_virt_fatal("ELF error buffer");
    struct kernel_read_source reader;
    for (unsigned kind=0;kind<3;kind++) {
        memcpy(copy,la_user_elf_start,size);
        if (!kind) copy[0]=0;
        else if (kind==1) {copy[18]=0xf3;copy[19]=0;}
        else { uint64_t ph=read64(copy+32);write64(copy+ph+56+16,read64(copy+ph+16)); }
        kernel_read_source_from_memory(copy,size,&reader);
        struct kernel_elf64_source *source=0;
        enum kernel_elf64_source_status status=kernel_elf64_source_create_reader(heap,&reader,BOAROS_PAGE_SIZE,ARCH_ELF_MACHINE,&source);
        enum kernel_elf64_source_status expected=kind==1 ? KERNEL_ELF64_SOURCE_STATUS_WRONG_ARCH : KERNEL_ELF64_SOURCE_STATUS_MALFORMED;
        if (status!=expected || source) la_virt_fatal("ELF rejection owner");
    }
    kernel_heap_release(heap,copy);
    if (physical_page_available(heap->page_allocator)!=before) la_virt_fatal("ELF rejection recovery");
    unsigned success=0, rejected=0;
    kernel_read_source_from_memory(la_user_elf_start,size,&reader);
    for (unsigned failure=0;failure<128;failure++) {
        struct kernel_elf64_source *source=0;struct kernel_exec_image image={0};
        fail_after=(int)failure;
        enum kernel_elf64_source_status status=kernel_elf64_source_create_reader(heap,&reader,BOAROS_PAGE_SIZE,ARCH_ELF_MACHINE,&source);
        enum kernel_exec_image_status result=KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
        int64_t error=-12;
        if (status==KERNEL_ELF64_SOURCE_STATUS_OK) {
            struct kernel_exec_string arg={"/la-probe",9};
            struct kernel_exec_image_request request={.executable_source=source,.executable=arg,.arguments=&arg,.argument_count=1};
            result=kernel_exec_image_prepare(&request,heap,&image,&error);
            if (result!=KERNEL_EXEC_IMAGE_STATUS_OK &&
                (result!=KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR || error!=-12)) la_virt_fatal("ELF OOM status");
        } else if (status!=KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY || source) la_virt_fatal("source OOM status");
        fail_after=-1;
        if (kernel_exec_image_cleanup(heap,&image)!=KERNEL_EXEC_IMAGE_STATUS_OK) la_virt_fatal("ELF OOM cleanup");
        if (source && kernel_elf64_source_release(&source)!=KERNEL_ELF64_SOURCE_STATUS_OK) la_virt_fatal("ELF OOM source");
        if (physical_page_available(heap->page_allocator)!=before) la_virt_fatal("ELF OOM recovery");
        if (status==KERNEL_ELF64_SOURCE_STATUS_OK && result==KERNEL_EXEC_IMAGE_STATUS_OK) {success=1;break;}
        rejected++;
    }
    if (!success || !rejected) la_virt_fatal("ELF OOM coverage");
    la_virt_puts("LA ELF rejection/OOM recovery passed; exhausted boundaries=");la_virt_hex(rejected);la_virt_puts("\n");
}
