#ifndef BOAROS_USERLAND_SYNC_H
#define BOAROS_USERLAND_SYNC_H

static int check_file_sync(void)
{
    const int flags[] = {0, O_DSYNC, O_SYNC};
    for (unsigned n = 0; n < sizeof(flags) / sizeof(flags[0]); n++) {
        char data[8] = {0};
        int fd = open("/user-sync", O_CREAT | O_TRUNC | O_RDWR | flags[n], 0600);
        if (fd < 0 || write(fd, "persist", 7) != 7 || fsync(fd) != 0 ||
            fdatasync(fd) != 0 || close(fd) != 0) return 1;
        fd = open("/user-sync", O_RDONLY);
        if (fd < 0 || read(fd, data, sizeof(data)) != 7 ||
            memcmp(data, "persist", 7) != 0 || fsync(fd) != 0 ||
            close(fd) != 0) return 2;
    }
    int directory = open("/", O_RDONLY | O_DIRECTORY);
    if (directory < 0 || fsync(directory) != 0 || close(directory) != 0)
        return 3;
    int pipefd[2];
    if (pipe(pipefd) != 0 || fsync(pipefd[1]) != -1 || errno != EINVAL ||
        close(pipefd[0]) != 0 || close(pipefd[1]) != 0 ||
        fdatasync(-1) != -1 || errno != EBADF) return 4;
    return unlink("/user-sync") != 0 ? 5 : 0;
}

#endif
