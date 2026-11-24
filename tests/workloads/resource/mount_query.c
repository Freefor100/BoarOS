#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "MOUNT QUERY FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); _exit(1); } } while (0)
#define ERROR(x, e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)

enum {
    SB_BASIC = 1, MNT_BASIC = 2, PROPAGATE_FROM = 4, MNT_ROOT = 8,
    MNT_POINT = 16, FS_TYPE = 32, MNT_NS_ID = 64, SB_SOURCE = 512,
    SUPPORTED_MASK = 4096, BY_FD = 1, REVERSE = 1,
    UNIQUE_STATX = 0x4000, LEGACY_STATX = 0x1000,
};
#define MOUNT_QUERY_ROOT UINT64_MAX
#define CORE_MASK (SB_BASIC | MNT_BASIC | PROPAGATE_FROM | MNT_ROOT | MNT_POINT | FS_TYPE | MNT_NS_ID | SB_SOURCE | SUPPORTED_MASK)

struct request {
    uint32_t size, fd;
    uint64_t id, param, ns;
};
struct mount_result {
    uint32_t size, opts;
    uint64_t mask;
    uint32_t major, minor;
    uint64_t magic;
    uint32_t sb_flags, type;
    uint64_t id, parent;
    uint32_t old, old_parent;
    uint64_t attr, propagation, peer, master, dominating;
    uint32_t root, point;
    uint64_t ns;
    uint32_t subtype, source, opt_count, opt_array, sec_count, sec_array;
    uint64_t supported;
    uint32_t uid_count, uid_map, gid_count, gid_map;
    uint64_t spare[43];
    char strings[512];
};

_Static_assert(sizeof(struct request)==32, "mnt_id_req");
_Static_assert(__builtin_offsetof(struct mount_result,strings)==512, "statmount");

static const char work[] = "/tmp/mount-query";
static const char original[] = "/tmp/mount-query/a";
static const char subdir[] = "/tmp/mount-query/a/sub";
static const char bind_point[] = "/tmp/mount-query/a/sub/in";

static long query(const struct request *request, void *buffer, size_t size, unsigned flags)
{ return syscall(457, request, buffer, size, flags); }

static long list(const struct request *request, uint64_t *ids, size_t count, unsigned flags)
{ return syscall(458, request, ids, count, flags); }

static struct mount_result by_file(int fd)
{
    struct mount_result mount;
    const struct request request = {.size=24, .fd=(uint32_t)fd, .param=CORE_MASK};
    CHECK(!query(&request, &mount, sizeof(mount), BY_FD));
    CHECK(mount.size>=513 && mount.size<=sizeof(mount));
    CHECK((mount.mask&(MNT_BASIC|MNT_ROOT|FS_TYPE|SUPPORTED_MASK))==(MNT_BASIC|MNT_ROOT|FS_TYPE|SUPPORTED_MASK));
    CHECK((mount.supported&CORE_MASK)==CORE_MASK);
    CHECK(mount.strings[0]==0);
    return mount;
}

static uint64_t path_id(const char *path, unsigned mask)
{
    struct statx stat;
    CHECK(!syscall(SYS_statx, AT_FDCWD, path, 0, mask, &stat));
    CHECK(stat.stx_mask&mask);
    return stat.stx_mnt_id;
}

static void verify_chroot(uint64_t outside, uint64_t inside)
{
    pid_t pid=fork();
    CHECK(pid>=0);
    if (!pid) {
        CHECK(!chroot(subdir) && !chdir("/") && !setgid(65534) && !setuid(65534));
        struct request request={.size=24, .id=outside, .param=MNT_BASIC};
        struct mount_result output;
        uint64_t ids[4];
        ERROR(query(&request,&output,sizeof(output),0), EPERM);
        request.param=0;
        ERROR(list(&request,ids,4,0), EPERM);
        request.id=MOUNT_QUERY_ROOT;
        CHECK(list(&request,ids,4,0)==1 && ids[0]==inside);
        request.id=inside;
        request.param=MNT_BASIC|MNT_POINT;
        CHECK(!query(&request,&output,sizeof(output),0) && output.id==inside);
        CHECK(!strcmp(output.strings+output.point,"/in"));
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

int main(void)
{
    CHECK(!unshare(CLONE_NEWNS) && !mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL));
    CHECK(!mkdir(work,0755) && !mkdir(original,0755));
    CHECK(!mount("query-tmpfs",original,"tmpfs",MS_NOEXEC|MS_NODIRATIME,NULL));
    CHECK(!mkdir(subdir,0755) && !mkdir(bind_point,0755));
    CHECK(!mount(NULL,original,NULL,MS_SHARED,NULL));
    CHECK(!mount(subdir,bind_point,NULL,MS_BIND,NULL));
    int fd=open(original,O_PATH|O_DIRECTORY|O_CLOEXEC);
    int bindfd=open(bind_point,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(fd>=0 && bindfd>=0);
    struct mount_result a=by_file(fd);
    struct mount_result b=by_file(bindfd);
    CHECK((a.mask&CORE_MASK)==CORE_MASK && a.ns!=0 && b.ns==a.ns);
    CHECK(a.id>(1ULL<<31) && a.old==path_id(original,LEGACY_STATX));
    CHECK(a.id==path_id(original,UNIQUE_STATX) && b.id==path_id(bind_point,UNIQUE_STATX));
    CHECK(b.parent==a.id && b.old_parent==a.old && b.id!=a.id);
    CHECK(a.magic==0x01021994 && (a.attr&0x88)==0x88 && a.peer && a.propagation&MS_SHARED);
    CHECK(!strcmp(a.strings+a.type,"tmpfs") && !strcmp(a.strings+a.source,"query-tmpfs"));
    CHECK(!strcmp(a.strings+a.root,"/") && !strcmp(a.strings+a.point,original));
    CHECK(!strcmp(b.strings+b.root,"/sub") && !strcmp(b.strings+b.point,bind_point));
    struct { unsigned len; int type; unsigned char data[128]; } handle={.len=128};
    uint64_t handle_id=0;
    CHECK(!syscall(SYS_name_to_handle_at,AT_FDCWD,original,&handle,&handle_id,1) && handle_id==a.id);
    verify_chroot(a.id,b.id);

    int oldns=open("/proc/self/ns/mnt",O_RDONLY|O_CLOEXEC);
    CHECK(oldns>0 && !unshare(CLONE_NEWNS));
    struct mount_result still_old=by_file(fd);
    CHECK(still_old.id==a.id && still_old.ns==a.ns);
    struct request req={.size=32,.fd=(uint32_t)oldns,.id=a.id,.param=MNT_BASIC|MNT_NS_ID};
    struct mount_result output;
    CHECK(!query(&req,&output,sizeof(output),0) && output.id==a.id && output.ns==a.ns);
    req.param=0;
    uint64_t ids[8];
    CHECK(list(&req,ids,8,0)==1 && ids[0]==b.id);
    CHECK(!setns(oldns,CLONE_NEWNS));
    req.fd=0;
    req.ns=a.ns;
    req.param=MNT_BASIC;
    CHECK(!query(&req,&output,16,0) && output.size==16 && output.mask==MNT_BASIC);
    req.ns=0;
    req.param=MNT_POINT;
    ERROR(query(&req,&output,512,0),EOVERFLOW);
    ERROR(query(&req,(void *)1,sizeof(output),0),EFAULT);
    req.param=MNT_BASIC;
    struct {struct request request;uint64_t tail;} future={req,1};
    future.request.size=sizeof(future);
    ERROR(query(&future.request,&output,sizeof(output),0),E2BIG);
    future.tail=0;
    CHECK(!query(&future.request,&output,sizeof(output),0) && output.id==a.id);
    req.size=23;
    ERROR(query(&req,&output,sizeof(output),0),EINVAL);
    req.size=24;
    req.fd=UINT32_MAX;
    ERROR(query(&req,&output,sizeof(output),0),EBADF);
    req.fd=(uint32_t)fd;
    ERROR(query(&req,&output,sizeof(output),0),EBADF);
    ERROR(query(&req,&output,sizeof(output),BY_FD),EINVAL);
    req.fd=(uint32_t)bindfd;
    ERROR(query(&req,&output,sizeof(output),0),EINVAL);
    req.fd=0;
    req.id=a.old;
    ERROR(query(&req,&output,sizeof(output),0),EINVAL);
    req.id=MOUNT_QUERY_ROOT;
    req.param=0;
    CHECK(list(&req,ids,2,0)==2 && ids[0]<ids[1]);
    req.param=ids[1];
    uint64_t first=ids[0];
    CHECK(list(&req,ids,1,REVERSE)==1 && ids[0]==first);
    req.param=0;
    ERROR(list(&req,ids,1000001U,0),EOVERFLOW);
    ERROR(list(&req,(void *)1,1,0),EFAULT);

    int tree=syscall(428,AT_FDCWD,bind_point,O_CLOEXEC);
    CHECK(tree>=0 && by_file(tree).id==b.id && !close(tree));
    CHECK(!umount2(original,MNT_DETACH));
    struct mount_result detached=by_file(fd);
    CHECK(detached.id==a.id && detached.parent==a.id);
    CHECK(!(detached.mask&(MNT_POINT|MNT_NS_ID)) && !strcmp(detached.strings+detached.root,"/"));
    req.id=a.id;
    req.param=MNT_BASIC;
    ERROR(query(&req,&output,sizeof(output),0),ENOENT);
    CHECK(!close(oldns) && !close(bindfd) && !close(fd));
    CHECK(!rmdir(original) && !rmdir(work));
    puts("MOUNT QUERY PASS");
    return 0;
}
