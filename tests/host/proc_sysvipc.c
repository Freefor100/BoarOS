/* Production lookup/directory and text snapshots; object providers are modeled. */
#include "../../fs/procfs.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct kernel_heap heap;
static struct kernel_namespace ipc_namespace={.type=KERNEL_NS_IPC};
static unsigned allocations, fail_at, allocation_calls, rows = 1;
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h, size_t size, void **out)
{
    assert(h == &heap);
    if (++allocation_calls == fail_at) return KERNEL_HEAP_STATUS_EMPTY;
    *out = malloc(size); assert(*out); allocations++;
    return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *h, void *data)
{ assert(h == &heap && data && allocations); allocations--; free(data); }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    enum kernel_heap_status status=kernel_heap_allocate(h,n*size,out);
    if(!status)memset(*out,0,n*size);
    return status;
}
static int next(uint32_t *cursor, int32_t *id, struct kernel_ipc64_perm *p)
{
    if (*cursor >= rows) return 0;
    *id = (int32_t)(100 + (*cursor)++);
    *p = (struct kernel_ipc64_perm){.key = -12345, .mode = 0640,
        .uid = 1001, .gid = 1002, .cuid = 1003, .cgid = 1004};
    return 1;
}
int kernel_msg_proc_next(struct kernel_namespace *ns,struct kernel_pid_registry *view,uint32_t *cursor, int32_t *id, struct kernel_msqid64_ds *row)
{
    assert(ns==&ipc_namespace&&!view);
    *row = (struct kernel_msqid64_ds){.msg_cbytes = 123, .msg_qnum = 7,
        .msg_lspid = 44, .msg_lrpid = 55, .msg_stime = 111, .msg_rtime = 222, .msg_ctime = 333};
    return next(cursor, id, &row->msg_perm);
}
int kernel_sem_proc_next(struct kernel_namespace *ns,uint32_t *cursor, int32_t *id, struct kernel_semid64_ds *row)
{
    assert(ns==&ipc_namespace);
    *row = (struct kernel_semid64_ds){.sem_nsems = 9, .sem_otime = 222, .sem_ctime = 333};
    return next(cursor, id, &row->sem_perm);
}
int kernel_shm_proc_next(struct kernel_namespace *ns,struct kernel_pid_registry *view,uint32_t *cursor, int32_t *id, struct kernel_shmid64_ds *row, uint64_t *rss)
{
    assert(ns==&ipc_namespace&&!view);
    *row = (struct kernel_shmid64_ds){.shm_segsz = 123456, .shm_nattch = 2,
        .shm_cpid = 44, .shm_lpid = 55, .shm_atime = 111, .shm_dtime = 222, .shm_ctime = 333};
    *rss = BOAROS_PAGE_SIZE;
    return next(cursor, id, &row->shm_perm);
}

int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity)
{ if(pid!=42)return -KERNEL_ENOENT;*identity=7;return 0; }
int kernel_proc_thread_identity(kernel_pid_t group, uint64_t identity, kernel_pid_t tid, uint64_t *out)
{ (void)group; (void)identity;if(tid!=43)return -KERNEL_ENOENT;*out=8;return 0; }
int kernel_proc_thread_resources_live(kernel_pid_t tid,uint64_t identity)
{return tid==43 && identity==8 ? 0 : -KERNEL_ENOENT;}
int kernel_proc_thread_group(kernel_pid_t tid, uint64_t identity, kernel_pid_t *tgid, uint64_t *out)
{ (void)tid; (void)identity; (void)tgid; (void)out; return -KERNEL_ENOENT; }
int kernel_proc_next_thread(kernel_pid_t tgid, uint64_t identity, kernel_pid_t after,
    kernel_pid_t *tid, uint64_t *out)
{ (void)tgid; (void)identity; (void)after; (void)tid; (void)out; return -KERNEL_ENOENT; }
int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity, int after, int *fd)
{ (void)pid; (void)identity; (void)after; (void)fd; return -KERNEL_ENOENT; }
int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid, uint64_t *identity)
{ (void)after; (void)pid; (void)identity; return -KERNEL_ENOENT; }
uint64_t kernel_vfs_file_inode(const struct kernel_vfs_file *file)
{ return ((struct kernel_vfs_node *)file->private_data)->inode; }
int kernel_proc_inode_owner(kernel_pid_t pid,uint64_t identity,int thread,uint32_t mode,uint32_t *uid,uint32_t *gid)
{
    (void)mode;
    if(pid!=(thread?43:42) || identity!=(thread?8U:7U))return -KERNEL_ENOENT;
    *uid=*gid=1000;return 0;
}
int kernel_proc_process_identity_acquire(kernel_pid_t pid,uint64_t identity,struct kernel_pid **owner)
{(void)pid;(void)identity;(void)owner;return -KERNEL_ENOENT;}
int kernel_vfs_publish_node(struct kernel_vfs_mount *mount,struct kernel_vfs_node *node,
    struct kernel_vfs_file *file,int creating)
{
    assert(!creating && node->uid==1000 && node->gid==1000);
    *file=(struct kernel_vfs_file){.mount=mount,.private_data=node,.mode=node->mode};return 0;
}
static void namespace_directories(void)
{
    const char *const names[]={"mnt","uts","ipc","pid","pid_for_children"};
    const uint32_t types[]={KERNEL_NS_MNT,KERNEL_NS_UTS,KERNEL_NS_IPC,KERNEL_NS_PID,KERNEL_NS_PID|KERNEL_PROC_PID_FOR_CHILDREN};
    struct kernel_vfs_instance instance={.heap=&heap};
    struct kernel_vfs_mount mount={.private_data=&instance};
    for(unsigned thread=0;thread<2;thread++) {
        kernel_pid_t pid=thread?43:42;uint64_t identity=thread?8:7;
        struct kernel_vfs_node directory={.inode=thread ?
            proc_thread_resource_inode(pid,identity,PROC_PID_NS_DIR_KIND,0) :
            proc_pid_inode(pid,identity,PROC_PID_NS_DIR_KIND)};
        struct kernel_vfs_file file={.private_data=&directory};
        for(unsigned i=0;i<5;i++) {
            uint64_t inode,next,looked;uint8_t type;char name[24];uint32_t mode;
            assert(proc_dir_entry(&file,i+2,&next,&inode,&type,name,sizeof(name))==1);
            assert(type==10 && next==i+3 && !strcmp(name,names[i]));
            assert(!proc_lookup(0,directory.inode,name,strlen(name),&looked,&mode) && looked==inode);
            assert(proc_namespace_type(proc_inode_kind(inode))==types[i]);
            assert(proc_inode_is_thread(inode)==(int)thread && proc_inode_identity(inode)==identity);
            assert(mode==(KERNEL_VFS_S_IFLNK|0777U));
            struct kernel_vfs_file opened={0};
            assert(!proc_open(&mount,name,inode,mode,&opened));
            assert(opened.mode==mode && kernel_vfs_file_inode(&opened)==inode);
            kernel_heap_release(&heap,opened.private_data);
            opened=(struct kernel_vfs_file){0};
            assert(proc_open(&mount,name,inode,KERNEL_VFS_S_IFREG|0444U,&opened)==-KERNEL_EINVAL);
            assert(!opened.private_data && !allocations);
        }
        uint64_t next,inode;uint8_t type;char name[24];
        assert(!proc_dir_entry(&file,7,&next,&inode,&type,name,sizeof(name)));
    }
}
int kernel_proc_view_lookup(struct kernel_pid_registry *view, kernel_pid_t number,
    kernel_pid_t group, uint64_t generation, int thread, kernel_pid_t *canonical, uint64_t *out)
{ (void)view; (void)number; (void)group; (void)generation; (void)thread; (void)canonical; (void)out; abort(); }
int kernel_proc_view_next(struct kernel_pid_registry *view, kernel_pid_t after,
    kernel_pid_t group, uint64_t generation, int thread, kernel_pid_t *number,
    kernel_pid_t *canonical, uint64_t *out)
{ (void)view; (void)after; (void)group; (void)generation; (void)thread; (void)number; (void)canonical; (void)out; abort(); }

int main(void)
{
    namespace_directories();
    uint64_t inode; uint32_t mode;
    assert(!proc_lookup(0, PROC_ROOT_INODE, "sysvipc", 7, &inode, &mode));
    assert(mode == (KERNEL_VFS_S_IFDIR | 0555));
    struct kernel_vfs_node directory = {.inode = inode};
    struct kernel_vfs_file file = {.private_data = &directory};
    uint64_t position = 0, next_position; uint8_t type; char name[24];
    const char *const entries[] = {".", "..", "shm", "sem", "msg"};
    for (unsigned i = 0; i < 5; i++) {
        assert(proc_dir_entry(&file, position, &next_position, &inode, &type, name, sizeof(name)) == 1);
        assert(next_position == position + 1 && !strcmp(name, entries[i]) && type == (i < 2 ? 4 : 8));
        if (i >= 2) {
            uint64_t target; assert(!proc_lookup(0, PROC_SYSVIPC_INODE, name, 3, &target, &mode));
            assert(target == inode && mode == (KERNEL_VFS_S_IFREG | 0444));
        }
        position = next_position;
    }
    assert(!proc_dir_entry(&file, position, &next_position, &inode, &type, name, sizeof(name)));
    directory.inode = PROC_ROOT_INODE;
    assert(proc_dir_entry(&file, 8, &next_position, &inode, &type, name, sizeof(name)) == 1);
    assert(!strcmp(name, "sysvipc") && inode == PROC_SYSVIPC_INODE && type == 4);
    assert(proc_dir_entry(&file, next_position, &next_position, &inode, &type, name, sizeof(name)) == 1);
    assert(!strcmp(name, "net") && inode == PROC_NET_INODE && type == 4);
    assert(!proc_lookup(0, PROC_NET_INODE, "route", 5, &inode, &mode));
    assert(inode == PROC_NET_ROUTE_INODE && mode == (KERNEL_VFS_S_IFREG | 0444));
    const uint64_t inodes[] = {PROC_SYSVIPC_SHM_INODE, PROC_SYSVIPC_SEM_INODE, PROC_SYSVIPC_MSG_INODE};
    for (unsigned i = 0; i < 3; i++) {
        char *text = 0; size_t length = 0;
        assert(!proc_sysvipc_snapshot(inodes[i], &ipc_namespace,NULL,&heap, &text, &length));
        assert(length == strlen(text) && allocations == 1);
        char *line = strchr(text, '\n') + 1;
        int key, id; unsigned mode_value;
        unsigned long long values[14];
        int fields = sscanf(line, "%d %d %o %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &key, &id, &mode_value, &values[0], &values[1], &values[2], &values[3], &values[4],
            &values[5], &values[6], &values[7], &values[8], &values[9], &values[10], &values[11], &values[12]);
        assert(key == -12345 && id == 100 && mode_value == 0640);
        if (i == 0) {
            assert(fields == 16 && values[0] == 123456 && values[1] == 44 && values[2] == 55 &&
                values[3] == 2 && values[4] == 1001 && values[7] == 1004 && values[8] == 111 &&
                values[10] == 333 && values[11] == BOAROS_PAGE_SIZE && values[12] == 0);
        } else if (i == 1) {
            assert(fields == 10 && values[0] == 9 && values[1] == 1001 && values[4] == 1004 &&
                values[5] == 222 && values[6] == 333);
        } else {
            assert(fields == 14 && values[0] == 123 && values[1] == 7 && values[2] == 44 &&
                values[3] == 55 && values[4] == 1001 && values[7] == 1004 && values[8] == 111 && values[10] == 333);
        }
        kernel_heap_release(&heap, text);
        rows = 200;
        text = 0; assert(!proc_sysvipc_snapshot(inodes[i], &ipc_namespace,NULL,&heap, &text, &length));
        unsigned lines = 0; for (char *p = text; *p; p++) lines += *p == '\n';
        assert(lines == rows + 1 && allocations == 1); kernel_heap_release(&heap, text);
        for (unsigned failed = 1; failed <= 2; failed++) {
            fail_at = failed; allocation_calls = 0; text = 0;
            assert(proc_sysvipc_snapshot(inodes[i], &ipc_namespace,NULL,&heap, &text, &length) == -KERNEL_ENOMEM);
            assert(!text && !allocations);
        }
        fail_at = 0; rows = 0; text = 0;
        assert(!proc_sysvipc_snapshot(inodes[i], &ipc_namespace,NULL,&heap, &text, &length));
        assert(strchr(text, '\n') == text + length - 1); kernel_heap_release(&heap, text);
        rows = 1;
    }
    puts("PASS proc sysvipc directory/cookies, native fields, signed keys/octal mode, growth, empty tables and OOM owner release");
    return 0;
}
