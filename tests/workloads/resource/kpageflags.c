#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "KPAGEFLAGS FAIL %d: %s (errno=%d)\n", __LINE__, #expr, errno); return 1; } \
} while (0)

static int read_flags(int pagemap, int flags, const void *page, size_t page_size,
    uint64_t *entry, off_t *position)
{
    uint64_t map;
    off_t offset = (off_t)((uintptr_t)page/page_size*sizeof(map));
    if (pread(pagemap, &map, sizeof(map), offset) != sizeof(map) || !(map>>63))
        return -1;
    uint64_t frame = map & ((UINT64_C(1)<<55)-1);
    if (!frame) return -1;
    *position = (off_t)(frame*sizeof(*entry));
    return pread(flags, entry, sizeof(*entry), *position) == sizeof(*entry) ? 0 : -1;
}

int main(void)
{
    const uint64_t dirty = UINT64_C(1)<<4, writeback = UINT64_C(1)<<8;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    int flags = open("/proc/kpageflags",O_RDONLY | O_CLOEXEC);
    CHECK(flags >= 0);
    struct stat st;
    CHECK(!fstat(flags,&st) && S_ISREG(st.st_mode) && (st.st_mode&0777)==0400 && st.st_size==0);
    uint64_t entry;
    CHECK(pread(flags,&entry,7,0)==-1 && errno==EINVAL);
    CHECK(pread(flags,&entry,8,1)==-1 && errno==EINVAL);
    CHECK(pread(flags,&entry,8,INT64_MAX-7)==0);
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        if (setgid(62001) || setuid(62001)) _exit(2);
        int denied=open("/proc/kpageflags",O_RDONLY);
        _exit(denied<0 && errno==EACCES ? 0 : 3);
    }
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    int pagemap=open("/proc/self/pagemap",O_RDONLY | O_CLOEXEC);
    CHECK(pagemap>=0);
    char name[]="kpageflags-probe.XXXXXX";
    int file=mkstemp(name);
    CHECK(file>=0 && !ftruncate(file,(off_t)page_size));
    unsigned char *area=mmap(NULL,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,file,0);
    CHECK(area!=MAP_FAILED);
    CHECK(!unlink(name));
    volatile unsigned char *shared=area;
    CHECK(shared[0]==0);
    off_t position;
    CHECK(!read_flags(pagemap,flags,area,page_size,&entry,&position) && !(entry&(dirty|writeback)));
    CHECK(lseek(flags,position,SEEK_SET)==position);
    CHECK(read(flags,&entry,8)==8 && lseek(flags,0,SEEK_CUR)==position+8);
    for (unsigned i=0;i<2;i++) {
        shared[i]=(unsigned char)(0x40+i);
        CHECK(!read_flags(pagemap,flags,area,page_size,&entry,&position) && (entry&dirty));
        CHECK(!msync(area,page_size,MS_SYNC));
        CHECK(!read_flags(pagemap,flags,area,page_size,&entry,&position) && !(entry&(dirty|writeback)));
    }
    unsigned char bytes[2];
    CHECK(pread(file,bytes,sizeof(bytes),0)==sizeof(bytes) && bytes[0]==0x40 && bytes[1]==0x41);
    CHECK(!munmap(area,page_size) && !close(file) && !close(pagemap) && !close(flags));
    puts("KPAGEFLAGS PASS PFN offsets, permissions, real mapped dirty state, msync and redirty");
    return 0;
}
