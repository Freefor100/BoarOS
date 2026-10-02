#include "vfs_objects.h"
#include <kernel/tmpfs.h>
#include <kernel/memory_object.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <string.h>

/* Fixed Linux mm/shmem.c BOGO_DIRENT_SIZE: includes . and .. in i_size. */
#define TMPFS_DIRENT_SIZE 20U
#define TMPFS_INODE_SPACE 1024U

struct tmp_inode {
    struct tmp_inode *next;
    uint64_t number, parent, size;
    uint32_t mode, links, handles;
    struct kernel_memory_object *memory;
    char *target;
    struct kernel_vfs_timespec atime, mtime, ctime;
};
struct tmp_entry {
    struct tmp_entry *next;
    struct tmp_inode *inode;
    uint64_t parent, cookie;
    char name[];
};
struct tmp_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct tmp_inode *inodes;
    struct tmp_entry *entries;
    struct kernel_memory_budget pages;
    uint64_t inode_limit, inode_count, next_inode, next_cookie;
};
static struct kernel_vfs_backend tmp_ops;
int kernel_tmpfs_is_mount(const struct kernel_vfs_mount *mount)
{
    const struct kernel_vfs_instance *instance = mount ? mount->private_data : 0;
    return instance && instance->ops == &tmp_ops;
}
static struct tmp_mount *tm(struct kernel_vfs_instance *i) { return i->backend_data; }
static struct tmp_inode *ti(struct kernel_vfs_node *n) { return n->backend_data; }
static struct kernel_vfs_timespec now(void)
{
    uint64_t ns = kernel_time_realtime_ns();
    return (struct kernel_vfs_timespec){ns / 1000000000, ns % 1000000000};
}
static void dispose(struct tmp_mount *m, void *p)
{ if (p && kernel_heap_release(m->instance.heap, p) != KERNEL_HEAP_STATUS_OK) __builtin_trap(); }
static int allocate(struct tmp_mount *m, size_t n, void **out)
{
    enum kernel_heap_status r = kernel_heap_allocate_zeroed(m->instance.heap, 1, n, out);
    return r == KERNEL_HEAP_STATUS_OK ? 0 : r == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
}
static struct tmp_inode *inode(struct tmp_mount *m, uint64_t id)
{ for (struct tmp_inode *i = m->inodes; i; i = i->next) if (i->number == id) return i; return 0; }
static struct tmp_entry *entry(struct tmp_mount *m, uint64_t parent, const char *name, size_t n)
{
    for (struct tmp_entry *e = m->entries; e; e = e->next)
        if (e->parent == parent && strlen(e->name) == n && !memcmp(e->name, name, n)) return e;
    return 0;
}
static void collect_inode(struct tmp_mount *m, struct tmp_inode *i)
{
    if (i->links || i->handles) return;
    struct tmp_inode **p = &m->inodes;
    while (*p && *p != i) p = &(*p)->next;
    if (!*p || !m->inode_count) __builtin_trap();
    *p = i->next; m->inode_count--;
    if (!i->memory) dispose(m, i->target);
    if (i->memory) kernel_memory_object_release(&i->memory);
    dispose(m, i);
}
static int resolve(struct tmp_mount *m, const char *path, struct tmp_inode **out)
{
    if (!path || path[0] != '/') return -KERNEL_EINVAL;
    struct tmp_inode *i = inode(m, 1);
    while (*path == '/') path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/') end++;
        if ((i->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
        struct tmp_entry *e = entry(m, i->number, path, (size_t)(end - path));
        if (!e) return -KERNEL_ENOENT;
        i = e->inode; path = end;
        while (*path == '/') path++;
    }
    *out = i; return 0;
}
static int parent_name(struct tmp_mount *m, const char *path, struct tmp_inode **parent,
                       const char **name, size_t *len)
{
    if (!path || path[0] != '/') return -KERNEL_EINVAL;
    struct tmp_inode *i = inode(m, 1);
    while (*path == '/') path++;
    for (;;) {
        const char *end = path;
        while (*end && *end != '/') end++;
        size_t n = (size_t)(end - path);
        if (!n || n > 255) return n ? -KERNEL_ENAMETOOLONG : -KERNEL_EINVAL;
        const char *next = end;
        while (*next == '/') next++;
        if (!*next) { *parent = i; *name = path; *len = n; return 0; }
        struct tmp_entry *e = entry(m, i->number, path, n);
        if (!e) return -KERNEL_ENOENT;
        if ((e->inode->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
        i = e->inode; path = next;
    }
}
static int new_entry(struct tmp_mount *m, uint64_t parent, const char *name, size_t n,
                      struct tmp_entry **out)
{
    if (entry(m, parent, name, n)) return -KERNEL_EEXIST;
    if (m->next_cookie == UINT64_MAX) return -KERNEL_EOVERFLOW;
    int r = allocate(m, sizeof(**out) + n + 1, (void **)out);
    if (r) return r;
    (*out)->parent = parent; (*out)->cookie = m->next_cookie++;
    memcpy((*out)->name, name, n); return 0;
}
static void publish_entry(struct tmp_mount *m, struct tmp_entry *e, struct tmp_inode *i)
{
    struct tmp_inode *parent = inode(m, e->parent);
    if (!parent || parent->size > UINT64_MAX - TMPFS_DIRENT_SIZE) __builtin_trap();
    parent->size += TMPFS_DIRENT_SIZE;
    e->inode = i; e->next = m->entries; m->entries = e;
}
static void remove_entry(struct tmp_mount *m, struct tmp_entry *e)
{
    struct tmp_entry **p = &m->entries;
    while (*p && *p != e) p = &(*p)->next;
    if (!*p) __builtin_trap();
    struct tmp_inode *parent = inode(m, e->parent);
    if (!parent || parent->size < 3 * TMPFS_DIRENT_SIZE) __builtin_trap();
    parent->size -= TMPFS_DIRENT_SIZE;
    *p = e->next; dispose(m, e);
}
static int create_inode(struct tmp_mount *m, const char *path, uint32_t mode,
                         const char *target, struct tmp_inode **out)
{
    if (m->instance.read_only) return -KERNEL_EROFS;
    if (m->inode_limit && m->inode_count >= m->inode_limit) return -KERNEL_ENOSPC;
    struct tmp_inode *parent, *i = 0;
    struct tmp_entry *e = 0;
    const char *name; size_t n;
    int r = parent_name(m, path, &parent, &name, &n);
    if (r) return r;
    r = new_entry(m, parent->number, name, n, &e);
    if (r) return r;
    r = allocate(m, sizeof(*i), (void **)&i);
    if (r) { dispose(m, e); return r; }
    i->mode = mode; i->parent = parent->number;
    if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) i->size = 2 * TMPFS_DIRENT_SIZE;
    if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG) {
        if (kernel_memory_object_create(m->instance.heap, m->instance.heap->page_allocator,
                                        &i->memory) != KERNEL_MEMORY_OBJECT_OK) r = -KERNEL_ENOMEM;
        else kernel_memory_object_set_budget(i->memory, &m->pages);
    }
    if (target && !r) {
        i->size = strlen(target);
        if (i->size + 1 > BOAROS_PAGE_SIZE) r = -KERNEL_ENAMETOOLONG;
        else if (i->size + 1 <= 128) {
            r = allocate(m, i->size + 1, (void **)&i->target);
            if (!r) memcpy(i->target, target, i->size + 1);
        } else {
            enum kernel_memory_object_status status = kernel_memory_object_create(
                m->instance.heap, m->instance.heap->page_allocator, &i->memory);
            if (status != KERNEL_MEMORY_OBJECT_OK) r = -KERNEL_ENOMEM;
            else {
                kernel_memory_object_set_budget(i->memory, &m->pages);
                uint64_t address; int created;
                status = kernel_memory_object_get_page(i->memory, 0, &address, &created);
                if (status != KERNEL_MEMORY_OBJECT_OK)
                    r = status == KERNEL_MEMORY_OBJECT_NO_SPACE ? -KERNEL_ENOSPC : -KERNEL_ENOMEM;
                else {
                    void *bytes;
                    if (physical_page_resolve(m->instance.heap->page_allocator, address, &bytes) != PHYSICAL_PAGE_STATUS_OK)
                        __builtin_trap();
                    i->target = bytes;
                    memcpy(i->target, target, i->size + 1);
                    (void)physical_page_release(m->instance.heap->page_allocator, address);
                }
            }
        }
    }
    if (r || m->next_inode == UINT64_MAX) {
        if (!i->memory) dispose(m, i->target);
        if (i->memory) kernel_memory_object_release(&i->memory);
        dispose(m, i); dispose(m, e);
        return r ? r : -KERNEL_EOVERFLOW;
    }
    i->number = m->next_inode++;
    i->links = (mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? 2 : 1;
    i->atime = i->mtime = i->ctime = now();
    i->next = m->inodes; m->inodes = i; m->inode_count++;
    publish_entry(m, e, i);
    if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) parent->links++;
    parent->mtime = parent->ctime = now();
    *out = i; return 0;
}
static int error(struct kernel_vfs_instance *instance) { (void)instance; return 0; }
static int root(struct kernel_vfs_instance *instance, uint64_t *id, uint32_t *mode)
{ *id = 1; *mode = inode(tm(instance), 1)->mode; return 0; }
static int lookup(struct kernel_vfs_instance *instance, uint64_t parent, const char *name,
                   size_t n, uint64_t *id, uint32_t *mode)
{
    struct tmp_mount *m = tm(instance); struct tmp_inode *p = inode(m, parent);
    if (!p) return -KERNEL_ENOENT;
    if ((p->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
    struct tmp_entry *e = entry(m, parent, name, n);
    if (!e) return -KERNEL_ENOENT;
    *id = e->inode->number; *mode = e->inode->mode; return 0;
}
static int open_inode(struct kernel_vfs_mount *mount, const char *path, uint64_t id,
                       uint32_t mode, struct kernel_vfs_file *file)
{
    (void)mode;
    struct tmp_mount *m = tm(mount->private_data); struct tmp_inode *i = 0;
    int r = id ? 0 : resolve(m, path, &i);
    if (id) i = inode(m, id);
    if (r || !i) return r ? r : -KERNEL_ENOENT;
    struct kernel_vfs_node *n = 0;
    r = allocate(m, sizeof(*n), (void **)&n); if (r) return r;
    n->inode = i->number; n->mode = i->mode; n->size = i->size; n->max_size = INT64_MAX;
    n->backend_data = i; n->memory = i->memory; i->handles++;
    return kernel_vfs_publish_node(mount, n, file, 0);
}
static int close_node(struct kernel_vfs_node *n)
{
    struct tmp_mount *m = tm(n->instance); struct tmp_inode *i = ti(n);
    if (!i->handles) __builtin_trap();
    i->handles--; collect_inode(m, i); return 0;
}
static int create(struct kernel_vfs_mount *mount, const char *path, uint32_t mode,
                   struct kernel_vfs_file *file)
{
    struct tmp_mount *m = tm(mount->private_data); struct tmp_inode *i;
    int r = create_inode(m, path, KERNEL_VFS_S_IFREG | (mode & 07777), 0, &i);
    if (r) return r;
    r = open_inode(mount, 0, i->number, i->mode, file);
    if (r) {
        struct tmp_entry *e = m->entries;
        if (!e || e->inode != i) __builtin_trap();
        remove_entry(m, e); i->links = 0; collect_inode(m, i);
    }
    return r;
}
static int mkdir(struct kernel_vfs_mount *mount, const char *path, uint32_t mode)
{ struct tmp_inode *i; return create_inode(tm(mount->private_data), path, KERNEL_VFS_S_IFDIR | (mode & 07777), 0, &i); }
static int symlink(struct kernel_vfs_instance *instance, const char *target, const char *path)
{ struct tmp_inode *i; return create_inode(tm(instance), path, KERNEL_VFS_S_IFLNK | 0777, target, &i); }
static int mknod_regular(struct kernel_vfs_instance *instance, const char *path,
    uint32_t type, uint32_t mode, uint32_t device)
{
    (void)device;
    if (type != KERNEL_VFS_S_IFREG && type != KERNEL_VFS_S_IFIFO) return -KERNEL_ENOTSUP;
    struct tmp_inode *created;
    return create_inode(tm(instance), path, type | (mode & 07777), 0, &created);
}
static int readlink(struct kernel_vfs_node *n, char *buffer, size_t size, size_t *count)
{
    struct tmp_inode *i = ti(n);
    if (!i->target) return -KERNEL_EINVAL;
    *count = i->size < size ? i->size : size;
    memcpy(buffer, i->target, *count); return 0;
}
static int empty(struct tmp_mount *m, struct tmp_inode *i)
{ for (struct tmp_entry *e = m->entries; e; e = e->next) if (e->parent == i->number) return 0; return 1; }
static void unlink_inode(struct tmp_mount *m, struct tmp_inode *i)
{
    if (!i->links) __builtin_trap();
    if ((i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) i->links = 0;
    else { if (i->links > 1) { if (!m->inode_count) __builtin_trap(); m->inode_count--; } i->links--; }
    i->ctime = now();
    if (!i->links) for (struct kernel_vfs_node *n = m->instance.nodes; n; n = n->next)
        if (n->inode == i->number) { n->unlinked = 1; }
    collect_inode(m, i);
}
static int remove_path(struct kernel_vfs_mount *mount, const char *path, int directory)
{
    struct tmp_mount *m = tm(mount->private_data); struct tmp_inode *parent;
    const char *name; size_t n;
    int r = parent_name(m, path, &parent, &name, &n); if (r) return r;
    struct tmp_entry *e = entry(m, parent->number, name, n); if (!e) return -KERNEL_ENOENT;
    struct tmp_inode *i = e->inode;
    int isdir = (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if (directory != isdir) return directory ? -KERNEL_ENOTDIR : -KERNEL_EISDIR;
    if (isdir && !empty(m, i)) return -KERNEL_ENOTEMPTY;
    remove_entry(m, e); if (isdir) parent->links--;
    parent->ctime = parent->mtime = now(); unlink_inode(m, i); return 0;
}
static int unlink_file(struct kernel_vfs_mount *m, const char *p) { return remove_path(m,p,0); }
static int rmdir(struct kernel_vfs_mount *m, const char *p) { return remove_path(m,p,1); }
static void release_unlinked(struct kernel_vfs_node *n)
{
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->unlinked && !n->open_files && !n->exec_users) n->retired = 1;
}
static int link_inode(struct kernel_vfs_instance *instance, uint64_t source, uint64_t parent, const char *name)
{
    struct tmp_mount *m = tm(instance); struct tmp_inode *i = inode(m, source), *p = inode(m,parent);
    if (!i || !p || !i->links) return -KERNEL_ENOENT;
    if ((i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) return -KERNEL_EPERM;
    if (i->links == UINT32_MAX) return -KERNEL_EMLINK;
    if (m->inode_limit && m->inode_count >= m->inode_limit) return -KERNEL_ENOSPC;
    struct tmp_entry *e = 0; int r = new_entry(m,parent,name,strlen(name),&e); if(r) return r;
    publish_entry(m,e,i); m->inode_count++; i->links++; i->ctime = now(); p->ctime = p->mtime = now(); return 0;
}
static int rename_inode(struct kernel_vfs_instance *instance, uint64_t old_parent, const char *old_name,
                          uint64_t new_parent, const char *new_name, unsigned flags,
                          struct kernel_vfs_rename_result *result)
{
    struct tmp_mount *m = tm(instance);
    struct tmp_entry *old = entry(m,old_parent,old_name,strlen(old_name));
    struct tmp_entry *dest = entry(m,new_parent,new_name,strlen(new_name));
    if (!old) return -KERNEL_ENOENT;
    if (dest && (flags & 1)) return -KERNEL_EEXIST;
    if (dest && dest->inode == old->inode) return 0;
    struct tmp_inode *i = old->inode, *op = inode(m,old_parent), *np = inode(m,new_parent);
    int dir = (i->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
    if (!np || !op) return -KERNEL_ENOENT;
    if (dest) {
        int dd = (dest->inode->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR;
        if (dir != dd) return dir ? -KERNEL_ENOTDIR : -KERNEL_EISDIR;
        if (dd && !empty(m,dest->inode)) return -KERNEL_ENOTEMPTY;
    }
    if (dir) for (struct tmp_inode *p = np;; p = inode(m,p->parent)) {
        if (!p) __builtin_trap();
        if (p == i) return -KERNEL_EINVAL;
        if (p->number == 1) break;
    }
    struct tmp_entry *replacement = 0;
    /* 先预留新名字，之后的目录项与 link count 提交不再分配。 */
    int r = allocate(m,sizeof(*replacement)+strlen(new_name)+1,(void **)&replacement);
    if(r) return r;
    replacement->parent=new_parent; replacement->cookie=m->next_cookie++;
    memcpy(replacement->name,new_name,strlen(new_name)+1);
    if(dest) {
        struct tmp_inode *di=dest->inode;
        result->replaced_inode=di->number;
        result->replaced_last_link=dir || di->links==1;
        remove_entry(m,dest); if(dir) np->links--; unlink_inode(m,di);
    }
    remove_entry(m,old); publish_entry(m,replacement,i);
    if(dir && op!=np) { op->links--; np->links++; i->parent=new_parent; }
    i->ctime=now(); op->mtime=op->ctime=np->mtime=np->ctime=now(); result->changed=1;
    return 0;
}
static int stat_inode(const struct kernel_vfs_file *f, struct kernel_vfs_stat *s)
{
    struct kernel_vfs_node *n=f->private_data; struct tmp_inode *i=ti(n);
    *s=(struct kernel_vfs_stat){.dev=f->mount->id,.ino=i->number,.mode=i->mode,.nlink=i->links,
        .size=i->size,.blksize=BOAROS_PAGE_SIZE,.blocks=i->memory ? kernel_memory_object_resident_pages(i->memory)*8 : 0,
        .atime=i->atime,.mtime=i->mtime,.ctime=i->ctime}; return 0;
}
static int statfs(struct kernel_vfs_mount *mount, struct kernel_vfs_statfs *s)
{
    struct tmp_mount *m=tm(mount->private_data);
    *s=(struct kernel_vfs_statfs){.type=0x01021994,.block_size=BOAROS_PAGE_SIZE,.blocks=m->pages.limit,
        .free_blocks=m->pages.limit ? m->pages.limit-m->pages.used : 0,
        .available_blocks=m->pages.limit ? m->pages.limit-m->pages.used : 0,
        .inodes=m->inode_limit,.free_inodes=m->inode_limit ? m->inode_limit-m->inode_count : 0,
        .fsid=mount->id,.name_length=255,.flags=m->instance.read_only ? 1 : 0}; return 0;
}
static int dir_entry(struct kernel_vfs_file *f,uint64_t pos,uint64_t *next,uint64_t *id,
                       uint8_t *type,char *name,size_t capacity)
{
    struct kernel_vfs_node *n=f->private_data; struct tmp_mount *m=tm(n->instance); struct tmp_inode *i=ti(n);
    if((i->mode & KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR) return -KERNEL_ENOTDIR;
    if(pos<2) { if(capacity<3) return -KERNEL_ENAMETOOLONG; strcpy(name,pos ? ".." : ".");
        *id=pos ? i->parent : i->number; *next=pos+1; *type=4; return 1; }
    struct tmp_entry *best=0;
    for(struct tmp_entry *e=m->entries;e;e=e->next)
        if(e->parent==i->number && e->cookie>=pos && (!best || e->cookie<best->cookie)) best=e;
    if(!best) return 0;
    if(strlen(best->name)>=capacity) return -KERNEL_ENAMETOOLONG;
    strcpy(name,best->name); *id=best->inode->number; *next=best->cookie+1;
    *type=(best->inode->mode & KERNEL_VFS_S_IFMT)>>12; return 1;
}
static int truncate_inode(struct kernel_vfs_node *n,uint64_t size,uint64_t *actual,int *changed)
{
    struct tmp_inode *i=ti(n); kernel_memory_object_truncate(i->memory,size);
    i->size=size; i->ctime=i->mtime=now(); *actual=size; *changed=1; return 0;
}
static int pread_inode(struct kernel_vfs_node *n,uint64_t offset,void *buffer,size_t size,size_t *read)
{
    struct tmp_mount *m=tm(n->instance); struct tmp_inode *i=ti(n); *read=0;
    while(*read<size && offset<i->size) {
        uint64_t address; size_t start=offset & BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if(count>size-*read) count=size-*read;
        if(count>i->size-offset) count=i->size-offset;
        enum kernel_memory_object_status r=kernel_memory_object_find_page(i->memory,offset>>BOAROS_PAGE_SHIFT,&address);
        if(r==KERNEL_MEMORY_OBJECT_NOT_FOUND) memset((char *)buffer+*read,0,count);
        else if(r==KERNEL_MEMORY_OBJECT_OK) {
            void *p; if(physical_page_resolve(m->instance.heap->page_allocator,address,&p)!=PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            memcpy((char *)buffer+*read,(char *)p+start,count);
            (void)physical_page_release(m->instance.heap->page_allocator,address);
        } else return -KERNEL_EIO;
        *read+=count; offset+=count;
    }
    return 0;
}
static int write_inode(struct kernel_vfs_node *n,uint64_t offset,const void *buffer,size_t size,size_t *written)
{
    struct tmp_mount *m=tm(n->instance); struct tmp_inode *i=ti(n); *written=0;
    while(*written<size) {
        uint64_t address; int created; size_t start=offset & BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start; if(count>size-*written) count=size-*written;
        enum kernel_memory_object_status r=kernel_memory_object_get_page(i->memory,offset>>BOAROS_PAGE_SHIFT,&address,&created);
        if(r!=KERNEL_MEMORY_OBJECT_OK) return r==KERNEL_MEMORY_OBJECT_NO_SPACE ? -KERNEL_ENOSPC : -KERNEL_ENOMEM;
        void *p; if(physical_page_resolve(m->instance.heap->page_allocator,address,&p)!=PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        memcpy((char *)p+start,(const char *)buffer+*written,count);
        (void)physical_page_release(m->instance.heap->page_allocator,address);
        *written+=count; offset+=count;
        if(offset>i->size) i->size=offset;
        n->size=i->size; i->ctime=i->mtime=now();
    }
    return 0;
}
static int set_times(struct kernel_vfs_file *f,const struct kernel_vfs_timespec t[2])
{
    if (t) for (unsigned j = 0; j < 2; j++)
        if (t[j].nanoseconds != KERNEL_VFS_UTIME_NOW && t[j].nanoseconds != KERNEL_VFS_UTIME_OMIT &&
            (t[j].nanoseconds < 0 || t[j].nanoseconds >= 1000000000)) return -KERNEL_EINVAL;
    if (t && t[0].nanoseconds == KERNEL_VFS_UTIME_OMIT && t[1].nanoseconds == KERNEL_VFS_UTIME_OMIT) return 0;
    struct kernel_vfs_node *n = f->private_data;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->instance->read_only) return -KERNEL_EROFS;
    struct tmp_inode *i = ti(n); struct kernel_vfs_timespec time = now();
    if (!t || t[0].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        i->atime = !t || t[0].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : t[0];
    if (!t || t[1].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        i->mtime = !t || t[1].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : t[1];
    i->ctime = time;
    return 0;
}
static int set_mode(struct kernel_vfs_file *f,uint32_t mode)
{
    struct kernel_vfs_node *n = f->private_data;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->instance->read_only) return -KERNEL_EROFS;
    struct tmp_inode *i = ti(n);
    i->mode = (i->mode & KERNEL_VFS_S_IFMT) | (mode & 07777);
    n->mode = f->mode = i->mode; i->ctime = now();
    return 0;
}
static int time_not_after(struct kernel_vfs_timespec a, struct kernel_vfs_timespec b)
{
    return a.seconds < b.seconds || (a.seconds == b.seconds && a.nanoseconds <= b.nanoseconds);
}
static void accessed(struct kernel_vfs_file *f)
{
    struct tmp_inode *i=ti(f->private_data); struct kernel_vfs_timespec t=now();
    if (time_not_after(i->atime, i->mtime) || time_not_after(i->atime, i->ctime) ||
        (t.seconds >= i->atime.seconds && (uint64_t)t.seconds - (uint64_t)i->atime.seconds >= 86400))
        i->atime=t;
}
static int modified(struct kernel_vfs_file *f,uint64_t offset,int append)
{ (void)offset; (void)append; struct tmp_inode *i=ti(f->private_data); i->mtime=i->ctime=now(); return 0; }
void kernel_tmpfs_memory_modified(struct kernel_vfs_file *file)
{
    if (!file || !kernel_tmpfs_is_mount(file->mount) || !file->private_data)
        __builtin_trap();
    /* 调用者在单核上屏蔽中断；不分配、不睡眠，不升级缺页持有的 inode 读锁。 */
    (void)modified(file, 0, 0);
}
static int no_writeback(struct kernel_vfs_instance *i) { (void)i; return 0; }
static int sync_metadata(struct kernel_vfs_node *n, int data_only) { (void)n; (void)data_only; return 0; }
static int unmount(struct kernel_vfs_mount *mount)
{
    struct tmp_mount *m=tm(mount->private_data);
    if(mount->covered_path || mount->root_path || mount->child_mounts || m->instance.external_files || m->instance.nodes) return -KERNEL_EBUSY;
    while (m->entries) {
        struct tmp_inode *i = m->entries->inode;
        /* 额外硬链接预留也属于挂载；保留 inode 到所有父目录项移除之后。 */
        if ((i->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR && i->links > 1) {
            if (!m->inode_count) __builtin_trap();
            i->links--; m->inode_count--;
        }
        remove_entry(m, m->entries);
    }
    while(m->inodes) { m->inodes->links=0; collect_inode(m,m->inodes); }
    if(m->pages.used || m->inode_count) __builtin_trap();
    struct kernel_heap *heap=m->instance.heap;
    if(kernel_heap_release(heap,m)!=KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return 0;
}
static void initialize_ops(void)
{
    volatile struct kernel_vfs_backend *o=&tmp_ops;
    o->error=error; o->root=root; o->lookup=lookup; o->open=open_inode; o->close_node=close_node;
    o->create=create; o->mkdir=mkdir; o->symlink=symlink; o->mknod=mknod_regular; o->readlink=readlink; o->unlink=unlink_file;
    o->rmdir=rmdir; o->release_unlinked=release_unlinked; o->link=link_inode; o->rename=rename_inode;
    o->stat=stat_inode; o->statfs=statfs; o->dir_entry=dir_entry; o->truncate=truncate_inode;
    o->pread=pread_inode; o->memory_write=write_inode; o->set_times=set_times; o->set_mode=set_mode;
    o->accessed=accessed; o->modified=modified; o->writeback_allowed=no_writeback;
    o->sync_metadata=sync_metadata; o->flush=error; o->unmount=unmount;
}
static int number(const char *p,size_t n,unsigned base,uint64_t *value)
{
    if(!n) return -KERNEL_EINVAL;
    uint64_t v=0;
    for(size_t j=0;j<n;j++) {
        unsigned d=(unsigned)(p[j]-'0');
        if(d>=base || v>(UINT64_MAX-d)/base) return -KERNEL_EINVAL;
        v=v*base+d;
    }
    *value=v; return 0;
}
static unsigned digit(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A') + 10;
    return 16;
}
static size_t memory_number(const char *p, size_t n, uint64_t *value)
{
    size_t j = 0;
    unsigned base = n && p[0] == '0' ? 8 : 10;
    if (n >= 3 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) {
        base = 16; j = 2;
    }
    uint64_t v = 0;
    for (; j < n && digit(p[j]) < base; j++) {
        unsigned d = digit(p[j]);
        v = v > (UINT64_MAX - d) / base ? UINT64_MAX : v * base + d;
    }
    if (j && j < n) {
        unsigned shift = 0;
        switch (p[j]) {
        case 'k': case 'K': shift = 10; break;
        case 'm': case 'M': shift = 20; break;
        case 'g': case 'G': shift = 30; break;
        case 't': case 'T': shift = 40; break;
        case 'p': case 'P': shift = 50; break;
        case 'e': case 'E': shift = 60; break;
        }
        if (shift) { v = v > (UINT64_MAX >> shift) ? UINT64_MAX : v << shift; j++; }
    }
    *value = v;
    return j;
}
int kernel_tmpfs_create(struct kernel_heap *heap,uint64_t flags,const char *options,struct kernel_vfs_mount **owner)
{
    if(!heap || !owner || *owner) return -KERNEL_EINVAL;
    struct tmp_mount *m=0;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*m),(void **)&m)!=KERNEL_HEAP_STATUS_OK) return -KERNEL_ENOMEM;
    m->instance.heap=heap; m->instance.backend_data=m;
    m->pages.limit=physical_page_total(heap->page_allocator)/2;
    m->inode_limit=m->pages.limit; uint32_t mode=01777;
    int r=0;
    for(const char *p=options ? options : "";*p;) {
        const char *end=p; while(*end && *end!=',') end++;
        const char *eq=p; while(eq<end && *eq!='=') eq++;
        if(eq==end) { r=-KERNEL_EINVAL; break; }
        uint64_t v; size_t n=(size_t)(end-eq-1);
        size_t key=(size_t)(eq-p);
        int ismode=key==4 && !memcmp(p,"mode",4);
        if (ismode) {
            r=number(eq+1,n,8,&v);
            if (r || v > UINT32_MAX) { r=-KERNEL_EINVAL; break; }
            mode=(uint32_t)v & 07777;
        } else {
            size_t consumed = memory_number(eq+1,n,&v);
            int issize=key==4 && !memcmp(p,"size",4);
            if (issize && consumed < n && eq[1+consumed]=='%') {
                /* 固定 Linux memparse 饱和；shmem 百分比和向上取整使用 u64 算术。 */
                v = ((v << BOAROS_PAGE_SHIFT) * physical_page_total(heap->page_allocator)) / 100;
                consumed++;
            }
            if (!n || consumed != n) { r=-KERNEL_EINVAL; break; }
            if (issize) m->pages.limit=(v+BOAROS_PAGE_MASK)>>BOAROS_PAGE_SHIFT;
            else if(key==9 && !memcmp(p,"nr_blocks",9) && v<=INT64_MAX) m->pages.limit=v;
            else if(key==9 && !memcmp(p,"nr_inodes",9) && v<=UINT64_MAX/TMPFS_INODE_SPACE) m->inode_limit=v;
            else { r=-KERNEL_EINVAL; break; }
        }
        p=*end ? end+1 : end;
    }
    if(r) {dispose(m,m);return r;}
    struct tmp_inode *i=0; r=allocate(m,sizeof(*i),(void **)&i);
    if(r) {dispose(m,m);return r;}
    i->number=i->parent=1; i->mode=KERNEL_VFS_S_IFDIR|mode; i->links=2;
    i->size=2 * TMPFS_DIRENT_SIZE;
    i->atime=i->mtime=i->ctime=now(); m->inodes=i; m->inode_count=1; m->next_inode=2; m->next_cookie=2;
    initialize_ops(); m->instance.ops=&tmp_ops; m->instance.read_only=(flags & 1)!=0;
    kernel_mutex_init(&m->instance.namespace_lock,20,(uintptr_t)&m->instance);
    m->mount.private_data=&m->instance; m->mount.id=kernel_vfs_allocate_mount_id(); m->mount.state=VFS_MOUNT_STATE_LIVE;
    *owner=&m->mount; return 0;
}
