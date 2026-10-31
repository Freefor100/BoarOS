/* Exercise real libacl against a persistent ext4 fixture. */
#define _GNU_SOURCE
#include <acl/libacl.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "ACL-LIB FAIL %s errno=%d\n", what, errno);
        exit(1);
    }
}

static void check_access(const char *path, uid_t uid, gid_t gid,
                         int read_ok, int write_ok)
{
    pid_t pid = fork();
    int status;
    require(pid >= 0, "fork");
    if (!pid) {
        int fd, rd, wr;
        if (setgroups(0, NULL) || setgid(gid) || setuid(uid)) _exit(10);
        fd = open(path, O_RDONLY); rd = fd >= 0; if (rd) close(fd);
        fd = open(path, O_WRONLY); wr = fd >= 0; if (wr) close(fd);
        _exit(rd == read_ok && wr == write_ok ? 0 : 11);
    }
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0, "access as numeric user/group");
}

static void set_acl(const char *path, acl_type_t type, const char *text)
{
    acl_t acl = acl_from_text(text);
    require(acl != NULL && acl_valid(acl) == 0, "acl_from_text/valid");
    require(acl_set_file(path, type, acl) == 0, "acl_set_file");
    require(acl_free(acl) == 0, "acl_free");
}

static void compare_acl(const char *path, acl_type_t type, const char *text)
{
    acl_t expected = acl_from_text(text), actual = acl_get_file(path, type);
    require(expected != NULL && actual != NULL, "acl_get_file");
    require(acl_cmp(expected, actual) == 0, "stored ACL content");
    acl_free(actual); acl_free(expected);
}

int main(void)
{
    const char *acl = "u::rw-,u:601:rw-,g::---,g:602:r--,m::r--,o::---";
    const char *default_acl = "u::rwx,u:601:rwx,g::r-x,m::r-x,o::---";
    const char *inherited = "u::rw-,u:601:rwx,g::r-x,m::r--,o::---";
    int fd, status;
    pid_t pid;

    if (access("/acl-lib-done", F_OK) == 0) {
        compare_acl("/acl-lib-dir", ACL_TYPE_DEFAULT, default_acl);
        compare_acl("/acl-lib-dir/inherited", ACL_TYPE_ACCESS, inherited);
        check_access("/acl-lib-dir/inherited", 601, 601, 1, 0);
        puts("ACL-LIB PERSIST PASS");
        return 0;
    }
    umask(0);
    require(mkdir("/acl-lib-dir", 0777) == 0, "mkdir");
    fd = open("/acl-lib-file", O_CREAT | O_EXCL | O_RDWR, 0666);
    require(fd >= 0, "create"); close(fd);
    set_acl("/acl-lib-file", ACL_TYPE_ACCESS, acl);
    compare_acl("/acl-lib-file", ACL_TYPE_ACCESS, acl);
    check_access("/acl-lib-file", 601, 601, 1, 0);
    check_access("/acl-lib-file", 603, 602, 1, 0);
    check_access("/acl-lib-file", 604, 604, 0, 0);
    require(chmod("/acl-lib-file", 0600) == 0, "chmod");
    check_access("/acl-lib-file", 601, 601, 0, 0);
    puts("ACL-LIB ACCESS-MASK-CHMOD PASS");

    set_acl("/acl-lib-dir", ACL_TYPE_DEFAULT, default_acl);
    fd = open("/acl-lib-dir/inherited", O_CREAT | O_EXCL | O_RDWR, 0666);
    require(fd >= 0, "create with default ACL");
    require(fsync(fd) == 0 && close(fd) == 0, "sync inherited");
    compare_acl("/acl-lib-dir/inherited", ACL_TYPE_ACCESS, inherited);
    check_access("/acl-lib-dir/inherited", 601, 601, 1, 0);
    puts("ACL-LIB DEFAULT-INHERIT PASS");

    require(chdir("/") == 0 && mkdir("acl-original-dir", 0777) == 0,
            "original setup");
    fd = open("acl-original-file", O_CREAT | O_EXCL | O_RDWR, 0777);
    require(fd >= 0 && close(fd) == 0, "original file");
    pid = fork(); require(pid >= 0, "original fork");
    if (!pid) {
        execl("/acl1", "acl1", "acl-original-file", "acl-original-dir", (char *)0);
        _exit(127);
    }
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 1, "original exit");
    puts("ACL-LIB ORIGINAL EXECUTED");
    fd = open("/acl-lib-done", O_CREAT | O_WRONLY, 0600);
    require(fd >= 0 && fsync(fd) == 0 && close(fd) == 0, "persist marker");
    sync();
    puts("ACL-LIB INITIAL PASS");
    return 0;
}
