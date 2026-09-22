#ifndef BOAROS_USERLAND_METADATA_H
#define BOAROS_USERLAND_METADATA_H

static int check_explicit_metadata(void)
{
    struct stat st, saved, target;
    struct statfs before, allocated, released, by_path;
    struct statvfs libc_stats;
    struct timespec times[2] = {{-1, 123456789}, {4102444800LL, 987654321}};
    struct timespec low, high, pause = {0, 50000000};
    if (mkdir("/metadata-dir", 0700)) return 1;
    int dir = open("/metadata-dir", O_RDONLY | O_DIRECTORY);
    int fd = openat(dir, "file", O_CREAT | O_RDWR, 0600);
    int ro = openat(dir, "file", O_RDONLY);
    if (dir < 0 || fd < 0 || ro < 0 ||
        utimensat(dir, "file", times, 0) || fstat(fd, &st) ||
        timestamp_compare(st.st_atim, times[0]) || timestamp_compare(st.st_mtim, times[1])) return 2;
    saved = st;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_nsec = UTIME_NOW;
    if (clock_gettime(CLOCK_REALTIME, &low) || nanosleep(&pause, 0) ||
        futimens(ro, times) || clock_gettime(CLOCK_REALTIME, &high) || fstat(fd, &st) ||
        timestamp_compare(st.st_atim, saved.st_atim) ||
        timestamp_compare(st.st_mtim, low) < 0 || timestamp_compare(st.st_mtim, high) > 0 ||
        timestamp_compare(st.st_ctim, st.st_mtim)) return 3;
    saved = st;
    times[1].tv_nsec = UTIME_OMIT;
    if (syscall(SYS_utimensat, -1, (void *)1, times, -1) ||
        futimens(fd, times) || fstat(fd, &st) ||
        timestamp_compare(st.st_atim, saved.st_atim) ||
        timestamp_compare(st.st_mtim, saved.st_mtim) ||
        timestamp_compare(st.st_ctim, saved.st_ctim)) return 4;
    times[0].tv_nsec = 1000000000;
    if (utimensat(dir, "missing", times, 0) != -1 || errno != ENOENT ||
        futimens(-1, times) != -1 || errno != EBADF ||
        futimens(fd, times) != -1 || errno != EINVAL) return 5;
    times[0] = (struct timespec){12345, 111};
    times[1] = (struct timespec){67890, 222};
    if (symlinkat("file", dir, "link") || fstat(fd, &saved) ||
        utimensat(dir, "link", times, AT_SYMLINK_NOFOLLOW) ||
        fstatat(dir, "link", &st, AT_SYMLINK_NOFOLLOW) ||
        fstat(fd, &target) || timestamp_compare(st.st_atim, times[0]) ||
        timestamp_compare(st.st_mtim, times[1]) ||
        timestamp_compare(saved.st_mtim, target.st_mtim) ||
        unlinkat(dir, "link", 0)) return 6;
    /* Empty pathname and NULL pathname are different ABI forms. */
    if (utimensat(ro, "", times, AT_EMPTY_PATH) || fstat(fd, &st) ||
        timestamp_compare(st.st_mtim, times[1]) ||
        syscall(SYS_utimensat, AT_FDCWD, 0, times, 0) != -1 || errno != EFAULT ||
        syscall(SYS_utimensat, fd, 0, times, AT_SYMLINK_NOFOLLOW) != -1 || errno != EINVAL)
        return 7;
    unsigned char *map = mmap(0, 8192, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED || mprotect(map + 4096, 4096, PROT_NONE)) return 8;
    memcpy(map + 4080, times, 16);
    if (syscall(SYS_utimensat, fd, 0, map + 4080, 0) != -1 || errno != EFAULT ||
        munmap(map, 8192)) return 9;
    times[0] = (struct timespec){INT64_MIN, 999};
    times[1] = (struct timespec){INT64_MAX, 999};
    if (futimens(fd, times) || fstat(fd, &st) ||
        st.st_atim.tv_sec != INT32_MIN || st.st_atim.tv_nsec ||
        st.st_mtim.tv_sec != 15032385535LL || st.st_mtim.tv_nsec) return 10;
    if (fstatfs(fd, &before) || statfs("/metadata-dir/file", &by_path) ||
        memcmp(&before, &by_path, sizeof(before)) ||
        before.f_type != 0xef53 || before.f_bsize < 1024 || before.f_bsize > 4096 ||
        before.f_frsize != before.f_bsize || before.f_namelen != 255 ||
        !before.f_blocks || before.f_bfree > before.f_blocks ||
        before.f_bavail > before.f_bfree || before.f_ffree > before.f_files ||
        (before.f_flags & 1) || statvfs("/metadata-dir/file", &libc_stats) ||
        libc_stats.f_blocks != before.f_blocks || libc_stats.f_bsize != before.f_bsize)
        return 11;
    char block[4096];
    memset(block, 'M', sizeof(block));
    if (write(fd, block, sizeof(block)) != sizeof(block) || fsync(fd) ||
        fstatfs(fd, &allocated) || allocated.f_bfree >= before.f_bfree ||
        allocated.f_ffree != before.f_ffree || ftruncate(fd, 0) ||
        fstatfs(fd, &released) || released.f_bfree != before.f_bfree) return 12;
    if (unlinkat(dir, "file", 0) || futimens(ro, 0) ||
        fstat(ro, &st) || st.st_nlink || fstatfs(ro, &released) ||
        released.f_ffree != before.f_ffree || close(fd) || close(ro) ||
        fstatfs(dir, &released) || released.f_ffree != before.f_ffree + 1 ||
        close(dir) || rmdir("/metadata-dir")) return 13;
    /* musl statfs zeroes its output in userspace; exercise bad pointers at
     * the raw syscall boundary instead. */
    if (syscall(SYS_statfs, "/missing", (void *)1) != -1 || errno != ENOENT ||
        syscall(SYS_fstatfs, -1, (void *)1) != -1 || errno != EBADF ||
        syscall(SYS_statfs, "/", (void *)1) != -1 || errno != EFAULT) return 14;
    return 0;
}

#endif
