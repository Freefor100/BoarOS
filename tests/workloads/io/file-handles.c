#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("FILE-HANDLE FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1; } } while (0)
struct handle {
    unsigned bytes;
    int type;
    unsigned char data[128];
};

static int name(int dir, const char *path, struct handle *handle, int *id, int flags)
{
    return syscall(264, dir, path, handle, id, flags);
}

static int reopen(int mount, struct handle *handle, int flags)
{
    return syscall(265, mount, handle, flags);
}

static int check_permissions(int dir, struct handle *handle)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        struct __user_cap_data_struct data[2];
        if (prctl(PR_SET_KEEPCAPS,1,0,0,0) || setuid(65534) ||
            syscall(SYS_capget,&header,data)) _exit(2);
        if (reopen(dir,handle,O_RDONLY) != -1 || errno != EPERM) _exit(3);
        data[0].effective = 1U << CAP_DAC_READ_SEARCH;
        data[1].effective = 0;
        if (syscall(SYS_capset,&header,data)) _exit(4);
        if (reopen(dir,handle,O_WRONLY) != -1 || errno != EACCES) _exit(5);
        int readable = reopen(dir,handle,O_RDONLY);
        _exit(readable < 0 || close(readable) ? 6 : 0);
    }
    int status;
    CHECK(waitpid(child,&status,0) == child && status == 0);
    return 0;
}

int main(void)
{
    char folder[] = "/tmp/file-handles-XXXXXX";
    CHECK(mkdtemp(folder) && !chdir(folder));
    int root = open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(root >= 0);
    int original = open("item",O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(original >= 0 && write(original,"value",5) == 5);
    int mount_id = -1;
    struct handle value = {0};
    CHECK(name(root,"item",&value,&mount_id,0) == -1 && errno == EOVERFLOW);
    CHECK(value.bytes > 0 && mount_id > 0);
    const unsigned needed = value.bytes;
    struct stat old_inode;
    CHECK(!fstat(original,&old_inode));
    struct handle via_path = {.bytes=sizeof(value.data)};
    int path_only = open(".",O_PATH|O_DIRECTORY);
    CHECK(path_only >= 0);
    CHECK(name(path_only,"",&via_path,&mount_id,AT_EMPTY_PATH) == 0);
    CHECK(reopen(path_only,&via_path,O_RDONLY) == -1 && errno == EBADF && !close(path_only));
    CHECK(name(original,"",&value,&mount_id,AT_EMPTY_PATH) == 0 && value.bytes == needed);
    struct handle via_name = {.bytes = sizeof(value.data)};
    CHECK(name(root,"item",&via_name,&mount_id,0) == 0);
    CHECK(value.bytes == via_name.bytes && value.type == via_name.type &&
          !memcmp(value.data,via_name.data,value.bytes));
    int fd = reopen(root,&value,O_RDONLY|O_CLOEXEC);
    CHECK(fd >= 0 && fcntl(fd,F_GETFD) == FD_CLOEXEC);
    char contents[5];
    CHECK(read(fd,contents,5) == 5 && !memcmp(contents,"value",5) && !close(fd));
    CHECK(!rename("item","renamed") && !link("renamed","link") && !unlink("renamed"));
    CHECK((fd = reopen(AT_FDCWD,&value,O_RDONLY)) >= 0 && !close(fd));
    CHECK(!symlink("link","symlink"));
    struct handle symlink_handle = {.bytes=sizeof(value.data)};
    CHECK(name(root,"symlink",&symlink_handle,&mount_id,0) == 0);
    CHECK(reopen(root,&symlink_handle,O_RDONLY) == -1 && errno == ELOOP);
    CHECK((fd = reopen(root,&symlink_handle,O_PATH)) >= 0);
    CHECK(readlinkat(fd,"",contents,sizeof(contents)) == 4 && !memcmp(contents,"link",4));
    CHECK(!close(fd) && !unlink("symlink"));
    CHECK(!check_permissions(root,&value));
    CHECK(!mkdir("tree",0700) && !mkdir("tree/leaf",0700));
    struct handle dir_handle = {.bytes=sizeof(value.data)};
    CHECK(name(root,"tree/leaf",&dir_handle,&mount_id,0) == 0);
    int from_handle = reopen(root,&dir_handle,O_RDONLY|O_DIRECTORY);
    CHECK(from_handle >= 0);
    int parent = openat(from_handle,"..",O_PATH|O_DIRECTORY);
    struct stat parent_stat, expected;
    CHECK(parent >= 0 && !fstat(parent,&parent_stat) && !stat("tree",&expected));
    CHECK(parent_stat.st_ino == expected.st_ino && !close(parent) && !close(from_handle));
    CHECK(!mkdir("alias",0700) && !mkdir("other",0700));
    CHECK(!mount(".", "alias", NULL, MS_BIND, NULL));
    int alias = open("alias",O_RDONLY|O_DIRECTORY);
    CHECK(alias >= 0 && (fd = reopen(alias,&value,O_RDONLY)) >= 0 && !close(fd));
    CHECK(!mount("tmpfs","other","tmpfs",0,NULL));
    int other = open("other",O_RDONLY|O_DIRECTORY);
    CHECK(other >= 0 && reopen(other,&value,O_RDONLY) == -1 && errno == ESTALE);
    int tmpfile = openat(other,"temporary",O_CREAT|O_RDWR,0600);
    CHECK(tmpfile >= 0 && !close(tmpfile));
    struct handle temporary = {.bytes=sizeof(value.data)};
    CHECK(!name(other,"temporary",&temporary,&mount_id,0));
    CHECK((fd = reopen(other,&temporary,O_RDONLY)) >= 0 && !close(fd));
    CHECK(!unlinkat(other,"temporary",0));
    CHECK(reopen(other,&temporary,O_RDONLY) == -1 && errno == ESTALE);
    CHECK(!close(other) && !umount("other") && !rmdir("other"));
    CHECK(!umount2("alias",MNT_DETACH));
    CHECK((fd = reopen(alias,&value,O_RDONLY)) >= 0 && !close(fd) && !close(alias));
    CHECK(!rmdir("alias") && !rmdir("tree/leaf") && !rmdir("tree"));
    CHECK(!unlink("link"));
    errno=0;
    fd = reopen(root,&value,O_RDONLY);
    printf("FILE-HANDLE after-last-unlink-with-open-fd result=%s errno=%d\n",fd<0?"error":"open",errno);
    CHECK(fd >= 0 && !close(fd));
    CHECK(!close(original));
    CHECK(reopen(root,&value,O_RDONLY) == -1 && errno == ESTALE);
    CHECK(!syncfs(root));
    int reused = 0;
    for (unsigned attempt = 0; attempt < 16 && !reused; ++attempt) {
        struct stat current;
        int replacement = open("item",O_RDWR|O_CREAT|O_EXCL,0600);
        CHECK(replacement >= 0 && !fstat(replacement,&current));
        if (current.st_ino == old_inode.st_ino) {
            CHECK(reopen(root,&value,O_RDONLY) == -1 && errno == ESTALE);
            struct handle fresh = {.bytes=sizeof(value.data)};
            CHECK(!name(root,"item",&fresh,&mount_id,0));
            CHECK((fd = reopen(root,&fresh,O_RDONLY)) >= 0 && !close(fd));
            reused = 1;
        }
        CHECK(!close(replacement) && !unlink("item") && !syncfs(root));
    }
    printf("FILE-HANDLE inode reuse observed=%d\n",reused);
    CHECK(!close(root) && !chdir("/") && !rmdir(folder));
    puts("FILE-HANDLE PASS");
    return 0;
}
