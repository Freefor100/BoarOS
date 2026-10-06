#include <arch/mmu.h>
#include <arch/context.h>
#include <platform/loongarch_virt.h>
#include <string.h>
#define ENTRIES (BOAROS_PAGE_SIZE/8)
#define PTE_VALID UINT64_C(1)
#define PTE_DIRTY UINT64_C(2)
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
static void flush_all(void) { __asm__ volatile("invtlb 0, $zero, $zero" ::: "memory"); }
uint64_t arch_mmu_current_context(void)
{ uint64_t v; __asm__ volatile("csrrd %0, 0x19" : "=r"(v)); return v; }
enum arch_mmu_status arch_mmu_switch_context(uint64_t root)
{
    if (root & BOAROS_PAGE_MASK || root > PTE_ADDRESS) return ARCH_MMU_STATUS_INVALID;
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
int arch_mmu_kernel_window_active(void) { return 0; }
enum arch_mmu_status arch_mmu_kernel_window_map(struct physical_page_allocator *a, uint64_t v, uint64_t p)
{ (void)a; (void)v; (void)p; return ARCH_MMU_STATUS_STATE; }
enum arch_mmu_status arch_mmu_kernel_window_unmap(struct physical_page_allocator *a, uint64_t v)
{ (void)a; (void)v; return ARCH_MMU_STATUS_STATE; }
static uint64_t *table(struct arch_mmu_user_space *s, uint64_t address)
{
    void *p;
    if (!address || (address&BOAROS_PAGE_MASK) || physical_page_resolve(s->allocator,address,&p)!=PHYSICAL_PAGE_STATUS_OK)
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
static enum arch_mmu_status walk(struct arch_mmu_user_space *s, uint64_t v, int create, uint64_t **slot)
{
    if (!live(s) || v>=ARCH_MMU_USER_LIMIT) return ARCH_MMU_STATUS_INVALID;
    uint64_t *current=table(s,s->root_address);
    for (unsigned level=2; level; level--) {
        unsigned index=(v>>(14+level*11)) & (ENTRIES-1);
        if (!current[index]) {
            if (!create) return ARCH_MMU_STATUS_NOT_MAPPED;
            uint64_t physical;
            enum arch_mmu_status status=allocate_table(s,&physical);
            if (status!=ARCH_MMU_STATUS_OK) return status;
            current[index]=physical;
        }
        current=table(s,current[index]);
    }
    *slot=&current[(v>>14)&(ENTRIES-1)]; return ARCH_MMU_STATUS_OK;
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
static enum arch_mmu_status map_owned(struct arch_mmu_user_space *s,uint64_t v,uint64_t p,uint32_t permissions,int cow)
{
    if (!live(s) || (v&BOAROS_PAGE_MASK) || (p&BOAROS_PAGE_MASK) || v>=ARCH_MMU_USER_LIMIT ||
        !p || p>PTE_ADDRESS || (permissions&~7U)) return ARCH_MMU_STATUS_INVALID;
    uint32_t refs;
    if (physical_page_reference_count(s->allocator,p,&refs)!=PHYSICAL_PAGE_STATUS_OK || !refs)
        return ARCH_MMU_STATUS_STATE;
    uint64_t *slot;
    enum arch_mmu_status status=walk(s,v,1,&slot);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    if (*slot) return ARCH_MMU_STATUS_CONFLICT;
    *slot=encode(p,permissions,cow); s->leaf_pages++;
    if (!permissions) s->protected_pages++;
    if (cow) s->cow_pages++;
    arch_mmu_flush_address(v); if (permissions&4) arch_mmu_sync_instructions();
    return ARCH_MMU_STATUS_OK;
}
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
    uint64_t *slot;
    enum arch_mmu_status status=walk((struct arch_mmu_user_space *)s,v,0,&slot);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    uint64_t pte=*slot;
    if (!(pte&PTE_PRESENT) || (pte&PTE_NONE)) return ARCH_MMU_STATUS_NOT_MAPPED;
    *out=(struct arch_mmu_mapping){(pte&PTE_ADDRESS)|(v&BOAROS_PAGE_MASK),ARCH_MMU_USER};
    if (!(pte&PTE_NR)) out->permissions|=ARCH_MMU_READ;
    if (pte&PTE_DIRTY) out->permissions|=ARCH_MMU_WRITE;
    if (!(pte&PTE_NX)) out->permissions|=ARCH_MMU_EXECUTE;
    return ARCH_MMU_STATUS_OK;
}
static int table_empty(uint64_t *p)
{ for (unsigned i=0;i<ENTRIES;i++) if (p[i]) return 0; return 1; }
static void unmap_tree(struct arch_mmu_user_space *s,uint64_t root,unsigned level,uint64_t base,uint64_t start,uint64_t end)
{
    uint64_t *p=table(s,root), span=UINT64_C(1)<<(14+level*11);
    for (unsigned i=0;i<ENTRIES;i++) {
        uint64_t address=base+i*span;
        if (!p[i] || address>=end || address+span<=start) continue;
        if (level) {
            unmap_tree(s,p[i],level-1,address,start,end);
            if (table_empty(table(s,p[i]))) {
                uint64_t child=p[i]; p[i]=0;
                if (physical_page_release(s->allocator,child)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("table release");
                s->table_pages--;
            }
        } else {
            uint64_t leaf=p[i]; p[i]=0;
            /* 先失效翻译再归还页，避免旧 TLB 指向已复用物理 owner。 */
            arch_mmu_flush_address(address);
            if (physical_page_release(s->allocator,leaf&PTE_ADDRESS)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("leaf release");
            s->leaf_pages--;
            if (leaf&PTE_NONE) s->protected_pages--;
            if (leaf&PTE_COW) s->cow_pages--;
        }
    }
}
enum arch_mmu_status arch_mmu_user_unmap_owned_range(struct arch_mmu_user_space *s,uint64_t start,uint64_t end)
{
    if (!live(s) || start>end || end>ARCH_MMU_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    unmap_tree(s,s->root_address,2,0,start,end); return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_protect_owned_page(struct arch_mmu_user_space *s,uint64_t v,uint32_t perms)
{
    if (!live(s) || (v&BOAROS_PAGE_MASK) || (perms&~7U)) return ARCH_MMU_STATUS_INVALID;
    uint64_t *slot;
    enum arch_mmu_status status=walk(s,v,0,&slot);
    s->protect_visits++;
    if (status==ARCH_MMU_STATUS_NOT_MAPPED) return ARCH_MMU_STATUS_OK;
    if (status!=ARCH_MMU_STATUS_OK) return status;
    if (!*slot) return ARCH_MMU_STATUS_OK;
    uint64_t old=*slot;
    *slot=encode(old&PTE_ADDRESS,perms,(old&PTE_COW)!=0);
    if (old&PTE_NONE) s->protected_pages--;
    if (!perms) s->protected_pages++;
    arch_mmu_flush_address(v); s->protect_address_flushes++;
    if (perms&ARCH_MMU_EXECUTE) arch_mmu_sync_instructions();
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_protect_owned_range(struct arch_mmu_user_space *s,uint64_t start,uint64_t end,uint32_t perms)
{
    if (!live(s) || start>end || end>ARCH_MMU_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK)) return ARCH_MMU_STATUS_INVALID;
    for (uint64_t v=start;v<end;v+=BOAROS_PAGE_SIZE) {
        enum arch_mmu_status status=arch_mmu_user_protect_owned_page(s,v,perms);
        if (status!=ARCH_MMU_STATUS_OK) return status;
    }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_populate(struct arch_mmu_user_space *s,uint64_t v,const void *bytes,size_t size)
{
    if (!live(s) || arch_mmu_current_context()==s->root_address || (!bytes&&size) || v>=ARCH_MMU_USER_LIMIT || size>ARCH_MMU_USER_LIMIT-v) return ARCH_MMU_STATUS_INVALID;
    const unsigned char *data=bytes;
    while (size) {
        struct arch_mmu_mapping mapping;
        enum arch_mmu_status status=arch_mmu_user_lookup(s,v,&mapping);
        if (status!=ARCH_MMU_STATUS_OK) return status;
        size_t offset=v&BOAROS_PAGE_MASK, chunk=BOAROS_PAGE_SIZE-offset;
        if (chunk>size) chunk=size;
        memcpy((unsigned char *)table(s,mapping.physical_address&~BOAROS_PAGE_MASK)+offset,data,chunk);
        data+=chunk; v+=chunk; size-=chunk;
    }
    arch_mmu_sync_instructions(); return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_context(const struct arch_mmu_user_space *s,uint64_t *root)
{ if (!live(s)||!root) return ARCH_MMU_STATUS_INVALID; *root=s->root_address; return ARCH_MMU_STATUS_OK; }
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
        if (level) {
            enum arch_mmu_status status=fork_tree(to,from,p[i],level-1,v,shared,context,commit);
            if (status!=ARCH_MMU_STATUS_OK) return status;
        } else {
            uint64_t leaf=p[i]; int cow=!shared || !shared(context,v);
            if (commit) {
                if (cow && !(leaf&PTE_COW)) { p[i]=(leaf|PTE_COW)&~PTE_DIRTY; from->cow_pages++; }
                arch_mmu_flush_address(v); continue;
            }
            uint32_t perms=0;
            if (!(leaf&PTE_NONE)) {
                if (!(leaf&PTE_NR)) perms|=ARCH_MMU_READ;
                if (!(leaf&PTE_NX)) perms|=ARCH_MMU_EXECUTE;
                if (leaf&PTE_WRITE) perms|=ARCH_MMU_WRITE;
            }
            uint64_t physical=leaf&PTE_ADDRESS;
            if (physical_page_acquire(from->allocator,physical)!=PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("fork ref");
            enum arch_mmu_status status=map_owned(to,v,physical,perms,cow);
            if (status!=ARCH_MMU_STATUS_OK) { arch_mmu_user_discard_owned_page(from,physical); return status; }
        }
    }
    return ARCH_MMU_STATUS_OK;
}
enum arch_mmu_status arch_mmu_user_space_fork(struct arch_mmu_user_space *to,struct arch_mmu_user_space *from,
    int (*shared)(void *,uint64_t),void *context)
{
    if (!live(from)) return ARCH_MMU_STATUS_STATE;
    struct arch_mmu_page_table kernel={.allocator=from->allocator,.state=ARCH_MMU_STATE_ACTIVE};
    enum arch_mmu_status status=arch_mmu_user_space_init(to,from->allocator,&kernel);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    status=fork_tree(to,from,from->root_address,2,0,shared,context,0);
    if (status!=ARCH_MMU_STATUS_OK) return status;
    return fork_tree(to,from,from->root_address,2,0,shared,context,1);
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
