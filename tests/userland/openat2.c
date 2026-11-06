#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

enum { NO_XDEV=1, NO_MAGICLINKS=2, NO_SYMLINKS=4, BENEATH=8, IN_ROOT=16, CACHED=32 };
struct how { uint64_t flags, mode, resolve; };

static int open2(int dir, const char *path, uint64_t flags, unsigned resolve)
{
    struct how args={flags,0,resolve};
    return (int)syscall(SYS_openat2,dir,path,&args,sizeof(args));
}

static void check_at(int good, int line)
{
    if (!good) {
        fprintf(stderr,"OPENAT2 line=%d errno=%d\n",line,errno);
        exit(1);
    }
}
#define CHECK(x) check_at(!!(x),__LINE__)
#define ERROR(call,e) CHECK((call)==-1 && errno==(e))

static void opened(int fd)
{
    CHECK(fd>=0);
    CHECK(!close(fd));
}

int main(void)
{
    char root[]="boar-openat2-XXXXXX";
    CHECK(mkdtemp(root));
    int base=open(root,O_RDONLY|O_DIRECTORY);
    CHECK(base>=0 && !mkdirat(base,"scope",0700));
    int scope=openat(base,"scope",O_PATH|O_DIRECTORY);
    CHECK(scope>=0 && !mkdirat(scope,"sub",0700) && !mkdirat(scope,"bind",0700));
    opened(openat(scope,"inside",O_CREAT|O_WRONLY,0600));
    CHECK(!symlinkat("inside",scope,"relative"));
    CHECK(!symlinkat("/inside",scope,"absolute"));
    CHECK(!symlinkat("../escaped",scope,"escape"));

    opened(open2(scope,"sub/../inside",O_RDONLY,BENEATH));
    ERROR(open2(scope,"../scope/inside",O_RDONLY,BENEATH),EXDEV);
    ERROR(open2(scope,"/inside",O_RDONLY,BENEATH),EXDEV);
    ERROR(open2(scope,"absolute",O_RDONLY,BENEATH),EXDEV);
    ERROR(open2(scope,"relative",O_RDONLY,NO_SYMLINKS),ELOOP);
    opened(open2(scope,"relative",O_PATH|O_NOFOLLOW,NO_SYMLINKS));
    opened(open2(scope,"/../inside",O_RDONLY,IN_ROOT));
    opened(open2(scope,"absolute",O_RDONLY,IN_ROOT));
    ERROR(open2(-999,"/inside",O_RDONLY,IN_ROOT),EBADF);

    struct how how={O_CREAT|O_WRONLY,0600,BENEATH};
    ERROR(syscall(SYS_openat2,scope,"escape",&how,sizeof(how)),EXDEV);
    struct stat stat;
    ERROR(fstatat(base,"escaped",&stat,0),ENOENT);

    char magic[64];
    CHECK(snprintf(magic,sizeof(magic),"/proc/self/fd/%d",scope)>0);
    opened(open2(AT_FDCWD,"/proc/self/status",O_RDONLY,NO_MAGICLINKS));
    ERROR(open2(AT_FDCWD,magic,O_PATH,NO_MAGICLINKS),ELOOP);
    opened(open2(AT_FDCWD,magic,O_PATH|O_NOFOLLOW,NO_MAGICLINKS));
    ERROR(open2(AT_FDCWD,magic,O_PATH,NO_XDEV),EXDEV);

    char source[128],target[128];
    CHECK(snprintf(source,sizeof(source),"%s/scope/sub",root)>0);
    CHECK(snprintf(target,sizeof(target),"%s/scope/bind",root)>0);
    if (!mount(source,target,0,MS_BIND,0)) {
        ERROR(open2(scope,"bind",O_PATH,NO_XDEV),EXDEV);
        int bind=open2(scope,"bind",O_PATH,0);
        CHECK(bind>=0);
        ERROR(open2(bind,"..",O_PATH,NO_XDEV),EXDEV);
        CHECK(!close(bind) && !umount(target));
    } else {
        /* The host reference may run without CAP_SYS_ADMIN. */
        CHECK(errno==EPERM);
        puts("OPENAT2 bind skipped (no mount permission)");
    }

    how=(struct how){O_RDONLY,0,0};
    ERROR(syscall(SYS_openat2,scope,"inside",&how,sizeof(how)-1),EINVAL);
    ERROR(syscall(SYS_openat2,scope,"inside",0,sizeof(how)),EFAULT);
    ERROR(syscall(SYS_openat2,scope,"inside",0,1U<<20),E2BIG);
    struct {struct how how; uint64_t extension;} extra={how,1};
    ERROR(syscall(SYS_openat2,scope,"inside",&extra,sizeof(extra)),E2BIG);
    extra.extension=0;
    opened((int)syscall(SYS_openat2,scope,"inside",&extra,sizeof(extra)));
    ERROR(open2(scope,"inside",O_PATH|O_NONBLOCK,0),EINVAL);
    ERROR(open2(scope,"inside",1ULL<<63,0),EINVAL);
    ERROR(open2(scope,"inside",O_RDONLY,1U<<31),EINVAL);
    ERROR(open2(scope,"inside",O_RDONLY,BENEATH|IN_ROOT),EINVAL);
    how=(struct how){O_RDONLY,0600,0};
    ERROR(syscall(SYS_openat2,scope,"inside",&how,sizeof(how)),EINVAL);
    ERROR(open2(scope,"new",O_CREAT|O_WRONLY,CACHED),EAGAIN);
    int regular=open2(scope,"inside",UINT64_C(1)<<32,0);
    if (regular>=0) {
        opened(regular);
        ERROR(open2(scope,"sub",UINT64_C(1)<<32,0),134);
    } else CHECK(errno==EINVAL); /* Linux references predating OPENAT2_REGULAR. */
    ERROR(fstatat(scope,"new",&stat,0),ENOENT);

    CHECK(!unlinkat(scope,"inside",0) && !unlinkat(scope,"relative",0));
    CHECK(!unlinkat(scope,"absolute",0) && !unlinkat(scope,"escape",0));
    CHECK(!unlinkat(scope,"sub",AT_REMOVEDIR) && !unlinkat(scope,"bind",AT_REMOVEDIR));
    CHECK(!close(scope) && !unlinkat(base,"scope",AT_REMOVEDIR));
    CHECK(!close(base) && !rmdir(root));
    puts("OPENAT2 PASS");
    return 0;
}
