#ifndef BOAROS_USERLAND_TMPFS_H
#define BOAROS_USERLAND_TMPFS_H

#include <sys/mount.h>

/* musl 1.2.5 shm_open maps names to /dev/shm and adds CLOEXEC/NOFOLLOW.
 * Pipes supply ordering; shared-file futex support is not a prerequisite. */
static int check_tmpfs_shm(void)
{
    int made_dev = mkdir("/dev", 0755) == 0;
    if (!made_dev && errno != EEXIST) return 1;
    int made_shm = mkdir("/dev/shm", 01777) == 0;
    if (!made_shm && errno != EEXIST) return 2;
    if (mount("none", "/dev/shm", "tmpfs", 0,
              "size=32768,nr_inodes=16,mode=1777")) return 3;
    const char *name = "/boaros-shm-contract";
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || !(fcntl(fd, F_GETFD) & FD_CLOEXEC) || ftruncate(fd, 8192))
        return 4;
    errno = 0;
    if (shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600) != -1 || errno != EEXIST)
        return 5;
    volatile unsigned char *shared = mmap(0, 8192, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED || shared[0] || shared[4096]) return 6;
    shared[0] = 'A';
    int ready[2], ack[2];
    if (pipe(ready) || pipe(ack)) return 7;
    pid_t child = fork();
    if (child < 0) return 8;
    if (!child) {
        close(ready[0]); close(ack[1]);
        int opened = shm_open(name, O_RDWR, 0);
        volatile unsigned char *alias = opened < 0 ? MAP_FAILED :
            mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, opened, 0);
        if (alias == MAP_FAILED || alias == shared || alias[0] != 'A' ||
            shared[0] != 'A') _exit(31);
        alias[0] = 'B';
        alias[4096] = 'C';
        char signal;
        if (write(ready[1], "r", 1) != 1 || read(ack[0], &signal, 1) != 1 ||
            signal != 'u' || alias[1] != 'D') _exit(32);
        errno = 0;
        int replacement = shm_open(name, O_RDONLY, 0);
        char value;
        if (replacement < 0 || pread(replacement, &value, 1, 0) != 1 ||
            value != 'N' || alias[0] != 'B' || shared[4096] != 'C') _exit(33);
        if (close(replacement) || close(opened) || close(fd) ||
            munmap((void *)alias, 8192) || munmap((void *)shared, 8192)) _exit(34);
        close(ready[1]); close(ack[0]);
        _exit(0);
    }
    close(ready[1]); close(ack[0]);
    char signal;
    if (read(ready[0], &signal, 1) != 1 || shared[0] != 'B' ||
        shared[4096] != 'C' || shm_unlink(name)) return 9;
    errno = 0;
    if (shm_open(name, O_RDONLY, 0) != -1 || errno != ENOENT) return 10;
    int fresh = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fresh < 0 || ftruncate(fresh, 4096) || pwrite(fresh, "N", 1, 0) != 1 ||
        shared[0] != 'B') return 11;
    shared[1] = 'D';
    int status;
    if (write(ack[1], "u", 1) != 1 || waitpid(child, &status, 0) != child)
        return 12;
    if (!WIFEXITED(status)) return 100 + WTERMSIG(status);
    if (WEXITSTATUS(status)) return WEXITSTATUS(status);
    if (close(ready[0]) || close(ack[1]) || close(fd) || close(fresh) ||
        shm_unlink(name)) return 13;
    errno = 0;
    if (umount("/dev/shm") != -1 || errno != EBUSY || shared[0] != 'B') return 14;
    if (munmap((void *)shared, 8192) || umount("/dev/shm")) return 15;
    if ((made_shm && rmdir("/dev/shm")) || (made_dev && rmdir("/dev"))) return 16;
    return 0;
}

#endif
