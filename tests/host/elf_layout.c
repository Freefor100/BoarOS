#include <kernel/elf_image.h>
#include <kernel/mm_backend.h>
#include <kernel/cpu.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/random.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
void mm_test_attach(unsigned);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[8*1024*1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct arch_mmu_page_table kernel;
__asm__(".pushsection .rodata\n.global arch_elf_trampoline_start,arch_elf_trampoline_end\narch_elf_trampoline_start:\n.byte 0x73,0,0,0\narch_elf_trampoline_end:\n.popsection");
int kernel_random_available(void) { return 0; }
enum kernel_random_status kernel_random_fill(void *buffer,size_t size)
{ (void)buffer; (void)size; abort(); }
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
        uint64_t main_entry=0,phdr=0,interpreter_bias=0;
        for (unsigned i=4;i+1<36 && stack[i];i+=2) {
            if (stack[i]==9) main_entry=stack[i+1];
            if (stack[i]==3) phdr=stack[i+1];
            if (stack[i]==7) interpreter_bias=stack[i+1];
        }
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
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) && !physical_page_allocator_bind_access(&pages,page_access) &&
        !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap,&pages,page_address));
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    build_case(2,0x40000000,2,0x60000000,0);
    build_case(2,0x40000000,2,0x40000000,1);
    build_case(2,0x40000000,0,0,0);
    build_case(3,0x20000,3,0x10000,0);
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    puts("ELF actual source/build/fault: disjoint ET_EXEC, overlap rejection, PIE/vDSO holes and final owners passed");
}
