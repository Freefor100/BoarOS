#include "vfs_objects.h"
#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/page_cache.h>
#include <kernel/time.h>
#include <arch/context.h>
#include "../third_party/lwext4/include/misc/tree.h"
#include <string.h>

/* FAT stores a file's address in its directory entry. Live identities have a
 * separate number: moving/removing that entry must not retarget an open fd. */
struct fat_inode {
    RB_ENTRY(fat_inode) by_id;
    RB_ENTRY(fat_inode) by_entry;
    uint64_t id, entry, parent, first_slot;
    uint32_t first, tail, clusters, size, mode, handles;
    uint32_t cursor, cursor_index;
    uint8_t slots, linked, measured, times_dirty;
    struct kernel_vfs_timespec atime, mtime, ctime;
    struct fat_inode *retired_next;
};
RB_HEAD(fat_id_tree, fat_inode);
RB_HEAD(fat_entry_tree, fat_inode);
struct fat_mount {
    struct kernel_vfs_instance instance;
    struct kernel_vfs_mount *mount;
    struct kernel_block_device *block;
    struct kernel_mutex metadata;
    struct fat_id_tree inodes;
    struct fat_entry_tree entries;
    struct fat_inode *retired_inodes;
    uint64_t next_id, fat_start, data_start, fat_bytes, sector_at;
    uint32_t sectors, sector_size, cluster_size, cluster_count, root_cluster;
    uint32_t free_clusters, next_free, fsinfo, backup_boot;
    unsigned fats, active_fat, mirrored;
    int error, prepared;
    uint8_t *sector, *zeros;
};
static int id_compare(const struct fat_inode *a, const struct fat_inode *b)
{ return (a->id > b->id) - (a->id < b->id); }
static int entry_compare(const struct fat_inode *a, const struct fat_inode *b)
{ return (a->entry > b->entry) - (a->entry < b->entry); }
RB_GENERATE_INTERNAL(fat_id_tree, fat_inode, by_id, id_compare, static inline)
RB_GENERATE_INTERNAL(fat_entry_tree, fat_inode, by_entry, entry_compare, static inline)
static const struct kernel_vfs_backend fat_ops;
static struct kernel_vfs_timespec from_dos(uint16_t date, uint16_t clock);
static struct fat_mount *fm(struct kernel_vfs_instance *i) { return i->backend_data; }
static struct fat_inode *fi(struct kernel_vfs_node *n) { return n->backend_data; }
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static int alloc(struct fat_mount *m, size_t size, void **out)
{
    enum kernel_heap_status r = kernel_heap_allocate_zeroed(m->instance.heap, 1, size, out);
    return r == KERNEL_HEAP_STATUS_OK ? 0 : r == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
}
static void dispose(struct fat_mount *m, void *p)
{ if (p) kernel_heap_release(m->instance.heap, p); }
static void retire_inode(struct fat_mount *m, struct fat_inode *i)
{
    struct fat_inode *head = __atomic_load_n(&m->retired_inodes, __ATOMIC_ACQUIRE);
    do { i->retired_next = head; }
    while (!__atomic_compare_exchange_n(&m->retired_inodes, &head, i, 1, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}
static void collect_retired(struct fat_mount *m)
{
    /* close_node may run inside reclaim. It only transfers the dead handle;
     * the next metadata owner removes its index and releases memory. */
    struct fat_inode *i = __atomic_exchange_n(&m->retired_inodes, 0, __ATOMIC_ACQ_REL);
    while (i) {
        struct fat_inode *next = i->retired_next;
        if (__atomic_load_n(&i->handles, __ATOMIC_ACQUIRE) || i->linked || i->first || i->clusters ||
            RB_REMOVE(fat_id_tree, &m->inodes, i) != i) __builtin_trap();
        dispose(m, i); i = next;
    }
}
static struct kernel_vfs_timespec now(void)
{
    uint64_t ns = kernel_time_realtime_ns();
    return (struct kernel_vfs_timespec){ns / 1000000000U, ns % 1000000000U};
}
static int fail(struct fat_mount *m, int error)
{
    if (error == -KERNEL_EIO || error == -KERNEL_EUCLEAN)
        __atomic_store_n(&m->error, error, __ATOMIC_RELEASE);
    return error;
}
static int block_error(enum kernel_block_status s)
{
    return s == KERNEL_BLOCK_STATUS_OK ? 0 :
           s == KERNEL_BLOCK_STATUS_READ_ONLY ? -KERNEL_EROFS :
           s == KERNEL_BLOCK_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
}
static int sector_read(struct fat_mount *m, uint64_t at, void *buffer)
{ return fail(m, block_error(kernel_block_read_at(m->block, at, buffer, m->sector_size))); }
static int sector_write(struct fat_mount *m, uint64_t at, const void *buffer)
{
    int r = fail(m, block_error(kernel_block_write_at(m->block, at, buffer, m->sector_size)));
    if (r || (at == m->sector_at && buffer != m->sector)) m->sector_at = UINT64_MAX;
    return r;
}
static int bytes(struct fat_mount *m, uint64_t at, void *buffer, size_t size, int write)
{
    while (size) {
        uint64_t base = at & ~((uint64_t)m->sector_size - 1);
        size_t start = at - base, n = m->sector_size - start;
        if (n > size) n = size;
        int r = 0;
        if (start || n != m->sector_size) {
            if (m->sector_at != base) {
                r = sector_read(m, base, m->sector);
                if (r) { m->sector_at = UINT64_MAX; return r; }
                m->sector_at = base;
            }
            if (write) {
                memcpy(m->sector + start, buffer, n);
                r = sector_write(m, base, m->sector);
            } else memcpy(buffer, m->sector + start, n);
        } else r = write ? sector_write(m, base, buffer) : sector_read(m, base, buffer);
        if (r) return r;
        at += n; buffer = (uint8_t *)buffer + n; size -= n;
    }
    return 0;
}
static int valid_cluster(struct fat_mount *m, uint32_t c)
{ return c >= 2 && c - 2 < m->cluster_count; }
static uint64_t cluster_at(struct fat_mount *m, uint32_t c)
{ return m->data_start + (uint64_t)(c - 2) * m->cluster_size; }
static int fat_get(struct fat_mount *m, uint32_t c, uint32_t *value)
{
    uint8_t p[4];
    if (c >= m->cluster_count + 2) return fail(m, -KERNEL_EUCLEAN);
    int r = bytes(m, m->fat_start + (uint64_t)m->active_fat * m->fat_bytes + c * 4ULL, p, 4, 0);
    if (!r) *value = le32(p) & 0x0fffffff;
    return r;
}
static int fat_set(struct fat_mount *m, uint32_t c, uint32_t value)
{
    for (unsigned j = 0; j < m->fats; j++) {
        if (!m->mirrored && j != m->active_fat) continue;
        uint64_t at = m->fat_start + j * m->fat_bytes + c * 4ULL;
        uint8_t p[4]; int r = bytes(m, at, p, 4, 0);
        if (r) return r;
        put32(p, (le32(p) & 0xf0000000) | value);
        r = bytes(m, at, p, 4, 1);
        if (r) return r;
    }
    return 0;
}
static int next_cluster(struct fat_mount *m, uint32_t c, uint32_t *next)
{
    uint32_t v; int r = fat_get(m, c, &v);
    if (r) return r;
    if (v >= 0x0ffffff8) v = 0;
    else if (!valid_cluster(m, v)) return fail(m, -KERNEL_EUCLEAN);
    *next = v; return 0;
}
static int measure(struct fat_mount *m, struct fat_inode *i)
{
    if (i->measured) return 0;
    uint32_t c = i->first, slow = c, fast = c, count = 0, tail = 0;
    while (c) {
        if (!valid_cluster(m, c) || ++count > m->cluster_count) return fail(m, -KERNEL_EUCLEAN);
        tail = c;
        int r = next_cluster(m, c, &c); if (r) return r;
        if (slow) { r = next_cluster(m, slow, &slow); if (r) return r; }
        if (fast) { r = next_cluster(m, fast, &fast); if (r) return r; }
        if (fast) { r = next_cluster(m, fast, &fast); if (r) return r; }
        if (slow && slow == fast) return fail(m, -KERNEL_EUCLEAN);
    }
    if ((uint64_t)count * m->cluster_size < i->size) return fail(m, -KERNEL_EUCLEAN);
    i->clusters = count; i->tail = tail; i->measured = 1;
    return 0;
}
static int cluster_index(struct fat_mount *m, struct fat_inode *i, uint32_t index, uint32_t *out)
{
    int r = measure(m, i); if (r) return r;
    if (index >= i->clusters) return fail(m, -KERNEL_EUCLEAN);
    uint32_t c = i->first, at = 0;
    if (i->cursor && i->cursor_index <= index) { c = i->cursor; at = i->cursor_index; }
    while (at++ < index) { r = next_cluster(m, c, &c); if (r) return r; }
    if (!c) return fail(m, -KERNEL_EUCLEAN);
    i->cursor = c; i->cursor_index = index; *out = c; return 0;
}
static int allocate_cluster(struct fat_mount *m, uint32_t *out)
{
    uint32_t c = m->next_free;
    if (!valid_cluster(m, c)) c = 2;
    for (uint32_t n = 0; n < m->cluster_count; n++) {
        uint32_t value; int r = fat_get(m, c, &value); if (r) return r;
        if (!value) {
            /* A new chain never exposes another file's old bytes. Publish the
             * FAT link only after every sector of this cluster was cleared. */
            for (uint32_t at = 0; at < m->cluster_size; at += m->sector_size) {
                r = sector_write(m, cluster_at(m, c) + at, m->zeros);
                if (r) return r;
            }
            r = fat_set(m, c, 0x0fffffff); if (r) return r;
            if (!m->free_clusters) return fail(m, -KERNEL_EUCLEAN);
            m->free_clusters--; m->next_free = c + 1; *out = c; return 0;
        }
        if (++c == m->cluster_count + 2) c = 2;
    }
    return -KERNEL_ENOSPC;
}
static int grow_chain(struct fat_mount *m, struct fat_inode *i, uint32_t count)
{
    int r = measure(m, i); if (r) return r;
    while (i->clusters < count) {
        uint32_t c; r = allocate_cluster(m, &c); if (r) return r;
        if (i->tail) {
            r = fat_set(m, i->tail, c);
            if (r) return r;
        } else i->first = c;
        i->tail = c; i->clusters++;
    }
    return 0;
}
static int free_chain(struct fat_mount *m, uint32_t c)
{
    for (uint32_t n = 0; c; n++) {
        if (n >= m->cluster_count) return fail(m, -KERNEL_EUCLEAN);
        uint32_t next; int r = next_cluster(m, c, &next); if (r) return r;
        r = fat_set(m, c, 0); if (r) return r;
        m->free_clusters++; if (c < m->next_free) m->next_free = c;
        c = next;
    }
    return 0;
}
static int trim_chain(struct fat_mount *m, struct fat_inode *i, uint32_t count)
{
    int r = measure(m, i); if (r || count >= i->clusters) return r;
    uint32_t rest = i->first, tail = 0;
    if (count) {
        r = cluster_index(m, i, count - 1, &tail); if (r) return r;
        r = next_cluster(m, tail, &rest); if (r) return r;
        r = fat_set(m, tail, 0x0fffffff); if (r) return r;
    } else i->first = 0;
    i->clusters = count; i->tail = tail; i->cursor = 0;
    return free_chain(m, rest);
}
static int chain_bytes(struct fat_mount *m, struct fat_inode *i, uint64_t at,
    void *buffer, size_t count, int write, size_t *done)
{
    *done = 0;
    while (*done < count) {
        uint32_t c; int r = cluster_index(m, i, at / m->cluster_size, &c);
        if (r) return r;
        size_t in = at % m->cluster_size, n = m->cluster_size - in;
        if (n > count - *done) n = count - *done;
        r = bytes(m, cluster_at(m, c) + in, (uint8_t *)buffer + *done, n, write);
        if (r) return r;
        at += n; *done += n;
    }
    return 0;
}
static int zero_range(struct fat_mount *m, struct fat_inode *i, uint64_t from, uint64_t end)
{
    while (from < end) {
        size_t n = end - from < m->sector_size ? end - from : m->sector_size, done;
        int r = chain_bytes(m, i, from, m->zeros, n, 1, &done);
        if (r) return r;
        from += done;
    }
    return 0;
}

struct fat_record {
    uint8_t raw[32], slots;
    uint64_t entry, first_slot, next;
    char name[766]; /* 255 UTF-16 units may occupy three UTF-8 bytes each. */
};
static const uint8_t lfn_offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
static unsigned upper(unsigned c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int contains(const char *text, unsigned c)
{ while (*text) if ((unsigned char)*text++ == c) return 1; return 0; }
static int same_name(const char *a, const char *b, size_t n)
{
    if (strlen(a) != n) return 0;
    for (size_t j = 0; j < n; j++) if (upper((uint8_t)a[j]) != upper((uint8_t)b[j])) return 0;
    return 1;
}
static uint8_t short_checksum(const uint8_t *p)
{
    uint8_t sum = 0;
    for (unsigned j = 0; j < 11; j++) sum = ((sum & 1) << 7) + (sum >> 1) + p[j];
    return sum;
}
static int utf16_name(const uint16_t *u, size_t units, char *out)
{
    size_t n = 0;
    for (size_t j = 0; j < units && u[j] && u[j] != 0xffff; j++) {
        if (j >= 255) return -KERNEL_EILSEQ;
        uint32_t c = u[j];
        if (c >= 0xd800 && c <= 0xdbff) {
            if (j + 1 >= units || u[j + 1] < 0xdc00 || u[j + 1] > 0xdfff) return -KERNEL_EILSEQ;
            c = 0x10000 + ((c - 0xd800) << 10) + u[++j] - 0xdc00;
        } else if (c >= 0xdc00 && c <= 0xdfff) return -KERNEL_EILSEQ;
        if (c < 0x80) out[n++] = c;
        else if (c < 0x800) { out[n++] = 0xc0 | (c >> 6); out[n++] = 0x80 | (c & 63); }
        else if (c < 0x10000) {
            out[n++] = 0xe0 | (c >> 12); out[n++] = 0x80 | ((c >> 6) & 63); out[n++] = 0x80 | (c & 63);
        } else {
            out[n++] = 0xf0 | (c >> 18); out[n++] = 0x80 | ((c >> 12) & 63);
            out[n++] = 0x80 | ((c >> 6) & 63); out[n++] = 0x80 | (c & 63);
        }
    }
    out[n] = 0; return 0;
}
static int encode_name(const char *name, size_t n, uint16_t u[260], size_t *units)
{
    size_t count = 0;
    if (!n || name[n - 1] == ' ' || name[n - 1] == '.') return -KERNEL_EINVAL;
    for (size_t j = 0; j < n;) {
        uint32_t c = (uint8_t)name[j++], min = 0;
        unsigned more = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; more = 1; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; more = 2; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; more = 3; min = 0x10000; }
        else if (c >= 0x80) return -KERNEL_EILSEQ;
        if (more > n - j) return -KERNEL_EILSEQ;
        while (more--) {
            unsigned d = (uint8_t)name[j++];
            if ((d & 0xc0) != 0x80) return -KERNEL_EILSEQ;
            c = (c << 6) | (d & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -KERNEL_EILSEQ;
        if (c < 32 || c == 127 || (c < 128 && contains("\"*/:<>?\\|", c))) return -KERNEL_EINVAL;
        if (count + (c >= 0x10000 ? 2U : 1U) > 255) return -KERNEL_ENAMETOOLONG;
        if (c >= 0x10000) { c -= 0x10000; u[count++] = 0xd800 | (c >> 10); u[count++] = 0xdc00 | (c & 1023); }
        else u[count++] = c;
    }
    *units = count;
    while (count < 260) u[count++] = 0xffff;
    if (*units < 260) u[*units] = 0;
    return 0;
}
static void short_name(const uint8_t raw[32], char *name)
{
    size_t n = 0;
    for (unsigned j = 0; j < 8 && raw[j] != ' '; j++) {
        unsigned c = raw[j]; if (!j && c == 5) c = 0xe5;
        if ((raw[12] & 8) && c >= 'A' && c <= 'Z') c += 32;
        name[n++] = c;
    }
    if (raw[8] != ' ') name[n++] = '.';
    for (unsigned j = 8; j < 11 && raw[j] != ' '; j++) {
        unsigned c = raw[j]; if ((raw[12] & 16) && c >= 'A' && c <= 'Z') c += 32;
        name[n++] = c;
    }
    name[n] = 0;
}
static int directory_slot(struct fat_mount *m, struct fat_inode *dir, uint64_t slot, uint64_t *at)
{
    uint64_t offset = slot * 32;
    if (slot >= 65536) return -KERNEL_ENOSPC;
    int r = measure(m, dir); if (r) return r;
    if (offset >= (uint64_t)dir->clusters * m->cluster_size) return 0;
    uint32_t c; r = cluster_index(m, dir, offset / m->cluster_size, &c);
    if (r) return r;
    *at = cluster_at(m, c) + offset % m->cluster_size; return 1;
}
static int read_record(struct fat_mount *m, struct fat_inode *dir, uint64_t from, struct fat_record *out)
{
    uint16_t unicode[260]; unsigned expected = 0, total = 0; uint8_t checksum = 0;
    uint64_t first_slot = 0;
    for (uint64_t slot = from; slot < 65536; slot++) {
        uint64_t at; int r = directory_slot(m, dir, slot, &at);
        if (r <= 0) return r;
        uint8_t *p = out->raw;
        r = bytes(m, at, p, 32, 0); if (r) return r;
        if (!p[0]) return 0;
        if (p[0] == 0xe5) { expected = total = 0; continue; }
        if (p[11] == 15) {
            unsigned sequence = p[0] & 31;
            if ((p[0] & 0x40) && sequence && sequence <= 20) {
                total = expected = sequence; checksum = p[13]; first_slot = slot;
                memset(unicode, 0xff, sizeof(unicode));
            }
            if (!expected || sequence != expected || p[13] != checksum || p[12] || le16(p + 26)) {
                expected = total = 0; continue;
            }
            for (unsigned j = 0; j < 13; j++) unicode[(sequence - 1) * 13 + j] = le16(p + lfn_offsets[j]);
            expected--; continue;
        }
        if (p[11] & 8) { expected = total = 0; continue; }
        int long_name = total && !expected && short_checksum(p) == checksum;
        if (long_name && utf16_name(unicode, total * 13, out->name)) long_name = 0;
        if (!long_name) short_name(p, out->name);
        out->entry = at; out->next = slot + 1;
        out->first_slot = long_name ? first_slot : slot;
        out->slots = long_name ? total + 1 : 1;
        return 1;
    }
    return 0;
}
static int find_record(struct fat_mount *m, struct fat_inode *dir, const char *name, size_t length, struct fat_record *out)
{
    for (uint64_t pos = 0;; pos = out->next) {
        int r = read_record(m, dir, pos, out); if (r <= 0) return r ? r : -KERNEL_ENOENT;
        char alias[16]; short_name(out->raw, alias);
        if (same_name(out->name, name, length) || same_name(alias, name, length)) return 0;
    }
}
static struct fat_inode *inode_id(struct fat_mount *m, uint64_t id)
{
    struct fat_inode key = {.id = id}; return RB_FIND(fat_id_tree, &m->inodes, &key);
}
static int record_inode(struct fat_mount *m, uint64_t parent, const struct fat_record *record, struct fat_inode **out)
{
    struct fat_inode key = {.entry = record->entry};
    struct fat_inode *i = RB_FIND(fat_entry_tree, &m->entries, &key);
    if (!i) {
        if (m->next_id == UINT64_MAX) return -KERNEL_EOVERFLOW;
        int r = alloc(m, sizeof(*i), (void **)&i); if (r) return r;
        i->id = m->next_id++; i->parent = parent; i->entry = record->entry;
        i->first_slot = record->first_slot; i->slots = record->slots; i->linked = 1;
        i->first = ((uint32_t)le16(record->raw + 20) << 16) | le16(record->raw + 26);
        i->first &= 0x0fffffff; i->size = le32(record->raw + 28);
        i->mode = (record->raw[11] & 16 ? KERNEL_VFS_S_IFDIR : KERNEL_VFS_S_IFREG) | 0777;
        if (record->raw[11] & 1) i->mode &= ~0222U;
        if (record->raw[11] & 16) i->size = 0;
        i->atime = from_dos(le16(record->raw + 18), 0);
        i->mtime = from_dos(le16(record->raw + 24), le16(record->raw + 22));
        i->ctime = from_dos(le16(record->raw + 16), le16(record->raw + 14));
        if (RB_INSERT(fat_id_tree, &m->inodes, i) || RB_INSERT(fat_entry_tree, &m->entries, i)) __builtin_trap();
    }
    *out = i; return 0;
}
static int child_inode(struct fat_mount *m, struct fat_inode *parent, const char *name, size_t n, struct fat_inode **out)
{
    if ((parent->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
    if (n == 1 && name[0] == '.') { *out = parent; return 0; }
    if (n == 2 && !memcmp(name, "..", 2)) { *out = inode_id(m, parent->parent); return *out ? 0 : -KERNEL_EUCLEAN; }
    struct fat_record record; int r = find_record(m, parent, name, n, &record);
    return r ? r : record_inode(m, parent->id, &record, out);
}
static int parent_path(struct fat_mount *m, const char *path, struct fat_inode **parent, const char **name, size_t *length)
{
    if (!path || *path != '/') return -KERNEL_EINVAL;
    struct fat_inode *i = inode_id(m, 1);
    while (*path == '/') path++;
    for (;;) {
        const char *end = path; while (*end && *end != '/') end++;
        size_t n = end - path;
        if (!n || n > 255) return n ? -KERNEL_ENAMETOOLONG : -KERNEL_EINVAL;
        const char *next = end; while (*next == '/') next++;
        if (!*next) { *parent = i; *name = path; *length = n; return 0; }
        int r = child_inode(m, i, path, n, &i); if (r) return r;
        path = next;
    }
}
static int resolve_path(struct fat_mount *m, const char *path, struct fat_inode **out)
{
    if (path && !strcmp(path, "/")) { *out = inode_id(m, 1); return 0; }
    struct fat_inode *parent; const char *name; size_t n;
    int r = parent_path(m, path, &parent, &name, &n);
    return r ? r : child_inode(m, parent, name, n, out);
}
static int leap(unsigned y) { return !(y % 4) && (y % 100 || !(y % 400)); }
static const uint8_t month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
static uint32_t dos_time(struct kernel_vfs_timespec time)
{
    uint64_t seconds = time.seconds > 315532800 ? time.seconds - 315532800 : 0;
    unsigned days = seconds / 86400, year = 1980, month = 0;
    while (year < 2107 && days >= 365U + leap(year)) { days -= 365U + leap(year); year++; }
    while (month < 11 && days >= (unsigned)month_days[month] + (month == 1 && leap(year))) {
        days -= month_days[month] + (month == 1 && leap(year)); month++;
    }
    uint16_t date = ((year - 1980) << 9) | ((month + 1) << 5) | (days + 1);
    uint16_t clock = ((seconds / 3600 % 24) << 11) | ((seconds / 60 % 60) << 5) | (seconds % 60 / 2);
    return (uint32_t)date << 16 | clock;
}
static struct kernel_vfs_timespec from_dos(uint16_t date, uint16_t clock)
{
    unsigned y = 1980 + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
    if (!mo || mo > 12 || !d || d > (unsigned)month_days[mo - 1] + (mo == 2 && leap(y)) ||
        (clock >> 11) > 23 || ((clock >> 5) & 63) > 59 || (clock & 31) > 29)
        return (struct kernel_vfs_timespec){0};
    uint64_t days = 3652;
    for (unsigned j = 1980; j < y; j++) days += 365 + leap(j);
    for (unsigned j = 1; j < mo; j++) days += month_days[j - 1] + (j == 2 && leap(y));
    days += d - 1;
    return (struct kernel_vfs_timespec){days * 86400 + (clock >> 11) * 3600 + ((clock >> 5) & 63) * 60 + (clock & 31) * 2, 0};
}
static void entry_times(uint8_t raw[32], struct fat_inode *i)
{
    uint32_t c = dos_time(i->ctime), a = dos_time(i->atime), t = dos_time(i->mtime);
    put32(raw + 14, c); put16(raw + 18, a >> 16); put32(raw + 22, t);
}
static int store_inode(struct fat_mount *m, struct fat_inode *i)
{
    if (!i->linked || i->id == 1) { i->times_dirty = 0; return 0; }
    uint8_t raw[32]; int r = bytes(m, i->entry, raw, 32, 0); if (r) return r;
    if (!raw[0] || raw[0] == 0xe5 || raw[11] == 15) return fail(m, -KERNEL_EUCLEAN);
    put16(raw + 20, i->first >> 16); put16(raw + 26, i->first);
    put32(raw + 28, (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? 0 : i->size);
    raw[11] = (raw[11] & ~1U) | ((i->mode & 0222) ? 0 : 1);
    entry_times(raw, i);
    r = bytes(m, i->entry, raw, 32, 1);
    if (!r) i->times_dirty = 0;
    return r;
}
static int free_slots(struct fat_mount *m, struct fat_inode *dir, unsigned count, uint64_t *first, int *past_end)
{
    unsigned run = 0; uint64_t start = 0; int end = 0;
    for (uint64_t slot = 0; slot < 65536; slot++) {
        uint64_t at; int r = directory_slot(m, dir, slot, &at);
        if (r < 0 && r != -KERNEL_ENOSPC) return r;
        if (r <= 0) {
            r = grow_chain(m, dir, slot * 32 / m->cluster_size + 1);
            if (r) return r;
            end = 1;
        }
        uint8_t tag = 0;
        if (!end) { r = bytes(m, at, &tag, 1, 0); if (r) return r; }
        if (!tag) end = 1;
        if (end || tag == 0xe5) {
            if (!run) start = slot;
            if (++run == count) { *first = start; *past_end = end; return 0; }
        } else run = 0;
    }
    return -KERNEL_ENOSPC;
}
static int short_exists(struct fat_mount *m, struct fat_inode *dir, const uint8_t name[11])
{
    struct fat_record record;
    for (uint64_t pos = 0;; pos = record.next) {
        int r = read_record(m, dir, pos, &record); if (r <= 0) return r;
        if (!memcmp(record.raw, name, 11)) return 1;
    }
}
static int short_character(unsigned c)
{ return c >= 33 && c < 127 && !contains("\"*+,./:;<=>?[\\]|", c); }
static int make_short(struct fat_mount *m, struct fat_inode *dir, const char *name, size_t n, uint8_t out[11])
{
    const char *dot = 0;
    for (size_t j = 0; j < n; j++) if (name[j] == '.') dot = name + j;
    size_t base = dot ? (size_t)(dot - name) : n, ext = dot ? n - base - 1 : 0;
    int direct = base && base <= 8 && ext <= 3;
    memset(out, ' ', 11);
    for (size_t j = 0; j < n; j++) if ((name + j != dot && !short_character((uint8_t)name[j])) ||
        ((uint8_t)name[j] >= 'a' && (uint8_t)name[j] <= 'z')) direct = 0;
    if (direct) {
        memcpy(out, name, base); if (ext) memcpy(out + 8, dot + 1, ext);
        int r = short_exists(m, dir, out); if (r <= 0) return r;
    }
    uint8_t stem[8]; size_t stem_n = 0;
    for (size_t j = 0; j < base && stem_n < 8; j++) {
        unsigned c = (uint8_t)name[j];
        if (c == ' ' || c == '.') continue;
        stem[stem_n++] = short_character(c) ? upper(c) : '_';
    }
    if (!stem_n) stem[stem_n++] = '_';
    for (unsigned serial = 1; serial < 1000000; serial++) {
        char digits[6]; unsigned d = 0, v = serial;
        do { digits[d++] = '0' + v % 10; v /= 10; } while (v);
        memset(out, ' ', 11);
        size_t prefix = stem_n < 7 - d ? stem_n : 7 - d;
        memcpy(out, stem, prefix); out[prefix++] = '~';
        while (d) out[prefix++] = digits[--d];
        for (size_t j = 0; j < ext && j < 3; j++) {
            unsigned c = (uint8_t)dot[j + 1]; out[8 + j] = short_character(c) ? upper(c) : '_';
        }
        int r = short_exists(m, dir, out); if (r <= 0) return r;
    }
    return -KERNEL_ENOSPC;
}
static int write_slot(struct fat_mount *m, struct fat_inode *dir, uint64_t slot, uint8_t raw[32])
{
    uint64_t at; int r = directory_slot(m, dir, slot, &at);
    return r < 0 ? r : !r ? fail(m, -KERNEL_EUCLEAN) : bytes(m, at, raw, 32, 1);
}
static int erase_slots(struct fat_mount *m, struct fat_inode *dir, uint64_t slot, unsigned count)
{
    /* Remove the short entry first. Its preceding LFN fragments alone never
     * name a file if a subsequent media operation fails. */
    while (count) {
        uint64_t at; int r = directory_slot(m, dir, slot + --count, &at);
        if (r <= 0) return r ? r : fail(m, -KERNEL_EUCLEAN);
        uint8_t deleted = 0xe5; r = bytes(m, at, &deleted, 1, 1); if (r) return r;
    }
    return 0;
}
static int add_entry(struct fat_mount *m, struct fat_inode *dir, const char *name, size_t n,
    const uint8_t original[32], struct fat_record *record)
{
    uint16_t unicode[260]; size_t units;
    int r = encode_name(name, n, unicode, &units); if (r) return r;
    uint8_t alias[11]; r = make_short(m, dir, name, n, alias); if (r) return r;
    uint8_t raw[32]; memcpy(raw, original, 32); memcpy(raw, alias, 11); raw[12] = 0;
    char short_text[16]; short_name(raw, short_text);
    unsigned long_slots = strlen(short_text) == n && !memcmp(short_text, name, n) ? 0 : (units + 12) / 13;
    uint64_t start = 0; int past_end = 0;
    r = free_slots(m, dir, long_slots + 1, &start, &past_end); if (r) return r;
    if (past_end && start + long_slots + 1 < 65536) {
        uint64_t at; r = directory_slot(m, dir, start + long_slots + 1, &at);
        if (r < 0) return r;
        if (r) {
            uint8_t end = 0;
            /* Bytes beyond the old terminator can contain deleted history.
             * Move the terminator before publishing the new directory entry. */
            r = bytes(m, at, &end, 1, 1); if (r) return r;
        }
    }
    uint8_t checksum = short_checksum(alias);
    for (unsigned j = long_slots; j; j--) {
        uint8_t fragment[32]; memset(fragment, 0xff, 32);
        fragment[0] = j | (j == long_slots ? 0x40 : 0); fragment[11] = 15;
        fragment[12] = 0; fragment[13] = checksum; put16(fragment + 26, 0);
        for (unsigned k = 0; k < 13; k++) put16(fragment + lfn_offsets[k], unicode[(j - 1) * 13 + k]);
        r = write_slot(m, dir, start + long_slots - j, fragment); if (r) return r;
    }
    r = write_slot(m, dir, start + long_slots, raw); if (r) return r;
    uint64_t at; r = directory_slot(m, dir, start + long_slots, &at);
    if (r <= 0) return r ? r : fail(m, -KERNEL_EUCLEAN);
    *record = (struct fat_record){.entry = at, .first_slot = start, .slots = long_slots + 1, .next = start + long_slots + 1};
    memcpy(record->raw, raw, 32); memcpy(record->name, name, n); record->name[n] = 0;
    dir->mtime = dir->ctime = now(); return store_inode(m, dir);
}
static int fat_error(struct kernel_vfs_instance *instance)
{ return __atomic_load_n(&fm(instance)->error, __ATOMIC_ACQUIRE); }
static int root(struct kernel_vfs_instance *instance, uint64_t *id, uint32_t *mode)
{
    struct fat_mount *m = fm(instance); *id = 1; *mode = inode_id(m, 1)->mode; return fat_error(instance);
}
static int lookup(struct kernel_vfs_instance *instance, uint64_t parent, const char *name, size_t n,
    uint64_t *id, uint32_t *mode)
{
    struct fat_mount *m = fm(instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(instance); if (r) return r;
    struct fat_inode *p = inode_id(m, parent), *i;
    if (!p || !p->linked) return -KERNEL_ENOENT;
    r = child_inode(m, p, name, n, &i);
    if (!r) { *id = i->id; *mode = i->mode; }
    return r;
}
static int entry_name(struct kernel_vfs_instance *instance, uint64_t id, char *name, size_t capacity)
{
    struct fat_mount *m = fm(instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    struct fat_inode *i = inode_id(m, id);
    if (!i || !i->linked) return -KERNEL_ENOENT;
    struct fat_inode *parent = inode_id(m, i->parent);
    if (!parent) return fail(m, -KERNEL_EUCLEAN);
    struct fat_record record; int r = read_record(m, parent, i->first_slot, &record);
    if (r < 0) return r;
    if (!r || record.entry != i->entry) return fail(m, -KERNEL_EUCLEAN);
    size_t n = strlen(record.name); if (n >= capacity) return -KERNEL_ENAMETOOLONG;
    memcpy(name, record.name, n + 1); return 0;
}
static int open_inode(struct kernel_vfs_mount *mount, const char *path, uint64_t id,
    uint32_t mode, struct kernel_vfs_file *file)
{
    (void)mode;
    struct fat_mount *m = fm(mount->private_data); struct fat_inode *i = 0;
    struct kernel_vfs_node *node = 0;
    {
        KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
        int r = fat_error(&m->instance); if (r) return r;
        if (id) i = inode_id(m, id);
        else { r = resolve_path(m, path, &i); if (r) return r; }
        if (!i || !i->linked) return -KERNEL_ENOENT;
        r = alloc(m, sizeof(*node), (void **)&node); if (r) return r;
        node->inode = i->id; node->mode = i->mode; node->size = i->size;
        node->max_size = UINT32_MAX; node->backend_data = i;
        __atomic_add_fetch(&i->handles, 1, __ATOMIC_RELAXED);
    }
    return kernel_vfs_publish_node(mount, node, file, 0);
}
static int close_node(struct kernel_vfs_node *node)
{
    struct fat_inode *i = fi(node);
    unsigned handles = __atomic_fetch_sub(&i->handles, 1, __ATOMIC_ACQ_REL);
    if (!handles) __builtin_trap();
    if (handles == 1 && !__atomic_load_n(&i->linked, __ATOMIC_ACQUIRE))
        retire_inode(fm(node->instance), i);
    return 0;
}
static int create_child(struct fat_mount *m, uint64_t parent_inode, const char *path, uint32_t mode, struct fat_inode **out)
{
    if (m->instance.read_only) return -KERNEL_EROFS;
    collect_retired(m);
    struct fat_inode *parent; const char *name; size_t n;
    parent = inode_id(m, parent_inode);
    if (!parent || !parent->linked) return -KERNEL_ENOENT;
    name = path; n = strlen(name);
    int r;
    struct fat_record record;
    r = find_record(m, parent, name, n, &record);
    if (r != -KERNEL_ENOENT) return r ? r : -KERNEL_EEXIST;
    int directory = (mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    struct fat_inode fresh = {.mode = mode, .linked = 1, .measured = 1};
    fresh.atime = fresh.mtime = fresh.ctime = now();
    if (directory) {
        r = grow_chain(m, &fresh, 1); if (r) return r;
        uint8_t dot[32] = {0}; memcpy(dot, ".          ", 11); dot[11] = 16;
        put16(dot + 20, fresh.first >> 16); put16(dot + 26, fresh.first); entry_times(dot, &fresh);
        r = bytes(m, cluster_at(m, fresh.first), dot, 32, 1);
        uint32_t p = parent->id == 1 ? 0 : parent->first;
        memcpy(dot, "..         ", 11); put16(dot + 20, p >> 16); put16(dot + 26, p);
        if (!r) r = bytes(m, cluster_at(m, fresh.first) + 32, dot, 32, 1);
        if (r) return r;
    }
    uint8_t raw[32] = {0}; raw[11] = directory ? 16 : 32;
    if (!(mode & 0222)) raw[11] |= 1;
    put16(raw + 20, fresh.first >> 16); put16(raw + 26, fresh.first); entry_times(raw, &fresh);
    r = add_entry(m, parent, name, n, raw, &record);
    if (r) { if (!m->error && fresh.first) (void)free_chain(m, fresh.first); return r; }
    struct fat_inode *i; r = record_inode(m, parent->id, &record, &i);
    if (r) {
        int cleanup = erase_slots(m, parent, record.first_slot, record.slots);
        if (!cleanup && fresh.first) cleanup = free_chain(m, fresh.first);
        return cleanup ? cleanup : r;
    }
    if (out) *out = i;
    return 0;
}
static int create(struct kernel_vfs_mount *mount, uint64_t parent_inode, const char *path, uint32_t mode, struct kernel_vfs_file *file)
{
    struct fat_mount *m = fm(mount->private_data); uint64_t id;
    {
        KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
        int r = fat_error(&m->instance); if (r) return r;
        struct fat_inode *i; r = create_child(m, parent_inode, path, KERNEL_VFS_S_IFREG | (mode & 0777), &i);
        if (r) return r;
        id = i->id;
    }
    return open_inode(mount, 0, id, 0, file);
}
static int mkdir_inode(struct kernel_vfs_mount *mount, uint64_t parent_inode, const char *path, uint32_t mode)
{
    struct fat_mount *m = fm(mount->private_data);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(&m->instance);
    return r ? r : create_child(m, parent_inode, path, KERNEL_VFS_S_IFDIR | (mode & 0777), 0);
}
static int empty_directory(struct fat_mount *m, struct fat_inode *i)
{
    struct fat_record record;
    for (uint64_t pos = 0;; pos = record.next) {
        int r = read_record(m, i, pos, &record); if (r <= 0) return r ? r : 1;
        if (strcmp(record.name, ".") && strcmp(record.name, "..")) return 0;
    }
}
static void unlink_identity(struct fat_mount *m, struct fat_inode *i)
{
    if (!i->linked || RB_REMOVE(fat_entry_tree, &m->entries, i) != i) __builtin_trap();
    __atomic_store_n(&i->linked, 0, __ATOMIC_RELEASE);
    uintptr_t irq = arch_interrupt_save();
    KERNEL_TASK_LOCK_SCOPE(vfs_state, &kernel_vfs_state_lock);
    struct kernel_vfs_node *node = kernel_vfs_node_find(&m->instance, i->id);
    if (node) node->unlinked = 1;
    arch_interrupt_restore(irq);
}
static int remove_path(struct kernel_vfs_mount *mount, uint64_t parent_inode, const char *path, int directory)
{
    struct fat_mount *m = fm(mount->private_data);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(&m->instance); if (r) return r;
    struct fat_inode *parent, *i; const char *name; size_t n;
    parent = inode_id(m, parent_inode);
    if (!parent || !parent->linked) return -KERNEL_ENOENT;
    name = path; n = strlen(name);
    r = child_inode(m, parent, name, n, &i); if (r) return r;
    int isdir = (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if (directory != isdir) return directory ? -KERNEL_ENOTDIR : -KERNEL_EISDIR;
    if (isdir) { r = empty_directory(m, i); if (r <= 0) return r ? r : -KERNEL_ENOTEMPTY; }
    r = erase_slots(m, parent, i->first_slot, i->slots); if (r) return r;
    unlink_identity(m, i); parent->mtime = parent->ctime = i->ctime = now();
    /* No node exists for a name that has only been looked up. Otherwise the
     * common VFS closes writeback and mapping users before releasing content. */
    if (!__atomic_load_n(&i->handles, __ATOMIC_ACQUIRE)) {
        r = trim_chain(m, i, 0); i->size = 0;
        if (!r) retire_inode(m, i);
    }
    return r ? r : store_inode(m, parent);
}
static int unlink_file(struct kernel_vfs_mount *m, uint64_t parent_inode, const char *p) { return remove_path(m, parent_inode, p, 0); }
static int rmdir_inode(struct kernel_vfs_mount *m, uint64_t parent_inode, const char *p) { return remove_path(m, parent_inode, p, 1); }
static void release_unlinked(struct kernel_vfs_node *node)
{
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node, &io, 1);
    if (!node->unlinked || node->retired || node->open_files || node->exec_users) return;
    struct fat_mount *m = fm(node->instance);
    enum kernel_page_cache_status status = kernel_page_cache_invalidate_node(node->instance->page_cache, node);
    if (status != KERNEL_PAGE_CACHE_STATUS_OK) return;
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance);
    if (!r) r = trim_chain(m, fi(node), 0);
    if (r) kernel_vfs_record_writeback_error(node, r);
    else { fi(node)->size = 0; node->retired = 1; }
}
static int rename_inode(struct kernel_vfs_instance *instance, uint64_t old_parent, const char *old_name,
    uint64_t new_parent, const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result)
{
    struct fat_mount *m = fm(instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(instance); if (r) return r;
    struct fat_inode *op = inode_id(m, old_parent), *np = inode_id(m, new_parent), *i, *dest = 0;
    if (!op || !np) return -KERNEL_ENOENT;
    r = child_inode(m, op, old_name, strlen(old_name), &i); if (r) return r;
    r = child_inode(m, np, new_name, strlen(new_name), &dest);
    if (r && r != -KERNEL_ENOENT) return r;
    if (dest && (flags & 1)) return -KERNEL_EEXIST;
    if (dest == i) {
        if (!strcmp(old_name, new_name) && op == np) return 0;
        /* A case-only rename must still replace the on-disk spelling. */
        dest = 0;
    }
    int directory = (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if (dest) {
        int dd = (dest->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
        if (dd != directory) return directory ? -KERNEL_ENOTDIR : -KERNEL_EISDIR;
        if (dd) { r = empty_directory(m, dest); if (r <= 0) return r ? r : -KERNEL_ENOTEMPTY; }
    }
    if (directory) for (struct fat_inode *p = np;; p = inode_id(m, p->parent)) {
        if (!p) return fail(m, -KERNEL_EUCLEAN);
        if (p == i) return -KERNEL_EINVAL;
        if (p->id == 1) break;
    }
    uint8_t raw[32]; r = bytes(m, i->entry, raw, 32, 0); if (r) return r;
    struct fat_record replacement;
    /* Reserve the complete destination before altering either existing name.
     * FAT has no journal; media failures retain the mount and its dirty flag. */
    r = add_entry(m, np, new_name, strlen(new_name), raw, &replacement); if (r) return r;
    if (directory && op != np) {
        uint8_t dotdot[32]; size_t done;
        r = chain_bytes(m, i, 32, dotdot, 32, 0, &done);
        if (!r && memcmp(dotdot, "..         ", 11)) r = fail(m, -KERNEL_EUCLEAN);
        uint32_t p = np->id == 1 ? 0 : np->first;
        if (!r) {
            put16(dotdot + 20, p >> 16); put16(dotdot + 26, p);
            r = chain_bytes(m, i, 32, dotdot, 32, 1, &done);
        }
        if (r) return r;
    }
    if (dest) {
        r = erase_slots(m, np, dest->first_slot, dest->slots); if (r) return r;
        result->replaced_inode = dest->id; result->replaced_last_link = 1;
        unlink_identity(m, dest);
        if (!__atomic_load_n(&dest->handles, __ATOMIC_ACQUIRE)) {
            r = trim_chain(m, dest, 0); if (r) return r;
            dest->size = 0;
            retire_inode(m, dest);
        }
    }
    r = erase_slots(m, op, i->first_slot, i->slots); if (r) return r;
    if (RB_REMOVE(fat_entry_tree, &m->entries, i) != i) __builtin_trap();
    i->entry = replacement.entry; i->first_slot = replacement.first_slot; i->slots = replacement.slots;
    i->parent = new_parent; i->ctime = op->mtime = op->ctime = np->mtime = np->ctime = now();
    if (RB_INSERT(fat_entry_tree, &m->entries, i)) __builtin_trap();
    result->changed = 1;
    r = store_inode(m, i);
    if (!r) r = store_inode(m, op);
    if (!r && np != op) r = store_inode(m, np);
    return r;
}
static int dir_entry(struct kernel_vfs_file *file, uint64_t pos, uint64_t *next, uint64_t *id,
    uint8_t *type, char *name, size_t capacity)
{
    struct kernel_vfs_node *node = file->private_data;
    struct fat_mount *m = fm(node->instance); struct fat_inode *dir = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance); if (r) return r;
    if ((dir->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
    if (pos < 2) {
        if (capacity < 3) return -KERNEL_ENAMETOOLONG;
        strcpy(name, pos ? ".." : "."); *next = pos + 1; *id = pos ? dir->parent : dir->id; *type = 4; return 1;
    }
    struct fat_record record;
    for (uint64_t slot = pos - 2;; slot = record.next) {
        r = read_record(m, dir, slot, &record); if (r <= 0) return r;
        if (!strcmp(record.name, ".") || !strcmp(record.name, "..")) continue;
        struct fat_inode *i; r = record_inode(m, dir->id, &record, &i); if (r) return r;
        size_t n = strlen(record.name); if (n >= capacity) return -KERNEL_ENAMETOOLONG;
        memcpy(name, record.name, n + 1); *next = record.next + 2; *id = i->id;
        *type = (i->mode & KERNEL_VFS_S_IFMT) >> 12; return 1;
    }
}
static int pread_inode(struct kernel_vfs_node *node, uint64_t offset, void *buffer, size_t size, size_t *read)
{
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node); *read = 0;
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance); if (r) return r;
    if (offset >= i->size) return 0;
    if (size > i->size - offset) size = i->size - offset;
    return chain_bytes(m, i, offset, buffer, size, 0, read);
}
static int prepare_write(struct kernel_vfs_node *node, uint64_t offset, size_t size, size_t *ready, int *complete)
{
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    *ready = 0; *complete = 0;
    int r = fat_error(node->instance); if (r) return r;
    if (offset > UINT32_MAX || size > UINT32_MAX - offset) return -KERNEL_EFBIG;
    uint64_t end = offset + size, before = node->size;
    if (!size) return 0;
    r = measure(m, i); if (r) return r;
    if (end > before) *complete = 1;
    r = grow_chain(m, i, (end + m->cluster_size - 1) / m->cluster_size);
    if (r && r != -KERNEL_ENOSPC) return r;
    uint64_t available = (uint64_t)i->clusters * m->cluster_size;
    if (available > end) available = end;
    if (offset < available) *ready = available - offset;
    if (offset > before && available > before) {
        int zero = zero_range(m, i, before, offset < available ? offset : available);
        if (zero) { *ready = 0; return zero; }
    }
    int stored = store_inode(m, i);
    return stored ? stored : r;
}
static int writeback(struct kernel_vfs_node *node, struct file_write_span *spans, size_t count)
{
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance);
    for (size_t j = 0; j < count; j++) spans[j].written = 0;
    for (size_t j = 0; !r && j < count; j++) {
        struct file_write_span *s = spans + j;
        if (s->offset > UINT32_MAX || s->size > UINT32_MAX - s->offset) return -KERNEL_EFBIG;
        r = chain_bytes(m, i, s->offset, (void *)s->buffer, s->size, 1, &s->written);
        if (s->written && s->offset + s->written > i->size) i->size = s->offset + s->written;
    }
    if (!r) r = store_inode(m, i);
    return r;
}
static int complete_write(struct kernel_vfs_node *node)
{
    int r = kernel_page_cache_writeback(node->instance->page_cache, node);
    if (r) return r;
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node, &io, 1);
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    r = fat_error(node->instance); if (r) return r;
    i->size = node->size;
    r = trim_chain(m, i, ((uint64_t)i->size + m->cluster_size - 1) / m->cluster_size);
    return r ? r : store_inode(m, i);
}
static int truncate_inode(struct kernel_vfs_node *node, uint64_t size, uint64_t *actual, int *changed)
{
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    *actual = i->size; *changed = 0;
    int r = fat_error(node->instance); if (r) return r;
    if (size > UINT32_MAX) return -KERNEL_EFBIG;
    uint32_t old = i->size, needed = (size + m->cluster_size - 1) / m->cluster_size;
    r = grow_chain(m, i, needed);
    if (r) {
        if (r == -KERNEL_ENOSPC) {
            int rollback = trim_chain(m, i, ((uint64_t)old + m->cluster_size - 1) / m->cluster_size);
            if (rollback) return rollback;
        }
        return r;
    }
    if (size > old) { r = zero_range(m, i, old, size); if (r) return r; }
    if (size < old && size % m->cluster_size) {
        r = zero_range(m, i, size, (size / m->cluster_size + 1) * m->cluster_size); if (r) return r;
    }
    i->size = size; i->mtime = i->ctime = now();
    /* Publish the smaller EOF before releasing clusters. On an error the VFS
     * must retain this committed size and invalidate the same cache suffix. */
    uint32_t first = i->first;
    if (!size) i->first = 0;
    r = store_inode(m, i);
    if (r) { i->first = first; i->size = old; return r; }
    *actual = size; *changed = 1;
    i->first = first;
    return trim_chain(m, i, needed);
}
static struct kernel_block_device *block_device(struct kernel_vfs_instance *instance)
{ return fm(instance)->block; }
static int writeback_allowed(struct kernel_vfs_instance *instance)
{
    (void)instance;
    /* A latched media error is a writeback result, not temporary lock
     * contention. Let the backend report that errno to the sync caller. */
    return 1;
}
static int stat_inode(const struct kernel_vfs_file *file, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_node *node = file->private_data;
    struct fat_mount *m = fm(node->instance); struct fat_inode *i = fi(node);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance); if (r) return r;
    r = measure(m, i); if (r) return r;
    uint64_t size = (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? (uint64_t)i->clusters * m->cluster_size : node->size;
    *stat = (struct kernel_vfs_stat){.dev = m->block->device_number, .ino = i->id, .mode = i->mode,
        .nlink = i->linked ? ((i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? 2 : 1) : 0,
        .size = size, .blocks = (uint64_t)i->clusters * m->cluster_size / 512,
        .blksize = m->cluster_size, .atime = i->atime, .mtime = i->mtime, .ctime = i->ctime};
    return 0;
}
static int statfs(struct kernel_vfs_mount *mount, struct kernel_vfs_statfs *stat)
{
    struct fat_mount *m = fm(mount->private_data);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    *stat = (struct kernel_vfs_statfs){.type = 0x4d44, .block_size = m->cluster_size,
        .blocks = m->cluster_count, .free_blocks = m->free_clusters, .available_blocks = m->free_clusters,
        .fsid = mount->id, .name_length = 255, .flags = m->instance.read_only ? 1 : 0};
    return fat_error(&m->instance);
}
static int time_not_after(struct kernel_vfs_timespec a, struct kernel_vfs_timespec b)
{ return a.seconds < b.seconds || (a.seconds == b.seconds && a.nanoseconds <= b.nanoseconds); }
static void accessed(struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = file->private_data;
    if (node->instance->read_only) return;
    struct fat_mount *m = fm(node->instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    struct fat_inode *i = fi(node); struct kernel_vfs_timespec time = now();
    if (time_not_after(i->atime, i->mtime) || time_not_after(i->atime, i->ctime) ||
        (time.seconds >= i->atime.seconds && (uint64_t)time.seconds - i->atime.seconds >= 86400)) {
        i->atime = time; i->times_dirty = 1;
    }
}
static int modified(struct kernel_vfs_file *file, uint64_t offset, int append)
{
    (void)offset; (void)append;
    struct kernel_vfs_node *node = file->private_data;
    struct fat_mount *m = fm(node->instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance); if (r) return r;
    fi(node)->mtime = fi(node)->ctime = now();
    /* Hot reads/writes can run on the calling CPU. Actual metadata I/O stays
     * with writeback/fsync's CPU0 owner, including a write that later faults. */
    fi(node)->times_dirty = 1;
    return 0;
}
static int set_times(struct kernel_vfs_file *file, const struct kernel_vfs_timespec times[2])
{
    if (times) for (unsigned j = 0; j < 2; j++)
        if (times[j].nanoseconds != KERNEL_VFS_UTIME_NOW && times[j].nanoseconds != KERNEL_VFS_UTIME_OMIT &&
            (times[j].nanoseconds < 0 || times[j].nanoseconds >= 1000000000)) return -KERNEL_EINVAL;
    if (times && times[0].nanoseconds == KERNEL_VFS_UTIME_OMIT && times[1].nanoseconds == KERNEL_VFS_UTIME_OMIT) return 0;
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node, &io, 1);
    struct fat_mount *m = fm(node->instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    if (node->instance->read_only) return -KERNEL_EROFS;
    int allowed=kernel_vfs_inode_times_allowed(node,times); if (allowed) return allowed;
    int r = fat_error(node->instance); if (r) return r;
    struct fat_inode *i = fi(node); struct kernel_vfs_timespec time = now();
    if (!times || times[0].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        i->atime = !times || times[0].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : times[0];
    if (!times || times[1].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        i->mtime = !times || times[1].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : times[1];
    i->ctime = time;
    return store_inode(m, i);
}
static int set_mode(struct kernel_vfs_file *file, uint32_t mode)
{
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node, &io, 1);
    struct fat_mount *m = fm(node->instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    if (m->instance.read_only) return -KERNEL_EROFS;
    int allowed=kernel_vfs_chmod_allowed(node->uid,node->gid,&mode); if (allowed) return allowed;
    int r = fat_error(node->instance); if (r) return r;
    struct fat_inode *i = fi(node);
    /* FAT's persistent permission is the read-only attribute. */
    i->mode = (i->mode & KERNEL_VFS_S_IFMT) | 0555 | (mode & 0222 ? 0222 : 0);
    node->mode = file->mode = i->mode; i->ctime = now();
    return store_inode(m, i);
}
static int set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid)
{
    struct kernel_vfs_node *node=file->private_data;
    KERNEL_LOCK_SCOPE(io);kernel_vfs_node_lock(node,&io,1);
    int allowed=kernel_vfs_chown_allowed(node->uid,node->gid,node->mode,uid,gid);if (allowed) return allowed;
    return (uid == 0 || uid == UINT32_MAX) && (gid == 0 || gid == UINT32_MAX) ? 0 : -KERNEL_EPERM;
}
static int flush_locked(struct fat_mount *m)
{
    if (m->error) return m->error;
    collect_retired(m);
    if (!m->instance.read_only && m->fsinfo) {
        uint8_t values[8]; put32(values, m->free_clusters); put32(values + 4, m->next_free);
        int r = bytes(m, (uint64_t)m->fsinfo * m->sector_size + 488, values, 8, 1);
        if (!r && m->backup_boot)
            r = bytes(m, (uint64_t)(m->backup_boot + m->fsinfo) * m->sector_size + 488, values, 8, 1);
        if (r) return r;
    }
    return fail(m, block_error(kernel_block_flush(m->block)));
}
static int flush(struct kernel_vfs_instance *instance)
{
    struct fat_mount *m = fm(instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int error = fat_error(instance); if (error) return error;
    struct fat_inode *i;
    RB_FOREACH(i, fat_id_tree, &m->inodes) if (i->times_dirty) {
        int r = store_inode(m, i); if (r) return r;
    }
    return flush_locked(m);
}
static int sync_filesystem(struct kernel_vfs_instance *instance, struct kernel_vfs_node *root_node)
{ (void)root_node; return flush(instance); }
static void sync_prepare(struct kernel_vfs_node *node, int data_only, struct kernel_vfs_sync_request *request)
{
    (void)data_only;
    struct fat_mount *m = fm(node->instance);
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    int r = fat_error(node->instance);
    if (!r && fi(node)->times_dirty) r = store_inode(m, fi(node));
    if (!r) r = flush_locked(m);
    kernel_vfs_sync_complete(request, r);
}
static int prepare_unmount(struct kernel_vfs_mount *mount)
{
    struct fat_mount *m = fm(mount->private_data);
    struct kernel_vfs_node *root_node = mount->root_path ? mount->root_path->file.private_data : 0;
    if (m->prepared) return 0;
    if (m->instance.external_files != (root_node ? 1U : 0U)) return -KERNEL_EBUSY;
    m->instance.quiescing = 1;
    kernel_page_cache_stop_worker(m->instance.page_cache);
    if (m->error) return m->error;
    enum kernel_page_cache_status status = kernel_page_cache_purge_mount(m->instance.page_cache, mount);
    if (status != KERNEL_PAGE_CACHE_STATUS_OK)
        return status == KERNEL_PAGE_CACHE_STATUS_STATE ? -KERNEL_EBUSY : -KERNEL_EIO;
    if (!kernel_vfs_nodes_idle(&m->instance, root_node)) return -KERNEL_EBUSY;
    while (m->instance.cleanup_nodes) {
        struct kernel_vfs_node *node = m->instance.cleanup_nodes;
        if (node->unlinked && !node->retired) {
            KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
            int r = trim_chain(m, fi(node), 0);
            if (r) return r;
            fi(node)->size = 0; node->retired = 1;
        }
        if (m->error) return m->error;
        if (!node->retired && node->unlinked) return -KERNEL_EIO;
        m->instance.cleanup_nodes = node->next;
        if (!node->closed) close_node(node);
        kernel_file_mapping_destroy(node); dispose(m, node);
    }
    if (!kernel_vfs_nodes_idle(&m->instance, root_node)) return -KERNEL_EBUSY;
    KERNEL_LOCK_SCOPE(g); kernel_mutex_lock(&m->metadata, &g);
    struct fat_inode *i;
    RB_FOREACH(i, fat_id_tree, &m->inodes) if (i->times_dirty) {
        int r = store_inode(m, i); if (r) return r;
    }
    int r = flush_locked(m); if (r) return r;
    if (!m->instance.read_only) {
        uint32_t state; r = fat_get(m, 1, &state);
        if (!r) r = fat_set(m, 1, state | 0x08000000);
        if (!r) r = fail(m, block_error(kernel_block_flush(m->block)));
        if (r) return r;
    }
    if (root_node && !root_node->closed) { close_node(root_node); root_node->closed = 1; }
    m->prepared = 1; return 0;
}
static int unmount(struct kernel_vfs_mount *mount)
{
    struct fat_mount *m = fm(mount->private_data);
    int r = prepare_unmount(mount); if (r) return r;
    if (m->instance.external_files || m->instance.nodes || mount->root_path) return -KERNEL_EBUSY;
    kernel_block_release_claim(m->block, mount);
    struct fat_inode *i;
    while ((i = RB_MIN(fat_id_tree, &m->inodes))) {
        if (__atomic_load_n(&i->handles, __ATOMIC_RELAXED)) __builtin_trap();
        RB_REMOVE(fat_id_tree, &m->inodes, i);
        if (i->linked && i->id != 1) RB_REMOVE(fat_entry_tree, &m->entries, i);
        dispose(m, i);
    }
    dispose(m, m->sector); dispose(m, m->zeros); dispose(m, m);
    mount->private_data = 0; mount->state = VFS_MOUNT_STATE_EMPTY; mount->id = 0;
    return 0;
}
static int check_domain(const struct kernel_vfs_node *node, const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if (!node || !node->instance || node->instance->ops != &fat_ops || node->closed || node->retired ||
        !node->mount || node->mount->private_data != node->instance || !node->backend_data ||
        node->instance->heap != heap || (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        !kernel_page_cache_check_domain(node->instance->page_cache, heap, allocator)) return -KERNEL_EINVAL;
    return fi((struct kernel_vfs_node *)node)->id == node->inode ? 0 : -KERNEL_EINVAL;
}
static const struct kernel_vfs_backend fat_ops = {
    .check_domain = check_domain,
    .error = fat_error, .root = root, .lookup = lookup, .entry_name = entry_name, .open = open_inode, .close_node = close_node,
    .create = create, .mkdir = mkdir_inode, .unlink = unlink_file, .rmdir = rmdir_inode,
    .rename = rename_inode, .release_unlinked = release_unlinked, .dir_entry = dir_entry,
    .pread = pread_inode, .prepare_write = prepare_write, .complete_write = complete_write,
    .writeback = writeback, .truncate = truncate_inode, .block_device = block_device,
    .writeback_allowed = writeback_allowed, .stat = stat_inode, .statfs = statfs,
    .accessed = accessed, .modified = modified, .set_times = set_times, .set_mode = set_mode, .set_owner = set_owner,
    .flush = flush, .sync_prepare = sync_prepare, .sync_filesystem = sync_filesystem,
    .prepare_unmount = prepare_unmount, .unmount = unmount,
};

int kernel_vfs_mount_fat32(struct kernel_vfs_mount *mount, struct kernel_block_device *block,
    struct kernel_heap *heap, struct kernel_page_cache *cache, int read_only)
{
    if (!mount || mount->state || mount->private_data || !block || !block->logical_block_size || !heap || !cache ||
        cache->state != KERNEL_PAGE_CACHE_LIVE) return -KERNEL_EINVAL;
    if (!read_only && kernel_block_read_only(block)) return -KERNEL_EROFS;
    int r = kernel_block_claim(block, mount); if (r) return r;
    uint8_t boot[512];
    r = block_error(kernel_block_read_at(block, 0, boot, sizeof(boot)));
    struct fat_mount *m = 0;
    if (!r) {
        enum kernel_heap_status a = kernel_heap_allocate_zeroed(heap, 1, sizeof(*m), (void **)&m);
        if (a != KERNEL_HEAP_STATUS_OK) r = a == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    if (r) { kernel_block_release_claim(block, mount); return r; }
    m->mount = mount; m->block = block; m->instance.heap = heap; m->instance.page_cache = cache;
    m->instance.backend_data = m; m->instance.ops = &fat_ops; m->instance.read_only = read_only != 0;
    m->sector_at = UINT64_MAX; m->next_id = 2;
    unsigned bps = le16(boot + 11), spc = boot[13], reserved = le16(boot + 14);
    unsigned fats = boot[16]; uint32_t sectors = le32(boot + 32), fat_sectors = le32(boot + 36);
    uint16_t flags = le16(boot + 40);
    uint64_t data_sector = reserved + (uint64_t)fats * fat_sectors;
    if (boot[510] != 0x55 || boot[511] != 0xaa || bps < 512 || bps > 4096 || (bps & (bps - 1)) ||
        bps < block->logical_block_size || bps % block->logical_block_size ||
        !spc || (spc & (spc - 1)) || spc > 128 || !reserved || !fats || fats > 2 ||
        !fat_sectors || le16(boot + 17) || le16(boot + 19) || le16(boot + 22) || le16(boot + 42) ||
        data_sector >= sectors || (uint64_t)sectors * bps > block->capacity_bytes ||
        (flags & ~0x008fU) || ((flags & 0x80) && (flags & 15) >= fats)) { r = -KERNEL_EINVAL; goto invalid; }
    m->sector_size = bps; m->cluster_size = bps * spc; m->sectors = sectors;
    m->fat_start = (uint64_t)reserved * bps; m->fat_bytes = (uint64_t)fat_sectors * bps;
    m->data_start = data_sector * bps; m->cluster_count = (sectors - data_sector) / spc;
    m->root_cluster = le32(boot + 44); m->fats = fats; m->mirrored = !(flags & 0x80);
    m->active_fat = m->mirrored ? 0 : flags & 15;
    if (m->cluster_count < 65525 || m->cluster_count >= 0x0ffffff5 ||
        m->fat_bytes / 4 < m->cluster_count + 2 || !valid_cluster(m, m->root_cluster)) {
        r = -KERNEL_EINVAL; goto invalid;
    }
    r = alloc(m, bps, (void **)&m->sector);
    if (!r) r = alloc(m, bps, (void **)&m->zeros);
    if (r) goto invalid;
    uint32_t state, media;
    r = fat_get(m, 0, &media); if (!r) r = fat_get(m, 1, &state);
    if (r) goto invalid;
    if ((media & 0x0fffff00) != 0x0fffff00 || (media & 255) != boot[21] ||
        (state & 0x03ffffff) != 0x03ffffff || (state & 0x0c000000) != 0x0c000000) {
        r = -KERNEL_EUCLEAN; goto invalid;
    }
    if (m->mirrored && fats > 1) {
        uint8_t *other = 0; r = alloc(m, bps, (void **)&other); if (r) goto invalid;
        for (uint64_t at = 0; !r && at < m->fat_bytes; at += bps) {
            r = sector_read(m, m->fat_start + at, m->sector);
            if (!r) r = sector_read(m, m->fat_start + m->fat_bytes + at, other);
            if (!r && memcmp(m->sector, other, bps)) r = -KERNEL_EUCLEAN;
        }
        dispose(m, other); m->sector_at = UINT64_MAX;
        if (r) goto invalid;
    }
    /* FSInfo is a hint. Count the actual bitmap-equivalent FAT once, with one
     * sector buffer, so a stale hint never creates false ENOSPC or free space. */
    m->next_free = 2;
    for (uint32_t c = 2; c < m->cluster_count + 2; c++) {
        uint32_t value; r = fat_get(m, c, &value); if (r) goto invalid;
        if (!value) m->free_clusters++;
        else if (value < 0x0ffffff8 && value != 0x0ffffff7 && !valid_cluster(m, value)) {
            r = -KERNEL_EUCLEAN; goto invalid;
        }
    }
    unsigned info = le16(boot + 48), backup = le16(boot + 50);
    if (info && info < reserved) {
        r = sector_read(m, (uint64_t)info * bps, m->sector); m->sector_at = UINT64_MAX;
        if (r) goto invalid;
        if (le32(m->sector) == 0x41615252 && le32(m->sector + 484) == 0x61417272 && le32(m->sector + 508) == 0xaa550000) {
            m->fsinfo = info;
            uint32_t hint = le32(m->sector + 492); if (valid_cluster(m, hint)) m->next_free = hint;
            if (backup && backup != 0xffff && backup + info < reserved) m->backup_boot = backup;
        }
    }
    struct fat_inode *root_inode = 0; r = alloc(m, sizeof(*root_inode), (void **)&root_inode);
    if (r) goto invalid;
    root_inode->id = root_inode->parent = 1; root_inode->linked = 1;
    root_inode->mode = KERNEL_VFS_S_IFDIR | 0777; root_inode->first = m->root_cluster;
    root_inode->atime = root_inode->mtime = root_inode->ctime = now();
    if (RB_INSERT(fat_id_tree, &m->inodes, root_inode)) __builtin_trap();
    r = measure(m, root_inode); if (r) goto invalid;
    kernel_mutex_init(&m->metadata, 40, (uintptr_t)m);
    kernel_mutex_init(&m->instance.namespace_lock, 20, (uintptr_t)&m->instance);
    kernel_raw_lock_init(&m->instance.error_lock, KERNEL_RAW_RANK_OBJECT);
    m->instance.writeback_block_size = bps;
    mount->private_data = &m->instance; mount->id = kernel_vfs_allocate_mount_id(); mount->state = VFS_MOUNT_STATE_CLEANUP;
    if (!read_only) {
        r = fat_set(m, 1, state & ~0x08000000U);
        if (!r) r = fail(m, block_error(kernel_block_flush(block)));
        if (r) return r; /* The failing mount still owns uncertain media I/O. */
    }
    mount->state = VFS_MOUNT_STATE_LIVE;
    return 0;
invalid:
    while (!RB_EMPTY(&m->inodes)) {
        struct fat_inode *i = RB_MIN(fat_id_tree, &m->inodes); RB_REMOVE(fat_id_tree, &m->inodes, i); dispose(m, i);
    }
    dispose(m, m->sector); dispose(m, m->zeros); dispose(m, m);
    kernel_block_release_claim(block, mount);
    return r;
}
