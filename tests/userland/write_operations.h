#ifndef BOAROS_USERLAND_WRITE_OPERATIONS_H
#define BOAROS_USERLAND_WRITE_OPERATIONS_H
static unsigned char operation_data[2][8193];
static int operation_fds[2];
static pthread_barrier_t operation_barrier;
static void *append_records(void *argument)
{
    unsigned index = (uintptr_t)argument;
    struct iovec iov[2] = {{operation_data[index] + 1, 3}, {operation_data[index] + 4, 8189}};
    pthread_barrier_wait(&operation_barrier);
    for (unsigned i = 0; i < 8; i++)
        if (writev(operation_fds[index], iov, 2) != 8192) return (void *)1;
    return 0;
}
static int check_write_operations(void)
{
    int fd = open("/write-atomic", O_CREAT | O_RDWR | O_TRUNC | O_APPEND, 0600);
    if (fd < 0) return 1;
    memset(operation_data[0], 'A', sizeof(operation_data[0]));
    memset(operation_data[1], 'B', sizeof(operation_data[1]));
    if (write(fd, operation_data[0], 4096) != 4096) return 2;
    void *source = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (source == MAP_FAILED) return 3;
    struct iovec mapped[2] = {{operation_data[0], 8192}, {source, 4096}};
    /* 同 inode 的未驻留用户映射会进入 read fault，整次写门闩不能持 inode 数据锁。 */
    if (writev(fd, mapped, 2) != 12288 || munmap(source, 4096)) return 4;
    if (ftruncate(fd, 0)) return 5;
    operation_fds[0] = fd;
    operation_fds[1] = open("/write-atomic", O_RDWR | O_APPEND);
    if (operation_fds[1] < 0 || pthread_barrier_init(&operation_barrier, 0, 3)) return 6;
    pthread_t writers[2];
    for (uintptr_t i = 0; i < 2; i++)
        if (pthread_create(&writers[i], 0, append_records, (void *)i)) return 7;
    pthread_barrier_wait(&operation_barrier);
    for (unsigned i = 0; i < 2; i++) {
        void *result;
        if (pthread_join(writers[i], &result) || result) return 8;
    }
    pthread_barrier_destroy(&operation_barrier);
    unsigned counts[2] = {0};
    for (unsigned record = 0; record < 16; record++) {
        if (pread(fd, operation_data[0], 8192, record * 8192) != 8192) return 9;
        unsigned char first = operation_data[0][0];
        if (first != 'A' && first != 'B') return 10;
        for (unsigned i = 1; i < 8192; i++) if (operation_data[0][i] != first) return 11;
        counts[first == 'B']++;
    }
    if (counts[0] != 8 || counts[1] != 8) return 12;
    close(operation_fds[1]); close(fd); unlink("/write-atomic");
    return 0;
}
#endif
