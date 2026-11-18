#include <arch/mmu.h>
#include <arch/context.h>
#include <arch/task.h>
#include <arch/loongarch/smp.h>
#include <platform/loongarch_virt.h>
#include <string.h>
#define ENTRIES (BOAROS_PAGE_SIZE/8)
#define PTE_VALID UINT64_C(1)
#define PTE_DIRTY UINT64_C(2)
#define PTE_HUGE (UINT64_C(1)<<6)
#define HUGE_ORDER 11U
#define HUGE_SIZE (UINT64_C(1)<<25)
#define PTE_PRESENT (UINT64_C(1)<<7)
#define PTE_WRITE (UINT64_C(1)<<8)
#define PTE_COW (UINT64_C(1)<<9)
#define PTE_NONE (UINT64_C(1)<<10)
#define PTE_NR (UINT64_C(1)<<61)
#define PTE_NX (UINT64_C(1)<<62)
#define PTE_ADDRESS UINT64_C(0x0000ffffffffc000)
extern void la_tlb_refill(void);
void arch_mmu_sync_instructions(void) { __asm__ volatile("ibar 0" ::: "memory"); }
void arch_mmu_flush_address(uint64_t address)
{ __asm__ volatile("invtlb 0x5, $zero, %0" :: "r"(address) : "memory"); }
static void resident_peak(struct arch_mmu_user_space *space)
{
    uint64_t pages = (uint64_t)space->leaf_pages + space->protected_pages;
    if (space->peak_resident_pages < pages) space->peak_resident_pages = pages;
}

static void flush_all(void) { __asm__ volatile("invtlb 0, $zero, $zero" ::: "memory"); }
void arch_mmu_flush_all(void) { flush_all(); }
void arch_mmu_sync_user(uint32_t targets, uint64_t address, uint64_t size,
    int instructions)
{
#if BOAROS_MAX_CPUS > 1
    la_smp_sync_user(targets,address,size,instructions);
#else
    if (targets & ~UINT32_C(1)) la_virt_fatal("unsupported MM CPU target");
    if (size == BOAROS_PAGE_SIZE) arch_mmu_flush_address(address);
    else flush_all();
    if (instructions) arch_mmu_sync_instructions();
#endif
}
uint64_t arch_mmu_current_context(void)
{ uint64_t v; __asm__ volatile("csrrd %0, 0x19" : "=r"(v)); return v; }
enum arch_mmu_status arch_mmu_switch_context(uint64_t root)
{
    if (root & BOAROS_PAGE_MASK || root > PTE_ADDRESS) return ARCH_MMU_STATUS_INVALID;
    if (root == arch_mmu_current_context()) return ARCH_MMU_STATUS_OK;
    __asm__ volatile("dbar 0\ncsrwr %0, 0x19" : "+r"(root) :: "memory");
    flush_all(); return ARCH_MMU_STATUS_OK;
}
void la_mmu_initialize(void)
{
    /* 16 KiB 页、每级 11 位索引：页内/叶/中间/根位移为 14/25/36。 */
    uint64_t low=14 | (UINT64_C(11)<<5) | (UINT64_C(25)<<10) | (UINT64_C(11)<<15);
    uint64_t high=36 | (UINT64_C(11)<<6), page=14;
    uint64_t entry=(uintptr_t)la_tlb_refill-LA_DIRECT_BASE;
    __asm__ volatile("csrwr %0, 0x1c\ncsrwr %1, 0x1d\ncsrwr %2, 0x88\ncsrwr %3, 0x1e"
        : "+r"(low), "+r"(high), "+r"(entry), "+r"(page) :: "memory");
    arch_mmu_switch_context(0);
}
static struct arch_mmu_user_space kernel_window;
int arch_mmu_kernel_window_active(void) { return kernel_window.root_address!=0; }
static uint64_t *table(struct arch_mmu_user_space *s, uint64_t address)
{
    void *p;
    if (!address || (address&BOAROS_PAGE_MASK) || physical_page_map_owned(s->allocator,address,&p)!=PHYSICAL_PAGE_STATUS_OK)
        la_virt_fatal("page table owner");
    return p;
}
static int live(const struct arch_mmu_user_space *s)
{ return s && s->state==ARCH_MMU_USER_SPACE_LIVE && s->allocator && s->root_address && s->table_pages; }
static enum arch_mmu_status allocate_table(struct arch_mmu_user_space *s, uint64_t *address)
{
    if (physical_page_allocate(s->allocator,address)!=PHYSICAL_PAGE_STATUS_OK) return ARCH_MMU_STATUS_NO_MEMORY;
    memset(table(s,*address),0,BOAROS_PAGE_SIZE); s->table_pages++; return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status la_mmu_kernel_window_initialize(struct physical_page_allocator *a)
{
    if(!a)return ARCH_MMU_STATUS_INVALID;
    if(kernel_window.root_address)return ARCH_MMU_STATUS_STATE;
    struct arch_mmu_user_space candidate={.allocator=a,.state=ARCH_MMU_USER_SPACE_LIVE};
    enum arch_mmu_status status=allocate_table(&candidate,&candidate.root_address);
    uint64_t middle=0;
    if(status==ARCH_MMU_STATUS_OK)status=allocate_table(&candidate,&middle);
    if(status==ARCH_MMU_STATUS_OK) {
        table(&candidate,candidate.root_address)[(ARCH_KERNEL_STACK_WINDOW_BASE>>36)&(ENTRIES-1)]=middle;
        for(uint64_t offset=0;offset<ARCH_KERNEL_STACK_WINDOW_SIZE;offset+=UINT64_C(1)<<25) {
            uint64_t child;
            status=allocate_table(&candidate,&child);
            if(status!=ARCH_MMU_STATUS_OK)break;
            table(&candidate,middle)[((ARCH_KERNEL_STACK_WINDOW_BASE+offset)>>25)&(ENTRIES-1)]=child;
        }
    }
    if(status!=ARCH_MMU_STATUS_OK) {
        if(middle) {
            uint64_t *entries=table(&candidate,middle);
            for(unsigned i=0;i<ENTRIES;i++)if(entries[i])
                (void)physical_page_release(a,entries[i]);
            (void)physical_page_release(a,middle);
        }
        if(candidate.root_address)(void)physical_page_release(a,candidate.root_address);
        return status;
    }
    /* 骨架属于永久 boot owner；叶只借用任务栈页，不转移物理页引用。 */
    kernel_window=candidate;
    uint64_t root=candidate.root_address;
    __asm__ volatile("dbar 0;csrwr %0,0x1a":"+r"(root)::"memory");
    flush_all();return ARCH_MMU_STATUS_OK;
}
static enum arch_mmu_status kernel_slot(struct physical_page_allocator *a,uint64_t v,uint64_t **slot)
{
    if(!kernel_window.root_address || a!=kernel_window.allocator)return ARCH_MMU_STATUS_STATE;
    if(v<ARCH_KERNEL_STACK_WINDOW_BASE || v-ARCH_KERNEL_STACK_WINDOW_BASE>=ARCH_KERNEL_STACK_WINDOW_SIZE ||
       (v&BOAROS_PAGE_MASK) || (v-ARCH_KERNEL_STACK_WINDOW_BASE)%ARCH_KERNEL_STACK_SLOT_SIZE<BOAROS_PAGE_SIZE)
        return ARCH_MMU_STATUS_INVALID;
    uint64_t root=table(&kernel_window,kernel_window.root_address)[(v>>36)&(ENTRIES-1)];
    uint64_t leaf=table(&kernel_window,root)[(v>>25)&(ENTRIES-1)];
    *slot=&table(&kernel_window,leaf)[(v>>14)&(ENTRIES-1)];return ARCH_MMU_STATUS_OK;
}
#if BOAROS_MAX_CPUS > 1
void la_mmu_secondary_initialize(void)
{
    la_mmu_initialize();
    uint64_t root=kernel_window.root_address;
    if(!root)la_virt_fatal("secondary kernel page tables");
    __asm__ volatile("csrwr %0,0x1a" : "+r"(root) :: "memory");
    flush_all();
}
#endif
enum arch_mmu_status arch_mmu_kernel_window_map(struct physical_page_allocator *a,uint64_t v,uint64_t p)
{
    uint64_t *slot;void *bytes;
    enum arch_mmu_status status=kernel_slot(a,v,&slot);
    if(status!=ARCH_MMU_STATUS_OK)return status;
    if((p&BOAROS_PAGE_MASK) || p>PTE_ADDRESS || physical_page_map_owned(a,p,&bytes)!=PHYSICAL_PAGE_STATUS_OK)return ARCH_MMU_STATUS_INVALID;
    if(*slot)return ARCH_MMU_STATUS_CONFLICT;
    *slot=p|PTE_PRESENT|PTE_VALID|PTE_DIRTY|PTE_WRITE|PTE_NX|0x10;
    __asm__ volatile("dbar 0":::"memory");
#if BOAROS_MAX_CPUS > 1
    la_smp_sync_kernel(v,BOAROS_PAGE_SIZE);
#else
    arch_mmu_flush_address(v);
#endif
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_kernel_window_unmap(struct physical_page_allocator *a,uint64_t v)
{
    uint64_t *slot;
    enum arch_mmu_status status=kernel_slot(a,v,&slot);
    if(status!=ARCH_MMU_STATUS_OK)return status;
    *slot=0;
    __asm__ volatile("dbar 0":::"memory");
#if BOAROS_MAX_CPUS > 1
    la_smp_sync_kernel(v,BOAROS_PAGE_SIZE);
#else
    arch_mmu_flush_address(v);
#endif
    return ARCH_MMU_STATUS_OK;
}
static uint64_t encode(uint64_t physical, uint32_t permissions, int cow)
{
    uint64_t pte=physical|PTE_PRESENT|0x1c;
    /* PROT_NONE 只撤硬件权限，不能丢掉后续恢复 RW 所需的 COW owner。 */
    if (cow) pte|=PTE_COW;
    if (!permissions) return pte|PTE_NONE;
    pte|=PTE_VALID;
    /* Linux LA 对非 NONE 用户页使用可读 PTE；冷页仍按请求 VMA 权限 fault。 */
    if (!(permissions&ARCH_MMU_EXECUTE)) pte|=PTE_NX;
    if (permissions&ARCH_MMU_WRITE) pte|=PTE_WRITE;
    if (!cow && (permissions&ARCH_MMU_WRITE)) pte|=PTE_DIRTY;
    return pte;
}
static int supported_order(uint32_t order) { return !order || order == HUGE_ORDER; }
static uint64_t leaf_size(uint32_t order) { return (uint64_t)BOAROS_PAGE_SIZE << order; }
static int directory_entry(uint64_t entry)
{ return entry && !(entry & ~PTE_ADDRESS); }
static uint32_t leaf_permissions(uint64_t entry)
{
    if (entry & PTE_NONE) return 0;
    uint32_t permissions = ARCH_MMU_USER;
    if (!(entry & PTE_NR)) permissions |= ARCH_MMU_READ;
    if ((entry & PTE_DIRTY) && !(entry & PTE_COW)) permissions |= ARCH_MMU_WRITE;
    if (!(entry & PTE_NX)) permissions |= ARCH_MMU_EXECUTE;
    return permissions;
}
static enum arch_mmu_status walk_order(struct arch_mmu_user_space *s, uint64_t v,
    uint32_t order, int create, uint64_t **slot)
{
    if (!live(s) || v >= ARCH_MMU_USER_LIMIT || !supported_order(order))
        return ARCH_MMU_STATUS_INVALID;
    uint64_t *current = table(s, s->root_address);
    unsigned stop = order ? 1 : 0;
    for (unsigned level = 2; level > stop; level--) {
        unsigned index = (v >> (14 + level * 11)) & (ENTRIES - 1);
        if (!current[index]) {
            if (!create) return ARCH_MMU_STATUS_NOT_MAPPED;
            uint64_t physical;
            enum arch_mmu_status status = allocate_table(s, &physical);
            if (status) return status;
            __atomic_store_n(&current[index], physical, __ATOMIC_RELEASE);
        }
        if (!directory_entry(current[index]))
            return level == 1 && (current[index] & PTE_HUGE) ?
                ARCH_MMU_STATUS_CONFLICT : ARCH_MMU_STATUS_STATE;
        current = table(s, current[index]);
    }
    *slot = &current[(v >> (14 + stop * 11)) & (ENTRIES - 1)];
    if (order && **slot && !(**slot & PTE_HUGE)) return ARCH_MMU_STATUS_CONFLICT;
    return ARCH_MMU_STATUS_OK;
}
static enum arch_mmu_status walk(struct arch_mmu_user_space *s, uint64_t v,
    int create, uint64_t **slot)
{ return walk_order(s, v, 0, create, slot); }
static enum arch_mmu_status walk_leaf(const struct arch_mmu_user_space *space,
    uint64_t v, uint64_t **slot, uint32_t *order)
{
    struct arch_mmu_user_space *s = (struct arch_mmu_user_space *)space;
    if (!live(s) || v >= ARCH_MMU_USER_LIMIT) return ARCH_MMU_STATUS_INVALID;
    uint64_t *current = table(s, s->root_address);
    for (unsigned level = 2;; level--) {
        uint64_t *entry = &current[(v >> (14 + level * 11)) & (ENTRIES - 1)];
        if (!*entry) { *slot=entry;*order=level*HUGE_ORDER;return ARCH_MMU_STATUS_NOT_MAPPED; }
        if (!level || (level == 1 && (*entry & PTE_HUGE))) {
            uint32_t found_order = level ? HUGE_ORDER : 0;
            if (!(*entry & PTE_PRESENT) || (!level && (*entry & PTE_HUGE)) ||
                ((*entry & PTE_ADDRESS) & (leaf_size(found_order) - 1)))
                return ARCH_MMU_STATUS_STATE;
            *slot = entry; *order = found_order; return ARCH_MMU_STATUS_OK;
        }
        if (!directory_entry(*entry)) return ARCH_MMU_STATUS_STATE;
        current = table(s, *entry);
    }
}
enum arch_mmu_status arch_mmu_user_leaf_snapshot(const struct arch_mmu_user_space *s,
    uint64_t v, struct kernel_mm_leaf *out)
{
    if (!out) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot; uint32_t order;
    enum arch_mmu_status status = walk_leaf(s, v, &slot, &order);
    if (status) return status;
    *out = (struct kernel_mm_leaf){
        .virtual_address = v & ~(leaf_size(order) - 1),
        .physical_address = *slot & PTE_ADDRESS, .cookie = *slot,
        .permissions = leaf_permissions(*slot), .order = order,
        .cow = !!(*slot & PTE_COW)};
    return ARCH_MMU_STATUS_OK;
}

enum arch_mmu_status arch_mmu_user_empty_window(const struct arch_mmu_user_space *s,
    uint64_t address, unsigned count, unsigned *empty)
{
    const uint64_t span=UINT64_C(1)<<25;
    if (!empty || !count || count>32 || (address&BOAROS_PAGE_MASK) ||
        count>(span-(address&(span-1)))/BOAROS_PAGE_SIZE) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot;
    uint32_t order;
    enum arch_mmu_status status=walk_leaf(s,address,&slot,&order);
    if (status && status!=ARCH_MMU_STATUS_NOT_MAPPED) return status;
    unsigned mask=0;
    if (order) mask=status ? UINT32_MAX>>(32-count) : 0;
    else for (unsigned i=0;i<count;i++) if (!slot[i]) mask|=1U<<i;
    *empty=mask;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_leaf_missing_order(struct arch_mmu_user_space *s,
    uint64_t v, uint32_t order, unsigned *mask)
{
    if (!live(s) || !mask || !supported_order(order) ||
        (v & (leaf_size(order) - 1)) || v < BOAROS_PAGE_SIZE ||
        v >= ARCH_MMU_USER_LIMIT) return ARCH_MMU_STATUS_INVALID;
    uint64_t middle = table(s, s->root_address)[(v >> 36) & (ENTRIES - 1)];
    if (!middle) { *mask = order ? 1 : 3; return ARCH_MMU_STATUS_OK; }
    if (!directory_entry(middle)) return ARCH_MMU_STATUS_STATE;
    uint64_t leaf = table(s, middle)[(v >> 25) & (ENTRIES - 1)];
    if (order) {
        if (leaf && !(leaf & PTE_HUGE)) return ARCH_MMU_STATUS_CONFLICT;
        *mask = 0;
    } else {
        if (leaf && !directory_entry(leaf)) return ARCH_MMU_STATUS_CONFLICT;
        *mask = leaf ? 0 : 2;
    }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_leaf_missing(struct arch_mmu_user_space *s,
    uint64_t v, unsigned *mask)
{ return arch_mmu_user_leaf_missing_order(s, v, 0, mask); }
static enum arch_mmu_status install_prepared_leaf(struct arch_mmu_user_space *s,
    uint64_t v, uint64_t entry, uint32_t order, struct kernel_mm_leaf_preparation *prepared)
{
    if (!live(s) || !prepared || !supported_order(order) ||
        (v & (leaf_size(order) - 1)) || v < BOAROS_PAGE_SIZE ||
        v >= ARCH_MMU_USER_LIMIT) return ARCH_MMU_STATUS_INVALID;
    uint64_t *root = table(s, s->root_address);
    unsigned r = (v >> 36) & (ENTRIES - 1), m = (v >> 25) & (ENTRIES - 1);
    uint64_t middle_address=root[r];
    if (middle_address && !directory_entry(middle_address)) return ARCH_MMU_STATUS_STATE;
    unsigned missing=middle_address ? 0 : 1;
    if (!middle_address) middle_address=prepared->tables[0];
    if (middle_address == KERNEL_MM_NO_PAGE) {
        prepared->table_mask=order ? 1 : 3;
        return ARCH_MMU_STATUS_NEED_TABLES;
    }
    if (!middle_address) return ARCH_MMU_STATUS_INVALID;
    uint32_t actual;
    if ((missing & 1) && (physical_page_allocation_order(s->allocator, middle_address, &actual) || actual))
        return ARCH_MMU_STATUS_INVALID;
    uint64_t *middle = table(s, middle_address), *slot;
    uint64_t leaf_address = 0;
    if (order) slot = &middle[m];
    else {
        leaf_address=middle[m];
        if (leaf_address && !directory_entry(leaf_address)) return ARCH_MMU_STATUS_CONFLICT;
        if (!leaf_address) { missing|=2;leaf_address=prepared->tables[1]; }
        if (leaf_address == KERNEL_MM_NO_PAGE) {
            prepared->table_mask=missing;
            return ARCH_MMU_STATUS_NEED_TABLES;
        }
        if (!leaf_address || leaf_address == middle_address)
            return ARCH_MMU_STATUS_INVALID;
        if ((missing & 2) && (physical_page_allocation_order(s->allocator, leaf_address, &actual) || actual))
            return ARCH_MMU_STATUS_INVALID;
        slot = &table(s, leaf_address)[(v >> 14) & (ENTRIES - 1)];
    }
    if (*slot) return ARCH_MMU_STATUS_CONFLICT;
    unsigned extra = ((missing & 1) != 0) + ((missing & 2) != 0);
    if (s->table_pages > UINT32_MAX - extra) return ARCH_MMU_STATUS_STATE;
    __atomic_store_n(slot, entry, __ATOMIC_RELEASE);
    if (missing & 2) {
        __atomic_store_n(&middle[m], leaf_address, __ATOMIC_RELEASE);
        prepared->tables[1] = KERNEL_MM_NO_PAGE;
    }
    if (missing & 1) {
        __atomic_store_n(&root[r], middle_address, __ATOMIC_RELEASE);
        prepared->tables[0] = KERNEL_MM_NO_PAGE;
    }
    s->table_pages += extra;
    prepared->table_mask=missing;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_prepare_move(struct arch_mmu_user_space *s,
    uint64_t v, struct kernel_mm_leaf_preparation *prepared)
{ return install_prepared_leaf(s, v, 0, 0, prepared); }
enum arch_mmu_status arch_mmu_user_map_prepared_order(struct arch_mmu_user_space *s,
    uint64_t v, uint64_t physical, uint32_t permissions, int cow, uint32_t order,
    struct kernel_mm_leaf_preparation *prepared)
{
    if (!live(s) || !supported_order(order) || (permissions & ~7U) || !physical ||
        physical > PTE_ADDRESS || (physical & (leaf_size(order) - 1))) return ARCH_MMU_STATUS_INVALID;
    uint32_t actual, pages = 1U << order;
    if (physical_page_allocation_order(s->allocator, physical, &actual) || actual != order)
        return ARCH_MMU_STATUS_INVALID;
    if ((permissions ? s->leaf_pages : s->protected_pages) > UINT32_MAX - pages ||
        (cow && s->cow_pages > UINT32_MAX - pages)) return ARCH_MMU_STATUS_STATE;
    enum arch_mmu_status status = install_prepared_leaf(s, v,
        encode(physical, permissions, cow) | (order ? PTE_HUGE : 0), order, prepared);
    if (status) return status;
    if (permissions) s->leaf_pages += pages; else s->protected_pages += pages;
    if (cow) s->cow_pages += pages;
    resident_peak(s);
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_map_prepared_page(struct arch_mmu_user_space *s,
    uint64_t v, uint64_t p, uint32_t permissions, int cow, struct kernel_mm_leaf_preparation *prepared)
{ return arch_mmu_user_map_prepared_order(s, v, p, permissions, cow, 0, prepared); }
enum arch_mmu_status arch_mmu_user_move_leaf(struct arch_mmu_user_space *s,
    uint64_t from, uint64_t to)
{
    uint64_t *source, *target;
    if (((from | to) & BOAROS_PAGE_MASK) || from == to) return ARCH_MMU_STATUS_INVALID;
    enum arch_mmu_status status = walk(s, from, 0, &source);
    if (status) return status;
    status = walk(s, to, 0, &target);
    if (status) return status;
    if (*target) return ARCH_MMU_STATUS_CONFLICT;
    if (!(*source & PTE_PRESENT)) return ARCH_MMU_STATUS_NOT_MAPPED;
    uint64_t entry = __atomic_exchange_n(source, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(target, entry, __ATOMIC_RELEASE);
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_cow_snapshot(struct arch_mmu_user_space *s,
    uint64_t v, uint64_t *cookie, uint64_t *physical)
{
    if (!cookie || !physical || (v & BOAROS_PAGE_MASK) || v < BOAROS_PAGE_SIZE)
        return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot;
    enum arch_mmu_status status = walk(s, v, 0, &slot);
    if (status != ARCH_MMU_STATUS_OK) return status;
    if (!(*slot & PTE_COW)) return ARCH_MMU_STATUS_NOT_MAPPED;
    *cookie = *slot; *physical = *slot & PTE_ADDRESS;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_cow_commit_order(struct arch_mmu_user_space *s, uint64_t v,
    uint64_t cookie, uint64_t replacement, uint32_t permissions, uint32_t order,
    struct kernel_mm_retirement *batch)
{
    if (!batch || batch->count >= KERNEL_MM_RETIRE_CAPACITY || !supported_order(order) ||
        (v & (leaf_size(order) - 1)) || (permissions & ~7U) ||
        (replacement != KERNEL_MM_NO_PAGE && (!replacement ||
        (replacement & (leaf_size(order) - 1)) || replacement > PTE_ADDRESS))) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot; uint32_t actual_order;
    enum arch_mmu_status status = walk_leaf(s, v, &slot, &actual_order);
    if (status) return status;
    uint64_t actual = *slot, physical = actual & PTE_ADDRESS;
    if (actual_order != order) return ARCH_MMU_STATUS_INVALID;
    if (actual != cookie) return ARCH_MMU_STATUS_CONFLICT;
    if (!(actual & PTE_COW)) return ARCH_MMU_STATUS_NOT_MAPPED;
    uint32_t pages = 1U << order, references;
    if (s->cow_pages < pages) return ARCH_MMU_STATUS_STATE;
    if (physical_page_reference_count(s->allocator, physical, &references)) return ARCH_MMU_STATUS_STATE;
    if (replacement == KERNEL_MM_NO_PAGE && references != 1) return ARCH_MMU_STATUS_CONFLICT;
    if (replacement != KERNEL_MM_NO_PAGE &&
        (physical_page_allocation_order(s->allocator, replacement, &actual_order) || actual_order != order))
        return ARCH_MMU_STATUS_INVALID;
    if ((actual & PTE_NONE) && permissions) {
        if (s->protected_pages < pages || s->leaf_pages > UINT32_MAX-pages) return ARCH_MMU_STATUS_STATE;
        s->protected_pages -= pages; s->leaf_pages += pages;
    }
    if (!(actual & PTE_NONE) && !permissions) {
        if (s->leaf_pages < pages || s->protected_pages > UINT32_MAX-pages) return ARCH_MMU_STATUS_STATE;
        s->leaf_pages -= pages; s->protected_pages += pages;
    }
    __atomic_store_n(slot, encode(replacement == KERNEL_MM_NO_PAGE ? physical : replacement,
        permissions, 0) | (order ? PTE_HUGE : 0), __ATOMIC_RELEASE);
    s->cow_pages -= pages;
    if (replacement != KERNEL_MM_NO_PAGE) {
        s->cow_copies++; kernel_mm_retirement_add_order(batch, physical, order, 0);
    } else s->cow_in_place++;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_cow_commit(struct arch_mmu_user_space *s, uint64_t v,
    uint64_t cookie, uint64_t replacement, uint32_t permissions, struct kernel_mm_retirement *batch)
{ return arch_mmu_user_cow_commit_order(s, v, cookie, replacement, permissions, 0, batch); }
enum arch_mmu_status arch_mmu_user_next_owned(struct arch_mmu_user_space *s,
    uint64_t *cursor, uint64_t end, uint64_t *address)
{
    if (!live(s) || !cursor || !address || *cursor > end || end > ARCH_MMU_USER_LIMIT ||
        ((*cursor | end) & BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    if (*cursor == end) return ARCH_MMU_STATUS_NOT_MAPPED;
    unsigned first_r = *cursor >> 36, first_m = (*cursor >> 25) & (ENTRIES - 1);
    unsigned first_l = (*cursor >> 14) & (ENTRIES - 1);
    uint64_t *root = table(s, s->root_address);
    for (unsigned r = first_r; r < ENTRIES && ((uint64_t)r << 36) < end; r++) {
        if (!root[r]) continue;
        if (!directory_entry(root[r])) return ARCH_MMU_STATUS_STATE;
        uint64_t *middle = table(s, root[r]);
        for (unsigned m = r == first_r ? first_m : 0; m < ENTRIES && (((uint64_t)r << 36) | ((uint64_t)m << 25)) < end; m++) {
            if (!middle[m]) continue;
            if (middle[m] & PTE_HUGE) {
                if (!(middle[m] & PTE_PRESENT) || ((middle[m] & PTE_ADDRESS) & (HUGE_SIZE-1)))
                    return ARCH_MMU_STATUS_STATE;
                *address = ((uint64_t)r << 36) | ((uint64_t)m << 25);
                *cursor = *address + HUGE_SIZE;
                return ARCH_MMU_STATUS_OK;
            }
            if (!directory_entry(middle[m])) return ARCH_MMU_STATUS_STATE;
            uint64_t *leaves = table(s, middle[m]);
            for (unsigned l = r == first_r && m == first_m ? first_l : 0; l < ENTRIES && (((uint64_t)r << 36) | ((uint64_t)m << 25) | ((uint64_t)l << 14)) < end; l++) {
                if (!leaves[l]) continue;
                if (!(leaves[l] & PTE_PRESENT) || (leaves[l] & PTE_HUGE)) return ARCH_MMU_STATUS_STATE;
                *address = ((uint64_t)r << 36) | ((uint64_t)m << 25) | ((uint64_t)l << 14);
                *cursor = *address + BOAROS_PAGE_SIZE;
                return ARCH_MMU_STATUS_OK;
            }
        }
    }
    *cursor = end; return ARCH_MMU_STATUS_NOT_MAPPED;
}
enum arch_mmu_status arch_mmu_user_owned_snapshot(struct arch_mmu_user_space *s,
    uint64_t v,uint64_t *physical,int *cow)
{
    if (!live(s) || !physical || !cow || (v&BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot;
    enum arch_mmu_status status=walk(s,v,0,&slot);
    if (status) return status;
    if (!(*slot&PTE_PRESENT)) return ARCH_MMU_STATUS_NOT_MAPPED;
    *physical=*slot&PTE_ADDRESS;*cow=!!(*slot&PTE_COW);
    return ARCH_MMU_STATUS_OK;
}

enum arch_mmu_status arch_mmu_user_mark_cow_page(struct arch_mmu_user_space *s, uint64_t v)
{
    uint64_t *slot; uint32_t order;
    enum arch_mmu_status status = walk_leaf(s, v, &slot, &order);
    if (status) return status;
    if (v & (leaf_size(order)-1)) return ARCH_MMU_STATUS_INVALID;
    if (*slot & PTE_COW) return ARCH_MMU_STATUS_OK;
    uint32_t pages = 1U << order;
    if (s->cow_pages > UINT32_MAX-pages) return ARCH_MMU_STATUS_STATE;
    __atomic_store_n(slot, (*slot | PTE_COW) & ~PTE_DIRTY, __ATOMIC_RELEASE);
    s->cow_pages += pages; return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_init(struct arch_mmu_user_space *s,
    struct physical_page_allocator *a, const struct arch_mmu_page_table *kernel)
{
    if (!s || !a || !kernel || kernel->allocator!=a || kernel->state!=ARCH_MMU_STATE_ACTIVE)
        return ARCH_MMU_STATUS_INVALID;
    if (s->state!=ARCH_MMU_USER_SPACE_EMPTY || s->allocator || s->root_address) return ARCH_MMU_STATUS_STATE;
    struct arch_mmu_user_space candidate={.allocator=a,.state=ARCH_MMU_USER_SPACE_LIVE};
    enum arch_mmu_status status=allocate_table(&candidate,&candidate.root_address);
    if (status==ARCH_MMU_STATUS_OK) *s=candidate;
    return status;
}
static enum arch_mmu_status map_owned_order(struct arch_mmu_user_space *s,uint64_t v,uint64_t p,uint32_t permissions,int cow,uint32_t order)
{
    if (!live(s) || !supported_order(order) || ((v|p)&(leaf_size(order)-1)) ||
        v>=ARCH_MMU_USER_LIMIT || !p || p>PTE_ADDRESS || (permissions&~7U)) return ARCH_MMU_STATUS_INVALID;
    uint32_t actual, pages=1U<<order;
    if (physical_page_allocation_order(s->allocator,p,&actual) || actual!=order)
        return ARCH_MMU_STATUS_INVALID;
    if ((permissions?s->leaf_pages:s->protected_pages)>UINT32_MAX-pages ||
        (cow && s->cow_pages>UINT32_MAX-pages)) return ARCH_MMU_STATUS_STATE;
    uint64_t *slot;
    enum arch_mmu_status status=walk_order(s,v,order,1,&slot);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    if (*slot) return ARCH_MMU_STATUS_CONFLICT;
    *slot=encode(p,permissions,cow)|(order?PTE_HUGE:0);
    if (!permissions) s->protected_pages+=pages; else s->leaf_pages+=pages;
    resident_peak(s);
    if (cow) s->cow_pages+=pages;
    arch_mmu_flush_address(v); if (permissions&4) arch_mmu_sync_instructions();
    return ARCH_MMU_STATUS_OK;
}
static enum arch_mmu_status map_owned(struct arch_mmu_user_space *s,uint64_t v,uint64_t p,uint32_t permissions,int cow)
{ return map_owned_order(s,v,p,permissions,cow,0); }
enum arch_mmu_status arch_mmu_user_map_owned_page(struct arch_mmu_user_space *s,uint64_t v,uint64_t p,uint32_t perms)
{ return map_owned(s,v,p,perms,0); }
enum arch_mmu_status arch_mmu_user_map_cow_page(struct arch_mmu_user_space *s,uint64_t v,uint64_t p,uint32_t perms)
{ return map_owned(s,v,p,perms,1); }
enum arch_mmu_status arch_mmu_user_discard_owned_page(struct arch_mmu_user_space *s,uint64_t p)
{
    if (!live(s)) return ARCH_MMU_STATUS_STATE;
    if (physical_page_release(s->allocator,p)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("discard page");
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_map_zeroed_page(struct arch_mmu_user_space *s,uint64_t v,uint32_t perms)
{
    if (!live(s)) return ARCH_MMU_STATUS_STATE;
    uint64_t p;
    if (physical_page_allocate(s->allocator,&p)!=PHYSICAL_PAGE_STATUS_OK) return ARCH_MMU_STATUS_NO_MEMORY;
    memset(table(s,p),0,BOAROS_PAGE_SIZE);
    enum arch_mmu_status status=map_owned(s,v,p,perms,0);
    if (status!=ARCH_MMU_STATUS_OK) arch_mmu_user_discard_owned_page(s,p);
    return status;
}
enum arch_mmu_status arch_mmu_user_lookup(const struct arch_mmu_user_space *s,uint64_t v,struct arch_mmu_mapping *out)
{
    if (!out) return ARCH_MMU_STATUS_INVALID;
    struct kernel_mm_leaf leaf;
    enum arch_mmu_status status = arch_mmu_user_leaf_snapshot(s, v, &leaf);
    if (status) return status;
    if (!leaf.permissions) return ARCH_MMU_STATUS_NOT_MAPPED;
    *out = (struct arch_mmu_mapping){leaf.physical_address + v - leaf.virtual_address, leaf.permissions};
    return ARCH_MMU_STATUS_OK;
}
/* Only the two range edges can cut a naturally aligned large leaf. Validate
 * both before modifying any PTE, including bounded detach's resumed calls. */
static enum arch_mmu_status whole_leaf_range(struct arch_mmu_user_space *s, uint64_t start, uint64_t end)
{
    if (!live(s) || start > end || end > ARCH_MMU_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK))
        return ARCH_MMU_STATUS_INVALID;
    if (start == end) return ARCH_MMU_STATUS_OK;
    uint64_t edges[2] = {start, end-1};
    for (unsigned i=0; i<2; i++) {
        struct kernel_mm_leaf leaf;
        enum arch_mmu_status status = arch_mmu_user_leaf_snapshot(s, edges[i], &leaf);
        if (status == ARCH_MMU_STATUS_NOT_MAPPED) continue;
        if (status) return status;
        if (leaf.order && (leaf.virtual_address < start || leaf.virtual_address + leaf_size(leaf.order) > end))
            return ARCH_MMU_STATUS_INVALID;
    }
    return ARCH_MMU_STATUS_OK;
}
static int table_empty(uint64_t *p)
{ for (unsigned i=0;i<ENTRIES;i++) if (p[i]) return 0; return 1; }
enum arch_mmu_status arch_mmu_user_detach_range(
    struct arch_mmu_user_space *s, uint64_t start, uint64_t end,
    struct kernel_mm_detach_cursor *cursor, struct kernel_mm_retirement *batch,
    void (*before_detach)(void *,uint64_t,uint64_t), void *owner)
{
    if (!s || s->state != ARCH_MMU_USER_SPACE_LIVE || !cursor || !batch || start>end ||
        end>ARCH_MMU_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    enum arch_mmu_status checked = whole_leaf_range(s, start, end);
    if (checked) return checked;
    const uint64_t leaf_span=UINT64_C(1)<<25, middle_span=UINT64_C(1)<<36;
    if (cursor->phase==3 || start==end) return ARCH_MMU_STATUS_NOT_MAPPED;
    if (!cursor->phase) { cursor->address=start; cursor->phase=1; }
    /* MM metadata protection keeps these table pages owned for this call;
     * detached pages are not released until after the caller drops it. */
    uint64_t *root=table(s,s->root_address), *middle=0, *leaves=0;
    uint64_t middle_physical=0, leaf_physical=0;
    unsigned visits=0;
    while (visits<16 && batch->count<=KERNEL_MM_RETIRE_CAPACITY-3) {
        if (cursor->phase==2) {
            unsigned r=cursor->middle_address>>36;
            /* The previous batch borrowed only a VA position. A disjoint
             * modifier may have retired or replaced the current parent. */
            if (!root[r]) { cursor->phase=1; continue; }
            if (!directory_entry(root[r])) return ARCH_MMU_STATUS_STATE;
            if (middle_physical!=root[r]) {
                middle_physical=root[r]; middle=table(s,middle_physical);
            }
            visits++;
            /* The final empty check shares qualification with parent
             * removal; a saved empty prefix cannot authorize retirement. */
            if (table_empty(middle)) {
                kernel_mm_retirement_add(batch,root[r],1);
                __atomic_store_n(&root[r],0,__ATOMIC_RELEASE);
                s->table_pages--;
            }
            cursor->phase=1;
            continue;
        }
        if (cursor->address>=end) { cursor->phase=3; return ARCH_MMU_STATUS_NOT_MAPPED; }
        uint64_t address=cursor->address;
        unsigned r=address>>36, m=(address>>25)&(ENTRIES-1);
        unsigned l=(address>>BOAROS_PAGE_SHIFT)&(ENTRIES-1);
        if (!root[r]) {
            cursor->address=(address & ~(middle_span-1))+middle_span;
            visits++; continue;
        }

        if (!directory_entry(root[r])) return ARCH_MMU_STATUS_STATE;
        if (middle_physical!=root[r]) {
            middle_physical=root[r]; middle=table(s,middle_physical);
        }
        if (!middle[m]) {
            cursor->address=(address & ~(leaf_span-1))+leaf_span;
            visits++;
        } else if (middle[m] & PTE_HUGE) {
            uint64_t old = middle[m], physical = old & PTE_ADDRESS;
            unsigned pages = 1U << HUGE_ORDER;
            if (!(old & PTE_PRESENT) || (physical & (HUGE_SIZE-1)) ||
                ((old & PTE_NONE) ? s->protected_pages : s->leaf_pages) < pages ||
                ((old & PTE_COW) && s->cow_pages < pages)) return ARCH_MMU_STATUS_STATE;
            if (before_detach) before_detach(owner, address, physical);
            __atomic_store_n(&middle[m], 0, __ATOMIC_RELEASE);
            kernel_mm_retirement_add_order(batch, physical, HUGE_ORDER, 0);
            if (old & PTE_NONE) s->protected_pages -= pages; else s->leaf_pages -= pages;
            if (old & PTE_COW) s->cow_pages -= pages;
            cursor->address += HUGE_SIZE;
            visits++;
        } else {
            if (!directory_entry(middle[m])) return ARCH_MMU_STATUS_STATE;
            if (leaf_physical!=middle[m]) {
                leaf_physical=middle[m]; leaves=table(s,leaf_physical);
            }
            uint64_t old=leaves[l];
            visits++;
            if (old && address>=start && address<end) {
                if (!(old&PTE_PRESENT) || (!(old&PTE_NONE) && !s->leaf_pages) || s->table_pages<3 ||
                    ((old&PTE_NONE) && !s->protected_pages) || ((old&PTE_COW) && !s->cow_pages)) return ARCH_MMU_STATUS_STATE;
                uint64_t physical=old & PTE_ADDRESS;
                void *owned;
                if (physical_page_map_owned(s->allocator,physical,&owned)!=PHYSICAL_PAGE_STATUS_OK)
                    return ARCH_MMU_STATUS_STATE;
                /* resident必须在身份仍属于这个PTE时解绑；释放仍由MM在raw外完成。 */
                if (before_detach) before_detach(owner,address,physical);
                __atomic_store_n(&leaves[l],0,__ATOMIC_RELEASE);
                kernel_mm_retirement_add(batch,physical,0);
                if (old&PTE_NONE) s->protected_pages--; else s->leaf_pages--;
                if (old&PTE_COW) s->cow_pages--;
            }
            cursor->address+=BOAROS_PAGE_SIZE;
            if (l+1<ENTRIES && cursor->address<end) continue;
            if (table_empty(leaves)) {
                kernel_mm_retirement_add(batch,middle[m],1);
                __atomic_store_n(&middle[m],0,__ATOMIC_RELEASE);
                s->table_pages--;
            }
        }
        if ((cursor->address>>36)!=r || cursor->address>=end) {
            cursor->middle_address=(uint64_t)r<<36;
            cursor->phase=2;
        }
    }
    return ARCH_MMU_STATUS_OK;
}

static void unmap_tree(struct arch_mmu_user_space *s,uint64_t root,unsigned level,uint64_t base,uint64_t start,uint64_t end)
{
    uint64_t *p=table(s,root), span=UINT64_C(1)<<(14+level*11);
    for (unsigned i=0;i<ENTRIES;i++) {
        uint64_t address=base+i*span;
        if (!p[i] || address>=end || address+span<=start) continue;
        if (level && !(level == 1 && (p[i] & PTE_HUGE))) {
            if (!directory_entry(p[i])) la_virt_fatal("invalid directory");
            unmap_tree(s,p[i],level-1,address,start,end);
            if (table_empty(table(s,p[i]))) {
                uint64_t child=p[i]; p[i]=0;
                flush_all(); /* 非叶翻译也结束后才能复用空表页。 */
                if (physical_page_release(s->allocator,child)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("table release");
                s->table_pages--;
            }
        } else {
            uint64_t leaf=p[i]; p[i]=0;
            uint32_t order = level ? HUGE_ORDER : 0, pages = 1U << order;
            if (!(leaf & PTE_PRESENT) || (!level && (leaf & PTE_HUGE))) la_virt_fatal("invalid leaf");
            /* 先失效翻译再归还页，避免旧 TLB 指向已复用物理 owner。 */
            arch_mmu_flush_address(address);
            if (physical_page_release_order(s->allocator,leaf&PTE_ADDRESS,order)) la_virt_fatal("leaf release");
            if (leaf&PTE_NONE) s->protected_pages-=pages; else s->leaf_pages-=pages;
            if (leaf&PTE_COW) s->cow_pages-=pages;
        }
    }
}
enum arch_mmu_status arch_mmu_user_unmap_owned_range(struct arch_mmu_user_space *s,uint64_t start,uint64_t end)
{
    if (!live(s) || start>end || end>ARCH_MMU_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    enum arch_mmu_status status = whole_leaf_range(s, start, end);
    if (status) return status;
    unmap_tree(s,s->root_address,2,0,start,end); return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_protect_resident_page(struct arch_mmu_user_space *s,uint64_t v,uint32_t perms)
{
    if (!live(s) || (v&BOAROS_PAGE_MASK) || (perms&~7U)) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot;
    enum arch_mmu_status status=walk(s,v,0,&slot);
    s->protect_visits++;
    if (status!=ARCH_MMU_STATUS_OK) return status;
    if (!*slot) return ARCH_MMU_STATUS_NOT_MAPPED;
    uint64_t old=*slot;
    if (!(old&PTE_PRESENT) || (old&PTE_HUGE)) return ARCH_MMU_STATUS_STATE;
    __atomic_store_n(slot,encode(old&PTE_ADDRESS,perms,(old&PTE_COW)!=0),__ATOMIC_RELEASE);
    if ((old&PTE_NONE) && perms) { s->protected_pages--; s->leaf_pages++; }
    if (!(old&PTE_NONE) && !perms) { s->leaf_pages--; s->protected_pages++; }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_protect_owned_page(struct arch_mmu_user_space *s,uint64_t v,uint32_t perms)
{
    enum arch_mmu_status status=arch_mmu_user_protect_resident_page(s,v,perms);
    return status==ARCH_MMU_STATUS_NOT_MAPPED ? ARCH_MMU_STATUS_OK : status;
}
enum arch_mmu_status arch_mmu_user_protect_owned_range(struct arch_mmu_user_space *s,uint64_t start,uint64_t end,uint32_t perms)
{
    if (perms & ~7U) return ARCH_MMU_STATUS_INVALID;
    enum arch_mmu_status status = whole_leaf_range(s, start, end);
    if (status) return status;
    uint64_t cursor = start, address;
    while ((status = arch_mmu_user_next_owned(s, &cursor, end, &address)) == ARCH_MMU_STATUS_OK) {
        uint64_t *slot; uint32_t order;
        status = walk_leaf(s, address, &slot, &order);
        if (status) return status;
        uint64_t old = *slot; uint32_t pages = 1U << order;
        __atomic_store_n(slot, encode(old & PTE_ADDRESS, perms, !!(old & PTE_COW)) |
            (order ? PTE_HUGE : 0), __ATOMIC_RELEASE);
        if ((old & PTE_NONE) && perms) { s->protected_pages -= pages; s->leaf_pages += pages; }
        if (!(old & PTE_NONE) && !perms) { s->leaf_pages -= pages; s->protected_pages += pages; }
        s->protect_visits++;
    }
    if (status != ARCH_MMU_STATUS_NOT_MAPPED) return status;
    if (start<end) {
        if (end-start==BOAROS_PAGE_SIZE) { arch_mmu_flush_address(start); s->protect_address_flushes++; }
        else { flush_all(); s->protect_global_flushes++; }
        if (perms&ARCH_MMU_EXECUTE) arch_mmu_sync_instructions();
    }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_populate(struct arch_mmu_user_space *s,uint64_t v,const void *bytes,size_t size)
{
    if (!live(s) || arch_mmu_current_context()==s->root_address || (!bytes&&size) || v>=ARCH_MMU_USER_LIMIT || size>ARCH_MMU_USER_LIMIT-v) return ARCH_MMU_STATUS_INVALID;
    const unsigned char *data=bytes;
    while (size) {
        struct kernel_mm_leaf leaf;
        enum arch_mmu_status status=arch_mmu_user_leaf_snapshot(s,v,&leaf);
        if (status!=ARCH_MMU_STATUS_OK) return status;
        if (!leaf.permissions) return ARCH_MMU_STATUS_NOT_MAPPED;
        size_t offset=v-leaf.virtual_address, chunk=leaf_size(leaf.order)-offset;
        if (chunk>size) chunk=size;
        memcpy((unsigned char *)table(s,leaf.physical_address)+offset,data,chunk);
        data+=chunk; v+=chunk; size-=chunk;
    }
    arch_mmu_sync_instructions(); return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_context(const struct arch_mmu_user_space *s,uint64_t *root)
{
#if BOAROS_MAX_CPUS > 1
    /* MM owner固定root寿命；切换不读取并行PTE编辑维护的表页计数。 */
    if (!s || s->state!=ARCH_MMU_USER_SPACE_LIVE || !s->allocator ||
        !s->root_address || !root) return ARCH_MMU_STATUS_INVALID;
#else
    if (!live(s)||!root) return ARCH_MMU_STATUS_INVALID;
#endif
    *root=s->root_address;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_move(struct arch_mmu_user_space *to,struct arch_mmu_user_space *from)
{
    if (!to || to==from || !live(from)) return ARCH_MMU_STATUS_INVALID;
    if (to->state!=ARCH_MMU_USER_SPACE_EMPTY || to->allocator || to->root_address) return ARCH_MMU_STATUS_STATE;
    *to=*from; *from=(struct arch_mmu_user_space){.state=ARCH_MMU_USER_SPACE_MOVED}; return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_destroy(struct arch_mmu_user_space *s)
{
    if (!live(s) || arch_mmu_current_context()==s->root_address) return ARCH_MMU_STATUS_STATE;
    unmap_tree(s,s->root_address,2,0,0,ARCH_MMU_USER_LIMIT);
    if (s->table_pages!=1 || s->leaf_pages || s->protected_pages || s->cow_pages) la_virt_fatal("MMU counts");
    if (physical_page_release(s->allocator,s->root_address)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("root release");
    *s=(struct arch_mmu_user_space){.state=ARCH_MMU_USER_SPACE_DESTROYED}; return ARCH_MMU_STATUS_OK;
}
static enum arch_mmu_status fork_tree(struct arch_mmu_user_space *to,struct arch_mmu_user_space *from,
    uint64_t root,unsigned level,uint64_t base,int (*shared)(void *,uint64_t),void *context,int commit)
{
    uint64_t *p=table(from,root), span=UINT64_C(1)<<(14+level*11);
    for (unsigned i=0;i<ENTRIES;i++) {
        if (!p[i]) continue;
        uint64_t v=base+i*span;
        if (level && !(level == 1 && (p[i] & PTE_HUGE))) {
            if (!directory_entry(p[i])) return ARCH_MMU_STATUS_STATE;
            enum arch_mmu_status status=fork_tree(to,from,p[i],level-1,v,shared,context,commit);
            if (status!=ARCH_MMU_STATUS_OK) return status;
        } else {
            uint64_t leaf=p[i]; uint32_t order=level?HUGE_ORDER:0, pages=1U<<order;
            if (!(leaf&PTE_PRESENT) || (!level && (leaf&PTE_HUGE))) return ARCH_MMU_STATUS_STATE;
            int action=shared ? shared(context,v) : ARCH_MMU_FORK_PRIVATE;
            if (action==ARCH_MMU_FORK_SKIP) continue;
            if (action!=ARCH_MMU_FORK_PRIVATE && action!=ARCH_MMU_FORK_SHARED) return ARCH_MMU_STATUS_STATE;
            int cow=action==ARCH_MMU_FORK_PRIVATE;
            if (commit) {
                if (cow && !(leaf&PTE_COW)) {
                    if (from->cow_pages > UINT32_MAX-pages) return ARCH_MMU_STATUS_STATE;
                    p[i]=(leaf|PTE_COW)&~PTE_DIRTY; from->cow_pages+=pages;
                }
                arch_mmu_flush_address(v); continue;
            }
            uint32_t perms=0;
            if (!(leaf&PTE_NONE)) {
                if (!(leaf&PTE_NR)) perms|=ARCH_MMU_READ;
                if (!(leaf&PTE_NX)) perms|=ARCH_MMU_EXECUTE;
                if (leaf&PTE_WRITE) perms|=ARCH_MMU_WRITE;
            }
            uint64_t physical=leaf&PTE_ADDRESS;
            if (physical_page_acquire_order(from->allocator,physical,order)) la_virt_fatal("fork ref");
            enum arch_mmu_status status=map_owned_order(to,v,physical,perms,cow || !!(leaf&PTE_COW),order);
            if (status!=ARCH_MMU_STATUS_OK) { physical_page_release_order(from->allocator,physical,order); return status; }
        }
    }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_fork_prepare(struct arch_mmu_user_space *to,struct arch_mmu_user_space *from,
    int (*shared)(void *,uint64_t),void *context)
{
    if (!live(from)) return ARCH_MMU_STATUS_STATE;
    struct arch_mmu_page_table kernel={.allocator=from->allocator,.state=ARCH_MMU_STATE_ACTIVE};
    enum arch_mmu_status status=arch_mmu_user_space_init(to,from->allocator,&kernel);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    status=fork_tree(to,from,from->root_address,2,0,shared,context,0);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_fork(struct arch_mmu_user_space *to,
    struct arch_mmu_user_space *from, int (*shared)(void *, uint64_t), void *context)
{
    enum arch_mmu_status status = arch_mmu_user_space_fork_prepare(to, from, shared, context);
    return status == ARCH_MMU_STATUS_OK ? fork_tree(to,from,from->root_address,2,0,shared,context,1) : status;
}
enum arch_mmu_status arch_mmu_user_resolve_cow(struct arch_mmu_user_space *s,uint64_t v,uint32_t perms)
{
    uint64_t *slot;
    enum arch_mmu_status status=walk(s,v,0,&slot);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    uint64_t old=*slot;
    if (!(old&PTE_COW) || (old&PTE_NONE)) return ARCH_MMU_STATUS_NOT_MAPPED;
    uint64_t physical=old&PTE_ADDRESS, replacement=physical; uint32_t refs;
    if (physical_page_reference_count(s->allocator,physical,&refs)!=PHYSICAL_PAGE_STATUS_OK || !refs) la_virt_fatal("COW ref");
    if (refs>1) {
        if (physical_page_allocate(s->allocator,&replacement)!=PHYSICAL_PAGE_STATUS_OK) return ARCH_MMU_STATUS_NO_MEMORY;
        memcpy(table(s,replacement),table(s,physical),BOAROS_PAGE_SIZE); s->cow_copies++;
    } else s->cow_in_place++;
    *slot=encode(replacement,perms,0); s->cow_pages--;
    arch_mmu_flush_address(v);
    if (replacement!=physical && physical_page_release(s->allocator,physical)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("COW release");
    if (perms&ARCH_MMU_EXECUTE) arch_mmu_sync_instructions();
    return ARCH_MMU_STATUS_OK;
}

int arch_dma_image_address(uint64_t address,uint64_t size,uint64_t *physical)
{
    extern unsigned char __kernel_start[],__kernel_end[];
    uint64_t start=(uintptr_t)__kernel_start,end=(uintptr_t)__kernel_end;
    if(!physical || !size || address<start || address>=end || size>end-address)return 0;
    return arch_direct_map_va_to_pa(address,size,physical)==ARCH_DIRECT_MAP_STATUS_OK;
}
