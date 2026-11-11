/* Use the established actual files/VFS/MM fixture and its excluded unrelated
 * consumers. This entry runs only immutable metadata file behavior. */
#define main file_io_fixture_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/bpf.h>
#include "../../fs/bpf_fs.h"
static unsigned char btf_image[2*BOAROS_PAGE_SIZE+123];
int kernel_bpf_initialize(struct kernel_heap *heap){(void)heap;return 0;}
const void *kernel_bpf_kernel_btf(uint32_t *size){*size=sizeof(btf_image);return btf_image;}
struct bpf_object;
void bpf_object_get(struct bpf_object *o){(void)o;__builtin_trap();}
void bpf_object_put(struct bpf_object *o){(void)o;__builtin_trap();}
struct kernel_cpu *riscv_smp_cpu(unsigned cpu){return cpu?0:kernel_cpu_current();}
static void read_file(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    const char *path,const void *expected,size_t size)
{
    struct kernel_open_file_description *file=0;int error;int64_t result;size_t done;
    assert(!kernel_open_file_create(heap,mount,path,&file,&error)&&!error);
    struct kernel_files fds={0};assert(!kernel_files_create(&fds,heap));
    assert(!kernel_files_install_new_owned_at(&fds,0,0,&file));
    assert(!kernel_files_read(&fds,&mm,0,0x200000,size+19,&result)&&result==(int64_t)size);
    unsigned char actual[sizeof(btf_image)+32];
    assert(!kernel_copy_from_user(&mm,actual,0x200000,size,&done)&&done==size&&!memcmp(actual,expected,size));
    assert(!kernel_files_read(&fds,&mm,0,0x200000,1,&result)&&!result);
    assert(!kernel_files_pread(&fds,&mm,0,0x200000,3,size-1,&result)&&result==1);
    assert(!kernel_copy_from_user(&mm,actual,0x200000,1,&done)&&actual[0]==((const unsigned char *)expected)[size-1]);
    assert(!kernel_files_release(&fds));
}
static unsigned char mapped_byte(uint64_t address)
{unsigned char value;size_t n;assert(!kernel_copy_from_user(&mm,&value,address,1,&n)&&n==1);return value;}
static void mappings(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_open_file_description *file=0,*source;int error;uint64_t address;
    assert(!kernel_open_file_create(heap,mount,"/kernel/btf/vmlinux",&file,&error)&&!error);
    /* Even a privileged O_RDWR opener cannot make immutable metadata writable. */
    file->open_flags=2;source=file;assert(!kernel_open_file_acquire(source));
    assert(kernel_mm_mmap_file_private(&mm,&source,0x600000,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address)==KERNEL_MM_STATUS_SEALED);
    assert(source==file&&!kernel_open_file_release(&source));file->open_flags=0;
    source=file;assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x400000,4*BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address)&&!source);
    source=file;assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x500000,3*BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address)&&!source);
    assert(mapped_byte(0x400000)==btf_image[0]);
    assert(mapped_byte(0x500000)==btf_image[0]);
    unsigned char changed=0x5d;size_t done;
    assert(!kernel_copy_to_user(&mm,0x500000,&changed,1,&done)&&done==1);
    assert(mapped_byte(0x500000)==changed&&mapped_byte(0x400000)==btf_image[0]);
    assert(!kernel_open_file_release(&file));
    assert(kernel_vfs_unmount(mount)==-KERNEL_EBUSY);
    /* The descriptor is closed before these two source pages are faulted. */
    assert(mapped_byte(0x400000+BOAROS_PAGE_SIZE+17)==btf_image[BOAROS_PAGE_SIZE+17]);
    assert(mapped_byte(0x400000+sizeof(btf_image)-1)==btf_image[sizeof(btf_image)-1]);
    assert(mapped_byte(0x400000+sizeof(btf_image))==0);
    assert(kernel_mm_resolve_user_fault(&mm,0x400000+3*BOAROS_PAGE_SIZE,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(!kernel_mm_munmap(&mm,0x400000,4*BOAROS_PAGE_SIZE));
    assert(!kernel_mm_munmap(&mm,0x500000,3*BOAROS_PAGE_SIZE));
}
int main(int argc,char **argv)
{
    (void)argc;(void)argv;setvbuf(stdout,0,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    kernel_cpu_current()->state=KERNEL_CPU_ONLINE;
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));uint64_t baseline=physical_page_available(&pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel)&&!kernel_mm_create(&mm,&user)&&
        !kernel_mm_vma_enable(&mm,&heap)&&!kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    uint64_t address;assert(!kernel_mm_mmap_anonymous(&mm,0x200000,4*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
    for(size_t j=0;j<sizeof(btf_image);j++)btf_image[j]=(unsigned char)(j*13+7);
    struct kernel_vfs_mount *sysfs=0,*tracefs=0;
    assert(!kernel_sysfs_mount(&heap,0,&sysfs));assert(!kernel_tracefs_mount(&heap,0,&tracefs));
    read_file(&heap,sysfs,"/kernel/btf/vmlinux",btf_image,sizeof(btf_image));
    read_file(&heap,tracefs,"/events/raw_syscalls/sys_enter/id","1\n",2);
    puts("PASS metadata actual files read/pread/EOF and user copy");
    mappings(&heap,sysfs);
    assert(!kernel_vfs_unmount(tracefs));assert(!kernel_vfs_unmount(sysfs));
    mm_test_context=0;assert(!kernel_mm_release(&mm)&&!physical_page_release(&pages,kernel.root_address));
    mm_test_finish();struct kernel_heap_statistics statistics;kernel_heap_get_statistics(&heap,&statistics);
    assert(!statistics.live_allocations&&!statistics.current_pages&&physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("PASS metadata actual file mmap shared/private COW, close-before-cold-fault, EOF/SIGBUS, busy unmount and final owners");
    return 0;
}
