#define _POSIX_C_SOURCE 200809L
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
static unsigned char pool[8*1024*1024] __attribute__((aligned(16384)));
static unsigned instruction_fences;
void kernel_console_putc(char c){fputc(c,stderr);}
void mm_test_unavailable(void){abort();}
void mm_test_fence(uint64_t address,int instructions)
{(void)address;instruction_fences+=!!instructions;__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static void *access_page(uint64_t p){return (void *)(uintptr_t)p;}
static int address_of(const void *p,uint64_t *a){*a=(uintptr_t)p;return 1;}
static pthread_barrier_t core_barrier;
static void rendezvous(void)
{ int rc=pthread_barrier_wait(&core_barrier);assert(!rc||rc==PTHREAD_BARRIER_SERIAL_THREAD); }
static void *remote_borrower(void *opaque)
{
    struct kernel_mm *mm=opaque;
    assert(kernel_mm_core_remote_begin(mm));rendezvous();rendezvous();
    kernel_mm_core_remote_end(mm);rendezvous();rendezvous();
    assert(!kernel_mm_core_remote_begin(mm));rendezvous();return NULL;
}
struct shared_writer { struct kernel_mm *mm; uint64_t address; };
static void *write_shared_alias(void *opaque)
{
    struct shared_writer *writer=opaque;
    mm_test_attach(1);
    assert(!kernel_mm_context(writer->mm,&mm_test_context));
    rendezvous();
    /* A different MM can still write the shared physical storage. The
     * target's dump gate is not a lock on every alias of that storage. */
    assert(kernel_mm_core_remote_begin(writer->mm));
    unsigned char value=22;size_t copied;
    assert(!kernel_mm_remote_transfer(writer->mm,writer->address,&value,1,1,0,&copied)&&copied==1);
    assert(!kernel_mm_remote_transfer(writer->mm,writer->address+BOAROS_PAGE_SIZE,&value,1,1,0,&copied)&&copied==1);
    kernel_mm_core_remote_end(writer->mm);
    rendezvous();mm_test_context=0;return NULL;
}
int main(void)
{
    mm_test_attach(0);
    struct physical_page_allocator allocator={0};
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)pool,sizeof(pool)}}};
    struct arch_mmu_page_table table={0};struct arch_mmu_user_space space={0};
    struct kernel_heap heap={0};struct kernel_mm mm={0},child={0},owner={0};
    assert(!physical_page_allocator_init(&allocator,&layout));
    assert(!physical_page_allocator_bind_access(&allocator,access_page));
    assert(!physical_page_allocator_finalize(&allocator));
    assert(!kernel_heap_init(&heap,&allocator,address_of));
    assert(!riscv_sv39_page_table_init(&table,&allocator));table.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&allocator);
    assert(!arch_mmu_user_space_init(&space,&allocator,&table));assert(!kernel_mm_create(&mm,&space));
    assert(!kernel_mm_vma_enable(&mm,&heap));assert(!kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    uint64_t address,context;assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    assert(!kernel_mm_mmap_anonymous(&mm,0x40000,2*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
    pthread_t borrower;assert(!pthread_barrier_init(&core_barrier,NULL,2));
    assert(!pthread_create(&borrower,NULL,remote_borrower,&mm));
    rendezvous();assert(!kernel_mm_core_begin(&mm));rendezvous();rendezvous();
    assert(kernel_mm_core_begin(&mm));rendezvous();rendezvous();
    assert(!pthread_join(borrower,NULL));kernel_mm_core_end(&mm);
    assert(!pthread_barrier_destroy(&core_barrier));
    assert(kernel_mm_core_remote_begin(&mm));
    assert(!kernel_mm_core_begin(&mm));
    kernel_mm_core_remote_end(&mm);
    assert(kernel_mm_core_begin(&mm));
    assert(!kernel_mm_core_remote_begin(&mm));
    assert(!kernel_mm_core_begin(&mm));
    kernel_mm_core_end(&mm);
    assert(kernel_mm_core_remote_begin(&mm));
    kernel_mm_core_remote_end(&mm);
    puts("MM core excludes active/new remote borrowers and releases admission");
    unsigned char value=41,got=0;size_t copied;
    assert(!kernel_mm_remote_transfer(&mm,address,&value,1,1,0,&copied)&&copied==1);
    struct kernel_mm_user_pin pending_write={0};
    assert(!kernel_mm_pin_user_page(&mm,address,KERNEL_MM_WRITE,&pending_write));
    assert(!kernel_mm_core_begin(&mm));
    kernel_mm_unpin_user_page(&pending_write);
    assert(kernel_mm_core_begin(&mm));kernel_mm_core_end(&mm);
    assert(!kernel_mm_mprotect(&mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_EXECUTE));
    uint64_t shared_address;
    assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&shared_address));
    unsigned char initial=11;
    assert(!kernel_mm_remote_transfer(&mm,shared_address,&initial,1,1,0,&copied)&&copied==1);
    assert(!kernel_mm_remote_transfer(&mm,shared_address+BOAROS_PAGE_SIZE,&initial,1,1,0,&copied)&&copied==1);
    assert(!kernel_mm_fork(&child,&mm));
    assert(mm.record!=child.record);
    assert(kernel_mm_core_begin(&mm));
    unsigned char first=0,last=0;
    assert(!kernel_mm_remote_transfer(&mm,shared_address,&first,1,0,1,&copied)&&copied==1);
    struct shared_writer writer={&child,shared_address};
    assert(!pthread_barrier_init(&core_barrier,NULL,2));
    assert(!pthread_create(&borrower,NULL,write_shared_alias,&writer));
    rendezvous();rendezvous();
    assert(!kernel_mm_remote_transfer(&mm,shared_address+BOAROS_PAGE_SIZE,&last,1,0,1,&copied)&&copied==1);
    assert(!pthread_join(borrower,NULL));assert(!pthread_barrier_destroy(&core_barrier));
    unsigned char first_after=0;
    assert(!kernel_mm_remote_transfer(&mm,shared_address,&first_after,1,0,1,&copied)&&copied==1);
    assert(first_after==22); /* Both pages really alias the writer's MM. */
    assert(first==11&&last==22); /* This pair never existed simultaneously. */
    assert(!kernel_mm_core_remote_begin(&mm));
    kernel_mm_core_end(&mm);
    puts("MM core shared aliases remain live; sequential copies are not a global snapshot");
    assert(kernel_mm_remote_transfer(&child,address,&value,1,1,0,&copied)==-KERNEL_EFAULT&&!copied);
    unsigned fences=instruction_fences;value=73;
    assert(!kernel_mm_remote_transfer(&child,address,&value,1,1,1,&copied)&&copied==1);
    assert(instruction_fences>fences);
    struct kernel_mm_mapping mapping;assert(!kernel_mm_lookup(&child,address,&mapping));
    assert((mapping.permissions&(KERNEL_MM_READ|KERNEL_MM_WRITE|KERNEL_MM_EXECUTE))==(KERNEL_MM_READ|KERNEL_MM_EXECUTE));
    assert(!kernel_mm_remote_transfer(&child,address,&got,1,0,0,&copied)&&got==73);
    assert(!kernel_mm_remote_transfer(&mm,address,&got,1,0,0,&copied)&&got==41);
    puts("MM remote readonly private COW and instruction publication verified");
    assert(!kernel_mm_mprotect(&child,address,BOAROS_PAGE_SIZE,0));
    assert(!kernel_mm_remote_transfer(&child,address,&got,1,0,1,&copied)&&got==73);
    value=82;assert(!kernel_mm_remote_transfer(&child,address,&value,1,1,1,&copied));
    assert(kernel_mm_lookup(&child,address,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(kernel_mm_remote_transfer(&child,address,&got,1,0,0,&copied)==-KERNEL_EFAULT&&!copied);
    assert(!kernel_mm_mprotect(&child,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    assert(!kernel_mm_remote_transfer(&child,address,&got,1,0,0,&copied)&&got==82);
    assert(!kernel_mm_mmap_anonymous(&mm,0x80000,BOAROS_PAGE_SIZE,0,KERNEL_MM_MAP_FIXED,&address));
    assert(!kernel_mm_remote_transfer(&mm,address,&got,1,0,1,&copied)&&!got);
    value=97;assert(!kernel_mm_remote_transfer(&mm,address,&value,1,1,1,&copied));
    assert(!kernel_mm_remote_transfer(&mm,address,&got,1,0,1,&copied)&&got==97);
    puts("MM remote resident and cold PROT_NONE retain hardware protection");
    assert(!kernel_mm_mmap_anonymous(&mm,0xc0000,BOAROS_PAGE_SIZE,KERNEL_MM_READ,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address));
    assert(kernel_mm_remote_transfer(&mm,address,&value,1,1,1,&copied)==-KERNEL_EFAULT&&!copied);
    unsigned char bytes[8]={0};
    assert(kernel_mm_remote_transfer(&mm,0x40000+2*BOAROS_PAGE_SIZE-4,bytes,8,0,0,&copied)==-KERNEL_EFAULT&&copied==4);
    assert(!kernel_io_context_current()->user_pins);
    assert(!kernel_mm_acquire(&owner,&mm));mm_test_context=0;assert(!kernel_mm_release(&mm));
    assert(!kernel_mm_remote_transfer(&owner,0x40000,&got,1,0,0,&copied)&&got==41);
    assert(!kernel_mm_release(&child)&&!kernel_mm_release(&owner));
    assert(physical_page_available(&allocator)==baseline&&!physical_page_allocator_audit(&allocator));
    puts("MM remote shared denial, exact prefix, old MM owner and resource return verified");
}
