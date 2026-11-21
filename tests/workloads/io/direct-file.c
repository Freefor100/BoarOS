#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"DIRECT FILE FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define FAIL(e,x) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static unsigned char data[32768] __attribute__((aligned(16384)));
static int open_direct_file(const char *dir,const char *name)
{
#ifdef BOAROS_NOJOURNAL_DIRECT
    char path[512];CHECK(snprintf(path,sizeof(path),"%s/%s",dir,name)<(int)sizeof(path));
    return open(path,O_RDWR|O_DIRECT);
#else
    (void)name;return open(dir,O_TMPFILE|O_RDWR|O_DIRECT,0600);
#endif
}

#ifdef BOAROS_SPARSE_STATX
static void test_sparse(const char *path, off_t page, int disk)
{
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(!ftruncate(fd, 7 * page));
    CHECK(pwrite(fd, data, page, 2 * page) == page);
    CHECK(lseek(fd, 0, SEEK_HOLE) == 0);
    CHECK(lseek(fd, 0, SEEK_DATA) == 2 * page);
    CHECK(lseek(fd, 2 * page + 1, SEEK_DATA) == 2 * page + 1);
    CHECK(lseek(fd, 2 * page + 1, SEEK_HOLE) == 3 * page);
    CHECK(!fsync(fd));
    CHECK(lseek(fd, 0, SEEK_DATA) == 2 * page);

    CHECK(!fallocate(fd, FALLOC_FL_KEEP_SIZE, 4 * page, page));
    FAIL(ENXIO, lseek(fd, 4 * page, SEEK_DATA));
    CHECK(lseek(fd, 4 * page, SEEK_HOLE) == 4 * page);
    if (!disk) {
        char byte = 1;
        CHECK(pread(fd, &byte, 1, 4 * page) == 1 && !byte);
        FAIL(ENXIO, lseek(fd, 4 * page, SEEK_DATA));
        unsigned char *mapped = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 4 * page);
        CHECK(mapped != MAP_FAILED && !mapped[0]);
        CHECK(!munmap(mapped, page) && lseek(fd, 4 * page, SEEK_DATA) == 4 * page);
    }

    CHECK(pwrite(fd, data, page, 6 * page) == page);
    CHECK(lseek(fd, 6 * page, SEEK_HOLE) == 7 * page);
    FAIL(ENXIO, lseek(fd, 7 * page, SEEK_HOLE));
    FAIL(ENXIO, lseek(fd, -1, SEEK_DATA));
    CHECK(lseek(fd, 0, SEEK_CUR) == 7 * page);
    CHECK(!fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 2 * page, page));
    CHECK(lseek(fd, 0, SEEK_DATA) == (disk ? 6 : 4) * page);
    CHECK(!ftruncate(fd, page));
    FAIL(ENXIO, lseek(fd, 0, SEEK_DATA));
    CHECK(lseek(fd, 0, SEEK_HOLE) == 0);

    if (disk) {
        struct statx path_info, fd_info;
        CHECK(!statx(AT_FDCWD, path, 0, STATX_DIOALIGN, &path_info));
        int pathfd = open(path, O_PATH | O_CLOEXEC);
        CHECK(pathfd >= 0 && !statx(pathfd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &fd_info));
        CHECK(path_info.stx_mask & STATX_DIOALIGN);
        CHECK(fd_info.stx_mask & STATX_DIOALIGN);
        CHECK(path_info.stx_dio_mem_align && path_info.stx_dio_offset_align);
        CHECK(path_info.stx_dio_mem_align == fd_info.stx_dio_mem_align);
        CHECK(path_info.stx_dio_offset_align == fd_info.stx_dio_offset_align);
        CHECK(!close(pathfd));
        printf("STATX direct alignment memory=%u offset=%u\n",
               path_info.stx_dio_mem_align, path_info.stx_dio_offset_align);
    }
    CHECK(!close(fd) && !unlink(path));
}

static void test_mount_root(const char *path, int fd, int expected)
{
    struct statx info;
    CHECK(!statx(fd, path, fd == AT_FDCWD ? 0 : AT_EMPTY_PATH, 0, &info));
    CHECK(info.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT);
    CHECK(!!(info.stx_attributes & STATX_ATTR_MOUNT_ROOT) == expected);
}

static int sparse_statx(off_t page)
{
    test_sparse("/sparse-file", page, 1);
    puts("SPARSE extent and direct alignment PASS");
    CHECK(!mkdir("/sparse-mount", 0700) && !mkdir("/sparse-bind", 0700));
    int before = open("/sparse-bind", O_PATH | O_DIRECTORY);
    CHECK(before >= 0);
    CHECK(!mount("tmpfs", "/sparse-mount", "tmpfs", 0, NULL));
    CHECK(!mkdir("/sparse-mount/sub", 0700));
    test_sparse("/sparse-mount/sparse-file", page, 0);
    CHECK(!mount("/sparse-mount/sub", "/sparse-bind", NULL, MS_BIND, NULL));
    int root = open("/sparse-bind", O_RDONLY | O_DIRECTORY);
    CHECK(root >= 0);
    test_mount_root("/", AT_FDCWD, 1);
    test_mount_root("/sparse-mount", AT_FDCWD, 1);
    test_mount_root("/sparse-mount/sub", AT_FDCWD, 0);
    test_mount_root("/sparse-bind", AT_FDCWD, 1);
    test_mount_root("", root, 1);
    test_mount_root("", before, 0);
    CHECK(!close(root) && !close(before));
    CHECK(!umount("/sparse-bind") && !rmdir("/sparse-bind"));
    CHECK(!rmdir("/sparse-mount/sub") && !umount("/sparse-mount") && !rmdir("/sparse-mount"));
    puts("SPARSE tmpfs and mount roots PASS");
    puts("DIRECT FILE ALL PASS");
    return 0;
}
#endif

int main(int argc,char **argv)
{
    setbuf(stdout,NULL);const char *dir=argc>1?argv[1]:"/";
    size_t page=sysconf(_SC_PAGESIZE);CHECK(page<=16384);
#ifdef BOAROS_SPARSE_STATX
    return sparse_statx((off_t)page);
#endif
    int fd=open_direct_file(dir,"direct-input");CHECK(fd>=0);
    memset(data,0x41,sizeof(data));
    int probe=argc>2&&!strcmp(argv[2],"--probe-alignment");
    if (probe) {
        errno=0;ssize_t result=pwrite(fd,data+1,512,0);
        printf("alignment probe unaligned address: result=%zd errno=%d\n",result,errno);
        errno=0;result=pwrite(fd,data,511,0);
        printf("alignment probe unaligned length: result=%zd errno=%d\n",result,errno);
    } else {
        FAIL(EINVAL,pwrite(fd,data+1,512,0));
        FAIL(EINVAL,pwrite(fd,data,511,0));
    }
    CHECK(pwrite(fd,data,2*page,0)==(ssize_t)(2*page));
    struct iovec bad[2]={{data,256},{data+256,256}};
    if (probe) {
        errno=0;ssize_t result=preadv(fd,bad,2,0);
        printf("alignment probe short vector segments: result=%zd errno=%d\n",result,errno);
    } else FAIL(EINVAL,preadv(fd,bad,2,0));
    unsigned char *map=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(map!=MAP_FAILED);
    map[0]=0x62;CHECK(pread(fd,data,page,0)==(ssize_t)page&&data[0]==0x62);
    map[0]=0x63;CHECK(pwrite(fd,map,page,page)==(ssize_t)page&&map[page]==0x63);
    CHECK(pread(fd,map,page,page)==(ssize_t)page&&map[0]==0x63);
    CHECK(!munmap(map,2*page));
    void *fault=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(fault!=MAP_FAILED);
    FAIL(EFAULT,pread(fd,fault,page,0));FAIL(EFAULT,pwrite(fd,fault,page,0));CHECK(!munmap(fault,page));
    CHECK(!ftruncate(fd,123));CHECK(pread(fd,data,512,0)==123);
    CHECK(pread(fd,data,512,512)==0);
    memset(data,0x71,page);CHECK(pwrite(fd,data,page,4*page)==(ssize_t)page);
    CHECK(pread(fd,data,page,page)==(ssize_t)page);for(size_t i=0;i<page;i++)CHECK(!data[i]);
    int flags=fcntl(fd,F_GETFL);CHECK(flags&O_DIRECT);int copy=dup(fd);CHECK(copy>=0);
    CHECK(!fcntl(copy,F_SETFL,flags&~O_DIRECT));CHECK(!(fcntl(fd,F_GETFL)&O_DIRECT));
    CHECK(pwrite(fd,"z",1,0)==1);CHECK(!fcntl(copy,F_SETFL,flags));
    CHECK(pread(fd,data,512,0)==512&&data[0]=='z');
    int target=open_direct_file(dir,"direct-output");CHECK(target>=0);
    off_t position=0;CHECK(sendfile(target,fd,&position,page)==(ssize_t)page&&position==(off_t)page);
    CHECK(pread(target,data,page,0)==(ssize_t)page&&data[0]=='z');
    int pipefd[2];CHECK(!pipe(pipefd));position=0;
    CHECK(splice(fd,&position,pipefd[1],NULL,page,0)==(ssize_t)page);
    position=page;CHECK(splice(pipefd[0],NULL,target,&position,page,0)==(ssize_t)page);
    CHECK(pread(target,data,page,page)==(ssize_t)page&&data[0]=='z');
    CHECK(!close(pipefd[0])&&!close(pipefd[1]));
    CHECK(!fsync(target)&&!fsync(fd));CHECK(!close(target)&&!close(copy)&&!close(fd));
    puts("DIRECT FILE ALL PASS");return 0;
}
