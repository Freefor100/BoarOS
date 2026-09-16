#ifndef BOAROS_USERLAND_TIMESTAMPS_H
#define BOAROS_USERLAND_TIMESTAMPS_H

/* Included by the real musl program after its system headers. */
static int timestamp_compare(struct timespec a, struct timespec b)
{
    return a.tv_sec != b.tv_sec ? (a.tv_sec > b.tv_sec ? 1 : -1) :
        a.tv_nsec != b.tv_nsec ? (a.tv_nsec > b.tv_nsec ? 1 : -1) : 0;
}

static int timestamp_check(const struct stat *before, const struct stat *after,
                           struct timespec low, struct timespec high, int mask)
{
    struct timespec previous[] = {before->st_atim, before->st_mtim, before->st_ctim};
    struct timespec current[] = {after->st_atim, after->st_mtim, after->st_ctim};
    for (int i = 0; i < 3; i++) {
        int changed = timestamp_compare(previous[i], current[i]) != 0;
        if (changed != !!(mask & (1 << i)) ||
            (changed && (timestamp_compare(current[i], low) < 0 ||
                         timestamp_compare(current[i], high) > 0))) return 0;
    }
    return !(mask & 2) || timestamp_compare(after->st_mtim, after->st_ctim) == 0;
}

static int check_file_timestamps(void)
{
    struct stat previous = {0}, current, parent_before, parent_after;
    struct timespec low, high, delay = {0, 50000000};
    int fd = -1, readonly = -1, parent = -1, failure = 0;
    unsigned char *map = mmap(0, 8192, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char byte;
    if (map == MAP_FAILED || mprotect(map + 4096, 4096, PROT_NONE) != 0 ||
        (parent = open("/", O_RDONLY | O_DIRECTORY)) < 0) {
        failure = 1;
        goto done;
    }
    map[4095] = 'P';
#define TIME_BEGIN(case_id) do { \
    failure = (case_id); \
    if ((fd >= 0 && fstat(fd, &previous) != 0) || \
        fstat(parent, &parent_before) != 0 || \
        clock_gettime(CLOCK_REALTIME, &low) != 0 || nanosleep(&delay, 0) != 0) \
        goto done; \
} while (0)
#define TIME_END(file_mask, parent_mask) do { \
    if (clock_gettime(CLOCK_REALTIME, &high) != 0 || fstat(fd, &current) != 0 || \
        fstat(parent, &parent_after) != 0 || \
        !timestamp_check(&previous, &current, low, high, (file_mask)) || \
        !timestamp_check(&parent_before, &parent_after, low, high, (parent_mask))) \
        goto done; \
} while (0)
    TIME_BEGIN(2);
    fd = open("/timestamp-musl", O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) goto done;
    TIME_END(7, 6);
    if (timestamp_compare(current.st_atim, current.st_mtim) != 0 ||
        (readonly = open("/timestamp-musl", O_RDONLY)) < 0) goto done;
    TIME_BEGIN(3);
    if (write(fd, map + 4096, 0) != 0) goto done;
    TIME_END(0, 0);
    TIME_BEGIN(4);
    errno = 0;
    if (write(readonly, "x", 1) != -1 || errno != EBADF) goto done;
    TIME_END(0, 0);
    TIME_BEGIN(5);
    if (write(fd, "A", 1) != 1 || lseek(fd, 0, SEEK_SET) != 0) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(6);
    if (read(fd, &byte, 1) != 1) goto done;
    TIME_END(1, 0);
    TIME_BEGIN(7);
    errno = 0;
    if (write(fd, map + 4096, 1) != -1 || errno != EFAULT) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(8);
    if (pread(fd, &byte, 1, 0) != 1) goto done;
    TIME_END(1, 0);
    TIME_BEGIN(9);
    if (pread(fd, &byte, 1, 0) != 1 || read(fd, map + 4096, 0) != 0) goto done;
    TIME_END(0, 0);
    if (write(fd, map + 4096, 1) != -1 || errno != EFAULT) goto done;
    TIME_BEGIN(10);
    if (pread(fd, &byte, 1, 1) != 0) goto done;
    TIME_END(1, 0);
    if (write(fd, map + 4096, 1) != -1 || errno != EFAULT ||
        lseek(fd, 0, SEEK_SET) != 0) goto done;
    TIME_BEGIN(11);
    if (read(fd, map + 4096, 1) != -1 || errno != EFAULT) goto done;
    TIME_END(1, 0);
    TIME_BEGIN(12);
    if (write(fd, map + 4095, 2) != 1) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(13);
    if (ftruncate(fd, 1) != 0) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(14);
    if (ftruncate(fd, 4096) != 0) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(15);
    if (ftruncate(fd, 1) != 0) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(16);
    if (unlink("/timestamp-musl") != 0) goto done;
    TIME_END(4, 6);
    TIME_BEGIN(17);
    if (write(fd, "U", 1) != 1 || lseek(fd, 0, SEEK_SET) != 0) goto done;
    TIME_END(6, 0);
    TIME_BEGIN(18);
    if (read(fd, &byte, 1) != 1) goto done;
    TIME_END(1, 0);
    if (fstat(fd, &current) != 0) goto done;
    off_t maxbytes = (off_t)((UINT64_C(1) << 32) - 1) * current.st_blksize;
    if (lseek(fd, maxbytes, SEEK_SET) != maxbytes) goto done;
    TIME_BEGIN(19);
    errno = 0;
    if (write(fd, "X", 1) != -1 || errno != EFBIG) goto done;
    TIME_END(0, 0);
    if (lseek(fd, 0, SEEK_SET) != 0 ||
        write(fd, map + 4096, 1) != -1 || errno != EFAULT) goto done;
    TIME_BEGIN(20);
    errno = 0;
    if (syscall(SYS_pread64, fd, (void *)-4096L, 1, 0) != -1 || errno != EFAULT)
        goto done;
    TIME_END(0, 0);
    TIME_BEGIN(21);
    errno = 0;
    if (syscall(SYS_pread64, fd, (void *)-4096L, 0, 0) != -1 || errno != EFAULT)
        goto done;
    TIME_END(0, 0);
    TIME_BEGIN(22);
    errno = 0;
    if (syscall(SYS_pread64, fd, (void *)-4096L, 1, current.st_size) != -1 || errno != EFAULT)
        goto done;
    TIME_END(0, 0);
    TIME_BEGIN(23);
    errno = 0;
    if (syscall(SYS_pread64, fd, map, (size_t)-1, 0) != -1 || errno != EFAULT)
        goto done;
    TIME_END(0, 0);
    failure = 0;
done:
    if (readonly >= 0) close(readonly);
    if (fd >= 0) close(fd);
    if (parent >= 0) close(parent);
    if (map != MAP_FAILED) munmap(map, 8192);
    if (failure) fprintf(stderr, "file timestamp case=%d errno=%d\n", failure, errno);
    return failure;
#undef TIME_BEGIN
#undef TIME_END
}
#endif
