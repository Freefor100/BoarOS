#include "cgroup_internal.h"
#include "vfs_objects.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/time.h>
#include <kernel/wait_internal.h>
#include <string.h>

/* No controllers are advertised. A domain hierarchy still has real task
 * membership and delegation; enabling a controller must never silently work. */
enum { CG_DIR, CG_PROCS, CG_THREADS, CG_CONTROLLERS, CG_SUBTREE, CG_TYPE, CG_EVENTS };
static const char *names[KERNEL_CGROUP_FILES]={"", "cgroup.procs", "cgroup.threads",
    "cgroup.controllers", "cgroup.subtree_control", "cgroup.type", "cgroup.events"};
static const uint32_t modes[KERNEL_CGROUP_FILES]={0755,0644,0644,0444,0644,0644,0444};
static struct kernel_cgroup root_group={.refs=1,.id=1,.linked=1};
static struct kernel_cgroup *groups=&root_group;
static uint64_t next_id=2,device_id;
static unsigned initialized;
static uint64_t allocated_groups,mounts;
struct cgroup_node { struct kernel_vfs_node node; struct cgroup_node *next; };
struct cgroup_mount { struct kernel_vfs_mount mount; struct kernel_vfs_instance instance; };
static struct kernel_vfs_backend cgroup_ops;
struct kernel_cgroup *kernel_cgroup_root(void){return &root_group;}
struct kernel_cgroup *kernel_cgroup_get(struct kernel_cgroup *g)
{
    if(g && g!=&root_group && __atomic_fetch_add(&g->refs,1,__ATOMIC_RELAXED)==UINT32_MAX)__builtin_trap();
    return g;
}
void kernel_cgroup_put(struct kernel_cgroup *g)
{
    while(g && g!=&root_group) {
        if(__atomic_fetch_sub(&g->refs,1,__ATOMIC_ACQ_REL)!=1)return;
        if(g->linked || g->members || g->children || g->nodes)__builtin_trap();
        struct kernel_cgroup *p=g->parent;
        __atomic_fetch_sub(&allocated_groups,1,__ATOMIC_RELAXED);
        kernel_heap_release(g->heap,g);g=p;
    }
}
static struct kernel_vfs_timespec now(void)
{uint64_t t=kernel_time_realtime_ns();return (struct kernel_vfs_timespec){t/1000000000,t%1000000000};}
static unsigned kind(uint64_t id){return id&15;}
static uint64_t ino(const struct kernel_cgroup *g,unsigned k){return (g->id<<4)|k;}
/* Callers hold wait_domain: no object allocation, disposal or uaccess here. */
static struct kernel_cgroup *find(uint64_t id)
{for(struct kernel_cgroup *g=groups;g;g=g->all_next)if(g->id==id)return g;return NULL;}
static struct kernel_cgroup *child(struct kernel_cgroup *p,const char *s,size_t n)
{for(struct kernel_cgroup *g=p->children;g;g=g->next)if(strlen(g->name)==n&&!memcmp(g->name,s,n))return g;return NULL;}
static void initialize_stat(struct kernel_cgroup *g,uint32_t uid,uint32_t gid,struct kernel_vfs_timespec time)
{
    for(unsigned k=0;k<KERNEL_CGROUP_FILES;k++)g->stat[k]=(struct kernel_vfs_stat){
        .dev=device_id,.ino=ino(g,k),.mode=(k?KERNEL_VFS_S_IFREG:KERNEL_VFS_S_IFDIR)|modes[k],
        .uid=uid,.gid=gid,.nlink=k?1:2,.blksize=BOAROS_PAGE_SIZE,.atime=time,.mtime=time,.ctime=time};
}
static int available(struct kernel_cgroup *g,unsigned k)
{return k<KERNEL_CGROUP_FILES && !(g==&root_group && (k==CG_EVENTS || k==CG_TYPE));}
static int dac(struct kernel_cgroup *g,unsigned k,const struct kernel_credentials *c,unsigned access)
{struct kernel_vfs_stat *s=&g->stat[k];return kernel_vfs_dac_permission(s->mode,s->uid,s->gid,access,c);}
int kernel_cgroup_migration_permission(struct kernel_cgroup *src,struct kernel_cgroup *dst,const struct kernel_credentials *c)
{
    if(!src)src=&root_group;
    if(!dst)dst=&root_group;
    if(!dst->linked)return -KERNEL_ENODEV;
    struct kernel_cgroup *a=src,*b=dst;
    /* Parent chains share root. Switching heads aligns their depths without
     * stored depth metadata or a quadratic ancestor search. */
    while(a!=b){a=a?a->parent:dst;b=b?b->parent:src;}
    return dac(a,CG_PROCS,c,2);
}
int kernel_cgroup_permission(struct kernel_cgroup *src,struct kernel_cgroup *dst,const struct kernel_credentials *c)
{
    if(!dst)dst=&root_group;
    if(!dst->linked)return -KERNEL_ENODEV;
    int r=dac(dst,CG_PROCS,c,2);return r?r:kernel_cgroup_migration_permission(src,dst,c);
}
int kernel_cgroup_file(const struct kernel_vfs_file *file)
{return file && file->mount && file->mount->private_data && ((struct kernel_vfs_instance *)file->mount->private_data)->ops==&cgroup_ops;}
int kernel_cgroup_from_file(struct kernel_open_file_description *file,struct kernel_cgroup **out)
{
    struct kernel_vfs_path *path=kernel_open_file_path(file);
    const struct kernel_vfs_file *f=path?&path->file:kernel_open_file_vfs_borrow(file);
    if(!kernel_cgroup_file(f))return -KERNEL_EBADF;
    struct kernel_vfs_node *n=f->private_data;
    if(!n || kind(n->inode)!=CG_DIR)return -KERNEL_EBADF;
    struct kernel_cgroup *g=n->backend_data;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!g->linked)return -KERNEL_ENOENT;
    if(n->instance->read_only)return -KERNEL_EROFS;
    *out=kernel_cgroup_get(g);return 0;
}
static int error(struct kernel_vfs_instance *i){(void)i;return 0;}
static int root(struct kernel_vfs_instance *i,uint64_t *id,uint32_t *mode)
{(void)i;KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);*id=ino(&root_group,0);*mode=root_group.stat[0].mode;return 0;}
static int lookup(struct kernel_vfs_instance *i,uint64_t parent,const char *name,size_t n,uint64_t *id,uint32_t *mode)
{
    (void)i;if(kind(parent))return -KERNEL_ENOTDIR;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct kernel_cgroup *p=find(parent>>4);
    if(!p || !p->linked)return -KERNEL_ENOENT;
    for(unsigned k=1;k<KERNEL_CGROUP_FILES;k++)if(available(p,k)&&strlen(names[k])==n&&!memcmp(name,names[k],n)){
        *id=ino(p,k);*mode=p->stat[k].mode;return 0;}
    struct kernel_cgroup *g=child(p,name,n);if(!g)return -KERNEL_ENOENT;
    *id=ino(g,0);*mode=g->stat[0].mode;return 0;
}
static int open_inode(struct kernel_vfs_mount *m,const char *path,uint64_t id,uint32_t mode,struct kernel_vfs_file *file)
{
    (void)path;(void)mode;struct kernel_vfs_instance *i=m->private_data;
    struct cgroup_node *cn=NULL;if(kernel_heap_allocate_zeroed(i->heap,1,sizeof(*cn),(void **)&cn))return -KERNEL_ENOMEM;
    struct kernel_vfs_node *n=&cn->node;int r=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct kernel_cgroup *g=find(id>>4);unsigned k=kind(id);
        if(!g || !g->linked || !available(g,k))r=-KERNEL_ENOENT;
        else {
            n->inode=id;n->backend_data=kernel_cgroup_get(g);n->mode=g->stat[k].mode;n->uid=g->stat[k].uid;n->gid=g->stat[k].gid;
            if(k){n->generated_control=KERNEL_VFS_CONTROL_WRITE;
                if(k!=CG_PROCS && k!=CG_THREADS)n->generated_control|=KERNEL_VFS_CONTROL_READ;
                n->max_size=UINT64_MAX;}
            cn->next=g->nodes;g->nodes=cn;
        }
    }
    if(r){kernel_heap_release(i->heap,cn);return r;}
    return kernel_vfs_publish_node(m,n,file,0);
}
static int close_node(struct kernel_vfs_node *n)
{
    struct kernel_cgroup *g=n->backend_data;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct cgroup_node **p=&g->nodes;
        while(*p && &(*p)->node!=n)p=&(*p)->next;
        if(!*p)__builtin_trap();
        *p=(*p)->next;}
    kernel_cgroup_put(g);return 0;
}
static int mkdir_inode(struct kernel_vfs_mount *m,uint64_t parent,const char *name,uint32_t mode,uint32_t umask)
{
    if(kind(parent))return -KERNEL_ENOTDIR;
    size_t length=strlen(name);if(!length || length>255)return -KERNEL_ENAMETOOLONG;
    struct kernel_vfs_instance *i=m->private_data;struct kernel_cgroup *g=NULL;
    if(kernel_heap_allocate_zeroed(i->heap,1,sizeof(*g),(void **)&g))return -KERNEL_ENOMEM;
    g->heap=i->heap;g->refs=1;memcpy(g->name,name,length+1);
    struct kernel_vfs_timespec time=now();const struct kernel_credentials *c=kernel_credentials_current();int r=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct kernel_cgroup *p=find(parent>>4);
        if(!p || !p->linked)r=-KERNEL_ENOENT;
        else if(child(p,name,length))r=-KERNEL_EEXIST;
        else if(next_id>=UINT64_MAX/16)r=-KERNEL_ENOSPC;
        else {
            for(unsigned k=1;k<KERNEL_CGROUP_FILES;k++)if(!strcmp(names[k],name)){r=-KERNEL_EEXIST;break;}
            if(!r){g->id=next_id++;g->parent=kernel_cgroup_get(p);g->linked=1;
                initialize_stat(g,c->fsuid,c->fsgid,time);g->stat[0].mode=KERNEL_VFS_S_IFDIR|(mode&0777&~umask);
                __atomic_fetch_add(&allocated_groups,1,__ATOMIC_RELAXED);
                g->next=p->children;p->children=g;g->all_next=groups;groups=g;
                p->stat[0].nlink++;p->stat[0].mtime=p->stat[0].ctime=time;}
        }
    }
    if(r)kernel_heap_release(i->heap,g);
    return r;
}
static int rmdir_inode(struct kernel_vfs_mount *m,uint64_t parent,const char *name)
{
    (void)m;struct kernel_cgroup *g;struct kernel_vfs_timespec time=now();
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct kernel_cgroup *p=find(parent>>4);
        if(!p || kind(parent))return -KERNEL_ENOENT;
        g=child(p,name,strlen(name));if(!g)return -KERNEL_ENOENT;
        if(g->children || g->members)return -KERNEL_EBUSY;
        struct kernel_cgroup **link=&p->children;while(*link!=g)link=&(*link)->next;*link=g->next;g->next=NULL;
        link=&groups;while(*link!=g)link=&(*link)->all_next;*link=g->all_next;g->all_next=NULL;
        g->linked=0;for(unsigned k=0;k<KERNEL_CGROUP_FILES;k++)g->stat[k].nlink=0;
        p->stat[0].nlink--;p->stat[0].mtime=p->stat[0].ctime=time;
    }
    kernel_cgroup_put(g);return 0;
}
static int unlink_inode(struct kernel_vfs_mount *m,uint64_t p,const char *n)
{(void)m;(void)p;(void)n;return -KERNEL_EPERM;}
static int rename_inode(struct kernel_vfs_instance *i,uint64_t p,const char *s,uint64_t q,const char *t,unsigned f,struct kernel_vfs_rename_result *r)
{(void)i;(void)p;(void)s;(void)q;(void)t;(void)f;(void)r;return -KERNEL_EPERM;}
static int permission(struct kernel_vfs_node *n,const struct kernel_credentials *c,unsigned access)
{KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);return dac(n->backend_data,kind(n->inode),c,access);}
static int stat_inode(const struct kernel_vfs_file *file,struct kernel_vfs_stat *s)
{struct kernel_vfs_node *n=file->private_data;KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);*s=((struct kernel_cgroup *)n->backend_data)->stat[kind(n->inode)];return 0;}
static int statfs_inode(struct kernel_vfs_mount *m,struct kernel_vfs_statfs *s)
{*s=(struct kernel_vfs_statfs){.type=0x63677270,.block_size=BOAROS_PAGE_SIZE,.name_length=255,.fsid=device_id,.flags=m->read_only?1:0};return 0;}
static void refresh_nodes(struct kernel_cgroup *g,unsigned k)
{for(struct cgroup_node *n=g->nodes;n;n=n->next)if(kind(n->node.inode)==k){n->node.mode=g->stat[k].mode;n->node.uid=g->stat[k].uid;n->node.gid=g->stat[k].gid;}}
static int set_mode(struct kernel_vfs_file *f,uint32_t mode)
{
    struct kernel_vfs_node *n=f->private_data;struct kernel_cgroup *g=n->backend_data;unsigned k=kind(n->inode);
    struct kernel_vfs_timespec time=now();KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!g->linked)return -KERNEL_ENODEV;
    g->stat[k].mode=(g->stat[k].mode&KERNEL_VFS_S_IFMT)|(mode&07777);g->stat[k].ctime=time;refresh_nodes(g,k);return 0;
}
static int set_owner(struct kernel_vfs_file *f,uint32_t uid,uint32_t gid)
{
    struct kernel_vfs_node *n=f->private_data;struct kernel_cgroup *g=n->backend_data;unsigned k=kind(n->inode);
    struct kernel_vfs_timespec time=now();KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!g->linked)return -KERNEL_ENODEV;
    if(uid!=UINT32_MAX)g->stat[k].uid=uid;
    if(gid!=UINT32_MAX)g->stat[k].gid=gid;
    g->stat[k].ctime=time;refresh_nodes(g,k);return 0;
}
static int set_times(struct kernel_vfs_file *f,const struct kernel_vfs_timespec t[2])
{struct kernel_vfs_node *n=f->private_data;KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);struct kernel_cgroup *g=n->backend_data;
    if(!g->linked)return -KERNEL_ENODEV;
    g->stat[kind(n->inode)].atime=t[0];g->stat[kind(n->inode)].mtime=t[1];return 0;}
static int dir_entry(struct kernel_vfs_file *f,uint64_t position,uint64_t *next,uint64_t *id,uint8_t *type,char *name,size_t size)
{
    struct kernel_vfs_node *n=f->private_data;if(kind(n->inode))return -KERNEL_ENOTDIR;
    struct kernel_cgroup *g=n->backend_data;KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!g->linked)return 0;
    const char *s=NULL;
    if(position<2){s=position?"..":".";*id=ino(position&&g->parent?g->parent:g,0);*next=position+1;*type=4;}
    else {
        for(unsigned k=1;k<KERNEL_CGROUP_FILES;k++)if(position<=k+1 && available(g,k)){
            s=names[k];*id=ino(g,k);*next=k+2;*type=8;break;}
        if(!s){struct kernel_cgroup *best=NULL;uint64_t after=position>16?position-16:0;
            for(struct kernel_cgroup *p=g->children;p;p=p->next)if(p->id>=after && (!best || p->id<best->id))best=p;
            if(!best)return 0;
            s=best->name;*id=ino(best,0);*next=best->id+17;*type=4;}
    }
    size_t len=strlen(s);if(size<=len)return -KERNEL_ERANGE;memcpy(name,s,len+1);return 1;
}
static void copy_part(uint64_t offset,uint64_t start,const char *part,size_t len,char *out,size_t size,size_t *done)
{
    if(offset>=start+len || offset+*done<start || *done>=size)return;
    size_t begin=(size_t)(offset+*done-start);if(begin>=len)return;
    size_t count=len-begin;if(count>size-*done)count=size-*done;memcpy(out+*done,part+begin,count);*done+=count;
}
static int whitespace(char c){return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f';}
/* Match kstrtoint(..., 0) followed by the cgroup PID nonnegative check. */
static int parse_pid(const char *text,size_t length,int32_t *pid)
{
    if(!length)return -KERNEL_EINVAL;
    size_t at=0;int negative=0;
    if(text[at]=='+' || text[at]=='-'){negative=text[at]=='-';if(++at==length)return -KERNEL_EINVAL;}
    unsigned base=10;
    if(text[at]=='0') {
        base=8;
        if(at+1<length && (text[at+1]=='x' || text[at+1]=='X')){base=16;at+=2;}
    }
    if(at==length)return -KERNEL_EINVAL;
    uint32_t value=0;
    for(;at<length;at++) {
        unsigned digit=text[at]>='0'&&text[at]<='9'?(unsigned)(text[at]-'0'):
            text[at]>='a'&&text[at]<='f'?(unsigned)(text[at]-'a'+10):
            text[at]>='A'&&text[at]<='F'?(unsigned)(text[at]-'A'+10):UINT32_MAX;
        if(digit>=base || value>((uint32_t)INT32_MAX-digit)/base)return -KERNEL_EINVAL;
        value=value*base+digit;
    }
    if(negative && value)return -KERNEL_EINVAL;
    *pid=(int32_t)value;return 0;
}
static int control(struct kernel_vfs_node *n,int write,uint64_t *offset,char *bytes,size_t size,size_t *count)
{
    struct kernel_cgroup *g=n->backend_data;unsigned k=kind(n->inode);*count=0;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);if(!g->linked)return -KERNEL_ENODEV;}
    if(write) {
        size_t begin=0,end=size;while(begin<end&&whitespace(bytes[begin]))begin++;while(end>begin&&whitespace(bytes[end-1]))end--;
        if(k==CG_PROCS || k==CG_THREADS){
            int32_t pid=0;int r=parse_pid(bytes+begin,end-begin,&pid);if(r)return r;
            r=kernel_cgroup_move(g,pid,k==CG_THREADS,kernel_credentials_current());if(r)return r;
        }else if(k==CG_SUBTREE){
            if(begin!=end)return -KERNEL_EINVAL;
        }else if(k==CG_TYPE){return end-begin==8&&!memcmp(bytes+begin,"threaded",8)?-KERNEL_EOPNOTSUPP:-KERNEL_EINVAL;}
        else return -KERNEL_EACCES;
        *count=size;*offset+=size;return 0;
    }
    if(k==CG_PROCS || k==CG_THREADS)return -KERNEL_EINVAL;
    const char *text="";
    if(k==CG_TYPE)text="domain\n";
    else if(k==CG_EVENTS)text=kernel_cgroup_populated(g)?"populated 1\nfrozen 0\n":"populated 0\nfrozen 0\n";
    copy_part(*offset,0,text,strlen(text),bytes,size,count);
    *offset+=*count;return 0;
}
static int snapshot(struct kernel_vfs_node *n,struct kernel_heap *heap,
    struct kernel_namespace *ns,char **buffer,size_t *size)
{
    (void)ns;unsigned k=kind(n->inode);
    if(k!=CG_PROCS && k!=CG_THREADS)return -KERNEL_EINVAL;
    return kernel_cgroup_members_snapshot(n->backend_data,k==CG_THREADS,heap,buffer,size);
}
static int no_truncate(struct kernel_vfs_node *n,uint64_t s,uint64_t *a,int *changed)
{(void)n;(void)s;(void)a;*changed=0;return -KERNEL_EINVAL;}
static int pread_inode(struct kernel_vfs_node *n,uint64_t o,void *b,size_t s,size_t *c)
{if(!kind(n->inode))return -KERNEL_EISDIR;return control(n,0,&o,b,s,c);}
static void accessed(struct kernel_vfs_file *f){(void)f;}
static int modified(struct kernel_vfs_file *f,uint64_t o,int a){(void)f;(void)o;(void)a;return -KERNEL_EINVAL;}
static void release_unlinked(struct kernel_vfs_node *n){(void)n;}
static int unmount(struct kernel_vfs_mount *m)
{
    struct kernel_vfs_instance *i=m->private_data;
    if(m->covered_path||m->root_path||m->child_mounts||i->external_files||!kernel_vfs_nodes_idle(i,0)||i->paths)return -KERNEL_EBUSY;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);if(!mounts)__builtin_trap();mounts--;}
    kernel_heap_release(i->heap,i->backend_data);return 0;
}
static struct kernel_vfs_backend cgroup_ops={.type="cgroup2",.control_open_credentials=true,
    .error=error,.root=root,.lookup=lookup,.open=open_inode,.close_node=close_node,
    .mkdir=mkdir_inode,.rmdir=rmdir_inode,.unlink=unlink_inode,.rename=rename_inode,
    .permission=permission,.stat=stat_inode,.statfs=statfs_inode,.set_mode=set_mode,.set_owner=set_owner,.set_times=set_times,
    .dir_entry=dir_entry,.control=control,.snapshot=snapshot,.pread=pread_inode,.truncate=no_truncate,
    .accessed=accessed,.modified=modified,.release_unlinked=release_unlinked,.writeback_allowed=error,.flush=error,.unmount=unmount};
int kernel_cgroup_mount(struct kernel_heap *heap,uint64_t flags,struct kernel_vfs_mount **out)
{
    if(!heap || !out || *out)return -KERNEL_EINVAL;
    struct cgroup_mount *m=NULL;if(kernel_heap_allocate_zeroed(heap,1,sizeof(*m),(void **)&m))return -KERNEL_ENOMEM;
    m->instance.heap=heap;m->instance.ops=&cgroup_ops;m->instance.backend_data=m;m->instance.read_only=!!(flags&1);
    kernel_rwlock_init(&m->instance.reconfiguration,19,(uintptr_t)&m->instance);
    kernel_mutex_init(&m->instance.namespace_lock,20,(uintptr_t)&m->instance);
    kernel_raw_lock_init(&m->instance.error_lock,KERNEL_RAW_RANK_OBJECT);
    m->mount.private_data=&m->instance;m->mount.id=kernel_vfs_allocate_mount_id();m->mount.state=VFS_MOUNT_STATE_LIVE;
    struct kernel_vfs_timespec time=now();
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);if(!initialized){device_id=m->mount.id;initialize_stat(&root_group,0,0,time);initialized=1;}mounts++;}
    kernel_vfs_mount_initialize(&m->mount);m->instance.device_id=device_id;*out=&m->mount;return 0;
}

/* Final kernel shutdown only, after tasks and VFS views have been destroyed.
 * Unmount deliberately retains the hierarchy. Reject every outside owner
 * before changing anything; detached groups also count until their final put. */
int kernel_cgroup_shutdown(struct kernel_heap *heap)
{
    struct kernel_cgroup *release;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(mounts || root_group.nodes || root_group.members)return -KERNEL_EBUSY;
        uint64_t count=0;
        for(struct kernel_cgroup *g=groups;g;g=g->all_next) {
            if(g==&root_group)continue;
            uint64_t expected=1;
            for(struct kernel_cgroup *c=g->children;c;c=c->next)expected++;
            if(g->heap!=heap || g->nodes || g->members || !g->linked ||
                __atomic_load_n(&g->refs,__ATOMIC_ACQUIRE)!=expected)return -KERNEL_EBUSY;
            count++;
        }
        if(count!=__atomic_load_n(&allocated_groups,__ATOMIC_RELAXED))return -KERNEL_EBUSY;
        release=groups;
        for(struct kernel_cgroup *g=release;g;g=g->all_next) {
            if(g==&root_group)continue;
            g->linked=0;g->children=NULL;g->next=NULL;
        }
        groups=&root_group;root_group.children=NULL;root_group.all_next=NULL;
        memset(root_group.stat,0,sizeof(root_group.stat));
        initialized=0;device_id=0;next_id=2;
    }
    /* Newest-first insertion orders descendants before ancestors; each group
     * still owns its parent until its own tree reference is dropped. */
    while(release && release!=&root_group) {
        struct kernel_cgroup *next=release->all_next;release->all_next=NULL;
        kernel_cgroup_put(release);release=next;
    }
    return 0;
}
