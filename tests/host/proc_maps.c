/* Exercise production procfs maps formatting and directory protocol. */
#include "../../fs/procfs.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct kernel_mm_proc_map fixture[6];
static struct kernel_vfs_file backing;
static unsigned references, allocations;
static int stat_error, allocation_fail;
static int fdinfo_pid = 42;
static struct kernel_heap heap;
static struct kernel_fs_context fs;

struct kernel_task *kernel_task_current(void) { return 0; }
enum kernel_task_status kernel_task_fs_context_borrow(const struct kernel_task *t,
    const struct kernel_fs_context **out)
{ (void)t; *out = &fs; return KERNEL_TASK_STATUS_OK; }
struct kernel_fs_paths kernel_fs_context_snapshot(const struct kernel_fs_context *context)
{ assert(context == &fs); return (struct kernel_fs_paths){0}; }
void kernel_fs_paths_release(struct kernel_fs_paths *paths) { (void)paths; }
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h, size_t size, void **out)
{
    assert(h == &heap);
    if (allocation_fail && --allocation_fail == 0) return KERNEL_HEAP_STATUS_EMPTY;
    *out = malloc(size); if (!*out) return KERNEL_HEAP_STATUS_EMPTY;
    allocations++; return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *h, void *data)
{ assert(h == &heap && data && allocations); allocations--; free(data); }
int kernel_proc_maps_snapshot(kernel_pid_t pid, uint64_t identity,
    struct kernel_heap *h, struct kernel_mm_proc_map **rows, uint32_t *count)
{
    assert(pid == 42 && identity == 17 && !*rows);
    if (kernel_heap_allocate(h, sizeof(fixture), (void **)rows)) return -KERNEL_ENOMEM;
    memcpy(*rows, fixture, sizeof(fixture)); *count = 6; references++;
    return 0;
}
const struct kernel_vfs_file *kernel_open_file_vfs_borrow(const struct kernel_open_file_description *file)
{ assert((uintptr_t)file == 1 && references); return &backing; }
struct kernel_vfs_path *kernel_open_file_path(const struct kernel_open_file_description *file)
{ assert((uintptr_t)file == 1 && references); return (void *)(uintptr_t)2; }
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **file)
{ assert((uintptr_t)*file == 1 && references); references--; *file = 0; return 0; }
int kernel_vfs_fstat(const struct kernel_vfs_file *file, struct kernel_vfs_stat *stat)
{
    assert(file == &backing && references);
    *stat = (struct kernel_vfs_stat){.dev = 0x123456789abcULL, .ino = 9876543210ULL};
    return stat_error;
}
int kernel_vfs_path_link_string(const struct kernel_vfs_path *path,
    const struct kernel_vfs_path *root, char *text, size_t size)
{
    (void)root; assert((uintptr_t)path == 2 && size > 64 && references);
    strcpy(text, "/actual\nfile (deleted)"); return 0;
}
uint64_t kernel_vfs_file_inode(const struct kernel_vfs_file *file)
{ return ((struct kernel_vfs_node *)file->private_data)->inode; }
int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity)
{ assert(pid == 42); *identity = 17; return 0; }
int kernel_proc_thread_identity(kernel_pid_t group, uint64_t identity, kernel_pid_t tid, uint64_t *out)
{ (void)group; (void)identity; (void)tid; (void)out; abort(); }
int kernel_proc_thread_group(kernel_pid_t tid, uint64_t identity, kernel_pid_t *tgid, uint64_t *out)
{ (void)tid; (void)identity; (void)tgid; (void)out; abort(); }
int kernel_proc_next_thread(kernel_pid_t tgid, uint64_t identity, kernel_pid_t after,
    kernel_pid_t *tid, uint64_t *out)
{ (void)tgid; (void)identity; (void)after; (void)tid; (void)out; abort(); }
int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity, int after, int *fd)
{ (void)pid; (void)identity; (void)after; (void)fd; abort(); }
int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid, uint64_t *identity)
{ (void)after; (void)pid; (void)identity; abort(); }
int kernel_proc_fdinfo_snapshot(kernel_pid_t pid,uint64_t identity,int fd,
    struct kernel_proc_fdinfo *info)
{
    assert(pid==42 && identity==17 && fd==9);
    *info=(struct kernel_proc_fdinfo){.position=99,.flags=02000002,.mount_id=12,
        .inode=7,.pidfd=1,.pid=fdinfo_pid};
    return 0;
}

int main(void)
{
    for (unsigned i = 0; i < 6; i++) fixture[i] = (struct kernel_mm_proc_map){
        .start = 0x100000ULL + i * 0x10000ULL, .end = 0x101000ULL + i * 0x10000ULL,
        .permissions = KERNEL_MM_READ | KERNEL_MM_WRITE };
    fixture[0].role = KERNEL_VMA_ROLE_HEAP;
    fixture[1].role = KERNEL_VMA_ROLE_STACK;
    fixture[2].role = KERNEL_VMA_ROLE_VDSO;
    fixture[2].permissions = KERNEL_MM_READ | KERNEL_MM_EXECUTE;
    fixture[3].permissions = 0;
    fixture[4].shared = 1;
    fixture[5].file = (void *)(uintptr_t)1; fixture[5].offset = 0x12345000;
    struct kernel_vfs_node node = {.inode = proc_pid_inode(42, 17, PROC_PID_MAPS_KIND)};
    assert(!proc_kind_is_thread(proc_inode_kind(node.inode)));
    uint64_t identity; assert(!proc_inode_identity_current(node.inode, &identity) && identity == 17);
    uint64_t directory = proc_pid_inode(42, 17, PROC_PID_DIR_KIND), inode, next;
    uint32_t mode; assert(!proc_lookup(0, directory, "maps", 4, &inode, &mode));
    assert(inode == node.inode && mode == (KERNEL_VFS_S_IFREG | 0444));
    struct kernel_vfs_node dir_node = {.inode = directory};
    struct kernel_vfs_file dir = {.private_data = &dir_node};
    char name[32]; uint8_t type;
    assert(proc_dir_entry(&dir, 10, &next, &inode, &type, name, sizeof(name)) == 1);
    assert(!strcmp(name, "maps") && next == 11 && inode == node.inode && type == 8);
    assert(proc_dir_entry(&dir, next, &next, &inode, &type, name, sizeof(name))==1);
    assert(!strcmp(name,"fdinfo") && type==4);
    assert(!proc_dir_entry(&dir,next,&next,&inode,&type,name,sizeof(name)));
    char *text = 0; size_t length = 0;
    assert(!proc_maps_snapshot(&node, &heap, &text, &length));
    assert(strlen(text) == length && !references && allocations == 1);
    const char *names[] = {"[heap]", "[stack]", "[vdso]", "", "", "/actual\\012file (deleted)"};
    char *line = text;
    for (unsigned i = 0; i < 6; i++) {
        unsigned long long start, end, offset, ino; unsigned major, minor; char perms[5]; int consumed = 0;
        assert(sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &start, &end,
            perms, &offset, &major, &minor, &ino, &consumed) == 7);
        assert(start == fixture[i].start && end == fixture[i].end && offset == fixture[i].offset);
        assert(perms[3] == (fixture[i].shared ? 's' : 'p'));
        assert(!strncmp(perms, i == 2 ? "r-x" : i == 3 ? "---" : "rw-", 3));
        if (i == 5) assert(major == 0x189a && minor == 0x234567bc && ino == 9876543210ULL);
        else assert(!major && !minor && !ino);
        char *end_line = strchr(line, '\n'); assert(end_line);
        if (*names[i]) assert((size_t)(end_line - line - consumed) == strlen(names[i]) && !strncmp(line + consumed, names[i], strlen(names[i])));
        line = end_line + 1;
    }
    assert(!*line); kernel_heap_release(&heap, text);
    for (int failed = 1; failed <= 3; failed++) {
        allocation_fail = failed; text = 0;
        assert(proc_maps_snapshot(&node, &heap, &text, &length) == -KERNEL_ENOMEM);
        assert(!text && !references && !allocations);
    }
    allocation_fail = 0; stat_error = -KERNEL_EIO; text = 0;
    assert(proc_maps_snapshot(&node, &heap, &text, &length) == -KERNEL_EIO);
    assert(!text && !references && !allocations);
    stat_error=0;allocation_fail=0;
    node.inode=proc_pid_inode_fd(42,17,PROC_PID_FDINFO_KIND,9);
    text=0;assert(!proc_fdinfo_snapshot(&node,&heap,&text,&length));
    assert(strstr(text,"pos:\t99\n") && strstr(text,"flags:\t02000002\n") &&
        strstr(text,"Pid:\t42\nNSpid:\t42\n"));kernel_heap_release(&heap,text);
    fdinfo_pid=-1;text=0;assert(!proc_fdinfo_snapshot(&node,&heap,&text,&length));
    assert(strstr(text,"Pid:\t-1\nNSpid:\t-1\n"));kernel_heap_release(&heap,text);
    allocation_fail=1;text=0;assert(proc_fdinfo_snapshot(&node,&heap,&text,&length)==-KERNEL_ENOMEM);
    assert(!text && !allocations);
    puts("PASS proc pidfd fdinfo emits both Pid/NSpid, retained-dead -1, flags and allocation ownership");
    puts("PASS proc maps directory/type, Linux fields, real backing identity/path, labels, permissions, escaping and failure owner release");
    return 0;
}
