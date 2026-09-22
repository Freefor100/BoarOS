#ifndef BOAROS_USERLAND_NAMESPACE_H
#define BOAROS_USERLAND_NAMESPACE_H

static unsigned char namespace_stack[32768] __attribute__((aligned(16)));
static int namespace_shared_cwd(void *unused)
{
    (void)unused;
    return chdir("/") != 0;
}

static int namespace_wait(pid_t child)
{
    int status;
    return child < 0 || waitpid(child, &status, 0) != child ||
           !WIFEXITED(status) || WEXITSTATUS(status);
}

static int check_namespace(void)
{
    char path[128], bytes[8];
    struct stat old_stat, new_stat;
    if (mkdir("/ns-a", 0700) || mkdir("/ns-b", 0700) ||
        mkdir("/ns-a/sub", 0700)) return 1;
    int a = open("/ns-a", O_RDONLY | O_DIRECTORY);
    int b = open("/ns-b", O_RDONLY | O_DIRECTORY);
    int sub = openat(a, "sub", O_RDONLY | O_DIRECTORY);
    if (a < 0 || b < 0 || sub < 0 || fchdir(sub) ||
        rename("/ns-a", "/ns-renamed") || !getcwd(path, sizeof(path)) ||
        strcmp(path, "/ns-renamed/sub")) return 2;
    int source = openat(sub, "temporary", O_CREAT | O_RDWR, 0600);
    int target = openat(b, "target", O_CREAT | O_RDWR, 0600);
    if (source < 0 || target < 0 || write(source, "new", 3) != 3 ||
        write(target, "old", 3) != 3 || fsync(source) ||
        syscall(SYS_renameat2, sub, "temporary", b, "target", 1) != -1 ||
        errno != EEXIST || renameat(sub, "temporary", b, "target") ||
        fsync(sub) || fsync(b) || fstat(target, &old_stat) || old_stat.st_nlink ||
        pread(target, bytes, sizeof(bytes), 0) != 3 || memcmp(bytes, "old", 3)) return 3;
    int published = openat(b, "target", O_RDONLY);
    if (published < 0 || fstat(published, &new_stat) ||
        new_stat.st_ino == old_stat.st_ino ||
        pread(published, bytes, sizeof(bytes), 0) != 3 || memcmp(bytes, "new", 3) ||
        close(published) || close(source) || close(target)) return 4;
    if (symlinkat("../target", sub, "link") ||
        readlinkat(sub, "link", bytes, sizeof(bytes)) != 8 ||
        memcmp(bytes, "../targe", 8) ||
        fstatat(sub, "link", &new_stat, AT_SYMLINK_NOFOLLOW) || !S_ISLNK(new_stat.st_mode) ||
        unlinkat(sub, "link", 0)) return 5;
    /* fork copies the fs record; exec retains cwd and resolves its executable
     * relative to that object. CLONE_FS shares subsequent cwd changes. */
    pid_t child = fork();
    if (!child) {
        char *args[] = {"relative-init", "namespace_exec", "/ns-renamed/sub", 0};
        char *environment[] = {0};
        execve("../../init", args, environment);
        _exit(1);
    }
    if (namespace_wait(child)) return 6;
    child = fork();
    if (!child) _exit(chdir("/") != 0);
    if (namespace_wait(child) || !getcwd(path, sizeof(path)) ||
        strcmp(path, "/ns-renamed/sub")) return 7;
    child = clone(namespace_shared_cwd, namespace_stack + sizeof(namespace_stack),
                   CLONE_FS | SIGCHLD, 0);
    if (namespace_wait(child) || !getcwd(path, sizeof(path)) || strcmp(path, "/") ||
        fchdir(sub) || renameat(a, "sub", b, "moved") ||
        !getcwd(path, sizeof(path)) || strcmp(path, "/ns-b/moved")) return 8;
    if (rename("/ns-b", "/ns-b/moved/cycle") != -1 || errno != EINVAL ||
        openat(-1, "relative", O_RDONLY) != -1 || errno != EBADF) return 9;
    int absolute = openat(-1, "/data", O_RDONLY);
    if (absolute < 0 || openat(absolute, "relative", O_RDONLY) != -1 ||
        errno != ENOTDIR || close(absolute)) return 10;
    if (unlinkat(b, "moved", AT_REMOVEDIR) || getcwd(path, sizeof(path)) ||
        errno != ENOENT) return 11;
    int disconnected = openat(sub, ".", O_RDONLY | O_DIRECTORY);
    if (disconnected < 0 || fstatat(sub, ".", &new_stat, 0) ||
        new_stat.st_nlink || close(disconnected) || chdir("..") ||
        !getcwd(path, sizeof(path)) || strcmp(path, "/ns-b") || fchdir(sub)) return 12;
    child = fork();
    if (!child) {
        char *args[] = {"relative-init", "namespace_exec_deleted", 0};
        char *environment[] = {0};
        execve("../../init", args, environment);
        _exit(1);
    }
    if (namespace_wait(child) || chdir("/") || close(sub) || close(a) ||
        unlinkat(b, "target", 0) || close(b) || rmdir("/ns-renamed") || rmdir("/ns-b")) return 13;
    return 0;
}

#endif
